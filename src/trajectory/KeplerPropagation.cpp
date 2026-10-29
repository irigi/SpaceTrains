#include "trajectory/KeplerPropagation.hpp"

#include <algorithm>
#include <numbers>

namespace spacetrains::trajectory {

namespace {

double dot(const math::Vec3d& a, const math::Vec3d& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

}  // namespace

KeplerState propagate_kepler(const math::Vec3d& r_m, const math::Vec3d& v_mps, double dt_s, double mu) {
    const double r0 = r_m.length();
    if (r0 <= 0.0 || dt_s == 0.0) {
        return {r_m, v_mps};
    }
    const double v0 = v_mps.length();
    const double vr0 = dot(r_m, v_mps) / r0;
    const double alpha = 2.0 / r0 - v0 * v0 / mu;  // 1/a
    const double sqrt_mu = std::sqrt(mu);
    // Whole revolutions of an ellipse change nothing: keep the Newton iteration short.
    if (alpha > 1e-30) {
        const double period = 2.0 * std::numbers::pi / std::sqrt(mu * alpha * alpha * alpha);
        dt_s = std::fmod(dt_s, period);
    }
    // Universal anomaly by Newton's method (Curtis, algorithm 3.3).
    double chi = sqrt_mu * std::abs(alpha) * dt_s;
    if (alpha <= 1e-30) {
        chi = sqrt_mu * dt_s / r0;
    }
    for (int i = 0; i < 100; ++i) {
        const double z = alpha * chi * chi;
        const double c = stumpff_c(z);
        const double s = stumpff_s(z);
        const double f = r0 * vr0 / sqrt_mu * chi * chi * c + (1.0 - alpha * r0) * chi * chi * chi * s + r0 * chi
            - sqrt_mu * dt_s;
        const double df = r0 * vr0 / sqrt_mu * chi * (1.0 - alpha * chi * chi * s) + (1.0 - alpha * r0) * chi * chi * c + r0;
        const double step = f / df;
        chi -= step;
        if (std::abs(step) < 1e-9 * std::max(1.0, std::abs(chi))) {
            break;
        }
    }
    const double z = alpha * chi * chi;
    const double c = stumpff_c(z);
    const double s = stumpff_s(z);
    const double f = 1.0 - chi * chi / r0 * c;
    const double g = dt_s - chi * chi * chi * s / sqrt_mu;
    const math::Vec3d r = r_m * f + v_mps * g;
    const double r1 = r.length();
    const double fdot = sqrt_mu / (r1 * r0) * (alpha * chi * chi * chi * s - chi);
    const double gdot = 1.0 - chi * chi / r1 * c;
    return {r, r_m * fdot + v_mps * gdot};
}

void sample_kepler(const math::Vec3d& r_m, const math::Vec3d& v_mps, double t0_s, double t1_s, double step_s, double mu,
    std::vector<math::Vec3d>& path, std::vector<double>& times) {
    const int steps = std::max(1, static_cast<int>(std::ceil((t1_s - t0_s) / std::max(1.0, step_s))));
    for (int i = 0; i <= steps; ++i) {
        const double t = t0_s + (t1_s - t0_s) * static_cast<double>(i) / steps;
        path.push_back(propagate_kepler(r_m, v_mps, t - t0_s, mu).position_m);
        times.push_back(t);
    }
}

}  // namespace spacetrains::trajectory
