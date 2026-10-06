#include "trajectory/TrajectoryPlanner.hpp"
#include "trajectory/Lambert.hpp"

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
    double best_total_time = hohmann_wait_s + hohmann_time_s;
    double best_dv = hohmann_dv_dep + hohmann_dv_arr;
    bool found_feasible = ship.propellant_kg >= propellant_required_kg(ship, ship_class, best_dv);
    bool best_from_lambert = false;
    math::Vec3d best_r1_pos{}, best_r2_pos{}, best_v1_lambert{};

    // Lambert grid search: N_DEP departure offsets × N_TRANSIT transit fractions.
    // Minimise total_time = wait + transit, subject to propellant feasibility.
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

            const double vc2 = std::sqrt(mu / r2_m_k);
            const math::Vec3d v_circ2{-r2_pos.z / r2_m_k * vc2, 0.0, r2_pos.x / r2_m_k * vc2};
            const double dv_dep_k = (lam.v1 - v_circ1).length();
            const double dv_arr_k = (lam.v2 - v_circ2).length();
            const double dv_k = dv_dep_k + dv_arr_k;
            const double total_k = wait_k + transit_k;
            const bool feas_k = ship.propellant_kg >= propellant_required_kg(ship, ship_class, dv_k);

            if (feas_k && total_k < best_total_time) {
                best_total_time = total_k;
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

    constexpr int kSamples = 48;
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
        const auto mean_anom = [&](double theta) {
            if (elliptic) {
                const double E = std::atan2(std::sqrt(1.0 - ecc * ecc) * std::sin(theta), ecc + std::cos(theta));
                return E - ecc * std::sin(E);
            }
            // Open orbits never pass apoapsis, so the arc lies within (−π, π).
            const double half = 0.5 * std::remainder(theta, TAU);
            if (hyperbolic) {
                const double H = 2.0 * std::atanh(std::sqrt((ecc - 1.0) / (ecc + 1.0)) * std::tan(half));
                return ecc * std::sinh(H) - H;
            }
            const double D = std::tan(half);
            return D + D * D * D / 3.0;
        };
        const double M1 = mean_anom(theta1);
        double dM_total = mean_anom(theta2) - M1;
        if (elliptic && dM_total < -1e-10) dM_total += TAU;
        if (dM_total < 1e-12) dM_total = TAU;  // degenerate guard

        for (int i = 0; i < kSamples; ++i) {
            const double alpha = static_cast<double>(i) / (kSamples - 1);
            const double theta = theta1 + (theta2 - theta1) * alpha;
            const double denominator = 1.0 + ecc * std::cos(theta);
            const double r_at = (p_orb > 1.0 && denominator > 1e-9) ? p_orb / denominator : r1m;
            plan.sampled_path.push_back({std::cos(omega + theta) * r_at, 0.0, std::sin(omega + theta) * r_at});
            double dM_i = mean_anom(theta) - M1;
            if (elliptic && dM_i < -1e-10) dM_i += TAU;
            plan.sampled_times_s.push_back(plan.departure_time_s + dM_i / dM_total * plan.coast_time_s);
        }
    } else {
        // Hohmann fallback path: angle sweep using reference radii.
        const double eccentricity = std::abs(r2 - r1) / std::max(r1 + r2, 1.0);
        const double parameter = transfer_axis * (1.0 - eccentricity * eccentricity);
        const bool outward = r2 >= r1;
        for (int i = 0; i < kSamples; ++i) {
            const double alpha = static_cast<double>(i) / (kSamples - 1);
            const double anomaly = outward ? alpha * PI : PI - alpha * PI;
            const double radius = eccentricity > 1.0e-9
                ? parameter / (1.0 + eccentricity * std::cos(anomaly))
                : r1;
            const double angle = start_angle + PI * alpha;
            plan.sampled_path.push_back({std::cos(angle) * radius, start.y * (1.0 - alpha) + finish.y * alpha, std::sin(angle) * radius});
            plan.sampled_times_s.push_back(plan.departure_time_s + alpha * plan.coast_time_s);
        }
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
    std::vector<double> arc_propellant(kSamples);
    {
        const double ve = effective_exhaust_velocity_mps(ship_class);
        const double m0 = ship_class.dry_mass_kg + ship.propellant_kg;
        const double prop_dep = (ve > 0.0) ? m0 * (1.0 - std::exp(-best_dv_dep / ve)) : 0.0;
        const double m1 = m0 - prop_dep;
        const double prop_arr = (ve > 0.0 && m1 > ship_class.dry_mass_kg)
            ? m1 * (1.0 - std::exp(-best_dv_arr / ve)) : 0.0;
        const double propellant_coast = std::max(0.0, ship.propellant_kg - prop_dep);
        arc_propellant.front() = ship.propellant_kg;
        for (int i = 1; i < kSamples - 1; ++i)
            arc_propellant[i] = propellant_coast;
        arc_propellant.back() = std::max(0.0, propellant_coast - prop_arr);
    }

    // Prepend wait-period samples so the rendered path tracks the origin station during the wait.
    // Without these, the frontend sees a gap between the ship's current position and path[0]
    // (the departure position) and draws a straight chord, which looks wrong visually.
    // Waits can last several origin orbits (up to ~2 years), so sample by the origin's
    // heliocentric sweep rather than a fixed count; a fixed 11 samples drew long waits as
    // star polygons around the Sun.
    if (plan.wait_time_s > 60.0) {
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
            wait_path.push_back(mechanics_.get_station_position(origin, t));
            wait_times.push_back(t);
            wait_propellant.push_back(ship.propellant_kg);
        }
        wait_path.insert(wait_path.end(), plan.sampled_path.begin(), plan.sampled_path.end());
        wait_times.insert(wait_times.end(), plan.sampled_times_s.begin(), plan.sampled_times_s.end());
        wait_propellant.insert(wait_propellant.end(), arc_propellant.begin(), arc_propellant.end());
        plan.sampled_path = std::move(wait_path);
        plan.sampled_times_s = std::move(wait_times);
        plan.sampled_propellant_kg = std::move(wait_propellant);
    } else {
        plan.sampled_propellant_kg = std::move(arc_propellant);
    }

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
