#include "trajectory/TrajectoryPlanner.hpp"
#include "trajectory/Lambert.hpp"
#include "trajectory/PathSampling.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <iostream>
#include <limits>

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

// One sample of an elliptic arc: true anomaly, radius, and fraction of the arc's
// flight time elapsed.
struct EllipseSample {
    double theta;
    double radius;
    double time_fraction;
};

// Samples the elliptic arc from true anomaly theta1 to theta2 (theta2 > theta1, both
// unwrapped, at most one revolution) evenly in eccentric anomaly E. Even true-anomaly
// steps leave the sharply curved far end of an eccentric ellipse (radius of curvature
// b²/a) with long chords and visible kinks; even E steps are dense at both vertices,
// and Kepler timing follows directly from M = E − e·sin E.
std::vector<EllipseSample> sample_elliptic_arc(
    double semi_latus_rectum, double ecc, double theta1, double theta2, int samples) {
    const double root = std::sqrt((1.0 - ecc) / (1.0 + ecc));
    // Unwrapped E and theta share their revolution count (E = theta at multiples of π).
    const auto eccentric_from_true = [&](double theta) {
        const double turns = std::round((theta - std::remainder(theta, TAU)) / TAU);
        return 2.0 * std::atan(root * std::tan(0.5 * std::remainder(theta, TAU))) + TAU * turns;
    };
    const auto true_from_eccentric = [&](double E) {
        const double turns = std::round((E - std::remainder(E, TAU)) / TAU);
        return 2.0 * std::atan(std::tan(0.5 * std::remainder(E, TAU)) / root) + TAU * turns;
    };
    // tan(x/2) is singular at x = ±π (apoapsis); those points map to themselves.
    const auto e_of = [&](double theta) {
        return std::abs(std::abs(std::remainder(theta, TAU)) - PI) < 1e-12 ? theta : eccentric_from_true(theta);
    };
    const auto theta_of = [&](double E) {
        return std::abs(std::abs(std::remainder(E, TAU)) - PI) < 1e-12 ? E : true_from_eccentric(E);
    };
    const double semi_major = semi_latus_rectum / (1.0 - ecc * ecc);
    const double E1 = e_of(theta1);
    const double E2 = e_of(theta2);
    const double M1 = E1 - ecc * std::sin(E1);
    const double dM_total = std::max(1e-12, (E2 - ecc * std::sin(E2)) - M1);
    std::vector<EllipseSample> out;
    out.reserve(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        const double alpha = static_cast<double>(i) / (samples - 1);
        const double E = E1 + (E2 - E1) * alpha;
        out.push_back({
            .theta = i == 0 ? theta1 : (i == samples - 1 ? theta2 : theta_of(E)),
            .radius = semi_major * (1.0 - ecc * std::cos(E)),
            .time_fraction = ((E - ecc * std::sin(E)) - M1) / dM_total,
        });
    }
    return out;
}

// Prepend wait-period samples so the rendered path tracks the origin station during the
// wait. Without these, the frontend sees a gap between the ship's current position and
// path[0] (the departure position) and draws a straight chord, which looks wrong visually.
// Waits can last several origin orbits (up to ~2 years), so sample by the origin's
// heliocentric sweep rather than a fixed count; a fixed 11 samples drew long waits as
// star polygons around the Sun.
void prepend_wait_samples(
    domain::TrajectoryPlan& plan,
    std::vector<double> arc_propellant,
    const celestial::CelestialMechanics& mechanics,
    const domain::StationDefinition& origin,
    double current_time_s,
    double origin_rate,
    double propellant_kg) {
    if (plan.wait_time_s <= 60.0) {
        plan.sampled_propellant_kg = std::move(arc_propellant);
        return;
    }
    constexpr double kWaitStepRad = 5.0 * PI / 180.0;
    constexpr int kMinWaitSegments = 11;
    constexpr int kMaxWaitSegments = 360;
    const double wait_sweep_rad = std::abs(origin_rate) * plan.wait_time_s;
    const int wait_segments = std::clamp(
        static_cast<int>(std::ceil(wait_sweep_rad / kWaitStepRad)), kMinWaitSegments, kMaxWaitSegments);
    std::vector<math::Vec3d> wait_path;
    std::vector<double> wait_times;
    std::vector<double> wait_propellant;
    wait_path.reserve(wait_segments + plan.sampled_path.size());
    wait_times.reserve(wait_segments + plan.sampled_times_s.size());
    wait_propellant.reserve(wait_segments + arc_propellant.size());
    for (int i = 0; i < wait_segments; ++i) {
        const double alpha = static_cast<double>(i) / wait_segments;
        const double t = current_time_s + alpha * plan.wait_time_s;
        wait_path.push_back(mechanics.get_station_position(origin, t));
        wait_times.push_back(t);
        wait_propellant.push_back(propellant_kg);
    }
    wait_path.insert(wait_path.end(), plan.sampled_path.begin(), plan.sampled_path.end());
    wait_times.insert(wait_times.end(), plan.sampled_times_s.begin(), plan.sampled_times_s.end());
    wait_propellant.insert(wait_propellant.end(), arc_propellant.begin(), arc_propellant.end());
    plan.sampled_path = std::move(wait_path);
    plan.sampled_times_s = std::move(wait_times);
    plan.sampled_propellant_kg = std::move(wait_propellant);
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

std::string planet_system_primary(const celestial::CelestialMechanics& mechanics, const std::string& body_id) {
    const std::string root = mechanics.get_root_body_id();
    const domain::CelestialBodyDefinition* body = &mechanics.get_body(body_id);
    while (!body->orbit.parent_id.empty() && body->orbit.parent_id != root) {
        body = &mechanics.get_body(body->orbit.parent_id);
    }
    return body->id;
}

TimedPath planet_system_path(
    const celestial::CelestialMechanics& mechanics,
    const std::string& primary_id,
    const domain::StationDefinition& origin,
    const domain::StationDefinition& destination,
    double departure_time_s,
    double transfer_time_s,
    bool kepler_timing) {
    const double arrival_time_s = departure_time_s + transfer_time_s;
    const auto start = mechanics.get_station_position(origin, departure_time_s);
    const auto finish = mechanics.get_station_position(destination, arrival_time_s);
    const auto start_rel = start - mechanics.get_body_position(primary_id, departure_time_s);
    const auto finish_rel = finish - mechanics.get_body_position(primary_id, arrival_time_s);
    const double r_start = std::max(1.0, std::hypot(start_rel.x, start_rel.z));
    const double r_finish = std::max(1.0, std::hypot(finish_rel.x, finish_rel.z));
    const double start_angle = std::atan2(start_rel.z, start_rel.x);
    const double sweep = normalize_positive_angle(std::atan2(finish_rel.z, finish_rel.x) - start_angle);
    const double eccentricity = std::abs(r_finish - r_start) / (r_start + r_finish);
    const double parameter = 0.5 * (r_start + r_finish) * (1.0 - eccentricity * eccentricity);
    const double theta_begin = r_finish >= r_start ? 0.0 : PI;

    constexpr int kDense = 721;
    const auto arc = eccentricity > 1.0e-9
        ? sample_elliptic_arc(parameter, eccentricity, theta_begin, theta_begin + PI, kDense)
        : std::vector<EllipseSample> {};
    TimedPath dense;
    dense.path.reserve(kDense);
    dense.times_s.reserve(kDense);
    for (int i = 0; i < kDense; ++i) {
        const double alpha = static_cast<double>(i) / (kDense - 1);
        const double radius = arc.empty() ? r_start : arc[static_cast<std::size_t>(i)].radius;
        const double progress = arc.empty() ? alpha : (arc[static_cast<std::size_t>(i)].theta - theta_begin) / PI;
        const double time_fraction = (kepler_timing && !arc.empty()) ? arc[static_cast<std::size_t>(i)].time_fraction : alpha;
        const double t = departure_time_s + time_fraction * transfer_time_s;
        const double angle = start_angle + sweep * progress;
        dense.path.push_back(mechanics.get_body_position(primary_id, t)
            + math::Vec3d {std::cos(angle) * radius, 0.0, std::sin(angle) * radius});
        dense.times_s.push_back(t);
    }
    dense.path.front() = start;
    dense.path.back() = finish;

    TimedPath out;
    for (const auto index : select_for_rendering(dense.path)) {
        out.path.push_back(dense.path[index]);
        out.times_s.push_back(dense.times_s[index]);
    }
    return out;
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
    double current_time_s,
    const PlanningCosts& costs) const {
    domain::TrajectoryPlan plan;

    if (origin.parent_body_id == destination.parent_body_id) {
        const auto start = mechanics_.get_station_position(origin, current_time_s);
        const auto initial_finish = mechanics_.get_station_position(destination, current_time_s);
        const auto& parent_body = *bodies_by_id_.at(origin.parent_body_id);
        const auto parent_position = mechanics_.get_body_position(parent_body.id, current_time_s);
        const double distance_m = std::max(1.0, (initial_finish - start).length());
        const double accel = std::max(ship_class.cruise_accel_mps2, 0.001);
        const double coast_time_s = std::clamp(2.0 * std::sqrt(distance_m / accel), LOCAL_TRANSFER_MIN_TIME_S, LOCAL_TRANSFER_MAX_TIME_S);
        double dv_departure = 0.0;
        double dv_arrival = 0.0;
        if (parent_body.mu_m3_s2 > 0.0) {
            const double r_orig = std::max(1.0, parent_body.radius_m + origin.altitude_m);
            const double r_dest_body = std::max(1.0, parent_body.radius_m + destination.altitude_m);
            const double local_axis = (r_orig + r_dest_body) * 0.5;
            const double v_c1 = std::sqrt(parent_body.mu_m3_s2 / r_orig);
            const double v_c2 = std::sqrt(parent_body.mu_m3_s2 / r_dest_body);
            const double v_t1 = std::sqrt(parent_body.mu_m3_s2 * ((2.0 / r_orig) - (1.0 / local_axis)));
            const double v_t2 = std::sqrt(parent_body.mu_m3_s2 * ((2.0 / r_dest_body) - (1.0 / local_axis)));
            dv_departure = std::abs(v_t1 - v_c1);
            dv_arrival = std::abs(v_c2 - v_t2);
        }
        const double delta_v = dv_departure + dv_arrival;

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
        {
            const double ve = effective_exhaust_velocity_mps(ship_class);
            const double m0 = ship_class.dry_mass_kg + ship.propellant_kg;
            const double prop_dep = (ve > 0.0) ? m0 * (1.0 - std::exp(-dv_departure / ve)) : 0.0;
            const double m1 = m0 - prop_dep;
            const double prop_arr = (ve > 0.0 && m1 > ship_class.dry_mass_kg)
                ? m1 * (1.0 - std::exp(-dv_arrival / ve)) : 0.0;
            const double propellant_coast = std::max(0.0, ship.propellant_kg - prop_dep);
            plan.sampled_propellant_kg.resize(kLocalSamples);
            plan.sampled_propellant_kg.front() = ship.propellant_kg;
            for (int i = 1; i < kLocalSamples - 1; ++i) {
                plan.sampled_propellant_kg[i] = propellant_coast;
            }
            plan.sampled_propellant_kg.back() = std::max(0.0, propellant_coast - prop_arr);
        }
        plan.trajectory_type = "keplerian_local";
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
    // Different bodies of one planetary system (Earth <-> Moon): Hohmann transfer around
    // the planet, timed for the moving body's phase. Treating these as heliocentric
    // transfers planned a 183-day half orbit of the Sun for a 400,000 km hop.
    if (const auto primary_id = planet_system_primary(mechanics_, origin.parent_body_id);
        primary_id == planet_system_primary(mechanics_, destination.parent_body_id)) {
        const auto& primary = mechanics_.get_body(primary_id);
        const double mu_p = primary.mu_m3_s2;
        const auto relative = [&](const domain::StationDefinition& station, double t) {
            return mechanics_.get_station_position(station, t) - mechanics_.get_body_position(primary_id, t);
        };
        const auto r_of = [](const math::Vec3d& v) { return std::max(1.0, std::hypot(v.x, v.z)); };
        const double r1 = r_of(relative(origin, current_time_s));
        const double r2 = r_of(relative(destination, current_time_s));
        const double axis = 0.5 * (r1 + r2);
        const double transfer_s = PI * std::sqrt(axis * axis * axis / mu_p);
        // Phase: arrive where the destination is, half a revolution from departure.
        // Scan one month (the Moon's period) for the best departure.
        double best_wait_s = 0.0;
        double best_mismatch = std::numeric_limits<double>::max();
        for (double wait = 0.0; wait <= 30.0 * 86400.0; wait += 0.05 * 86400.0) {
            const auto o = relative(origin, current_time_s + wait);
            const auto d = relative(destination, current_time_s + wait + transfer_s);
            const double mismatch = std::abs(std::remainder(
                std::atan2(d.z, d.x) - std::atan2(o.z, o.x) - PI, TAU));
            if (mismatch < best_mismatch - 1e-9) {
                best_mismatch = mismatch;
                best_wait_s = wait;
            }
        }
        const double v_c1 = std::sqrt(mu_p / r1);
        const double v_c2 = std::sqrt(mu_p / r2);
        const double v_t1 = std::sqrt(mu_p * (2.0 / r1 - 1.0 / axis));
        const double v_t2 = std::sqrt(mu_p * (2.0 / r2 - 1.0 / axis));
        const double dv_departure = std::abs(v_t1 - v_c1);
        const double dv_arrival = std::abs(v_c2 - v_t2);

        plan.wait_time_s = best_wait_s;
        plan.departure_time_s = current_time_s + best_wait_s;
        plan.coast_time_s = transfer_s;
        plan.arrival_time_s = plan.departure_time_s + transfer_s;
        plan.travel_time_s = best_wait_s + transfer_s;
        plan.propellant_required_kg = propellant_required_kg(ship, ship_class, dv_departure + dv_arrival);
        plan.feasible = ship.propellant_kg >= plan.propellant_required_kg;
        plan.trajectory_type = "keplerian_planet_system";

        auto timed = planet_system_path(
            mechanics_, primary_id, origin, destination, plan.departure_time_s, transfer_s, true);
        plan.sampled_path = std::move(timed.path);
        plan.sampled_times_s = std::move(timed.times_s);
        std::vector<double> arc_propellant(plan.sampled_path.size());
        {
            const double ve = effective_exhaust_velocity_mps(ship_class);
            const double m0 = ship_class.dry_mass_kg + ship.propellant_kg;
            const double prop_dep = ve > 0.0 ? m0 * (1.0 - std::exp(-dv_departure / ve)) : 0.0;
            const double coast = std::max(0.0, ship.propellant_kg - prop_dep);
            std::fill(arc_propellant.begin(), arc_propellant.end(), coast);
            arc_propellant.front() = ship.propellant_kg;
            arc_propellant.back() = std::max(0.0, ship.propellant_kg - plan.propellant_required_kg);
        }
        prepend_wait_samples(plan, std::move(arc_propellant), mechanics_, origin, current_time_s,
            heliocentric_orbital_rate_rad_s(primary, mechanics_.get_root_body_id(), bodies_by_id_), ship.propellant_kg);
        plan.summary = std::format(
            "Kepler planet-system transfer {} -> {} in {:.1f} days plus {:.1f} days wait, propellant {:.0f} kg ({})",
            origin.name, destination.name, transfer_s / 86400.0, best_wait_s / 86400.0,
            plan.propellant_required_kg, plan.feasible ? "feasible" : "insufficient fuel");
        return plan;
    }

    const auto& origin_body = *bodies_by_id_.at(origin.parent_body_id);
    const auto& destination_body = *bodies_by_id_.at(destination.parent_body_id);
    const double mu = root_body.mu_m3_s2;
    const double r1 = std::max(1.0, mechanics_.get_heliocentric_radius(origin.parent_body_id, current_time_s));
    const double r2 = std::max(1.0, mechanics_.get_heliocentric_radius(destination.parent_body_id, current_time_s));
    const double transfer_axis = std::max(1.0, (r1 + r2) * 0.5);
    const double hohmann_time_s = PI * std::sqrt((transfer_axis * transfer_axis * transfer_axis) / mu);

    // Hohmann reference used for fallback path and transit-fraction scaling.
    const double v1_circ = std::sqrt(mu / r1);
    const double v2_circ = std::sqrt(mu / r2);
    const double v_t1 = std::sqrt(mu * ((2.0 / r1) - (1.0 / transfer_axis)));
    const double v_t2 = std::sqrt(mu * ((2.0 / r2) - (1.0 / transfer_axis)));
    const double hohmann_dv_dep = std::abs(v_t1 - v1_circ);
    const double hohmann_dv_arr = std::abs(v2_circ - v_t2);

    const double origin_rate = heliocentric_orbital_rate_rad_s(origin_body, root_body.id, bodies_by_id_);
    const double destination_rate = heliocentric_orbital_rate_rad_s(destination_body, root_body.id, bodies_by_id_);
    const double relative_rate = destination_rate - origin_rate;
    const double synodic_period_s = (std::abs(relative_rate) > 1.0e-12)
        ? TAU / std::abs(relative_rate)
        : 2.0 * hohmann_time_s;

    // Hohmann alignment window — used as fallback if Lambert finds nothing.
    const double origin_angle_now = std::atan2(
        mechanics_.get_body_position(origin_body.id, current_time_s).z,
        mechanics_.get_body_position(origin_body.id, current_time_s).x);
    const double destination_angle_now = std::atan2(
        mechanics_.get_body_position(destination_body.id, current_time_s).z,
        mechanics_.get_body_position(destination_body.id, current_time_s).x);
    const double current_phase = normalize_positive_angle(destination_angle_now - origin_angle_now);
    const double required_phase = normalize_positive_angle(PI - destination_rate * hohmann_time_s);
    double hohmann_wait_s = 0.0;
    if (std::abs(relative_rate) > 1.0e-12) {
        hohmann_wait_s = positive_mod((required_phase - current_phase) / relative_rate, synodic_period_s);
    }

    // Initialise best to the Hohmann solution so we always have a valid fallback.
    double best_dep_time = current_time_s + hohmann_wait_s;
    double best_transit_time = hohmann_time_s;
    double best_dv_dep = hohmann_dv_dep;
    double best_dv_arr = hohmann_dv_arr;
    double best_dv = hohmann_dv_dep + hohmann_dv_arr;
    bool found_feasible = ship.propellant_kg >= propellant_required_kg(ship, ship_class, best_dv);
    // Cheapest feasible transfer when the owner's costs are known, else the fastest.
    const bool by_cost = costs.propellant_cr_per_kg > 0.0 || costs.time_cr_per_day > 0.0;
    const auto objective = [&](double total_s, double dv) {
        return by_cost
            ? propellant_required_kg(ship, ship_class, dv) * costs.propellant_cr_per_kg
                + total_s / 86400.0 * costs.time_cr_per_day
            : total_s;
    };
    double best_objective = objective(hohmann_wait_s + hohmann_time_s, best_dv);
    bool best_from_lambert = false;
    math::Vec3d best_r1_pos{}, best_r2_pos{}, best_v1_lambert{};

    // Lambert grid search: N_DEP departure offsets × N_TRANSIT transit fractions.
    // Minimise the objective above (cost, or wait + transit), subject to propellant feasibility.
    constexpr int N_DEP = 30;
    constexpr int N_TRANSIT = 10;
    const double search_window_s = std::min(synodic_period_s, 730.0 * 86400.0);

    for (int di = 0; di < N_DEP; ++di) {
        const double wait_k = search_window_s * static_cast<double>(di) / N_DEP;
        const double dep_time_k = current_time_s + wait_k;
        const auto r1_pos = mechanics_.get_body_position(origin_body.id, dep_time_k);
        const double r1_m_k = std::max(1.0, r1_pos.length());
        const double vc1 = std::sqrt(mu / r1_m_k);
        const math::Vec3d v_circ1{-r1_pos.z / r1_m_k * vc1, 0.0, r1_pos.x / r1_m_k * vc1};

        for (int ti = 0; ti < N_TRANSIT; ++ti) {
            const double frac = 0.3 + 1.2 * static_cast<double>(ti) / (N_TRANSIT - 1);
            const double transit_k = hohmann_time_s * frac;
            const auto r2_pos = mechanics_.get_body_position(destination_body.id, dep_time_k + transit_k);
            const double r2_m_k = std::max(1.0, r2_pos.length());

            const auto lam = solve_lambert(r1_pos, r2_pos, transit_k, mu);
            if (!lam.found) continue;
            if (conic_arc_min_radius(r1_pos, lam.v1, r2_pos, mu) < kMinPerihelionM) continue;

            const double vc2 = std::sqrt(mu / r2_m_k);
            const math::Vec3d v_circ2{-r2_pos.z / r2_m_k * vc2, 0.0, r2_pos.x / r2_m_k * vc2};
            const double dv_dep_k = (lam.v1 - v_circ1).length();
            const double dv_arr_k = (lam.v2 - v_circ2).length();
            const double dv_k = dv_dep_k + dv_arr_k;
            const double total_k = wait_k + transit_k;
            const bool feas_k = ship.propellant_kg >= propellant_required_kg(ship, ship_class, dv_k);

            const double objective_k = objective(total_k, dv_k);
            if (feas_k && (!found_feasible || objective_k < best_objective)) {
                best_objective = objective_k;
                best_dv = dv_k;
                found_feasible = true;
                best_dep_time = dep_time_k;
                best_transit_time = transit_k;
                best_dv_dep = dv_dep_k;
                best_dv_arr = dv_arr_k;
                best_r1_pos = r1_pos;
                best_r2_pos = r2_pos;
                best_v1_lambert = lam.v1;
                best_from_lambert = true;
            } else if (!found_feasible && dv_k < best_dv) {
                best_dv = dv_k;
                best_dep_time = dep_time_k;
                best_transit_time = transit_k;
                best_dv_dep = dv_dep_k;
                best_dv_arr = dv_arr_k;
                best_r1_pos = r1_pos;
                best_r2_pos = r2_pos;
                best_v1_lambert = lam.v1;
                best_from_lambert = true;
            }
        }
    }

    const double delta_v = best_dv_dep + best_dv_arr;
    plan.departure_time_s = best_dep_time;
    plan.coast_time_s = std::max(12.0 * 3600.0, best_transit_time);
    plan.wait_time_s = best_dep_time - current_time_s;
    plan.travel_time_s = plan.wait_time_s + plan.coast_time_s;
    plan.arrival_time_s = current_time_s + plan.travel_time_s;
    plan.propellant_required_kg = propellant_required_kg(ship, ship_class, delta_v);
    plan.feasible = ship.propellant_kg >= plan.propellant_required_kg;

    const auto start = mechanics_.get_station_position(origin, plan.departure_time_s);
    const auto finish = mechanics_.get_station_position(destination, plan.arrival_time_s);
    const double start_angle = std::atan2(start.z, start.x);

    // Generate densely, then thin by curvature for rendering (see PathSampling.hpp).
    constexpr int kSamples = 721;
    plan.sampled_path.reserve(kSamples);
    plan.sampled_times_s.reserve(kSamples);

    if (best_from_lambert && best_r1_pos.length() > 1.0) {
        // Derive orbital elements from Lambert departure state and sweep true anomaly.
        const double r1m = std::max(1.0, best_r1_pos.length());
        // h = (r × v).y  for XZ-plane orbit; L_vec = (0, −h, 0)
        // e = (v × L)/mu − r̂ = (v.z·h/mu − r.x/|r|, 0, −v.x·h/mu − r.z/|r|)
        const double h = best_r1_pos.x * best_v1_lambert.z - best_r1_pos.z * best_v1_lambert.x;
        const double p_orb = h * h / mu;
        const double ex =  best_v1_lambert.z * h / mu - best_r1_pos.x / r1m;
        const double ez = -best_v1_lambert.x * h / mu - best_r1_pos.z / r1m;
        const double ecc = std::sqrt(ex * ex + ez * ez);
        const double omega = std::atan2(ez, ex);
        double theta1 = std::atan2(best_r1_pos.z, best_r1_pos.x) - omega;
        double theta2 = std::atan2(best_r2_pos.z, best_r2_pos.x) - omega;
        while (theta2 <= theta1) theta2 += TAU;
        if (theta2 - theta1 > TAU) theta2 -= TAU;

        // Kepler time-of-flight: compute a mean anomaly (proportional to time since
        // periapsis) for each true anomaly sample so that time stamps respect the actual
        // orbital speed (fast near periapsis). Lambert arcs can be hyperbolic when the ship
        // has Δv to spare (e.g. a full-tank NTR to Saturn), so handle every conic:
        //   ellipse   M = E − e·sin E,   E = atan2(√(1−e²)·sin θ, e + cos θ)
        //   hyperbola M = e·sinh H − H,  tanh(H/2) = √((e−1)/(e+1))·tan(θ/2)
        //   parabola  M = D + D³/3,      D = tan(θ/2)   (Barker)
        // Only ratios of ΔM are used, so the conics' different scale factors cancel.
        constexpr double kParabolicBand = 1e-6;
        const bool elliptic = ecc < 1.0 - kParabolicBand;
        const bool hyperbolic = ecc > 1.0 + kParabolicBand;
        // Open orbits (mean_anom is only used for them; ellipses go through
        // sample_elliptic_arc) never pass apoapsis, so the arc lies within (−π, π).
        const auto mean_anom = [&](double theta) {
            const double half = 0.5 * std::remainder(theta, TAU);
            if (hyperbolic) {
                const double H = 2.0 * std::atanh(std::sqrt((ecc - 1.0) / (ecc + 1.0)) * std::tan(half));
                return ecc * std::sinh(H) - H;
            }
            const double D = std::tan(half);
            return D + D * D * D / 3.0;
        };
        if (elliptic && p_orb > 1.0) {
            for (const auto& sample : sample_elliptic_arc(p_orb, ecc, theta1, theta2, kSamples)) {
                plan.sampled_path.push_back({
                    std::cos(omega + sample.theta) * sample.radius, 0.0, std::sin(omega + sample.theta) * sample.radius});
                plan.sampled_times_s.push_back(plan.departure_time_s + sample.time_fraction * plan.coast_time_s);
            }
        } else {
            const double M1 = mean_anom(theta1);
            double dM_total = mean_anom(theta2) - M1;
            if (dM_total < 1e-12) dM_total = TAU;  // degenerate guard
            for (int i = 0; i < kSamples; ++i) {
                const double alpha = static_cast<double>(i) / (kSamples - 1);
                const double theta = theta1 + (theta2 - theta1) * alpha;
                const double denominator = 1.0 + ecc * std::cos(theta);
                const double r_at = (p_orb > 1.0 && denominator > 1e-9) ? p_orb / denominator : r1m;
                plan.sampled_path.push_back({std::cos(omega + theta) * r_at, 0.0, std::sin(omega + theta) * r_at});
                plan.sampled_times_s.push_back(plan.departure_time_s + (mean_anom(theta) - M1) / dM_total * plan.coast_time_s);
            }
        }
    } else {
        // Hohmann fallback path: a half transfer ellipse drawn through the actual start
        // and finish stations. The planning radii (parent bodies at planning time) and an
        // exact 180° sweep missed moon stations by up to ~0.05 AU, which the endpoint snap
        // turned into a dent; the small angular mismatch is spread over the arc instead.
        const double r_start = std::max(1.0, std::hypot(start.x, start.z));
        const double r_finish = std::max(1.0, std::hypot(finish.x, finish.z));
        const double sweep = normalize_positive_angle(std::atan2(finish.z, finish.x) - start_angle);
        const double semi_major = 0.5 * (r_start + r_finish);
        const double eccentricity = std::abs(r_finish - r_start) / (r_start + r_finish);
        const double parameter = semi_major * (1.0 - eccentricity * eccentricity);
        const bool outward = r_finish >= r_start;
        // Outward runs periapsis -> apoapsis (theta 0 -> π); inward apoapsis -> periapsis
        // (π -> 2π). The sweep is mapped onto the actual angle between the stations.
        const double theta_begin = outward ? 0.0 : PI;
        const auto arc = eccentricity > 1.0e-9
            ? sample_elliptic_arc(parameter, eccentricity, theta_begin, theta_begin + PI, kSamples)
            : std::vector<EllipseSample> {};
        for (int i = 0; i < kSamples; ++i) {
            const double alpha = static_cast<double>(i) / (kSamples - 1);
            const double radius = arc.empty() ? r_start : arc[static_cast<std::size_t>(i)].radius;
            const double progress = arc.empty() ? alpha : (arc[static_cast<std::size_t>(i)].theta - theta_begin) / PI;
            const double time_fraction = arc.empty() ? alpha : arc[static_cast<std::size_t>(i)].time_fraction;
            const double angle = start_angle + sweep * progress;
            plan.sampled_path.push_back({std::cos(angle) * radius, start.y * (1.0 - progress) + finish.y * progress, std::sin(angle) * radius});
            plan.sampled_times_s.push_back(plan.departure_time_s + time_fraction * plan.coast_time_s);
        }
    }
    {
        const auto keep = select_for_rendering(plan.sampled_path);
        std::vector<math::Vec3d> path;
        std::vector<double> times;
        path.reserve(keep.size());
        times.reserve(keep.size());
        for (const auto index : keep) {
            path.push_back(plan.sampled_path[index]);
            times.push_back(plan.sampled_times_s[index]);
        }
        plan.sampled_path = std::move(path);
        plan.sampled_times_s = std::move(times);
    }
    plan.diagnostics.start_miss_m = (plan.sampled_path.front() - start).length();
    plan.diagnostics.endpoint_miss_m = (plan.sampled_path.back() - finish).length();
    plan.sampled_path.front() = start;
    plan.sampled_path.back() = finish;
    plan.trajectory_type = best_from_lambert ? "keplerian_lambert" : "keplerian_hohmann";

    // Newton's law verification: for every 6th interior Lambert arc sample, log
    // implied velocity, gravitational acceleration, and specific orbital energy.
    // Specific energy E = 0.5*v² − mu/r must be constant on a Keplerian orbit.
    if (best_from_lambert) {
        const int n = static_cast<int>(plan.sampled_path.size());
        std::cerr << std::format("[Lambert dbg] arc {:.0f}d  ecc≈ r1={:.3e}m r2={:.3e}m\n",
            plan.coast_time_s / 86400.0, best_r1_pos.length(), best_r2_pos.length());
        for (int i = 1; i < n - 1; ++i) {
            if ((i % 6) != 0) continue;
            const double dt_fwd = plan.sampled_times_s[i + 1] - plan.sampled_times_s[i];
            const double dt_bck = plan.sampled_times_s[i]     - plan.sampled_times_s[i - 1];
            if (dt_fwd < 1.0 || dt_bck < 1.0) continue;
            const auto& p0 = plan.sampled_path[i - 1];
            const auto& p1 = plan.sampled_path[i];
            const auto& p2 = plan.sampled_path[i + 1];
            // Central-difference velocity (non-uniform grid)
            const double dt_tot = dt_fwd + dt_bck;
            const math::Vec3d v_impl{(p2.x - p0.x) / dt_tot, 0.0, (p2.z - p0.z) / dt_tot};
            const double speed = std::sqrt(v_impl.x * v_impl.x + v_impl.z * v_impl.z);
            // Non-uniform second derivative: a ≈ 2*(p2/dt_fwd − p1/dt_bck·dt_fwd*(1/dt_bck+1/dt_fwd) + p0/dt_bck) / dt_tot
            const math::Vec3d a_impl{
                2.0 * (p2.x / dt_fwd - p1.x * (dt_tot) / (dt_bck * dt_fwd) + p0.x / dt_bck) / dt_tot,
                0.0,
                2.0 * (p2.z / dt_fwd - p1.z * (dt_tot) / (dt_bck * dt_fwd) + p0.z / dt_bck) / dt_tot};
            const double a_impl_mag = std::sqrt(a_impl.x * a_impl.x + a_impl.z * a_impl.z);
            const double r = std::max(1.0, std::sqrt(p1.x * p1.x + p1.z * p1.z));
            const double a_grav_mag = mu / (r * r);
            const double err_pct = (a_grav_mag > 1e-20)
                ? 100.0 * std::abs(a_impl_mag - a_grav_mag) / a_grav_mag : 0.0;
            const double energy = 0.5 * speed * speed - mu / r;
            std::cerr << std::format(
                "  i={:2d}  r={:.3e}m  v={:.0f}m/s  |a_num|={:.4e}  |a_grav|={:.4e}  err={:.1f}%  E={:.4e}J/kg\n",
                i, r, speed, a_impl_mag, a_grav_mag, err_pct, energy);
        }
    }

    // Compute propellant profile for the transit arc.
    const std::size_t arc_samples = plan.sampled_path.size();
    std::vector<double> arc_propellant(arc_samples);
    {
        const double ve = effective_exhaust_velocity_mps(ship_class);
        const double m0 = ship_class.dry_mass_kg + ship.propellant_kg;
        const double prop_dep = (ve > 0.0) ? m0 * (1.0 - std::exp(-best_dv_dep / ve)) : 0.0;
        const double m1 = m0 - prop_dep;
        const double prop_arr = (ve > 0.0 && m1 > ship_class.dry_mass_kg)
            ? m1 * (1.0 - std::exp(-best_dv_arr / ve)) : 0.0;
        const double propellant_coast = std::max(0.0, ship.propellant_kg - prop_dep);
        arc_propellant.front() = ship.propellant_kg;
        for (std::size_t i = 1; i + 1 < arc_samples; ++i)
            arc_propellant[i] = propellant_coast;
        arc_propellant.back() = std::max(0.0, propellant_coast - prop_arr);
    }

    prepend_wait_samples(plan, std::move(arc_propellant), mechanics_, origin, current_time_s, origin_rate, ship.propellant_kg);

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

double chemical_exhaust_velocity_mps(const domain::ShipClassDefinition& ship_class) {
    return effective_exhaust_velocity_mps(ship_class);
}

}  // namespace spacetrains::trajectory
