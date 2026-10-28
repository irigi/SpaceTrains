#include "trajectory/ChaseSolver.hpp"
#include "trajectory/KeplerPropagation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

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

g::Target kepler_target(const math::Vec3d& r_m, const math::Vec3d& v_mps, const ChaseUnits& units) {
    return [r_m, v_mps, units](double T) {
        const auto s = propagate_kepler(r_m, v_mps, T * units.time_s(), units.mu_m3_s2);
        g::TargetPoint out;
        out.state = units.to_normalised(s.position_m, s.velocity_mps);
        out.velocity = out.state.v;
        const double r = norm(out.state.r);
        out.acceleration = (-1.0 / (r * r * r)) * out.state.r;
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

namespace {

// What the fixed-time searches share: the normalised problem and the propellant a solution costs.
struct FixedTimeProblem {
    const ChaseRequest& request;
    const ChaseUnits& units;
    double tu;
    double au;
    double budget;
    g::Propagator propagator;
    g::TransferRequest transfer;
    g::SolveOptions options;

    FixedTimeProblem(const ChaseRequest& r, const g::Target& target, const ChaseUnits& u)
        : request(r), units(u), tu(u.time_s()), au(u.accel_mps2()),
          budget(2.0 * r.power_w * (1.0 / r.dry_mass_kg - 1.0 / r.initial_mass_kg) / (au * au * tu)),
          transfer {u.to_normalised(r.position_m, r.velocity_mps), target, r.end_condition, r.min_perihelion_m / u.length_m} {
        options.fallbacks = false;   // a time that will not solve quickly is skipped
    }

    [[nodiscard]] double propellant_kg(double effort) const {
        const double effort_si = effort * au * au * tu;
        return request.initial_mass_kg - 1.0 / (1.0 / request.initial_mass_kg + effort_si / (2.0 * request.power_w));
    }

    // Minimum effort at T seconds; nullopt if it does not solve or the load does not cover it.
    [[nodiscard]] std::optional<MeetingCandidate> at(double t_s, const g::Primer* warm, std::optional<g::Primer>* solved = nullptr) const {
        const auto solution = g::solve_transfer(propagator, transfer, t_s / tu, options, warm);
        if (!solution.converged || !solution.end.ok) {
            return std::nullopt;
        }
        if (solved != nullptr) {
            *solved = solution.primer;
        }
        if (solution.end.effort > budget) {
            return std::nullopt;
        }
        MeetingCandidate c;
        c.transfer_time_s = solution.T * tu;
        c.propellant_kg = propellant_kg(solution.end.effort);
        c.primer = solution.primer;
        return c;
    }

    [[nodiscard]] ChasePlan plan(const MeetingCandidate& c, std::size_t path_samples) const {
        ChasePlan plan;
        plan.feasible = true;
        plan.transfer_time_s = c.transfer_time_s;
        plan.propellant_kg = c.propellant_kg;
        for (const auto& sample : propagator.sample(transfer.start, c.primer, c.transfer_time_s / tu, std::max<std::size_t>(2, path_samples))) {
            plan.path_m.push_back(units.position_m(sample.state.r));
            plan.times_s.push_back(request.depart_time_s + sample.t * tu);
        }
        return plan;
    }
};

bool valid_request(const ChaseRequest& request, double earliest_s, double latest_s, std::size_t grid) {
    return request.power_w > 0.0 && request.initial_mass_kg > request.dry_mass_kg && request.dry_mass_kg > 0.0
        && latest_s > earliest_s && earliest_s > 0.0 && grid >= 2;
}

}  // namespace

std::vector<MeetingCandidate> meeting_candidates(const ChaseRequest& request, const g::Target& target, const ChaseUnits& units,
    double earliest_s, double latest_s, double kg_per_s, std::size_t grid) {
    std::vector<MeetingCandidate> out;
    if (!valid_request(request, earliest_s, latest_s, grid)) {
        return out;
    }
    const FixedTimeProblem problem(request, target, units);
    // Geometric spacing over a wide window (days to a year), even spacing over a narrow one.
    const bool geometric = latest_s > 3.0 * earliest_s;
    const double ratio = geometric ? std::pow(latest_s / earliest_s, 1.0 / static_cast<double>(grid - 1)) : 1.0;
    const double step = (latest_s - earliest_s) / static_cast<double>(grid - 1);
    g::Primer warm {};
    bool have_warm = false;
    for (std::size_t i = 0; i < grid; ++i) {
        const double t_s = geometric ? earliest_s * std::pow(ratio, static_cast<double>(i)) : earliest_s + step * static_cast<double>(i);
        std::optional<g::Primer> solved;
        auto c = problem.at(t_s, have_warm ? &warm : nullptr, &solved);
        if (solved) {
            warm = *solved;
            have_warm = true;
        }
        if (!c) {
            continue;
        }
        c->objective = c->propellant_kg + kg_per_s * c->transfer_time_s;
        c->bracket_lo_s = std::max(earliest_s, geometric ? t_s / ratio : t_s - step);
        c->bracket_hi_s = std::min(latest_s, geometric ? t_s * ratio : t_s + step);
        out.push_back(*c);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.objective < b.objective; });
    return out;
}

ChasePlan refine_meeting(const ChaseRequest& request, const g::Target& target, const ChaseUnits& units, double kg_per_s,
    const MeetingCandidate& start, std::size_t iterations, std::size_t path_samples) {
    const FixedTimeProblem problem(request, target, units);
    MeetingCandidate best = start;
    best.objective = best.propellant_kg + kg_per_s * best.transfer_time_s;
    const auto f = [&](double t_s) {
        auto c = problem.at(t_s, &start.primer);
        if (!c) {
            return std::numeric_limits<double>::infinity();
        }
        c->objective = c->propellant_kg + kg_per_s * c->transfer_time_s;
        if (c->objective < best.objective) {
            best = *c;
        }
        return c->objective;
    };
    // Golden-section search between the grid point's neighbours.
    constexpr double kInvPhi = 0.6180339887498949;
    double a = start.bracket_lo_s > 0.0 ? start.bracket_lo_s : start.transfer_time_s;
    double b = start.bracket_hi_s > 0.0 ? start.bracket_hi_s : start.transfer_time_s;
    if (b > a) {
        double c = b - kInvPhi * (b - a);
        double d = a + kInvPhi * (b - a);
        double fc = f(c);
        double fd = f(d);
        for (std::size_t i = 0; i < iterations; ++i) {
            if (fc <= fd) {
                b = d;
                d = c;
                fd = fc;
                c = b - kInvPhi * (b - a);
                fc = f(c);
            } else {
                a = c;
                c = d;
                fc = fd;
                d = a + kInvPhi * (b - a);
                fd = f(d);
            }
        }
    }
    return problem.plan(best, path_samples);
}

ChasePlan plan_cheapest(const ChaseRequest& request, const g::Target& target, const ChaseUnits& units, double earliest_s,
    double latest_s, double kg_per_s, std::size_t grid, std::size_t refine_iterations, std::size_t path_samples) {
    const auto candidates = meeting_candidates(request, target, units, earliest_s, latest_s, kg_per_s, grid);
    if (candidates.empty()) {
        return {};
    }
    return refine_meeting(request, target, units, kg_per_s, candidates.front(), refine_iterations, path_samples);
}

ChasePlan plan_meeting(const ChaseRequest& request, const g::Target& target, const ChaseUnits& units, double earliest_s,
    double latest_s, double kg_per_s, std::size_t grid, std::size_t path_samples) {
    return plan_cheapest(request, target, units, earliest_s, latest_s, kg_per_s, grid, 6, path_samples);
}

}  // namespace spacetrains::trajectory
