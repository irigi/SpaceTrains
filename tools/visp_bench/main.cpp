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
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
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
    static const g::Propagator p = [] {
        g::PropagationSettings settings;
        if (const char* h = std::getenv("HIGH_ORDER")) settings.high_order = std::atoi(h) != 0;
        if (const char* r = std::getenv("RTOL")) {
            settings.relative_tolerance = std::atof(r);
            settings.absolute_tolerance = 0.1 * settings.relative_tolerance;
        }
        return g::Propagator(settings);
    }();
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
        const auto pt = g::shape_eval(c.start, c.end, s, sv, false);
        times.push_back(sv * c.T);
        pos.push_back(pt.state.r);
        thrust.push_back(pt.thrust);
    }
    return g::fit_primer(times, pos, thrust);
}


// ---- General boundary sets (no ground truth: compare against the best valid solution) ----------

struct GCase {
    std::string label;
    g::State start;
    g::Target target;
    double T {0.0};
    g::EndCondition end_condition {g::EndCondition::Rendezvous};
    g::Primer atlas_primer;
    bool has_atlas {false};
};

g::State orbit_state(double a, double e, double nu, double omega) {
    const double p = a * (1.0 - e * e);
    const double r = p / (1.0 + e * std::cos(nu));
    const double th = nu + omega;
    const double vr = std::sqrt(1.0 / p) * e * std::sin(nu);
    const double vt = std::sqrt(1.0 / p) * (1.0 + e * std::cos(nu));
    return polar_state(r, th, vr, vt);
}

std::vector<GCase> general_cases(const std::string& set, const std::vector<Case>& atlas, std::size_t limit) {
    std::vector<GCase> out;
    std::mt19937_64 rng(99);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const auto kick = [&](g::Vec2 v, double frac) {
        const double ang = 6.283185307179586 * u(rng);
        const double mag = frac * u(rng) * g::norm(v);
        return v + g::Vec2 {mag * std::cos(ang), mag * std::sin(ang)};
    };
    if (set == "AG") {
        // Atlas transfers (circular to circular) without the atlas seed; the atlas solution is
        // added as a method so "best" includes it.
        for (const auto& c : atlas) {
            if (out.size() >= limit) break;
            GCase gc;
            gc.label = "AG";
            gc.start = c.start;
            gc.target = g::fixed_target(c.end);
            gc.T = c.T;
            gc.atlas_primer = c.primer;
            gc.has_atlas = true;
            out.push_back(gc);
        }
        return out;
    }
    if (set == "E") {
        // Chase a thrusting ship: the target flies an atlas transfer (perihelion > 0.3); the
        // chaser leaves the origin planet tau later.
        for (const auto& c : atlas) {
            if (out.size() >= limit) break;
            if (c.min_radius < 0.3 || c.T < 0.3) continue;
            const double tau = 0.3 * c.T * u(rng);
            GCase gc;
            gc.label = "E";
            gc.start = polar_state(1.0, tau, 0.0, 1.0);
            const g::State ts = c.start;
            const g::Primer tz = c.primer;
            gc.target = [ts, tz, tau](double T) {
                const auto e = propagator().propagate(ts, tz, tau + T, false);
                const double rr = g::norm(e.state.r);
                return g::TargetPoint {e.state, e.state.v, e.primer.p + (-1.0 / (rr * rr * rr)) * e.state.r};
            };
            gc.T = (0.1 + 0.9 * u(rng)) * (c.T - tau);
            gc.end_condition = u(rng) < 0.5 ? g::EndCondition::Rendezvous : g::EndCondition::Intercept;
            out.push_back(gc);
        }
        return out;
    }
    if (set == "B") {
        for (const auto& c : atlas) {
            if (out.size() >= limit) break;
            GCase gc;
            gc.label = "B";
            gc.start = c.start;
            gc.start.v = kick(c.start.v, 0.3);
            g::State end = c.end;
            end.v = kick(end.v, 0.3);
            gc.target = g::fixed_target(end);
            gc.T = c.T;
            out.push_back(gc);
        }
    } else {
        const bool hard = set == "D";
        while (out.size() < limit) {
            GCase gc;
            gc.label = set;
            const double rc = std::exp(std::log(0.5) + u(rng) * std::log(10.0));
            gc.start = polar_state(rc, 0.0, 0.0, std::sqrt(1.0 / rc));
            gc.start.v = kick(gc.start.v, hard ? 0.3 : 0.15);
            const double a = rc * std::exp((hard ? 2.0 : 0.6) * (u(rng) - 0.5));
            const double e = (hard ? 0.6 : 0.3) * u(rng);
            const double nu = 6.283185307179586 * u(rng);
            // Place the target within ±1 rad of the chaser's angle.
            const double phase = (hard ? 6.283185307179586 : 2.0) * (u(rng) - 0.5);
            const g::State ts = orbit_state(a, e, nu, phase - nu);
            gc.target = g::coasting_target(ts);
            gc.T = hard ? std::exp(std::log(0.2) + u(rng) * std::log(50.0)) : std::exp(std::log(0.05) + u(rng) * std::log(60.0));
            gc.end_condition = u(rng) < 0.5 ? g::EndCondition::Rendezvous : g::EndCondition::Intercept;
            out.push_back(gc);
        }
    }
    return out;
}

struct GOutcome {
    bool ok {false};
    double sweep {0.0};
    double shape_J {0.0};
    double shape_theta {0.0};
    double J {std::numeric_limits<double>::infinity()};
    double seconds {0.0};
    std::size_t props {0};
};

GOutcome g_outcome(const g::Solution& sol, std::chrono::steady_clock::time_point t0) {
    GOutcome o;
    o.ok = sol.converged && sol.end.min_radius >= 0.1;
    if (o.ok) o.J = sol.end.effort;
    o.sweep = sol.end.sweep_rad;
    o.props = sol.propagations;
    o.seconds = seconds_since(t0);
    return o;
}

int run_general(const std::string& set, const std::vector<Case>& atlas, std::size_t limit, const std::string& only, int terms) {
    const auto cases = general_cases(set, atlas, limit);
    using GMethod = std::function<GOutcome(const GCase&)>;
    std::vector<std::pair<std::string, GMethod>> methods;
    const std::size_t fit_samples = std::getenv("FIT_SAMPLES") ? std::atoi(std::getenv("FIT_SAMPLES")) : 200;
    const auto shoot_seed = [](const GCase& c, const g::Primer& seed) {
        return g::shoot(propagator(), c.start, c.target, c.end_condition, seed, c.T);
    };
    if (set == "AG") {
        methods.push_back({"atlas-solution", [&](const GCase& c) {
            const auto t0 = std::chrono::steady_clock::now();
            g::Solution sol;
            sol.end = propagator().propagate(c.start, c.atlas_primer, c.T, false);
            const auto goal = c.target(c.T).state;
            sol.converged = sol.end.ok && g::norm(sol.end.state.r - goal.r) + g::norm(sol.end.state.v - goal.v) < 1e-5;
            return g_outcome(sol, t0);
        }});
    }
    methods.push_back({"zero(LQ-ballistic)", [&](const GCase& c) {
        const auto t0 = std::chrono::steady_clock::now();
        return g_outcome(shoot_seed(c, {}), t0);
    }});
    methods.push_back({"bhom", [&](const GCase& c) {
        const auto t0 = std::chrono::steady_clock::now();
        return g_outcome(g::continue_in_boundary(propagator(), c.start, c.target, c.end_condition, c.T), t0);
    }});
    methods.push_back({"free-space", [&](const GCase& c) {
        const auto t0 = std::chrono::steady_clock::now();
        return g_outcome(shoot_seed(c, g::free_space_seed(c.start, c.target(c.T).state, c.T)), t0);
    }});
    methods.push_back({"shapefit", [&](const GCase& c) {
        const auto t0 = std::chrono::steady_clock::now();
        Case cc;
        cc.start = c.start;
        cc.end = c.target(c.T).state;
        cc.T = c.T;
        g::ShapeOptions opt;
        opt.terms = static_cast<std::size_t>(terms);
        opt.nodes = std::getenv("SHAPE_NODES") ? std::atoi(std::getenv("SHAPE_NODES")) : 24;
        opt.free_end_velocity = c.end_condition == g::EndCondition::Intercept;
        g::ShapeResult best;
        for (const double th : g::arrival_angle_candidates(cc.start, cc.end, cc.T, 3)) {
            auto sh = g::shape_solve(cc.start, cc.end, cc.T, th, opt);
            if (sh.ok && (!best.ok || sh.effort < best.effort)) best = std::move(sh);
        }
        if (!best.ok) return GOutcome {};
        auto o = g_outcome(shoot_seed(c, shape_primer_fit(cc, best, fit_samples)), t0);
        o.shape_J = best.effort;
        o.shape_theta = best.theta_end;
        return o;
    }});
    methods.push_back({"shape+ms", [&](const GCase& c) {
        const auto t0 = std::chrono::steady_clock::now();
        g::State end = c.target(c.T).state;
        g::ShapeOptions opt;
        opt.terms = 12;
        opt.free_end_velocity = c.end_condition == g::EndCondition::Intercept;
        g::ShapeResult best;
        for (const double th : g::arrival_angle_candidates(c.start, end, c.T, 3)) {
            auto sh = g::shape_solve(c.start, end, c.T, th, opt);
            if (sh.ok && (!best.ok || sh.effort < best.effort)) best = std::move(sh);
        }
        if (!best.ok) return GOutcome {};
        const std::size_t K = std::clamp<std::size_t>(static_cast<std::size_t>(std::ceil(std::abs(best.theta_end) / 1.5)), 2, 16);
        const std::size_t per = 25;
        std::vector<double> times;
        std::vector<g::Vec2> pos, thrust;
        std::vector<g::State> states;
        for (std::size_t i = 0; i <= K * per; ++i) {
            const double sv = static_cast<double>(i) / static_cast<double>(K * per);
            const auto pt = g::shape_eval(c.start, end, best, sv);
            times.push_back(sv * c.T);
            pos.push_back(pt.state.r);
            thrust.push_back(pt.thrust);
            states.push_back(pt.state);
        }
        std::vector<g::Primer> history;
        g::fit_primer(times, pos, thrust, &history);
        std::vector<std::array<double, 8>> nodes;
        for (std::size_t k = 0; k < K; ++k) {
            const auto& st = states[k * per];
            const auto& pr = history[k * per];
            nodes.push_back({st.r.x, st.r.y, st.v.x, st.v.y, pr.p.x, pr.p.y, pr.w.x, pr.w.y});
        }
        g::ShootingOptions mso;
        mso.stall_iterations = std::getenv("MS_STALL") ? std::atoi(std::getenv("MS_STALL")) : 8;
        mso.max_iterations = 40;
        const auto msol = g::multiple_shoot(propagator(), c.start, c.target, c.end_condition, c.T, nodes, mso);
        if (std::getenv("MSDEBUG") && !(msol.converged && msol.end.min_radius >= 0.1))
            std::printf("ms fail T=%.2f K=%zu conv=%d it=%zu res=%.2e minr=%.3f shapeJ=%.4g shape_minr=%.3f\n", c.T, K, msol.converged, msol.iterations,
                msol.residual, msol.end.min_radius, best.effort, best.min_radius);
        auto o = g_outcome(msol, t0);
        o.shape_J = best.effort;
        o.shape_theta = best.theta_end;
        return o;
    }});
    methods.push_back({"solve_transfer", [&](const GCase& c) {
        const auto t0 = std::chrono::steady_clock::now();
        g::TransferRequest req {c.start, c.target, c.end_condition, 0.1};
        g::SolveOptions o;
        if (const char* v = std::getenv("ZERO_PROPS")) o.zero_max_propagations = std::atoi(v);
        if (const char* v = std::getenv("ZERO_STALL")) o.zero_stall_iterations = std::atoi(v);
        return g_outcome(g::solve_transfer(propagator(), req, c.T, o), t0);
    }});
    methods.push_back({"solve_transfer-nofallback", [&](const GCase& c) {
        const auto t0 = std::chrono::steady_clock::now();
        g::TransferRequest req {c.start, c.target, c.end_condition, 0.1};
        g::SolveOptions o;
        o.fallbacks = false;
        return g_outcome(g::solve_transfer(propagator(), req, c.T, o), t0);
    }});
    methods.push_back({"tcont", [&](const GCase& c) {
        const auto t0 = std::chrono::steady_clock::now();
        return g_outcome(g::continue_in_time(propagator(), c.start, c.target, c.end_condition, c.T), t0);
    }});

    if (std::getenv("MINTIME")) {
        // Minimum time for the effort the fixed-T solve needs at the case's T.
        std::size_t n = 0, found = 0, earlier = 0;
        std::vector<double> times, ratios;
        for (const auto& c : cases) {
            g::TransferRequest req {c.start, c.target, c.end_condition, 0.1};
            const auto ref = g::solve_transfer(propagator(), req, c.T);
            if (!ref.converged) continue;
            ++n;
            const auto t0 = std::chrono::steady_clock::now();
            g::MinTimeOptions mto;
            mto.recheck_over_budget = std::getenv("RECHECK") != nullptr;
            const double budget_factor = std::getenv("BUDGETX") ? std::atof(std::getenv("BUDGETX")) : 1.0;
            const auto mt = g::solve_min_time(propagator(), req, ref.end.effort * budget_factor, c.T / 20.0, c.T * 2.0, mto);
            times.push_back(seconds_since(t0));
            if (!mt.converged) continue;
            ++found;
            if (mt.end.effort > ref.end.effort * budget_factor * (1 + 1e-6)) std::printf("  over budget %.4g > %.4g\n", mt.end.effort, ref.end.effort);
            ratios.push_back(mt.T / c.T);
            if (mt.T < c.T * 0.999) ++earlier;
        }
        std::sort(times.begin(), times.end());
        std::sort(ratios.begin(), ratios.end());
        const auto q = [](const std::vector<double>& v, double f) { return v.empty() ? 0.0 : v[std::min(v.size() - 1, static_cast<std::size_t>(f * v.size()))]; };
        std::printf("min-time: %zu/%zu found, %zu earlier than the reference T; T*/T p10=%.3f p50=%.3f p90=%.3f; time t50=%.2fms t90=%.2fms t99=%.2fms\n",
            found, n, earlier, q(ratios, 0.1), q(ratios, 0.5), q(ratios, 0.9), 1e3 * q(times, 0.5), 1e3 * q(times, 0.9), 1e3 * q(times, 0.99));
        return 0;
    }
    std::vector<std::vector<GOutcome>> results(methods.size(), std::vector<GOutcome>(cases.size()));
    for (std::size_t m = 0; m < methods.size(); ++m) {
        if (!only.empty() && methods[m].first.find(only) == std::string::npos) continue;
        for (std::size_t i = 0; i < cases.size(); ++i) results[m][i] = methods[m].second(cases[i]);
    }
    std::vector<double> best(cases.size(), std::numeric_limits<double>::infinity());
    for (std::size_t m = 0; m < methods.size(); ++m)
        for (std::size_t i = 0; i < cases.size(); ++i)
            if (results[m][i].ok) best[i] = std::min(best[i], results[m][i].J);
    if (std::getenv("GDEBUG")) {
        std::size_t shape_m = 0;
        for (std::size_t m = 0; m < methods.size(); ++m) if (methods[m].first == "shapefit") shape_m = m;
        for (std::size_t i = 0; i < cases.size(); ++i) {
            const auto& r = results[shape_m][i];
            if (!std::isfinite(best[i]) || (r.ok && r.J <= best[i] * (1 + 1e-3))) continue;
            double best_sweep = 0;
            for (std::size_t m = 0; m < methods.size(); ++m) if (results[m][i].ok && results[m][i].J == best[i]) best_sweep = results[m][i].sweep;
            std::printf("case %3zu T=%.2f ec=%d shape ok=%d J=%.4g (shape %.4g, th %.2f, sweep %.2f) best J=%.4g sweep %.2f\n", i, cases[i].T,
                static_cast<int>(cases[i].end_condition), r.ok, r.J, r.shape_J, r.shape_theta, r.sweep, best[i], best_sweep);
        }
    }
    std::size_t solvable = 0;
    for (const double b : best) solvable += std::isfinite(b);
    std::printf("set %s: %zu cases, %zu with a valid solution\n", set.c_str(), cases.size(), solvable);
    // Portfolio: methods in order until the first valid one (cost: sum of their times).
    const auto summarize = [&](const std::string& name, const std::vector<GOutcome>& r) {
        std::size_t ok = 0, good = 0, props = 0;
        std::vector<double> times;
        for (std::size_t i = 0; i < cases.size(); ++i) {
            props += r[i].props;
            times.push_back(r[i].seconds);
            if (!r[i].ok) continue;
            ++ok;
            if (r[i].J <= best[i] * (1.0 + 1e-3)) ++good;
        }
        std::sort(times.begin(), times.end());
        const auto q = [&](double f) { return times.empty() ? 0.0 : times[std::min(times.size() - 1, static_cast<std::size_t>(f * times.size()))]; };
        std::printf("%-22s valid=%5.1f%% best=%5.1f%% (of solvable %5.1f%%)  t50=%7.3fms t90=%7.3fms t99=%7.3fms props/case=%5.1f\n",
            name.c_str(), 100.0 * ok / cases.size(), 100.0 * good / cases.size(), 100.0 * good / std::max<std::size_t>(1, solvable),
            1e3 * q(0.5), 1e3 * q(0.9), 1e3 * q(0.99), static_cast<double>(props) / cases.size());
    };
    for (std::size_t m = 0; m < methods.size(); ++m) {
        if (!only.empty() && methods[m].first.find(only) == std::string::npos) continue;
        summarize(methods[m].first, results[m]);
    }
    if (only.empty()) {
        // Portfolio "first valid" and "min of all".
        std::vector<GOutcome> first(cases.size()), all(cases.size());
        for (std::size_t i = 0; i < cases.size(); ++i) {
            double t = 0.0;
            std::size_t props = 0;
            bool done = false;
            for (std::size_t m = 0; m < methods.size(); ++m) {
                t += results[m][i].seconds;
                props += results[m][i].props;
                if (!done && results[m][i].ok) {
                    first[i] = results[m][i];
                    first[i].seconds = t;
                    first[i].props = props;
                    done = true;
                }
                if (results[m][i].ok && results[m][i].J < all[i].J) all[i] = results[m][i];
            }
            if (!done) {
                first[i].seconds = t;
                first[i].props = props;
            }
            all[i].seconds = t;
            all[i].props = props;
        }
        summarize("portfolio-first", first);
        summarize("portfolio-all", all);
        // Per end condition (set C).
        for (const auto ec : {g::EndCondition::Rendezvous, g::EndCondition::Intercept}) {
            std::size_t n = 0;
            std::vector<std::size_t> good(methods.size(), 0);
            for (std::size_t i = 0; i < cases.size(); ++i) {
                if (cases[i].end_condition != ec || !std::isfinite(best[i])) continue;
                ++n;
                for (std::size_t m = 0; m < methods.size(); ++m)
                    if (results[m][i].ok && results[m][i].J <= best[i] * (1.0 + 1e-3)) ++good[m];
            }
            if (n == 0) continue;
            std::printf("%s (%zu solvable):", ec == g::EndCondition::Rendezvous ? "rendezvous" : "intercept", n);
            for (std::size_t m = 0; m < methods.size(); ++m) std::printf("  %s %.0f%%", methods[m].first.c_str(), 100.0 * good[m] / n);
            std::printf("\n");
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string set = argc > 1 ? argv[1] : "A";
    std::size_t stride = 37;
    std::size_t limit = 400;
    std::string only;
    int shape_terms = 6;
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
    if (set == "B" || set == "C" || set == "D" || set == "E" || set == "AG") {
        return run_general(set, cases, limit, only, shape_terms);
    }
    if (set == "F") {
        // Budget fit with a moving (circular-orbit) destination: is J(T) at fixed departure
        // reachable for lower budgets?
        const vi::VariableIspIntegrator integrator;
        std::size_t tries = 0, ok = 0;
        for (const auto& c : cases) {
            if (c.min_radius < 0.2) continue;
            const double rho_end = g::norm(c.end.r);
            const double w_dest = std::pow(rho_end, -1.5);
            const double T0d = c.seed.transfer_time_days;
            const auto target = [&](double Td) {
                return std::pair {rho_end * kAU, c.sweep + w_dest * (Td - T0d) * vi::VariableIspIntegrator::kDayS / kT0};
            };
            for (const double f : {0.95, 0.8, 0.5}) {
                const auto cfg = vi::VariableIspIntegrator::canonical_config(c.rho, c.kappa * 0 + c.J * f);
                vi::ShootingSettings st;
                st.fit_fuel_budget = true;
                st.budget_fraction = 1.0;
                const auto r = integrator.refine_seed_stm(c.seed, cfg, target, st);
                ++tries;
                if (r.converged) { ++ok; continue; }
                if (f != 0.95) continue;
                // Scan J(T) along the branch with fixed-T shooting.
                std::printf("fail f=%.2f rho=%.2f T0=%.1fd J=%.4g:", f, c.rho, T0d, c.J);
                g::Primer z = c.primer;
                for (const double mult : {1.0, 1.05, 1.1, 1.2, 1.4, 1.7, 2.0}) {
                    const double Td = T0d * mult;
                    const auto [rm, th] = target(Td);
                    const g::State goal = polar_state(rm / kAU, th, 0.0, std::sqrt(kAU / rm));
                    const auto sol = g::shoot(propagator(), c.start, g::fixed_target(goal), g::EndCondition::Rendezvous, z, Td * vi::VariableIspIntegrator::kDayS / kT0);
                    if (sol.converged) z = sol.primer;
                    std::printf(" %.2f:%s%.4g", mult, sol.converged ? "" : "x", sol.end.effort / c.J);
                }
                std::printf("\n");
            }
        }
        std::printf("fit ok %zu/%zu\n", ok, tries);
        return 0;
    }
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
    methods.push_back({"ms-truth-perturbed", [](const Case& c) {
        const auto t0 = std::chrono::steady_clock::now();
        const std::size_t K = 4;
        const auto smp = propagator().sample(c.start, c.primer, c.T, K + 1);
        if (smp.size() < K + 1) return Outcome {};
        std::vector<std::array<double, 8>> nodes;
        for (std::size_t k = 0; k < K; ++k) {
            const auto end = propagator().propagate(c.start, c.primer, smp[k].t, false);
            const auto& st = k == 0 ? c.start : end.state;
            const auto pr = k == 0 ? c.primer : end.primer;
            nodes.push_back({st.r.x * 1.001, st.r.y, st.v.x, st.v.y, pr.p.x * 1.01, pr.p.y, pr.w.x, pr.w.y * 0.99});
        }
        const auto sol = g::multiple_shoot(propagator(), c.start, g::fixed_target(c.end), g::EndCondition::Rendezvous, c.T, nodes);
        Outcome o = from_solution(sol, seconds_since(t0));
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
