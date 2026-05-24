#include "trajectory/TrajectoryPlanner.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace spacetrains::trajectory {

namespace {
constexpr double PI = 3.14159265358979323846;
constexpr double TAU = 6.28318530717958647692;
constexpr double LOCAL_TRANSFER_MIN_TIME_S = 30.0 * 60.0;
constexpr double LOCAL_TRANSFER_MAX_TIME_S = 24.0 * 3600.0;

double normalize_positive_angle(double angle_rad) {
    angle_rad = std::fmod(angle_rad, TAU);
    if (angle_rad < 0.0) {
        angle_rad += TAU;
    }
    return angle_rad;
}

double positive_mod(double value, double modulus) {
    if (modulus <= 0.0) {
        return 0.0;
    }
    value = std::fmod(value, modulus);
    if (value < 0.0) {
        value += modulus;
    }
    return value;
}

double effective_exhaust_velocity_mps(const domain::ShipClassDefinition& ship_class) {
    const double full_mass_kg = ship_class.dry_mass_kg + ship_class.propellant_capacity_kg;
    if (ship_class.max_delta_v_mps <= 0.0 || ship_class.dry_mass_kg <= 0.0 || full_mass_kg <= ship_class.dry_mass_kg) {
        return 0.0;
    }
    return ship_class.max_delta_v_mps / std::log(full_mass_kg / ship_class.dry_mass_kg);
}

double propellant_required_kg(
    const domain::ShipState& ship,
    const domain::ShipClassDefinition& ship_class,
    double delta_v_mps) {
    const double exhaust_velocity_mps = effective_exhaust_velocity_mps(ship_class);
    const double current_wet_mass_kg = ship_class.dry_mass_kg + std::max(0.0, ship.propellant_kg);
    if (exhaust_velocity_mps <= 0.0 || current_wet_mass_kg <= ship_class.dry_mass_kg) {
        return ship_class.propellant_capacity_kg + 1.0;
    }
    return current_wet_mass_kg * (1.0 - std::exp(-delta_v_mps / exhaust_velocity_mps));
}

double orbital_rate_rad_s(const domain::CelestialBodyDefinition& body) {
    if (body.orbit.orbital_period_s <= 0.0) {
        return 0.0;
    }
    return TAU / body.orbit.orbital_period_s;
}

// For Hohmann phase calculations we need the rate at which a body's heliocentric angle
// changes. For moons this is the parent planet's heliocentric rate, not the moon's own
// orbital rate around its parent.
double heliocentric_orbital_rate_rad_s(
    const domain::CelestialBodyDefinition& body,
    const std::string& root_id,
    const std::unordered_map<std::string, const domain::CelestialBodyDefinition*>& bodies_by_id) {
    const domain::CelestialBodyDefinition* current = &body;
    while (!current->orbit.parent_id.empty() && current->orbit.parent_id != root_id) {
        const auto it = bodies_by_id.find(current->orbit.parent_id);
        if (it == bodies_by_id.end()) {
            break;
        }
        current = it->second;
    }
    return orbital_rate_rad_s(*current);
}
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

    if (origin.parent_body_id == destination.parent_body_id) {
        const auto start = mechanics_.get_station_position(origin, current_time_s);
        const auto initial_finish = mechanics_.get_station_position(destination, current_time_s);
        const auto& parent_body = *bodies_by_id_.at(origin.parent_body_id);
        const auto parent_position = mechanics_.get_body_position(parent_body.id, current_time_s);
        const double distance_m = std::max(1.0, (initial_finish - start).length());
        const double accel = std::max(ship_class.cruise_accel_mps2, 0.001);
        const double coast_time_s = std::clamp(2.0 * std::sqrt(distance_m / accel), LOCAL_TRANSFER_MIN_TIME_S, LOCAL_TRANSFER_MAX_TIME_S);
        const double delta_v = std::clamp(50.0 + distance_m * 2.0e-5, 50.0, 400.0);

        plan.departure_time_s = current_time_s;
        plan.arrival_time_s = current_time_s + coast_time_s;
        plan.wait_time_s = 0.0;
        plan.coast_time_s = coast_time_s;
        plan.travel_time_s = coast_time_s;
        plan.propellant_required_kg = propellant_required_kg(ship, ship_class, delta_v);
        plan.feasible = ship.propellant_kg >= plan.propellant_required_kg;

        const auto finish = mechanics_.get_station_position(destination, plan.arrival_time_s);
        const auto start_radial = (start - parent_position).normalized();
        auto finish_radial = (finish - parent_position).normalized();
        if (finish_radial.length() <= 0.0) {
            finish_radial = start_radial;
        }
        const double clearance_radius = parent_body.radius_m + std::max(origin.altitude_m, destination.altitude_m) + distance_m * 0.12;
        constexpr int kLocalSamples = 24;
        plan.sampled_path.reserve(kLocalSamples);
        plan.sampled_times_s.reserve(kLocalSamples);
        for (int i = 0; i < kLocalSamples; ++i) {
            const double alpha = static_cast<double>(i) / static_cast<double>(kLocalSamples - 1);
            const auto chord = start * (1.0 - alpha) + finish * alpha;
            auto radial = (start_radial * (1.0 - alpha) + finish_radial * alpha).normalized();
            if (radial.length() <= 0.0) {
                radial = start_radial;
            }
            const double arc_lift = std::sin(alpha * PI) * clearance_radius * 0.08;
            plan.sampled_path.push_back(chord + radial * arc_lift);
            plan.sampled_times_s.push_back(plan.departure_time_s + alpha * coast_time_s);
        }
        plan.sampled_path.front() = start;
        plan.sampled_path.back() = finish;
        plan.summary = std::format(
            "Local transfer {} -> {} in {:.1f} hours, propellant {:.0f} kg ({})",
            origin.name,
            destination.name,
            plan.travel_time_s / 3600.0,
            plan.propellant_required_kg,
            plan.feasible ? "feasible" : "insufficient fuel");
        return plan;
    }

    const auto& root_body = *bodies_by_id_.at(mechanics_.get_root_body_id());
    const auto& origin_body = *bodies_by_id_.at(origin.parent_body_id);
    const auto& destination_body = *bodies_by_id_.at(destination.parent_body_id);
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
    const double origin_rate = heliocentric_orbital_rate_rad_s(origin_body, root_body.id, bodies_by_id_);
    const double destination_rate = heliocentric_orbital_rate_rad_s(destination_body, root_body.id, bodies_by_id_);
    const double relative_rate = destination_rate - origin_rate;
    const double origin_angle_now = std::atan2(mechanics_.get_body_position(origin_body.id, current_time_s).z, mechanics_.get_body_position(origin_body.id, current_time_s).x);
    const double destination_angle_now = std::atan2(mechanics_.get_body_position(destination_body.id, current_time_s).z, mechanics_.get_body_position(destination_body.id, current_time_s).x);
    const double current_phase = normalize_positive_angle(destination_angle_now - origin_angle_now);
    const double required_phase = normalize_positive_angle(PI - destination_rate * hohmann_time_s);
    double wait_time_s = 0.0;
    if (std::abs(relative_rate) > 1.0e-12) {
        const double synodic_period_s = TAU / std::abs(relative_rate);
        wait_time_s = positive_mod((required_phase - current_phase) / relative_rate, synodic_period_s);
    }

    plan.departure_time_s = current_time_s + wait_time_s;
    plan.coast_time_s = std::max(12.0 * 3600.0, hohmann_time_s);
    plan.wait_time_s = wait_time_s;
    plan.travel_time_s = wait_time_s + plan.coast_time_s;
    plan.arrival_time_s = current_time_s + plan.travel_time_s;
    plan.propellant_required_kg = propellant_required_kg(ship, ship_class, delta_v);
    plan.feasible = ship.propellant_kg >= plan.propellant_required_kg;

    const auto start = mechanics_.get_station_position(origin, plan.departure_time_s);
    const auto finish = mechanics_.get_station_position(destination, plan.arrival_time_s);
    const double start_angle = std::atan2(start.z, start.x);
    const double transfer_direction = 1.0;

    const double eccentricity = std::abs(r2 - r1) / std::max(r1 + r2, 1.0);
    const double parameter = transfer_axis * (1.0 - eccentricity * eccentricity);
    const bool outward = r2 >= r1;
    constexpr int kSamples = 48;
    plan.sampled_path.reserve(kSamples);
    for (int i = 0; i < kSamples; ++i) {
        const double alpha = static_cast<double>(i) / static_cast<double>(kSamples - 1);
        const double anomaly = outward ? alpha * PI : PI - alpha * PI;
        const double radius = eccentricity > 1.0e-9
            ? parameter / (1.0 + eccentricity * std::cos(anomaly))
            : r1;
        const double angle = start_angle + transfer_direction * PI * alpha;
        plan.sampled_path.push_back({std::cos(angle) * radius, start.y * (1.0 - alpha) + finish.y * alpha, std::sin(angle) * radius});
        plan.sampled_times_s.push_back(plan.departure_time_s + alpha * plan.coast_time_s);
    }
    plan.sampled_path.front() = start;
    plan.sampled_path.back() = finish;

    plan.summary = std::format(
        "Kepler transfer {} -> {} in {:.1f} days plus {:.1f} days wait, propellant {:.0f} kg ({})",
        origin.name,
        destination.name,
        plan.coast_time_s / 86400.0,
        plan.wait_time_s / 86400.0,
        plan.propellant_required_kg,
        plan.feasible ? "feasible" : "insufficient fuel");
    return plan;
}

}  // namespace spacetrains::trajectory
