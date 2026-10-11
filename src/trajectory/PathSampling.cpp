#include "trajectory/PathSampling.hpp"

#include <algorithm>
#include <cmath>

namespace spacetrains::trajectory {

namespace {

double turn_rad(const math::Vec3d& u, const math::Vec3d& v) {
    const double lu = u.length();
    const double lv = v.length();
    if (lu <= 0.0 || lv <= 0.0) {
        return 0.0;
    }
    return std::acos(std::clamp((u.x * v.x + u.y * v.y + u.z * v.z) / (lu * lv), -1.0, 1.0));
}

}  // namespace

std::vector<std::size_t> select_by_curvature(
    const std::vector<math::Vec3d>& dense,
    double max_turn_rad,
    double max_segment_m) {
    std::vector<std::size_t> keep;
    if (dense.empty()) {
        return keep;
    }
    keep.push_back(0);
    double turn_since_kept = 0.0;
    double length_since_kept = 0.0;
    for (std::size_t i = 1; i + 1 < dense.size(); ++i) {
        const auto incoming = dense[i] - dense[i - 1];
        const auto outgoing = dense[i + 1] - dense[i];
        length_since_kept += incoming.length();
        const double turn = turn_rad(incoming, outgoing);
        // Keep i if skipping it would let the next kept segment exceed either budget.
        if (turn_since_kept + turn > max_turn_rad || length_since_kept + outgoing.length() > max_segment_m) {
            keep.push_back(i);
            turn_since_kept = 0.0;
            length_since_kept = 0.0;
        } else {
            turn_since_kept += turn;
        }
    }
    if (dense.size() > 1) {
        // A kept point just before the end leaves a tiny final segment, on which the
        // planners' endpoint snap (sub-0.001 AU) shows up as a sharp turn. Merge it.
        if (keep.size() > 1 && (dense.back() - dense[keep.back()]).length() < 0.25 * max_segment_m) {
            keep.pop_back();
        }
        keep.push_back(dense.size() - 1);
    }
    return keep;
}

std::vector<std::size_t> select_for_rendering(const std::vector<math::Vec3d>& dense) {
    constexpr double kMaxTurnRad = 6.0 * 3.14159265358979323846 / 180.0;
    constexpr double kMinSegments = 40.0;
    double total = 0.0;
    for (std::size_t i = 1; i < dense.size(); ++i) {
        total += (dense[i] - dense[i - 1]).length();
    }
    return select_by_curvature(dense, kMaxTurnRad, std::max(total / kMinSegments, 1.0));
}

}  // namespace spacetrains::trajectory
