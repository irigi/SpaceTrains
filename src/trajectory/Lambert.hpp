#pragma once
#include "math/Vec3d.hpp"

namespace spacetrains::trajectory {

struct LambertResult {
    math::Vec3d v1;  // departure velocity on transfer arc
    math::Vec3d v2;  // arrival velocity on transfer arc
    bool found {false};
};

// Universal-variable Lambert solver for coplanar (xz-plane) heliocentric orbits.
// Solves for the single-revolution prograde transfer arc from r1 to r2 in dt_s seconds.
// Returns found=false if no elliptic single-rev solution exists for the given time.
LambertResult solve_lambert(
    const math::Vec3d& r1,
    const math::Vec3d& r2,
    double dt_s,
    double mu);

}  // namespace spacetrains::trajectory
