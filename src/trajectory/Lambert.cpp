#include "trajectory/Lambert.hpp"

#include <algorithm>
#include <cmath>

namespace spacetrains::trajectory {

namespace {

// Stumpff functions with series fallback near z=0
double stumpff_c(double z) {
    if (z > 1e-4) return (1.0 - std::cos(std::sqrt(z))) / z;
    if (z < -1e-4) return (std::cosh(std::sqrt(-z)) - 1.0) / (-z);
    return 0.5 - z / 24.0 + z * z / 720.0;
}

double stumpff_s(double z) {
    if (z > 1e-4) {
        const double sq = std::sqrt(z);
        return (sq - std::sin(sq)) / (z * sq);
    }
    if (z < -1e-4) {
        const double sq = std::sqrt(-z);
        return (std::sinh(sq) - sq) / ((-z) * sq);
    }
    return 1.0 / 6.0 - z / 120.0 + z * z / 5040.0;
}

}  // namespace

LambertResult solve_lambert(
    const math::Vec3d& r1,
    const math::Vec3d& r2,
    double dt_s,
    double mu) {

    constexpr double PI = 3.14159265358979323846;

    const double r1_m = r1.length();
    const double r2_m = r2.length();
    if (r1_m < 1.0 || r2_m < 1.0 || dt_s <= 0.0 || mu <= 0.0) return {};

    // Transfer angle in the xz-plane.
    // Orbits are in XZ with CCW motion (angular momentum in -Y).
    // cross_y = (r1 × r2).y = r1.z*r2.x - r1.x*r2.z < 0 for the short-arc prograde (CCW) case.
    // In the Lagrange formulation A > 0 selects the prograde arc, so we negate cross_y.
    const double cos_dth = std::clamp((r1.x * r2.x + r1.z * r2.z) / (r1_m * r2_m), -1.0, 1.0);
    const double one_minus_cos = 1.0 - cos_dth;
    if (one_minus_cos < 1e-12) return {};  // degenerate: coincident positions

    const double cross_y = r1.z * r2.x - r1.x * r2.z;
    const double sin_dth_abs = std::sqrt(std::max(0.0, 1.0 - cos_dth * cos_dth));
    const double sin_dth = (cross_y <= 0.0 ? 1.0 : -1.0) * sin_dth_abs;

    // A is the signed "area" factor; positive for short-arc prograde (CCW), negative for long-arc.
    const double A = sin_dth * std::sqrt(r1_m * r2_m / one_minus_cos);

    // Stumpff-based TOF evaluation at a given universal variable z.
    // Returns -1 if y ≤ 0 (unphysical).
    const auto tof_of = [&](double z) -> double {
        const double C = stumpff_c(z);
        if (C < 1e-12) return -1.0;
        const double y = r1_m + r2_m + A * (z * stumpff_s(z) - 1.0) / std::sqrt(C);
        if (y <= 0.0) return -1.0;
        const double S = stumpff_s(z);
        return (std::pow(y / C, 1.5) * S + A * std::sqrt(y)) / std::sqrt(mu);
    };

    // Search for a bracket [z_lo, z_hi] where tof_of crosses dt_s.
    // A > 0 (short-arc prograde, CCW planets): TOF decreases with z.
    // A < 0 (long-arc):                        TOF increases with z.
    const double z_min = -50.0;
    const double z_max = 4.0 * PI * PI * 0.9999;
    constexpr int kScan = 120;
    const bool tof_decreases = (A >= 0.0);

    double z_lo = 0.0, z_hi = 0.0;
    bool found_bracket = false;
    double prev_z = z_min;
    double prev_tof = tof_of(z_min);

    for (int i = 1; i <= kScan; ++i) {
        const double z = z_min + (z_max - z_min) * static_cast<double>(i) / kScan;
        const double tof = tof_of(z);
        if (tof_decreases) {
            if (prev_tof > dt_s && tof > 0.0 && tof <= dt_s) {
                z_lo = prev_z; z_hi = z; found_bracket = true; break;
            }
        } else {
            if (prev_tof > 0.0 && prev_tof <= dt_s && tof > dt_s) {
                z_lo = prev_z; z_hi = z; found_bracket = true; break;
            }
        }
        if (tof > 0.0) {
            prev_z = z;
            prev_tof = tof;
        }
    }

    if (!found_bracket) return {};

    // Bisect the bracket to 1-second accuracy
    for (int iter = 0; iter < 64; ++iter) {
        const double z_mid = (z_lo + z_hi) * 0.5;
        const double tof_mid = tof_of(z_mid);
        if (tof_mid < 0.0) {
            if (tof_decreases) z_hi = z_mid; else z_lo = z_mid;
            continue;
        }
        if (std::abs(tof_mid - dt_s) < 1.0) break;
        if (tof_decreases) {
            if (tof_mid > dt_s) z_lo = z_mid; else z_hi = z_mid;
        } else {
            if (tof_mid > dt_s) z_hi = z_mid; else z_lo = z_mid;
        }
    }

    const double z_sol = (z_lo + z_hi) * 0.5;
    const double C_sol = stumpff_c(z_sol);
    if (C_sol < 1e-12) return {};
    const double y_sol = r1_m + r2_m + A * (z_sol * stumpff_s(z_sol) - 1.0) / std::sqrt(C_sol);
    if (y_sol <= 0.0) return {};

    // Lagrange f, g coefficients → velocities
    const double f = 1.0 - y_sol / r1_m;
    const double g = A * std::sqrt(y_sol / mu);
    const double g_dot = 1.0 - y_sol / r2_m;
    if (std::abs(g) < 1e-10) return {};

    LambertResult res;
    res.v1 = (r2 - r1 * f) * (1.0 / g);
    res.v2 = (r2 * g_dot - r1) * (1.0 / g);
    res.found = true;
    return res;
}

double conic_arc_min_radius(
    const math::Vec3d& r1,
    const math::Vec3d& v1,
    const math::Vec3d& r2,
    double mu) {
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
    const bool passes_periapsis = h >= 0.0 ? theta2 < theta1 : theta2 > theta1;
    const double periapsis = (h * h / mu) / (1.0 + ecc);
    return passes_periapsis ? std::min(endpoints, periapsis) : endpoints;
}

}  // namespace spacetrains::trajectory
