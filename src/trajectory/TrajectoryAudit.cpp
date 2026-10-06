#include "trajectory/TrajectoryAudit.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace spacetrains::trajectory {

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double RAD_TO_DEG = 180.0 / PI;

double wrap_pi(double angle_rad) {
    angle_rad = std::fmod(angle_rad + PI, 2.0 * PI);
    if (angle_rad < 0.0) {
        angle_rad += 2.0 * PI;
    }
    return angle_rad - PI;
}

// Angle between consecutive segments a->b and b->c, in degrees.
double turn_deg(const math::Vec3d& a, const math::Vec3d& b, const math::Vec3d& c) {
    const auto u = b - a;
    const auto v = c - b;
    const double lu = u.length();
    const double lv = v.length();
    if (lu <= 0.0 || lv <= 0.0) {
        return 0.0;
    }
    const double cos_angle = std::clamp((u.x * v.x + u.y * v.y + u.z * v.z) / (lu * lv), -1.0, 1.0);
    return std::acos(cos_angle) * RAD_TO_DEG;
}

}  // namespace

TrajectoryAuditMetrics audit_trajectory(
    const domain::TrajectoryPlan& plan,
    double r_origin_m,
    double r_dest_m,
    const TrajectoryAuditThresholds& thresholds) {

    TrajectoryAuditMetrics m;
    const auto& path = plan.sampled_path;
    const auto& times = plan.sampled_times_s;
    m.sample_count = path.size();

    // Skip the wait-period prefix (Kepler plans track the origin station before departure).
    std::size_t first = 0;
    if (times.size() == path.size()) {
        while (first + 1 < times.size() && times[first] < plan.departure_time_s - 1.0) {
            ++first;
        }
        for (std::size_t i = 1; i < times.size(); ++i) {
            if (!(times[i] >= times[i - 1])) {
                m.times_monotonic = false;
            }
        }
    }
    {
        double wait_swept = 0.0;
        for (std::size_t i = 1; i <= first; ++i) {
            const double step = wrap_pi(std::atan2(path[i].z, path[i].x) - std::atan2(path[i - 1].z, path[i - 1].x));
            wait_swept += step;
            m.wait_max_step_deg = std::max(m.wait_max_step_deg, std::abs(step) * RAD_TO_DEG);
        }
        m.wait_revolutions = std::abs(wait_swept) / (2.0 * PI);
    }
    const std::size_t n = path.size() - first;
    m.transfer_sample_count = n;

    for (const auto& p : path) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            m.finite = false;
        }
    }

    if (n >= 2 && m.finite) {
        double swept = 0.0;
        m.min_radius_m = std::numeric_limits<double>::max();
        std::vector<double> segment_lengths;
        segment_lengths.reserve(n - 1);
        for (std::size_t i = first; i < path.size(); ++i) {
            const double r = std::hypot(path[i].x, path[i].z);
            m.min_radius_m = std::min(m.min_radius_m, r);
            m.max_radius_m = std::max(m.max_radius_m, r);
            if (i == first) {
                continue;
            }
            const double step = wrap_pi(std::atan2(path[i].z, path[i].x) - std::atan2(path[i - 1].z, path[i - 1].x));
            swept += step;
            m.max_step_deg = std::max(m.max_step_deg, std::abs(step) * RAD_TO_DEG);
            segment_lengths.push_back((path[i] - path[i - 1]).length());
        }
        m.revolutions = std::abs(swept) / (2.0 * PI);

        auto sorted = segment_lengths;
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<long>(sorted.size() / 2), sorted.end());
        const double median = sorted[sorted.size() / 2];
        m.last_segment_ratio = median > 0.0 ? segment_lengths.back() / median : 0.0;

        if (n >= 3) {
            const std::size_t last = path.size() - 1;
            m.end_turn_deg = turn_deg(path[last - 2], path[last - 1], path[last]);
            for (std::size_t i = first + 1; i + 1 < last; ++i) {
                m.max_interior_turn_deg = std::max(m.max_interior_turn_deg, turn_deg(path[i - 1], path[i], path[i + 1]));
            }
        }
    }

    if (!m.finite) m.flags.emplace_back("non_finite");
    if (!m.times_monotonic) m.flags.emplace_back("non_monotonic_time");
    if (plan.diagnostics.endpoint_miss_m > thresholds.endpoint_miss_m) m.flags.emplace_back("endpoint_miss");
    if (plan.diagnostics.start_miss_m > thresholds.endpoint_miss_m) m.flags.emplace_back("start_miss");
    if (m.revolutions > thresholds.max_revolutions) m.flags.emplace_back("many_revolutions");
    if (m.max_step_deg > thresholds.max_step_deg) m.flags.emplace_back("coarse_sampling");
    if (m.wait_max_step_deg > thresholds.max_step_deg) m.flags.emplace_back("coarse_wait");
    if (m.end_turn_deg > thresholds.end_turn_deg && m.last_segment_ratio > thresholds.last_segment_ratio) {
        m.flags.emplace_back("end_dent");
    }
    if (m.min_radius_m > 0.0 && m.min_radius_m < thresholds.sun_dive_fraction * std::min(r_origin_m, r_dest_m)) {
        m.flags.emplace_back("sun_dive");
    }
    return m;
}

}  // namespace spacetrains::trajectory
