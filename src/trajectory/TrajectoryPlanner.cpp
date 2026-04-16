#include "trajectory/TrajectoryPlanner.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace spacetrains::trajectory {

namespace {
constexpr double PI = 3.14159265358979323846;
}

KeplerTrajectoryPlanner::KeplerTrajectoryPlanner(
    const domain::UniverseDefinition& universe,
    const celestial::CelestialMechanics& mechanics)
    : universe_(universe), mechanics_(mechanics) {
    for (const auto& body : universe_.bodies) {
        bodies_by_id_[body.id] = &body;
    }
}

domain::TrajectoryPlan KeplerTrajectoryPlanner::plan_transfer(
    const domain::StationDefinition& origin,
    const domain::StationDefinition& destination,
    const domain::ShipState& ship,
    const domain::ShipClassDefinition& ship_class,
    double current_time_s) const {
    domain::TrajectoryPlan plan;
    plan.departure_time_s = current_time_s;

    const auto& root_body = *bodies_by_id_.at(mechanics_.get_root_body_id());
    const double mu = root_body.mu_m3_s2;
    const double r1 = std::max(1.0, mechanics_.get_heliocentric_radius(origin.parent_body_id, current_time_s));
    const double r2 = std::max(1.0, mechanics_.get_heliocentric_radius(destination.parent_body_id, current_time_s));
    const double transfer_axis = std::max(1.0, (r1 + r2) * 0.5);

    const double hohmann_time_s = PI * std::sqrt((transfer_axis * transfer_axis * transfer_axis) / mu);
    const double v1 = std::sqrt(mu / r1);
    const double v2 = std::sqrt(mu / r2);
    const double v_transfer_1 = std::sqrt(mu * ((2.0 / r1) - (1.0 / transfer_axis)));
    const double v_transfer_2 = std::sqrt(mu * ((2.0 / r2) - (1.0 / transfer_axis)));
    const double delta_v = std::abs(v_transfer_1 - v1) + std::abs(v2 - v_transfer_2) + 250.0;

    plan.travel_time_s = std::max(12.0 * 3600.0, hohmann_time_s);
    plan.arrival_time_s = current_time_s + plan.travel_time_s;
    plan.propellant_required_kg = ship_class.propellant_capacity_kg * std::clamp(delta_v / ship_class.max_delta_v_mps, 0.05, 0.95);
    plan.feasible = ship.propellant_kg >= plan.propellant_required_kg;

    const auto start = mechanics_.get_station_position(origin, current_time_s);
    const auto finish = mechanics_.get_station_position(destination, plan.arrival_time_s);
    constexpr int kSamples = 24;
    plan.sampled_path.reserve(kSamples);
    for (int i = 0; i < kSamples; ++i) {
        const double alpha = static_cast<double>(i) / static_cast<double>(kSamples - 1);
        plan.sampled_path.push_back(start * (1.0 - alpha) + finish * alpha);
    }

    plan.summary = std::format(
        "Kepler transfer {} -> {} in {:.1f} days, propellant {:.0f} kg ({})",
        origin.name,
        destination.name,
        plan.travel_time_s / 86400.0,
        plan.propellant_required_kg,
        plan.feasible ? "feasible" : "insufficient fuel");
    return plan;
}

}  // namespace spacetrains::trajectory
