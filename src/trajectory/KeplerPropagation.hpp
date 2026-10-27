#pragma once

// Analytic two-body propagation (universal variables) of a heliocentric state in the game's x-z
// plane: derelicts coasting on the orbit they had when they failed, and the coasting arcs of
// salvage runs. Also the Stumpff functions the Lambert solver shares.

#include "math/Vec3d.hpp"

#include <cmath>
#include <vector>

namespace spacetrains::trajectory {

// Stumpff functions with a series near z = 0.
inline double stumpff_c(double z) {
    if (z > 1e-4) return (1.0 - std::cos(std::sqrt(z))) / z;
    if (z < -1e-4) return (std::cosh(std::sqrt(-z)) - 1.0) / (-z);
    return 0.5 - z / 24.0 + z * z / 720.0;
}

inline double stumpff_s(double z) {
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

struct KeplerState {
    math::Vec3d position_m;
    math::Vec3d velocity_mps;
};

// The state `dt_s` after (r, v) (dt may be negative) on the conic through it.
KeplerState propagate_kepler(const math::Vec3d& r_m, const math::Vec3d& v_mps, double dt_s, double mu);

// The coast sampled every `step_s` from t0 to t1 (both ends included).
void sample_kepler(const math::Vec3d& r_m, const math::Vec3d& v_mps, double t0_s, double t1_s, double step_s, double mu,
    std::vector<math::Vec3d>& path, std::vector<double>& times);

}  // namespace spacetrains::trajectory
