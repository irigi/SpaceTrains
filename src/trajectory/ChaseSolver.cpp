#include "trajectory/ChaseSolver.hpp"

#include <algorithm>
#include <cmath>

namespace spacetrains::trajectory {

namespace g = variable_isp::general;

double ChaseUnits::time_s() const { return std::sqrt(length_m * length_m * length_m / mu_m3_s2); }
double ChaseUnits::speed_mps() const { return length_m / time_s(); }
double ChaseUnits::accel_mps2() const { return mu_m3_s2 / (length_m * length_m); }

g::State ChaseUnits::to_normalised(const math::Vec3d& r_m, const math::Vec3d& v_mps) const {
    const double v = speed_mps();
    return {{r_m.x / length_m, r_m.z / length_m}, {v_mps.x / v, v_mps.z / v}};
}

math::Vec3d ChaseUnits::position_m(const g::Vec2& r) const { return {r.x * length_m, 0.0, r.y * length_m}; }

g::Target path_target(const domain::TrajectoryPlan& plan, double depart_time_s, const ChaseUnits& units) {
    // Samples in normalised units, with finite-difference velocities (non-uniform spacing).
    struct Node {
        double t;
        g::Vec2 r;
        g::Vec2 v;
    };
    std::vector<Node> nodes;
    const std::size_t n = std::min(plan.sampled_path.size(), plan.sampled_times_s.size());
    const double tu = units.time_s();
    for (std::size_t i = 0; i < n; ++i) {
        const auto& p = plan.sampled_path[i];
        nodes.push_back({(plan.sampled_times_s[i] - depart_time_s) / tu, {p.x / units.length_m, p.z / units.length_m}, {}});
    }
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const std::size_t a = i == 0 ? 0 : i - 1;
        const std::size_t b = std::min(nodes.size() - 1, i + 1);
        if (b > a && nodes[b].t > nodes[a].t) nodes[i].v = (1.0 / (nodes[b].t - nodes[a].t)) * (nodes[b].r - nodes[a].r);
    }
    return [nodes](double T) {
        g::TargetPoint out;
        if (nodes.empty()) return out;
        if (nodes.size() == 1 || T <= nodes.front().t || T >= nodes.back().t) {
            const Node& e = T <= nodes.front().t ? nodes.front() : nodes.back();
            out.state = {e.r + (T - e.t) * e.v, e.v};
            out.velocity = e.v;
            return out;
        }
        const auto it = std::upper_bound(nodes.begin(), nodes.end(), T, [](double t, const Node& node) { return t < node.t; });
        const Node& a = *(it - 1);
        const Node& b = *it;
        const double h = b.t - a.t;
        const double s = (T - a.t) / h;
        const double s2 = s * s, s3 = s2 * s;
        // Cubic Hermite and its first two time derivatives.
        const double h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s, h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
        const double d00 = 6 * s2 - 6 * s, d10 = 3 * s2 - 4 * s + 1, d01 = -6 * s2 + 6 * s, d11 = 3 * s2 - 2 * s;
        const double e00 = 12 * s - 6, e10 = 6 * s - 4, e01 = -12 * s + 6, e11 = 6 * s - 2;
        out.state.r = h00 * a.r + (h10 * h) * a.v + h01 * b.r + (h11 * h) * b.v;
        out.state.v = (1.0 / h) * (d00 * a.r + d01 * b.r) + d10 * a.v + d11 * b.v;
        out.velocity = out.state.v;
        out.acceleration = (1.0 / (h * h)) * (e00 * a.r + e01 * b.r) + (1.0 / h) * (e10 * a.v + e11 * b.v);
        return out;
    };
}

ChasePlan plan_chase(const ChaseRequest& request, const g::Target& target, const ChaseUnits& units, std::size_t path_samples) {
    ChasePlan plan;
    if (!(request.power_w > 0.0) || !(request.initial_mass_kg > request.dry_mass_kg) || !(request.dry_mass_kg > 0.0)) {
        return plan;
    }
    const double tu = units.time_s();
    const double au = units.accel_mps2();
    // Effort the propellant allows: ∫a²dt = 2P (1/m_dry − 1/m0), normalised by A² T.
    const double budget_si = 2.0 * request.power_w * (1.0 / request.dry_mass_kg - 1.0 / request.initial_mass_kg);
    const double budget = budget_si / (au * au * tu);
    const g::Propagator propagator;
    const g::TransferRequest transfer {units.to_normalised(request.position_m, request.velocity_mps), target, request.end_condition,
        request.min_perihelion_m / units.length_m};
    const auto solution = g::solve_min_time(propagator, transfer, budget, request.min_transfer_s / tu, request.max_transfer_s / tu);
    if (!solution.converged) return plan;
    plan.feasible = true;
    plan.transfer_time_s = solution.T * tu;
    const double effort_si = solution.end.effort * au * au * tu;
    const double final_mass = 1.0 / (1.0 / request.initial_mass_kg + effort_si / (2.0 * request.power_w));
    plan.propellant_kg = request.initial_mass_kg - final_mass;
    for (const auto& sample : propagator.sample(transfer.start, solution.primer, solution.T, std::max<std::size_t>(2, path_samples))) {
        plan.path_m.push_back(units.position_m(sample.state.r));
        plan.times_s.push_back(request.depart_time_s + sample.t * tu);
    }
    return plan;
}

}  // namespace spacetrains::trajectory
