// Benchmark for the general-boundary variable-Isp solvers (src/variable_isp/GeneralTransfer).
//
//   spacetrains_visp_bench <set> [--stride N] [--limit N] [--csv path]
//
// Sets: A (circular -> circular atlas transfers, ground truth from the atlas costates),
//       B (A with perturbed endpoint velocities), C (chase: moving coasting target).

#include "variable_isp/GeneralTransfer.hpp"
#include "variable_isp/VariableIsp.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace vi = spacetrains::variable_isp;
namespace g = spacetrains::variable_isp::general;

namespace {

constexpr double kAU = vi::VariableIspIntegrator::kAstronomicalUnitM;
constexpr double kMu = vi::VariableIspIntegrator::kMuSunSI;
const double kV0 = std::sqrt(kMu / kAU);
const double kT0 = std::sqrt(kAU * kAU * kAU / kMu);
const double kA0 = kMu / (kAU * kAU);

struct Case {
    std::string label;
    g::State start;
    g::State end;
    double T {0.0};
    double J {0.0};          // known solution's effort (not necessarily the optimum), 0 if unknown
    double sweep {0.0};      // known solution's swept angle
    g::Primer primer;        // known solution's primer
    bool has_truth {false};
    double min_radius {0.0};  // known solution's perihelion
    vi::AtlasSeed seed;
    vi::CanonicalMissionConfig config;
    double rho {0.0}, kappa {0.0}, theta_label {0.0};
};

g::State polar_state(double r, double th, double vr, double vt) {
    const g::Vec2 er {std::cos(th), std::sin(th)};
    const g::Vec2 et {-er.y, er.x};
    return {r * er, vr * er + vt * et};
}

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

const g::Propagator& propagator() {
    static const g::Propagator p;
    return p;
}

struct Filter {
    double rho_min {0.25}, rho_max {12.0}, kappa_min {0.0}, kappa_max {2000.0};
};

std::vector<Case> atlas_cases(std::size_t stride, std::size_t limit, const Filter& filter) {
    vi::VariableIspAtlas atlas;
    atlas.load_binary("tests/data/variable_isp/variable_isp_atlas.bin");
    const vi::VariableIspIntegrator integrator;
    const auto& rg = atlas.rho_grid();
    const auto& kg = atlas.kappa_grid();
    const auto& tg = atlas.theta_grid();
    std::vector<Case> cases;
    std::vector<std::array<std::size_t, 3>> eligible;
    for (std::size_t i = 0; i < rg.size(); ++i) {
        for (std::size_t j = 0; j < kg.size(); ++j) {
            for (std::size_t k = 0; k < tg.size(); ++k) {
                if (!atlas.is_solved(i, j, k)) continue;
                if (rg[i] < filter.rho_min || rg[i] > filter.rho_max || kg[j] < filter.kappa_min || kg[j] > filter.kappa_max) continue;
                eligible.push_back({i, j, k});
            }
        }
    }
    std::mt19937_64 rng(12345 + stride);
    std::shuffle(eligible.begin(), eligible.end(), rng);
    for (const auto& [i, j, k] : eligible) {
                const auto seed = atlas.seed_at(i, j, k);
                const auto config = vi::VariableIspIntegrator::canonical_config(rg[i], kg[j]);
                vi::IntegrationSummary run;
                try {
                    run = integrator.integrate_fixed_time(seed, config, 2);
                } catch (const std::exception&) {
                    continue;
                }
                const auto& e = run.samples.back();
                Case c;
                c.label = "A";
                c.rho = rg[i];
                c.kappa = kg[j];
                c.theta_label = tg[k];
                c.start = polar_state(1.0, 0.0, 0.0, config.vtheta0_mps / kV0);
                c.end = polar_state(e.r_m / kAU, e.theta_rad, e.vr_mps / kV0, e.vtheta_mps / kV0);
                c.T = seed.transfer_time_days * vi::VariableIspIntegrator::kDayS / kT0;
                const double j_si = 2.0 * config.power_w * (1.0 / e.mass_kg - 1.0 / config.m0_kg);
                c.J = j_si / (kA0 * kA0 * kT0);
                c.sweep = e.theta_rad;
                const double kgain = config.k_gain;
                const double lr = seed.params[0], lvr = seed.params[1], lvt = seed.params[2], ct = seed.params[4];
                const double r0 = config.r0_m, vr0 = config.vr0_mps, vt0 = config.vtheta0_mps;
                c.primer.p = {kgain * lvr / kA0, kgain * lvt / kA0};
                c.primer.w = {-kgain * lr / (kA0 / kT0), -kgain * (ct + vt0 * lvr - vr0 * lvt) / r0 / (kA0 / kT0)};
                c.has_truth = true;
                c.seed = seed;
                c.config = config;
                c.min_radius = propagator().propagate(c.start, c.primer, c.T, false).min_radius;
                cases.push_back(c);
                if (cases.size() >= limit) return cases;
    }
    return cases;
}

struct Outcome {
    bool ok {false};
    double J {0.0};
    double seconds {0.0};
    std::size_t iterations {0};
    std::size_t propagations {0};
    double sweep {0.0};
    double residual {0.0};
};

struct Stats {
    std::size_t n {0}, ok {0}, match {0}, better {0}, good {0};
    std::vector<double> times;
    std::vector<double> rel;
    std::size_t props {0};
};

void report(const std::string& name, Stats& s) {
    std::sort(s.times.begin(), s.times.end());
    std::sort(s.rel.begin(), s.rel.end());
    const auto q = [](const std::vector<double>& v, double f) { return v.empty() ? 0.0 : v[std::min(v.size() - 1, static_cast<std::size_t>(f * v.size()))]; };
    std::printf("%-22s n=%4zu ok=%5.1f%% good=%5.1f%% match=%5.1f%% better=%4zu  t50=%7.3fms t90=%7.3fms t99=%7.3fms  props/case=%5.1f  relJ50=%+.2e relJ90=%+.2e\n",
        name.c_str(), s.n, 100.0 * s.ok / std::max<std::size_t>(1, s.n), 100.0 * s.good / std::max<std::size_t>(1, s.n), 100.0 * s.match / std::max<std::size_t>(1, s.n), s.better,
        1e3 * q(s.times, 0.5), 1e3 * q(s.times, 0.9), 1e3 * q(s.times, 0.99),
        static_cast<double>(s.props) / std::max<std::size_t>(1, s.n), q(s.rel, 0.5), q(s.rel, 0.9));
}

using Method = std::function<Outcome(const Case&)>;

Outcome from_solution(const g::Solution& sol, double seconds) {
    Outcome o;
    o.ok = sol.converged;
    o.J = sol.end.effort;
    o.seconds = seconds;
    o.iterations = sol.iterations;
    o.propagations = sol.propagations;
    o.sweep = sol.end.sweep_rad;
    o.residual = sol.residual;
    return o;
}


Outcome shoot_from(const Case& c, const g::Primer& seed, std::chrono::steady_clock::time_point t0) {
    const auto sol = g::shoot(propagator(), c.start, g::fixed_target(c.end), g::EndCondition::Rendezvous, seed, c.T);
    return from_solution(sol, seconds_since(t0));
}

g::ShapeResult best_shape(const Case& c, const g::ShapeOptions& opt, int branches, bool use_truth_branch) {
    g::ShapeResult best;
    std::vector<double> angles = use_truth_branch ? std::vector<double> {c.sweep}
                                                  : g::arrival_angle_candidates(c.start, c.end, c.T, branches);
    if (use_truth_branch) {
        // Relative to start angle 0 (cases start on +x).
    }
    for (const double th : angles) {
        auto s = g::shape_solve(c.start, c.end, c.T, th, opt);
        if (s.ok && (!best.ok || s.effort < best.effort)) best = std::move(s);
    }
    return best;
}

g::Primer shape_primer_fit(const Case& c, const g::ShapeResult& s, std::size_t samples) {
    std::vector<double> times;
    std::vector<g::Vec2> pos, thrust;
    for (std::size_t i = 0; i < samples; ++i) {
        const double sv = static_cast<double>(i) / static_cast<double>(samples - 1);
        const auto pt = g::shape_eval(c.start, c.end, s, sv);
        times.push_back(sv * c.T);
        pos.push_back(pt.state.r);
        thrust.push_back(pt.thrust);
    }
    return g::fit_primer(times, pos, thrust);
}

}  // namespace

int main(int argc, char** argv) {
    std::string set = argc > 1 ? argv[1] : "A";
    std::size_t stride = 37;
    std::size_t limit = 400;
    std::string only;
    int shape_terms = 8;
    int debug_case = -1;
    Filter filter;
    std::string csv_path;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--stride" && i + 1 < argc) stride = std::stoul(argv[++i]);
        else if (a == "--limit" && i + 1 < argc) limit = std::stoul(argv[++i]);
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--terms" && i + 1 < argc) shape_terms = std::stoi(argv[++i]);
        else if (a == "--rho-min" && i + 1 < argc) filter.rho_min = std::stod(argv[++i]);
        else if (a == "--rho-max" && i + 1 < argc) filter.rho_max = std::stod(argv[++i]);
        else if (a == "--kappa-min" && i + 1 < argc) filter.kappa_min = std::stod(argv[++i]);
        else if (a == "--kappa-max" && i + 1 < argc) filter.kappa_max = std::stod(argv[++i]);
        else if (a == "--csv" && i + 1 < argc) csv_path = argv[++i];
        else if (a == "--debug" && i + 1 < argc) debug_case = std::stoi(argv[++i]);
    }

    std::vector<Case> cases = atlas_cases(stride, limit, filter);
    std::fprintf(stderr, "%zu cases\n", cases.size());
    if (set == "R") {
        // Refinement as the planner does it: an atlas cell's seed onto a nearby target.
        const vi::VariableIspIntegrator integrator;
        std::mt19937_64 rng(7);
        std::uniform_real_distribution<double> u(-1.0, 1.0);
        struct R { std::size_t ok {0}, n {0}; std::vector<double> t; double dJ {0}; };
        R old_r, new_r;
        std::size_t both = 0;
        double max_dT = 0.0, max_dJ = 0.0;
        double it_old = 0, it_new = 0;
        for (const auto& c : cases) {
            if (c.min_radius < 0.05) continue;
            const double r_target = c.end.r.x * 0.0 + g::norm(c.end.r) * std::exp(0.06 * u(rng)) * kAU;
            const double th_target = c.sweep + 0.06 * u(rng);
            const auto mass_of = [&](const vi::AtlasSeed& sd) {
                return integrator.integrate_fixed_time(sd, c.config, 2).samples.back().mass_kg;
            };
            auto t0 = std::chrono::steady_clock::now();
            vi::ShootingResult a, b;
            try { a = integrator.refine_seed(c.seed, c.config, r_target, th_target); } catch (...) {}
            const double ta = seconds_since(t0);
            t0 = std::chrono::steady_clock::now();
            try {
                b = integrator.refine_seed_stm(c.seed, c.config, [&](double) { return std::pair {r_target, th_target}; });
            } catch (...) {}
            const double tb = seconds_since(t0);
            ++old_r.n; ++new_r.n;
            it_old += a.iterations; it_new += b.iterations;
            if (a.converged) { ++old_r.ok; old_r.t.push_back(ta); }
            if (b.converged) { ++new_r.ok; new_r.t.push_back(tb); }
            else if (a.converged) std::printf("  stm fail: rho=%.3f kappa=%.2f T=%.1f it=%zu res=%.2e minr=%.3f sweep=%.2f\n", c.rho, c.kappa,
                c.seed.transfer_time_days, b.iterations, b.residual_norm, c.min_radius, c.sweep);
            if (a.converged && b.converged) {
                ++both;
                const double dT = std::abs(a.seed.transfer_time_days - b.seed.transfer_time_days) / a.seed.transfer_time_days;
                double ma = 0, mb = 0;
                try { ma = mass_of(a.seed); mb = mass_of(b.seed); } catch (...) {}
                const double ja = 1.0 / ma - 1.0 / 3000.0, jb = 1.0 / mb - 1.0 / 3000.0;
                const double dJ = std::abs(ja - jb) / ja;
                max_dT = std::max(max_dT, dT);
                if (dJ > 1e-3) std::printf("  differ: rho=%.3f kappa=%.2f T %.2f vs %.2f d, J %.4g vs %.4g\n", c.rho, c.kappa,
                    a.seed.transfer_time_days, b.seed.transfer_time_days, ja, jb);
                max_dJ = std::max(max_dJ, dJ);
            }
        }
        for (auto* r : {&old_r, &new_r}) {
            std::sort(r->t.begin(), r->t.end());
            const auto q = [&](double f) { return r->t.empty() ? 0.0 : r->t[std::min(r->t.size() - 1, static_cast<std::size_t>(f * r->t.size()))]; };
            std::printf("%s ok=%zu/%zu t50=%.3fms t90=%.3fms t99=%.3fms\n", r == &old_r ? "refine_seed    " : "refine_seed_stm", r->ok, r->n,
                1e3 * q(0.5), 1e3 * q(0.9), 1e3 * q(0.99));
        }
        std::printf("mean iterations old %.2f new %.2f\n", it_old / old_r.n, it_new / new_r.n);
        std::printf("both=%zu max rel dT=%.2e max rel dJ=%.2e\n", both, max_dT, max_dJ);
        return 0;
    }
    if (debug_case >= 0) {
        const auto& c = cases.at(static_cast<std::size_t>(debug_case));
        std::printf("case rho=%.3f kappa=%.3f theta=%.3f T=%.4f J=%.6f sweep=%.4f\n", c.rho, c.kappa, c.theta_label, c.T, c.J, c.sweep);
        const auto truth = propagator().sample(c.start, c.primer, c.T, 11);
        for (int terms : {0, 2, 4, 8, 12, 16}) {
            g::ShapeOptions o;
            o.terms = static_cast<std::size_t>(terms);
            const auto s = g::shape_solve(c.start, c.end, c.T, c.sweep, o);
            std::printf("terms=%2d ok=%d J=%.6f it=%zu minr=%.3f\n", terms, s.ok, s.effort, s.iterations, s.min_radius);
            if (terms == 8) {
                for (const auto& smp : truth) {
                    const auto pt = g::shape_eval(c.start, c.end, s, smp.t / c.T);
                    std::printf("  t=%.3f truth r=(%.4f,%.4f) a=(%.4f,%.4f) | shape r=(%.4f,%.4f) a=(%.4f,%.4f)\n", smp.t,
                        smp.state.r.x, smp.state.r.y, smp.thrust.x, smp.thrust.y, pt.state.r.x, pt.state.r.y, pt.thrust.x, pt.thrust.y);
                }
            }
        }
        return 0;
    }

    g::ShapeOptions shape_opt;
    shape_opt.terms = static_cast<std::size_t>(shape_terms);

    std::vector<std::pair<std::string, Method>> methods;
    methods.push_back({"truth-rollout", [](const Case& c) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto end = propagator().propagate(c.start, c.primer, c.T, false);
        Outcome o;
        const double miss = g::norm(end.state.r - c.end.r) + g::norm(end.state.v - c.end.v);
        o.ok = end.ok && miss < 1e-5;
        o.J = end.effort;
        o.residual = miss;
        o.seconds = seconds_since(t0);
        o.propagations = 1;
        return o;
    }});
    methods.push_back({"shoot-truth", [](const Case& c) {
        const auto t0 = std::chrono::steady_clock::now();
        return shoot_from(c, c.primer, t0);
    }});
    methods.push_back({"free-space", [](const Case& c) {
        const auto t0 = std::chrono::steady_clock::now();
        return shoot_from(c, g::free_space_seed(c.start, c.end, c.T), t0);
    }});
    methods.push_back({"shape-only(truthbr)", [shape_opt](const Case& c) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto s = best_shape(c, shape_opt, 1, true);
        Outcome o;
        o.ok = s.ok;
        o.J = s.effort;
        o.iterations = s.iterations;
        o.seconds = seconds_since(t0);
        return o;
    }});
    methods.push_back({"shape-only(3br)", [shape_opt](const Case& c) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto s = best_shape(c, shape_opt, 3, false);
        Outcome o;
        o.ok = s.ok;
        o.J = s.effort;
        o.iterations = s.iterations;
        o.sweep = s.theta_end;
        o.seconds = seconds_since(t0);
        return o;
    }});
    methods.push_back({"shape0+shoot", [shape_opt](const Case& c) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto s = best_shape(c, shape_opt, 3, false);
        if (!s.ok) return Outcome {};
        const auto pt = g::shape_eval(c.start, c.end, s, 0.0);
        return shoot_from(c, {pt.thrust, pt.thrust_rate}, t0);
    }});
    methods.push_back({"shapefit+shoot", [shape_opt](const Case& c) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto s = best_shape(c, shape_opt, 3, false);
        if (!s.ok) return Outcome {};
        return shoot_from(c, shape_primer_fit(c, s, 200), t0);
    }});

    std::ofstream csv;
    if (!csv_path.empty()) {
        csv.open(csv_path);
        csv << "method,case,rho,kappa,theta,T,J_truth,sweep_truth,minr_truth,ok,J,seconds,iterations,props,sweep\n";
    }
    methods.push_back({"angleshape-only(3br)", [shape_opt](const Case& c) {
        const auto t0 = std::chrono::steady_clock::now();
        g::ShapeResult best;
        for (const double th : g::arrival_angle_candidates(c.start, c.end, c.T, 3)) {
            auto s = g::angle_shape_solve(c.start, c.end, c.T, th, shape_opt);
            if (s.ok && (!best.ok || s.effort < best.effort)) best = std::move(s);
        }
        Outcome o;
        o.ok = best.ok;
        o.J = best.effort;
        o.iterations = best.iterations;
        o.sweep = best.theta_end;
        o.seconds = seconds_since(t0);
        return o;
    }});
    methods.push_back({"angleshapefit+shoot", [shape_opt](const Case& c) {
        const auto t0 = std::chrono::steady_clock::now();
        g::ShapeResult best;
        for (const double th : g::arrival_angle_candidates(c.start, c.end, c.T, 3)) {
            auto s = g::angle_shape_solve(c.start, c.end, c.T, th, shape_opt);
            if (s.ok && (!best.ok || s.effort < best.effort)) best = std::move(s);
        }
        if (!best.ok) return Outcome {};
        std::vector<double> times;
        std::vector<g::Vec2> pos, thrust;
        for (std::size_t i = 0; i < 400; ++i) {
            const auto pt = g::angle_shape_eval(c.start, c.end, best, static_cast<double>(i) / 399.0);
            times.push_back(pt.t);
            pos.push_back(pt.state.r);
            thrust.push_back(pt.thrust);
        }
        return shoot_from(c, g::fit_primer(times, pos, thrust), t0);
    }});
    methods.push_back({"portfolio-min", [shape_opt](const Case& c) {
        const auto t0 = std::chrono::steady_clock::now();
        auto o = shoot_from(c, g::free_space_seed(c.start, c.end, c.T), t0);
        const auto s = best_shape(c, shape_opt, 3, false);
        if (!s.ok) return o;
        auto o2 = shoot_from(c, shape_primer_fit(c, s, 200), t0);
        o2.propagations += o.propagations;
        if (o.ok && (!o2.ok || o.J < o2.J)) {
            o.seconds = o2.seconds;
            o.propagations = o2.propagations;
            return o;
        }
        return o2;
    }});
    for (auto& [name, method] : methods) {
        if (!only.empty() && name.find(only) == std::string::npos) continue;
        Stats st;
        for (std::size_t ci = 0; ci < cases.size(); ++ci) {
            const auto& c = cases[ci];
            const auto o = method(c);
            if (csv) {
                csv << name << ',' << ci << ',' << c.rho << ',' << c.kappa << ',' << c.theta_label << ',' << c.T << ',' << c.J << ','
                    << c.sweep << ',' << c.min_radius << ',' << o.ok << ',' << o.J << ',' << o.seconds << ',' << o.iterations << ',' << o.propagations << ','
                    << o.sweep << '\n';
            }
            ++st.n;
            st.props += o.propagations;
            if (!o.ok) continue;
            ++st.ok;
            st.times.push_back(o.seconds);
            if (c.has_truth) {
                const double rel = (o.J - c.J) / c.J;
                st.rel.push_back(rel);
                if (std::abs(rel) < 1e-3) ++st.match;
                else if (rel < 0) ++st.better;
                if (rel < 1e-3) ++st.good;
            }
        }
        report(name, st);
    }
    return 0;
}
