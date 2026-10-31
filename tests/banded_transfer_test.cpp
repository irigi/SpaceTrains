// Checks of the banded-exhaust solver (variable_isp/BandedTransfer.hpp) against closed forms and
// against the ideal solver.
#include <cmath>
#include <cstdio>
#include <string>

#include "variable_isp/BandedTransfer.hpp"
#include "variable_isp/GeneralTransfer.hpp"

namespace b = spacetrains::variable_isp::banded;
namespace g = spacetrains::variable_isp::general;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what.c_str());
    if (!ok) ++failures;
}

// Earth-like circle to a Mars-like circle, 0.6 years: a typical heliocentric leg.
struct Leg {
    g::State start {{1.0, 0.0}, {0.0, 1.0}};
    g::Target target;
    double T {3.8};
    Leg() {
        const double r = 1.524, v = std::sqrt(1.0 / r), phase = 0.9;
        target = g::coasting_target({{r * std::cos(phase), r * std::sin(phase)}, {-v * std::sin(phase), v * std::cos(phase)}});
    }
};

// No band: the banded flow is the ideal one, so the ideal primer meets the target as is.
void band_off_matches_ideal() {
    const Leg leg;
    const g::Propagator ideal_prop;
    const auto ideal = g::solve_transfer(ideal_prop, {leg.start, leg.target, g::EndCondition::Rendezvous, 0.0}, leg.T);
    check(ideal.converged, "ideal leg solves");
    const b::Propagator prop;
    const double s0 = 2.0;
    const auto end = prop.propagate(leg.start, ideal.primer, s0, {}, leg.T, true);
    const auto tp = leg.target(leg.T);
    const double miss = std::hypot(end.state.r.x - tp.state.r.x, end.state.r.y - tp.state.r.y);
    check(end.ok && miss < 1e-8, "no band: ideal primer reaches the target (miss " + std::to_string(miss) + ")");
    check(std::abs((end.s - s0) - ideal.end.effort) < 1e-8 * ideal.end.effort, "no band: s(T) − s0 = ideal effort");
    // A band far wider than the path needs changes nothing either.
    const auto wide = prop.propagate(leg.start, ideal.primer, s0, {1e-6, 1e6}, leg.T, false);
    check(std::abs(wide.s - end.s) < 1e-10 && wide.switches.empty(), "a band the path never touches: same flight");
    // Dual-number Jacobian against forward differences.
    double worst = 0.0;
    const b::Band band {1.0, 5.0};
    const auto base = prop.propagate(leg.start, ideal.primer, s0, band, leg.T, true);
    for (int k = 0; k < 4; ++k) {
        auto p = ideal.primer;
        const double h = 1e-6;
        double* z[4] = {&p.p.x, &p.p.y, &p.w.x, &p.w.y};
        *z[k] += h;
        const auto e = prop.propagate(leg.start, p, s0, band, leg.T, false);
        const double fd[4] = {(e.state.r.x - base.state.r.x) / h, (e.state.r.y - base.state.r.y) / h,
            (e.state.v.x - base.state.v.x) / h, (e.state.v.y - base.state.v.y) / h};
        for (int row = 0; row < 4; ++row) {
            worst = std::max(worst, std::abs(fd[row] - base.jacobian[row][k]) / (1.0 + std::abs(fd[row])));
        }
    }
    check(!base.switches.empty() && worst < 1e-4,
        "dual Jacobian with switches matches differences (" + std::to_string(base.switches.size()) + " switches, rel " + std::to_string(worst) + ")");
}

// Free space, rest to rest over D in T with only a ceiling and a long T: the optimum is a
// full-power burn at v_max, a coast and a braking burn. Closed form: Δv = −v_max ln(1 − u),
// t1 = u/c, t2 = u(1 − u)/c with c = s0/v_max², s(T) = s0/(1 − u)².
void free_space_bang_coast_bang() {
    const double D = 0.05, T = 1.0, vmax = 1.0, s0 = 1.0;
    const double c = s0 / (vmax * vmax);
    const auto distance = [&](double u) {
        const double V = -vmax * std::log(1.0 - u);
        const double t1 = u / c, c2 = c / (1.0 - u), t2 = u / c2;
        const double x1 = vmax * ((1.0 - u) * std::log(1.0 - u) + u) / c;
        const double x3 = V * t2 - vmax * ((1.0 - u) * std::log(1.0 - u) + u) / c2;
        return x1 + V * (T - t1 - t2) + x3;
    };
    double lo = 0.0, hi = 0.5;
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        (distance(mid) < D ? lo : hi) = mid;
    }
    const double u = 0.5 * (lo + hi);
    const double t1 = u / c, t2 = u * (1.0 - u) / c, s_end = s0 / ((1.0 - u) * (1.0 - u));

    b::Settings settings;
    settings.gravity = false;
    const b::Propagator prop(settings);
    b::Request request;
    request.start = {{1.0, 0.0}, {0.0, 0.0}};
    request.target = g::fixed_target({{1.0 + D, 0.0}, {0.0, 0.0}});
    request.s0 = s0;
    request.band = {0.0, vmax};
    const g::Primer ideal {{6.0 * D / (T * T), 0.0}, {-12.0 * D / (T * T * T), 0.0}};
    const auto sol = b::solve_fixed_time(prop, request, T, ideal);
    check(sol.converged, "free space with a ceiling solves (" + std::to_string(sol.continuation_steps) + " steps)");
    if (!sol.converged) return;
    const auto& sw = sol.end.switches;
    check(sw.size() == 2 && sw[0].to == b::Arc::Coast && sw[1].to == b::Arc::Ceiling,
        "burn, coast, burn (" + std::to_string(sw.size()) + " switches)");
    if (sw.size() == 2) {
        check(std::abs(sw[0].t - t1) < 1e-7 && std::abs(sw[1].t - (T - t2)) < 1e-7,
            "switch times " + std::to_string(sw[0].t) + ", " + std::to_string(sw[1].t) + " vs " + std::to_string(t1) + ", "
                + std::to_string(T - t2));
    }
    check(std::abs(sol.end.s - s_end) < 1e-9 * s_end, "final mass: rocket equation at v_max");
    // Approach A on the same leg flies the ideal path throttled: never better than the optimum.
    const auto followed = prop.follow_ideal(request.start, ideal, s0, request.band, T);
    check(followed.s_end > sol.end.s, "throttled ideal path burns more than bang-coast-bang");
    // In closed form: the linear profile a = 6D/T²(1 − 2t/T) stays under a_min, so the whole path
    // runs at v_max, ṁ = −m a / v_max: s(T) = s0 · exp(∫|a| dt / v_max), ∫|a| dt = 3D/T.
    const double integral_abs_a = 3.0 * D / T;
    check(std::abs(followed.s_end - s0 * std::exp(integral_abs_a / vmax)) < 1e-7, "throttled ideal: rocket equation at v_max");
}

// The Hamiltonian is constant along an optimal flow with a fixed target, across switches.
void hamiltonian_constant() {
    const Leg leg;
    const g::Propagator ideal_prop;
    const auto ideal = g::solve_transfer(ideal_prop, {leg.start, leg.target, g::EndCondition::Rendezvous, 0.0}, leg.T);
    const b::Propagator prop;
    const b::Band band {1.0, 5.0};
    const auto h0 = prop.propagate(leg.start, ideal.primer, 2.0, band, 1e-9, false).hamiltonian;
    const auto end = prop.propagate(leg.start, ideal.primer, 2.0, band, leg.T, false);
    check(!end.switches.empty() && std::abs(end.hamiltonian - h0) < 1e-7 * (1.0 + std::abs(h0)),
        "Hamiltonian constant across " + std::to_string(end.switches.size()) + " switches (" + std::to_string(h0) + " vs "
            + std::to_string(end.hamiltonian) + ")");
}

// A banded optimum on a heliocentric leg: never more fuel than the throttled ideal path.
void heliocentric_leg() {
    const Leg leg;
    const g::Propagator ideal_prop;
    const auto ideal = g::solve_transfer(ideal_prop, {leg.start, leg.target, g::EndCondition::Rendezvous, 0.0}, leg.T);
    const b::Propagator prop;
    b::Request request {leg.start, leg.target, 2.0, {1.0, 5.0}, 0.0};
    const auto sol = b::solve_fixed_time(prop, request, leg.T, ideal.primer);
    check(sol.converged, "heliocentric leg with a band solves (" + std::to_string(sol.continuation_steps) + " steps, "
        + std::to_string(sol.propagations) + " propagations)");
    const auto followed = prop.follow_ideal(leg.start, ideal.primer, 2.0, request.band, leg.T);
    check(sol.converged && sol.end.s <= followed.s_end,
        "optimum ≤ throttled ideal (" + std::to_string(sol.end.s) + " vs " + std::to_string(followed.s_end) + ")");
    // Warm start from the solution itself converges at once.
    const auto again = b::solve_fixed_time(prop, request, leg.T, ideal.primer, {}, &sol.primer);
    check(again.converged && again.from_warm && again.iterations <= 1, "warm start from the answer");
}

}  // namespace

int main() {
    band_off_matches_ideal();
    free_space_bang_coast_bang();
    hamiltonian_constant();
    heliocentric_leg();
    std::printf("%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
