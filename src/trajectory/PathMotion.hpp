#pragma once

// The motion of a ship along its planned path (timed samples kept for rendering): one conic per
// segment, through its two samples in the segment's time (Lambert), propagated analytically, plus
// the thrust's share on powered arcs (plasma): where neighbouring conics meet, their velocities
// differ by the thrust gained, which gives the true velocity there (a time-weighted mean) and a
// cubic correction inside each segment (exact for constant thrust). Coasts (nuclear-thermal
// arcs) come out exact.

#include "trajectory/KeplerPropagation.hpp"

#include <vector>

namespace spacetrains::trajectory {

class PathMotion {
public:
    PathMotion(const std::vector<math::Vec3d>& path, const std::vector<double>& times_s, double mu);

    // The state at time `t_s`. Before the first sample and after the last it coasts on the end
    // segments' conics. Segments without a conic (no motion, retrograde) move in a straight line.
    [[nodiscard]] KeplerState at(double t_s) const;
    [[nodiscard]] bool empty() const { return r_.empty(); }

private:
    std::vector<math::Vec3d> r_;
    std::vector<double> t_;
    std::vector<math::Vec3d> v_start_;  // each segment's velocity at its first sample
    std::vector<math::Vec3d> v_end_;    // ... and at its second
    std::vector<bool> conic_;
    std::vector<math::Vec3d> v_node_;   // the velocity at each sample, thrust included
    double mu_ {0.0};
};

}  // namespace spacetrains::trajectory
