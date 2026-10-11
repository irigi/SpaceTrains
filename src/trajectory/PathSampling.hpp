#pragma once

#include <cstddef>
#include <vector>

#include "math/Vec3d.hpp"

namespace spacetrains::trajectory {

// Thins a densely sampled path for rendering. Returns the indices of `dense` to keep
// (always the first and last) so that the polyline through them turns by at most about
// `max_turn_rad` at any kept point and no kept segment is longer than about
// `max_segment_m`. Flat stretches get few points and sharp bends (perihelion passes,
// the far end of eccentric ellipses) get many, which no fixed spacing in time, angle,
// or arc length achieves for every trajectory.
[[nodiscard]] std::vector<std::size_t> select_by_curvature(
    const std::vector<math::Vec3d>& dense,
    double max_turn_rad,
    double max_segment_m);

// Default rendering resolution: ≤ 6° per kept point, and at least ~40 segments.
[[nodiscard]] std::vector<std::size_t> select_for_rendering(const std::vector<math::Vec3d>& dense);

}  // namespace spacetrains::trajectory
