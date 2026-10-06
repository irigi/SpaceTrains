#include "trajectory/VariableIspTrajectoryPlanner.hpp"
#include "trajectory/PathSampling.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <utility>
#include <vector>
#include <stdexcept>

namespace spacetrains::trajectory {

namespace {

constexpr double TAU = 6.28318530717958647692;

// Returns angle in (-pi, pi].
double normalize_angle(double angle_rad) {
    constexpr double PI = 3.14159265358979323846;
    angle_rad = std::fmod(angle_rad, TAU);
    if (angle_rad <= -PI) {
        angle_rad += TAU;
    } else if (angle_rad > PI) {
        angle_rad -= TAU;
    }
    return angle_rad;
}

// Modulo that always returns a non-negative value.
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

// Heliocentric angular rate for a body: walks the parent chain until finding a
// direct child of root_id, then returns TAU / that body's orbital period.
// Matches the same helper in TrajectoryPlanner.cpp — needed so moon-station routes
// use the parent planet's heliocentric rate, not the moon's shorter local rate.
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
    return current->orbit.orbital_period_s > 0.0 ? TAU / current->orbit.orbital_period_s : 0.0;
}

// Canonical kappa scale factor: (1 AU)^2.5 / mu_sun^1.5
// Precomputed constant — same value as in generate_atlas.py KAPPA_SCALE_FACTOR.
double kappa_scale_factor() {
    using VI = variable_isp::VariableIspIntegrator;
    const double au = VI::kAstronomicalUnitM;
    const double mu = VI::kMuSunSI;
    return std::pow(au, 2.5) / std::pow(mu, 1.5);
}

// Binary search on a sorted vector; returns the lower grid index for interpolation.
std::size_t lower_grid_idx(const std::vector<double>& grid, double value) {
    if (value <= grid.front()) return 0;
    if (value >= grid.back()) return grid.size() - 2;
    const auto upper = std::lower_bound(grid.begin(), grid.end(), value);
    const auto idx = static_cast<std::size_t>(std::distance(grid.begin(), upper));
    return (*upper == value) ? std::min(idx, grid.size() - 2) : idx - 1;
}

}  // namespace

VariableIspTrajectoryPlanner::VariableIspTrajectoryPlanner(
    const domain::UniverseDefinition& universe,
    const celestial::CelestialMechanics& mechanics,
    const variable_isp::VariableIspAtlas& atlas)
    : universe_(universe), mechanics_(mechanics), atlas_(atlas) {
    for (const auto& body : universe_.bodies) {
        bodies_by_id_[body.id] = &body;
    }
}

domain::TrajectoryPlan VariableIspTrajectoryPlanner::plan_transfer(
    const domain::StationDefinition& origin,
    const domain::StationDefinition& destination,
    const domain::ShipState& ship,
    const domain::ShipClassDefinition& ship_class,
    double current_time_s) const {

    domain::TrajectoryPlan plan;

    // Same-parent transfers are not handled by VariableISP planner (ion ships
    // currently cannot fly them; there is no fallback in the simulation).
    if (origin.parent_body_id == destination.parent_body_id) {
        return plan;
    }

    const auto* origin_body_ptr = bodies_by_id_.at(origin.parent_body_id);
    const auto* dest_body_ptr = bodies_by_id_.at(destination.parent_body_id);

    // Heliocentric radii at current time (circular orbit assumption).
    const double r_origin_m = mechanics_.get_heliocentric_radius(origin.parent_body_id, current_time_s);
    const double r_dest_m = mechanics_.get_heliocentric_radius(destination.parent_body_id, current_time_s);
    if (r_origin_m <= 0.0 || r_dest_m <= 0.0) {
        return plan;
    }

    const double rho_raw = r_dest_m / r_origin_m;

    const auto& rho_grid = atlas_.rho_grid();
    const auto& kappa_grid = atlas_.kappa_grid();
    const auto& theta_grid = atlas_.theta_grid();

    const double rho = std::clamp(rho_raw, rho_grid.front(), rho_grid.back());

    // Canonical kappa — always uses r0 = 1 AU as reference.
    const double m_dry = ship_class.dry_mass_kg;
    const double m0 = m_dry + std::max(0.0, ship.propellant_kg);
    const double power_w = ship_class.specific_engine_power_w_per_kg * m_dry;
    if (power_w <= 0.0 || m0 <= m_dry) {
        return plan;
    }
    const double kappa = 2.0 * power_w * (1.0 / m_dry - 1.0 / m0) * kappa_scale_factor();
    if (kappa < kappa_grid.front() || kappa > kappa_grid.back()) {
        return plan;
    }

    // Planet angles and angular rates — use heliocentric rates so moon stations
    // use the parent planet's period rather than the moon's shorter local period.
    const std::string root_id = mechanics_.get_root_body_id();
    const double omega_origin = heliocentric_orbital_rate_rad_s(*origin_body_ptr, root_id, bodies_by_id_);
    const double omega_dest = heliocentric_orbital_rate_rad_s(*dest_body_ptr, root_id, bodies_by_id_);

    const auto origin_pos = mechanics_.get_body_position(origin.parent_body_id, current_time_s);
    const auto dest_pos = mechanics_.get_body_position(destination.parent_body_id, current_time_s);
    const double phi_origin = std::atan2(origin_pos.z, origin_pos.x);
    const double phi_dest = std::atan2(dest_pos.z, dest_pos.x);
    const double current_delta_phi = phi_dest - phi_origin;

    const double relative_rate = omega_dest - omega_origin;
    const double synodic_period_s = std::abs(relative_rate) > 1.0e-15
        ? TAU / std::abs(relative_rate) : 1.0e30;

    // Similarity scaling from canonical (1 AU) to actual origin radius.
    const double r_scale = r_origin_m / variable_isp::VariableIspIntegrator::kAstronomicalUnitM;
    const double t_scale = std::pow(r_scale, 1.5);

    // Grid indices for (rho, kappa) — used for direct cell access in the hot loop.
    // We look at the 2×2 rho×kappa neighborhood so we stay near the right solution family.
    const std::size_t i0 = lower_grid_idx(rho_grid, rho);
    const std::size_t i1 = std::min(i0 + 1, rho_grid.size() - 1);
    const std::size_t j0 = lower_grid_idx(kappa_grid, kappa);
    const std::size_t j1 = std::min(j0 + 1, kappa_grid.size() - 1);

    // Solved cells of the 2×2 rho×kappa neighborhood at theta index k, closest first.
    // Closeness in rho matters most: the path depends on the costates only, and
    // kappa merely decides whether the fuel suffices.
    const auto neighborhood_cells = [&](std::size_t k) {
        std::vector<std::pair<std::size_t, std::size_t>> cells;
        for (auto ii : {i0, i1}) {
            for (auto jj : {j0, j1}) {
                if (atlas_.is_solved(ii, jj, k)
                    && std::find(cells.begin(), cells.end(), std::pair{ii, jj}) == cells.end()) {
                    cells.emplace_back(ii, jj);
                }
            }
        }
        const auto distance = [&](const std::pair<std::size_t, std::size_t>& cell) {
            return 10.0 * std::abs(std::log(rho_grid[cell.first] / rho))
                + std::abs(std::log(kappa_grid[cell.second] / kappa));
        };
        std::sort(cells.begin(), cells.end(), [&](const auto& a, const auto& b) { return distance(a) < distance(b); });
        return cells;
    };

    // Rank launch windows by total trip time (wait + transfer) over the theta grid,
    // using the nearest solved cell's transfer time as the estimate. O(1) per cell.
    struct Window {
        double total_s;
        std::size_t k;
    };
    std::vector<Window> windows;
    for (std::size_t k = 0; k < theta_grid.size(); ++k) {
        const auto cells = neighborhood_cells(k);
        if (cells.empty()) {
            continue;
        }
        const auto seed = atlas_.seed_at(cells.front().first, cells.front().second, k);
        if (seed.transfer_time_days <= 0.0) {
            continue;
        }

        const double theta_f = theta_grid[k];
        const double T_transfer_s = seed.transfer_time_days
            * variable_isp::VariableIspIntegrator::kDayS * t_scale;

        // Phase condition: dest must be at (phi_origin_at_depart + theta_f) at arrival.
        // phi_origin(t_depart) + theta_f = phi_dest(t_arrive)
        const double required_delta_phi = theta_f - omega_dest * T_transfer_s;
        double wait_s = 0.0;
        if (std::abs(relative_rate) > 1.0e-15) {
            wait_s = positive_mod(
                (required_delta_phi - current_delta_phi) / relative_rate,
                synodic_period_s);
        } else {
            if (std::abs(normalize_angle(required_delta_phi - current_delta_phi)) > 0.05) {
                continue;
            }
        }
        windows.push_back({wait_s + T_transfer_s, k});
    }
    std::sort(windows.begin(), windows.end(), [](const Window& a, const Window& b) { return a.total_s < b.total_s; });

    // Atlas cells are solved for their own grid (rho, theta), not for this request,
    // so an unrefined seed misses the destination (by up to ~0.3 AU) and blending
    // corners can mix solution branches. Refine a real cell's seed onto the exact
    // target, and fall through to the next window if it does not converge or the
    // corrected path needs more fuel than the ship carries.
    constexpr std::size_t kMaxWindows = 4;
    constexpr std::size_t kMaxSeedsPerWindow = 2;
    // ODE mass can undershoot dry mass by ~0.1-0.2% on max-burn arcs.
    constexpr double kDryMassSlack = 0.998;
    const double r_target_canonical = rho * variable_isp::VariableIspIntegrator::kCanonicalR0SI;
    const variable_isp::CanonicalMissionConfig config =
        variable_isp::VariableIspIntegrator::canonical_config(rho, kappa);

    variable_isp::AtlasSeed best_seed;
    variable_isp::IntegrationSummary result;
    double best_theta_f = 0.0;
    double best_wait_s = 0.0;
    double best_r_scale = r_scale;
    double best_t_scale = t_scale;
    double best_phi_depart = 0.0;
    std::size_t refine_iterations = 0;
    std::size_t windows_tried = 0;
    bool found = false;
    bool fuel_limited = false;
    for (const auto& window : windows) {
        if (found || windows_tried >= kMaxWindows) {
            break;
        }
        ++windows_tried;
        const auto cells = neighborhood_cells(window.k);
        for (std::size_t c = 0; c < std::min(cells.size(), kMaxSeedsPerWindow) && !found; ++c) {
            const auto seed = atlas_.seed_at(cells[c].first, cells[c].second, window.k);
            try {
                // Keep the branch the seed already flies; its grid label is only valid mod 2π.
                const auto start = integrator_.integrate_fixed_time(seed, config, 2);
                const double theta_seed = start.samples.back().theta_rad;
                const double theta_goal = theta_seed + std::remainder(theta_grid[window.k] - theta_seed, TAU);
                auto refined = integrator_.refine_seed(seed, config, r_target_canonical, theta_goal);
                if (!refined.converged) {
                    continue;
                }
                // Choose the launch wait from the refined angle and transfer time (circular
                // phase model). Co-orbiting bodies (e.g. Earth and the Moon) have no wait to
                // absorb a phase mismatch; the targeting passes below fix it instead.
                double wait_s = 0.0;
                if (std::abs(relative_rate) > 1.0e-15) {
                    const double T_s = refined.seed.transfer_time_days
                        * variable_isp::VariableIspIntegrator::kDayS * t_scale;
                    wait_s = positive_mod(
                        (theta_goal - omega_dest * T_s - current_delta_phi) / relative_rate, synodic_period_s);
                }
                // With departure fixed, aim the shooting at the destination *station's* actual
                // position at departure + T (moon offsets, phase slack). The target moves with T,
                // and the ship arrives co-moving with it, so it must sit inside the residual —
                // an outer fixed-point loop over T does not contract.
                // Likewise start from the origin station's actual position at departure:
                // it sets the similarity scale and the frame rotation.
                const auto origin_at_departure = mechanics_.get_station_position(origin, current_time_s + wait_s);
                const double depart_r_scale = std::hypot(origin_at_departure.x, origin_at_departure.z)
                    / variable_isp::VariableIspIntegrator::kAstronomicalUnitM;
                const double depart_t_scale = std::pow(depart_r_scale, 1.5);
                const double phi_depart = std::atan2(origin_at_departure.z, origin_at_departure.x);
                const double theta_branch = theta_goal;
                const auto station_target = [&](double transfer_time_days) {
                    const double arrival_s = current_time_s + wait_s
                        + transfer_time_days * variable_isp::VariableIspIntegrator::kDayS * depart_t_scale;
                    const auto station = mechanics_.get_station_position(destination, arrival_s);
                    return std::pair {
                        std::hypot(station.x, station.z) / depart_r_scale,
                        theta_branch + std::remainder(std::atan2(station.z, station.x) - phi_depart - theta_branch, TAU),
                    };
                };
                refined = integrator_.refine_seed(refined.seed, config, station_target);
                if (!refined.converged) {
                    continue;
                }
                best_wait_s = wait_s;
                best_r_scale = depart_r_scale;
                best_t_scale = depart_t_scale;
                best_phi_depart = phi_depart;
                // Integrate the winning canonical trajectory. We use many internal samples so
                // that arc-length resampling (below) has enough source points to represent tight
                // solar passes accurately.
                // Dense enough that even a 0.1 AU perihelion on a multi-year transfer turns
                // only a few degrees per sample before thinning.
                auto full = integrator_.integrate_fixed_time(refined.seed, config, 4000);
                if (full.samples.back().mass_kg < kDryMassSlack * variable_isp::VariableIspIntegrator::kCanonicalDryMassKg) {
                    fuel_limited = true;
                    continue;
                }
                const auto closest = std::min_element(full.samples.begin(), full.samples.end(),
                    [](const auto& a, const auto& b) { return a.r_m < b.r_m; });
                if (closest->r_m * depart_r_scale < kMinPerihelionM) {
                    continue;  // time-optimal ion arcs like to dive sunward; keep them survivable
                }
                best_seed = refined.seed;
                result = std::move(full);
                best_theta_f = theta_grid[window.k];
                refine_iterations = refined.iterations;
                found = true;
            } catch (const std::runtime_error&) {
                // Seed dives into the Sun or similar: try the next seed/window.
            }
        }
    }

    if (!found) {
        plan.summary = std::format(
            "VariableISP {} -> {} no converged window ({} tried{})",
            origin.name, destination.name, windows_tried, fuel_limited ? ", fuel-limited" : "");
        return plan;
    }

    // Thin the dense integration by curvature for rendering: tight perihelion passes keep
    // many points, long flat spirals few (see PathSampling.hpp).
    std::vector<variable_isp::TrajectorySample> samples;
    {
        std::vector<math::Vec3d> dense;
        dense.reserve(result.samples.size());
        for (const auto& sample : result.samples) {
            dense.push_back({sample.r_m * std::cos(sample.theta_rad), 0.0, sample.r_m * std::sin(sample.theta_rad)});
        }
        for (const auto index : select_for_rendering(dense)) {
            samples.push_back(result.samples[index]);
        }
    }

    const double actual_theta = samples.back().theta_rad;
    const double T_interp_s = best_seed.transfer_time_days
        * variable_isp::VariableIspIntegrator::kDayS * best_t_scale;
    const double corrected_wait_s = best_wait_s;

    // Scale canonical trajectory to real coordinates and rotate to heliocentric frame.
    const double phi_origin_at_depart = best_phi_depart;
    const double departure_time_s = current_time_s + corrected_wait_s;

    plan.sampled_path.reserve(samples.size());
    plan.sampled_times_s.reserve(samples.size());
    for (const auto& sample : samples) {
        const double r_real = sample.r_m * best_r_scale;
        const double angle_real = sample.theta_rad + phi_origin_at_depart;
        const double t_real = departure_time_s + sample.time_s * best_t_scale;
        plan.sampled_path.push_back({
            std::cos(angle_real) * r_real,
            0.0,
            std::sin(angle_real) * r_real,
        });
        plan.sampled_times_s.push_back(t_real);
    }

    // Propellant cost via I = 1/m_f - 1/m0 invariant (integral of a²/(2P) dt).
    // I scales as P_canonical/P_real = delta_inv_real/delta_inv_canonical,
    // where delta_inv = 1/m_dry - 1/m0 encodes the ship's kappa fuel fraction.
    // Clamp m_final_canonical to m_dry_canonical: the ODE solver doesn't enforce
    // a mass floor, so trajectories at maximum burn can underflow by ~0.1-0.2%.
    const double m0_canonical = variable_isp::VariableIspIntegrator::kCanonicalM0Kg;
    const double m_dry_canonical = variable_isp::VariableIspIntegrator::kCanonicalDryMassKg;
    const double m_final_canonical = std::max(samples.back().mass_kg, m_dry_canonical);
    const double delta_inv_canonical = 1.0 / m_dry_canonical - 1.0 / m0_canonical;
    const double delta_inv_real = 1.0 / m_dry - 1.0 / m0;
    const double I_canonical = 1.0 / m_final_canonical - 1.0 / m0_canonical;
    const double I_real = I_canonical * (delta_inv_real / delta_inv_canonical);
    const double m_f_real = 1.0 / (I_real + 1.0 / m0);
    plan.propellant_required_kg = m0 - m_f_real;
    plan.feasible = ship.propellant_kg >= plan.propellant_required_kg;

    // Per-sample propellant via same I-invariant scaling — used for continuous
    // propellant display during transit.
    plan.sampled_propellant_kg.reserve(samples.size());
    for (const auto& sample : samples) {
        const double m_s_can = std::max(sample.mass_kg, m_dry_canonical);
        const double I_s = 1.0 / m_s_can - 1.0 / m0_canonical;
        const double I_s_real = I_s * (delta_inv_real / delta_inv_canonical);
        const double m_s_real = 1.0 / (I_s_real + 1.0 / m0);
        plan.sampled_propellant_kg.push_back(std::max(0.0, m_s_real - m_dry));
    }

    plan.departure_time_s = departure_time_s;
    plan.arrival_time_s = departure_time_s + T_interp_s;
    plan.wait_time_s = corrected_wait_s;
    plan.coast_time_s = T_interp_s;
    plan.travel_time_s = corrected_wait_s + T_interp_s;
    plan.trajectory_type = "variable_isp";
    plan.summary = std::format(
        "VariableISP {} -> {} rho={:.3f} kappa={:.2f} theta_f={:.3f} wait={:.1f}d transfer={:.1f}d propellant={:.0f}kg ({})",
        origin.name,
        destination.name,
        rho,
        kappa,
        best_theta_f,
        corrected_wait_s / variable_isp::VariableIspIntegrator::kDayS,
        T_interp_s / variable_isp::VariableIspIntegrator::kDayS,
        plan.propellant_required_kg,
        plan.feasible ? "feasible" : "insufficient fuel");

    // Snap endpoints to actual station positions. Atlas bilinear interpolation doesn't
    // perfectly hit the target radius, which causes the trajectory to end some distance
    // away from the destination body. Snapping eliminates the visual teleport on arrival.
    const auto origin_station_pos = mechanics_.get_station_position(origin, departure_time_s);
    const auto dest_station_pos = mechanics_.get_station_position(destination, plan.arrival_time_s);
    plan.diagnostics = {
        .start_miss_m = (plan.sampled_path.front() - origin_station_pos).length(),
        .endpoint_miss_m = (plan.sampled_path.back() - dest_station_pos).length(),
        .rho = rho,
        .kappa = kappa,
        .theta_target_rad = best_theta_f,
        .theta_actual_rad = actual_theta,
        .r_end_canonical_ratio = samples.back().r_m
            / (rho * variable_isp::VariableIspIntegrator::kCanonicalR0SI),
        .seed_source = "refined",
        .refine_iterations = refine_iterations,
        .windows_tried = windows_tried,
    };
    plan.sampled_path.front() = origin_station_pos;
    plan.sampled_path.back() = dest_station_pos;

    return plan;
}

}  // namespace spacetrains::trajectory
