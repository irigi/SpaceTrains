#include "trajectory/Lambert.hpp"
#include "trajectory/KeplerPropagation.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace spacetrains::trajectory {


namespace {

constexpr double kPi = 3.14159265358979323846;

// Universal-variable Lambert problem (Curtis, Orbital Mechanics for Engineering Students §5.3) in
// the game's x-z plane. Orbits run counter-clockwise in atan2(z, x) (angular momentum along −y).
struct LambertProblem {
    math::Vec3d r1, r2;
    double r1_m {0.0}, r2_m {0.0}, A {0.0}, mu {0.0};
    bool ok {false};

    LambertProblem(const math::Vec3d& a, const math::Vec3d& b, double mu_in) : r1(a), r2(b), mu(mu_in) {
        r1_m = r1.length();
        r2_m = r2.length();
        if (r1_m < 1.0 || r2_m < 1.0 || mu <= 0.0) return;
        const double cos_dth = std::clamp((r1.x * r2.x + r1.z * r2.z) / (r1_m * r2_m), -1.0, 1.0);
        const double one_minus_cos = 1.0 - cos_dth;
        if (one_minus_cos < 1e-12) return;  // coincident directions
        // r2 ahead of r1 by under half a turn (counter-clockwise): the short way, sin Δθ > 0.
        const double ahead = r1.x * r2.z - r1.z * r2.x;
        const double sin_dth = (ahead >= 0.0 ? 1.0 : -1.0) * std::sqrt(std::max(0.0, 1.0 - cos_dth * cos_dth));
        A = sin_dth * std::sqrt(r1_m * r2_m / one_minus_cos);
        ok = std::abs(A) > 0.0;
    }

    [[nodiscard]] double y(double z) const {
        return r1_m + r2_m + A * (z * stumpff_s(z) - 1.0) / std::sqrt(stumpff_c(z));
    }
    // Time of flight at z, or −1 where z gives no arc (y ≤ 0, or C = 0 between revolutions).
    [[nodiscard]] double tof(double z) const {
        const double C = stumpff_c(z);
        if (C < 1e-12) return -1.0;
        const double yz = y(z);
        if (yz <= 0.0) return -1.0;
        return (std::pow(yz / C, 1.5) * stumpff_s(z) + A * std::sqrt(yz)) / std::sqrt(mu);
    }
    [[nodiscard]] LambertResult velocities(double z) const {
        const double yz = y(z);
        if (stumpff_c(z) < 1e-12 || yz <= 0.0) return {};
        const double f = 1.0 - yz / r1_m;
        const double g = A * std::sqrt(yz / mu);
        const double g_dot = 1.0 - yz / r2_m;
        if (std::abs(g) < 1e-10) return {};
        return {(r2 - r1 * f) * (1.0 / g), (r2 * g_dot - r1) * (1.0 / g), true};
    }

    // Every root of tof(z) = dt on [z_lo, z_hi]: a scan for sign changes between valid points,
    // each refined by bisection to a second of flight time.
    [[nodiscard]] std::vector<LambertResult> roots(double dt, double z_lo, double z_hi, int scan) const {
        std::vector<LambertResult> out;
        double prev_z = z_lo;
        double prev_f = tof(z_lo);
        for (int i = 1; i <= scan; ++i) {
            const double z = z_lo + (z_hi - z_lo) * static_cast<double>(i) / scan;
            const double t = tof(z);
            if (t >= 0.0 && prev_f < 0.0) {
                // Entering the valid range (y > 0), where the flight time starts from zero: for
                // short arcs the root lies just inside its edge, so the edge is found first.
                double invalid = prev_z, valid = z;
                for (int iter = 0; iter < 60; ++iter) {
                    const double mid = 0.5 * (invalid + valid);
                    (tof(mid) >= 0.0 ? valid : invalid) = mid;
                }
                prev_z = valid;
                prev_f = tof(valid);
            }
            if (t >= 0.0 && prev_f >= 0.0 && (prev_f - dt) * (t - dt) <= 0.0) {
                double lo = prev_z, hi = z;
                const bool rising = t > prev_f;
                for (int iter = 0; iter < 80; ++iter) {
                    const double mid = 0.5 * (lo + hi);
                    const double tm = tof(mid);
                    if (tm < 0.0) break;
                    if (std::abs(tm - dt) < 1e-3) { lo = hi = mid; break; }
                    if ((tm < dt) == rising) lo = mid; else hi = mid;
                }
                if (auto r = velocities(0.5 * (lo + hi)); r.found) out.push_back(r);
            }
            prev_z = z;
            prev_f = t;
        }
        return out;
    }
};

}  // namespace

LambertResult solve_lambert(const math::Vec3d& r1, const math::Vec3d& r2, double dt_s, double mu) {
    const LambertProblem problem(r1, r2, mu);
    if (!problem.ok || dt_s <= 0.0) return {};
    // Under one revolution: z from hyperbolic to just short of 4π², where the flight time rises
    // monotonically, so there is one root at most.
    const auto roots = problem.roots(dt_s, -50.0, 4.0 * kPi * kPi * 0.9999, 240);
    return roots.empty() ? LambertResult {} : roots.front();
}

std::vector<LambertResult> solve_lambert_revolutions(const math::Vec3d& r1, const math::Vec3d& r2, double dt_s, double mu,
    int max_revolutions) {
    std::vector<LambertResult> out;
    if (auto direct = solve_lambert(r1, r2, dt_s, mu); direct.found) out.push_back(direct);
    const LambertProblem problem(r1, r2, mu);
    if (!problem.ok || dt_s <= 0.0) return out;
    // An ellipse through both points has a semi-major axis of at least half the farther radius,
    // so N revolutions need at least N such periods.
    const double a_min = 0.5 * std::max(problem.r1_m, problem.r2_m);
    const double period_min = 2.0 * kPi * std::sqrt(a_min * a_min * a_min / mu);
    for (int n = 1; n <= max_revolutions && dt_s > n * period_min; ++n) {
        // N whole revolutions more: √z in (2πN, 2π(N+1)); the flight time falls to a minimum and
        // rises again, so up to two roots (the shorter- and longer-period ellipses).
        const double lo = std::pow(2.0 * kPi * n, 2.0) * 1.0001;
        const double hi = std::pow(2.0 * kPi * (n + 1), 2.0) * 0.9999;
        for (auto r : problem.roots(dt_s, lo, hi, 200)) {
            r.revolutions = n;
            out.push_back(r);
        }
    }
    return out;
}

double conic_arc_min_radius(
    const math::Vec3d& r1,
    const math::Vec3d& v1,
    const math::Vec3d& r2,
    double mu,
    int revolutions) {
    constexpr double tau = 6.28318530717958647692;
    const double r1m = std::hypot(r1.x, r1.z);
    const double r2m = std::hypot(r2.x, r2.z);
    const double endpoints = std::min(r1m, r2m);
    // Same XZ-plane convention as the path sampler: h = (r × v).y, e from (v × L)/mu − r̂.
    const double h = r1.x * v1.z - r1.z * v1.x;
    const double ex = v1.z * h / mu - r1.x / r1m;
    const double ez = -v1.x * h / mu - r1.z / r1m;
    const double ecc = std::hypot(ex, ez);
    if (ecc < 1e-9 || r1m <= 0.0) {
        return endpoints;  // circular: constant radius
    }
    const double omega = std::atan2(ez, ex);
    const auto wrap = [&](double a) { a = std::fmod(a, tau); return a < 0.0 ? a + tau : a; };
    // True anomalies in [0, 2π); periapsis sits at 0 ≡ 2π.
    const double theta1 = wrap(std::atan2(r1.z, r1.x) - omega);
    const double theta2 = wrap(std::atan2(r2.z, r2.x) - omega);
    // Prograde (h > 0) motion increases theta and passes periapsis when it wraps past 2π;
    // retrograde motion decreases theta and passes it when it wraps below 0.
    const bool passes_periapsis = revolutions > 0 || (h >= 0.0 ? theta2 < theta1 : theta2 > theta1);
    const double periapsis = (h * h / mu) / (1.0 + ecc);
    return passes_periapsis ? std::min(endpoints, periapsis) : endpoints;
}

}  // namespace spacetrains::trajectory
