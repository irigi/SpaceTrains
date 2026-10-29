#pragma once
#include "math/Vec3d.hpp"

#include <vector>

namespace spacetrains::trajectory {

struct LambertResult {
    math::Vec3d v1;  // departure velocity on transfer arc
    math::Vec3d v2;  // arrival velocity on transfer arc
    bool found {false};
    int revolutions {0};  // whole revolutions before arriving
};

// Universal-variable Lambert solver for coplanar (xz-plane) heliocentric orbits.
// Solves for the prograde transfer arc from r1 to r2 in dt_s seconds under one revolution (the
// short way when r2 is less than half a turn ahead, the long way otherwise; elliptic or
// hyperbolic). Returns found=false if there is none.
LambertResult solve_lambert(
    const math::Vec3d& r1,
    const math::Vec3d& r2,
    double dt_s,
    double mu);

// All prograde arcs from r1 to r2 in dt_s: the one under a revolution (if any), then up to two for
// each 1..max_revolutions whole revolutions more (ellipses whose periods fit the time).
std::vector<LambertResult> solve_lambert_revolutions(
    const math::Vec3d& r1,
    const math::Vec3d& r2,
    double dt_s,
    double mu,
    int max_revolutions);

// Closest approach to the central body along the coplanar conic arc from r1 (with
// velocity v1) to r2, travelled in the direction of motion: the periapsis radius if
// the arc passes periapsis (always, with whole revolutions), otherwise the closer endpoint.
double conic_arc_min_radius(
    const math::Vec3d& r1,
    const math::Vec3d& v1,
    const math::Vec3d& r2,
    double mu,
    int revolutions = 0);

}  // namespace spacetrains::trajectory
