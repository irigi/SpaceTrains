#include "trajectory/PathMotion.hpp"
#include "trajectory/Lambert.hpp"

#include <algorithm>

namespace spacetrains::trajectory {

PathMotion::PathMotion(const std::vector<math::Vec3d>& path, const std::vector<double>& times_s, double mu) : mu_(mu) {
    const std::size_t n = std::min(path.size(), times_s.size());
    for (std::size_t i = 0; i < n; ++i) {
        // Repeated times (snapped ends) would make empty segments.
        if (!t_.empty() && times_s[i] <= t_.back()) continue;
        r_.push_back(path[i]);
        t_.push_back(times_s[i]);
    }
    for (std::size_t i = 0; i + 1 < r_.size(); ++i) {
        const double dt = t_[i + 1] - t_[i];
        const auto arc = solve_lambert(r_[i], r_[i + 1], dt, mu_);
        // A conic must also be near the chord's speed: a far-off branch is a solver artefact.
        const math::Vec3d chord = (r_[i + 1] - r_[i]) / dt;
        const bool plausible = arc.found && (arc.v1.length() < 3.0 * chord.length() + 1.0);
        conic_.push_back(plausible);
        v_start_.push_back(plausible ? arc.v1 : chord);
        v_end_.push_back(plausible ? arc.v2 : chord);
    }
    // Conics through the ends of a segment with thrust a: v_start = v + a·dt/2 at its first
    // sample and v_end = v − a·dt/2 at its second, so a sample's velocity is the time-weighted
    // mean of its two conics, and the ends are extrapolated with the neighbouring jump.
    const std::size_t segments = v_start_.size();
    v_node_.resize(r_.size());
    if (segments == 0) return;
    for (std::size_t k = 1; k < segments; ++k) {
        const double before = t_[k] - t_[k - 1];
        const double after = t_[k + 1] - t_[k];
        v_node_[k] = (v_end_[k - 1] * after + v_start_[k] * before) / (before + after);
    }
    if (segments == 1) {
        v_node_[0] = v_start_[0];
        v_node_[1] = v_end_[0];
        return;
    }
    const auto jump = [&](std::size_t k) { return v_start_[k] - v_end_[k - 1]; };
    const double dt0 = t_[1] - t_[0], dt1 = t_[2] - t_[1];
    v_node_[0] = v_start_[0] - jump(1) * (dt0 / (dt0 + dt1));
    const std::size_t last = segments;  // the last sample
    const double dtn = t_[last] - t_[last - 1], dtm = t_[last - 1] - t_[last - 2];
    v_node_[last] = v_end_[last - 1] + jump(last - 1) * (dtn / (dtm + dtn));
}

KeplerState PathMotion::at(double t_s) const {
    if (r_.empty()) return {};
    if (r_.size() == 1) return {r_.front(), {}};
    std::size_t i = 0;
    if (t_s >= t_.back()) {
        i = r_.size() - 2;
    } else if (t_s > t_.front()) {
        i = static_cast<std::size_t>(std::upper_bound(t_.begin(), t_.end(), t_s) - t_.begin()) - 1;
    }
    if (t_s >= t_.back()) {
        // Past the end: on from the last sample with its velocity.
        if (!conic_[i]) return {r_.back() + v_node_.back() * (t_s - t_.back()), v_node_.back()};
        return propagate_kepler(r_.back(), v_node_.back(), t_s - t_.back(), mu_);
    }
    if (t_s <= t_.front()) {
        if (!conic_[0]) return {r_.front() + v_node_.front() * (t_s - t_.front()), v_node_.front()};
        return propagate_kepler(r_.front(), v_node_.front(), t_s - t_.front(), mu_);
    }
    if (!conic_[i]) return {r_[i] + v_start_[i] * (t_s - t_[i]), v_start_[i]};
    // The conic, plus the thrust's displacement: zero at both samples, with the velocity
    // corrections there as its slopes (cubic Hermite).
    const double h = t_[i + 1] - t_[i];
    const double s = (t_s - t_[i]) / h;
    const math::Vec3d dv0 = v_node_[i] - v_start_[i];
    const math::Vec3d dv1 = v_node_[i + 1] - v_end_[i];
    const double h10 = s * s * s - 2.0 * s * s + s, h11 = s * s * s - s * s;
    const double d10 = 3.0 * s * s - 4.0 * s + 1.0, d11 = 3.0 * s * s - 2.0 * s;
    auto out = propagate_kepler(r_[i], v_start_[i], t_s - t_[i], mu_);
    out.position_m = out.position_m + (dv0 * h10 + dv1 * h11) * h;
    out.velocity_mps = out.velocity_mps + dv0 * d10 + dv1 * d11;
    return out;
}

}  // namespace spacetrains::trajectory
