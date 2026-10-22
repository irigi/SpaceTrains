#include "trajectory/VariableIspTrajectoryPlanner.hpp"

#include "trajectory/PathSampling.hpp"
#include "variable_isp/GeneralTransfer.hpp"
#include "util/Profiling.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
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
    double current_time_s,
    const PlanningOptions& options) const {
    const profiling::Scope profile_scope(profiling::Phase::VariableIspPlan);
    domain::TrajectoryPlan plan;

    // Mass model. The engine's power comes with the hull (alpha * hull dry mass); cargo and
    // provisions only add mass. A transfer may burn a budget B and departs with
    // L = max(aboard, (1 + reserve) B), up to what the ship can load.
    const double reserve = std::max(0.0, options.reserve_fraction);
    const double aboard_kg = std::max(0.0, ship.propellant_kg);
    const double max_load_kg = std::max(aboard_kg, std::min(ship_class.propellant_capacity_kg,
        aboard_kg + std::max(0.0, options.purchasable_propellant_kg)));
    const double laden_dry_kg = ship_class.dry_mass_kg + std::max(0.0, options.payload_kg);
    const double power_w = ship_class.specific_engine_power_w_per_kg * ship_class.dry_mass_kg;
    const double max_budget_kg = max_load_kg / (1.0 + reserve);
    const auto load_for_budget = [&](double budget_kg) { return std::max(aboard_kg, (1.0 + reserve) * budget_kg); };

    // Same-parent transfers are not handled by VariableISP planner (plasma ships
    // currently cannot fly them; there is no fallback in the simulation).
    if (origin.parent_body_id == destination.parent_body_id) {
        return plan;
    }

    // Different bodies of one planetary system (Earth <-> Moon): a low-thrust spiral
    // around the planet instead of a heliocentric arc. Constant acceleration over T with
    // Edelbaum's Δv between the two circular orbits. The shortest T the fuel load allows
    // has I = 1/m_f - 1/m0 = Δv²/(2PT) = 1/m_dry - 1/m0; a longer spiral burns less
    // (the burn falls as 1/T), so with the owner's costs T is the cheapest of a few
    // multiples of the shortest, otherwise 1.5x.
    if (const auto primary_id = planet_system_primary(mechanics_, origin.parent_body_id);
        primary_id == planet_system_primary(mechanics_, destination.parent_body_id)) {
        const double mu_p = mechanics_.get_body(primary_id).mu_m3_s2;
        const auto radius_around_primary = [&](const domain::StationDefinition& station) {
            const auto v = mechanics_.get_station_position(station, current_time_s)
                - mechanics_.get_body_position(primary_id, current_time_s);
            return std::max(1.0, std::hypot(v.x, v.z));
        };
        const double delta_v = std::abs(std::sqrt(mu_p / radius_around_primary(origin))
            - std::sqrt(mu_p / radius_around_primary(destination)));
        if (power_w <= 0.0 || max_budget_kg <= 0.0) {
            return plan;
        }
        const double max_m0 = laden_dry_kg + load_for_budget(max_budget_kg);
        const double min_transfer_s = delta_v * delta_v
            / (2.0 * power_w * (1.0 / (max_m0 - max_budget_kg) - 1.0 / max_m0));
        // Load only what a spiral burns plus the reserve (one refinement pass: the
        // lighter ship burns less, so the reserve still holds).
        const auto spiral_load_kg = [&](double integral) {
            const double start = laden_dry_kg + max_load_kg;
            return std::min(max_load_kg, load_for_budget(start - 1.0 / (integral + 1.0 / start)));
        };
        const bool by_cost = options.propellant_cr_per_kg > 0.0 || options.time_cr_per_day > 0.0;
        double transfer_s = std::max(86400.0, 1.5 * min_transfer_s);
        if (by_cost) {
            double best_cost = std::numeric_limits<double>::infinity();
            for (const double factor : {1.5, 2.0, 3.0, 4.5, 6.5, 10.0}) {
                const double candidate_s = std::max(86400.0, factor * min_transfer_s);
                const double integral = delta_v * delta_v / (2.0 * power_w * candidate_s);
                const double start = laden_dry_kg + spiral_load_kg(integral);
                const double burn_kg = start - 1.0 / (integral + 1.0 / start);
                const double cost = burn_kg * options.propellant_cr_per_kg + candidate_s / 86400.0 * options.time_cr_per_day;
                if (cost < best_cost) {
                    best_cost = cost;
                    transfer_s = candidate_s;
                }
            }
        }
        const double integral = delta_v * delta_v / (2.0 * power_w * transfer_s);
        const double load_kg = spiral_load_kg(integral);
        const double m_dry = laden_dry_kg;
        const double m0 = laden_dry_kg + load_kg;
        const double m_final = 1.0 / (integral + 1.0 / m0);

        plan.departure_time_s = current_time_s;
        plan.wait_time_s = 0.0;
        plan.coast_time_s = transfer_s;
        plan.arrival_time_s = current_time_s + transfer_s;
        plan.travel_time_s = transfer_s;
        plan.propellant_required_kg = m0 - m_final;
        plan.propellant_load_kg = load_kg;
        plan.feasible = plan.propellant_required_kg * (1.0 + reserve) <= load_kg * (1.0 + 1e-9);
        plan.trajectory_type = "variable_isp_planet_system";
        auto timed = planet_system_path(mechanics_, primary_id, origin, destination, current_time_s, transfer_s, false);
        plan.sampled_path = std::move(timed.path);
        plan.sampled_times_s = std::move(timed.times_s);
        for (const double t : plan.sampled_times_s) {
            const double fraction = (t - current_time_s) / transfer_s;
            // Constant acceleration: 1/m grows linearly with time.
            const double m = 1.0 / (1.0 / m0 + integral * fraction);
            plan.sampled_propellant_kg.push_back(std::max(0.0, m - m_dry));
        }
        plan.summary = std::format(
            "VariableISP planet-system spiral {} -> {} in {:.1f} days, delta-v {:.0f} m/s, propellant {:.0f} kg",
            origin.name, destination.name, transfer_s / 86400.0, delta_v, plan.propellant_required_kg);
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

    if (power_w <= 0.0 || max_budget_kg <= 0.0) {
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

    // Grid indices for rho — used for direct cell access in the hot loop.
    // We look at the 2×2 rho×kappa neighborhood so we stay near the right solution family.
    const std::size_t i0 = lower_grid_idx(rho_grid, rho);
    const std::size_t i1 = std::min(i0 + 1, rho_grid.size() - 1);

    // Solved cells of the 2×2 rho×kappa neighborhood at theta index k, closest first.
    // Closeness in rho matters most: the path depends on the costates only, and
    // kappa merely decides whether the fuel suffices.
    const auto neighborhood_cells = [&](double kappa, std::size_t k) {
        const std::size_t j0 = lower_grid_idx(kappa_grid, kappa);
        const std::size_t j1 = std::min(j0 + 1, kappa_grid.size() - 1);
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
    const auto rank_windows = [&](double kappa) {
        std::vector<Window> ranked;
        for (std::size_t k = 0; k < theta_grid.size(); ++k) {
            const auto cells = neighborhood_cells(kappa, k);
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
            ranked.push_back({wait_s + T_transfer_s, k});
        }
        std::sort(ranked.begin(), ranked.end(), [](const Window& a, const Window& b) { return a.total_s < b.total_s; });
        return ranked;
    };

    // Fuel budget. A larger budget (higher kappa) flies faster; with the owner's costs
    // known, try a few budgets and refine the cheapest by the atlas estimate first: fuel
    // cost plus time cost of its best window.
    // The atlas is solved at r0 = 1 AU and scaled to the origin's radius (lengths by
    // r_scale, times by r_scale^1.5, accelerations by r_scale^-2), so a transfer from
    // radius r needs r_scale^-2.5 of the canonical integral of a^2 dt: the ship's kappa
    // counts r_scale^2.5 times. With that kappa the canonical ship shares the real one's
    // dimensionless kappa, and the propellant formula below holds unchanged. (Before v33
    // kappa was taken at 1 AU for every origin: a ship leaving Saturn was planned 280x too
    // weak and could not get home, one leaving Mercury 10x too strong.)
    const double kappa_origin_factor = std::pow(r_scale, 2.5);
    struct Budget {
        double burn_kg;
        double load_kg;
        double m0;
        double m_floor;  // laden dry mass plus the unburned reserve
        double kappa;
        std::vector<Window> windows;
        double estimated_cost;
    };
    const bool by_cost = options.propellant_cr_per_kg > 0.0 || options.time_cr_per_day > 0.0;
    std::vector<Budget> budgets;
    for (const double fraction : by_cost ? std::vector<double>{1.0, 0.75, 0.55, 0.4} : std::vector<double>{1.0}) {
        const double burn_kg = max_budget_kg * fraction;
        const double load_kg = load_for_budget(burn_kg);
        const double m0 = laden_dry_kg + load_kg;
        const double m_floor = m0 - burn_kg;
        const double kappa = 2.0 * power_w * (1.0 / m_floor - 1.0 / m0) * kappa_scale_factor() * kappa_origin_factor;
        if (kappa < kappa_grid.front() || kappa > kappa_grid.back()) {
            continue;
        }
        auto ranked = rank_windows(kappa);
        if (ranked.empty()) {
            continue;
        }
        const double estimated_cost = burn_kg * options.propellant_cr_per_kg
            + ranked.front().total_s / 86400.0 * options.time_cr_per_day;
        budgets.push_back({burn_kg, load_kg, m0, m_floor, kappa, std::move(ranked), estimated_cost});
    }
    std::stable_sort(budgets.begin(), budgets.end(),
        [](const Budget& a, const Budget& b) { return a.estimated_cost < b.estimated_cost; });

    // Atlas cells are solved for their own grid (rho, theta), not for this request,
    // so an unrefined seed misses the destination (by up to ~0.3 AU) and blending
    // corners can mix solution branches. Refine a real cell's seed onto the exact
    // target, and fall through to the next window if it does not converge or the
    // corrected path needs more fuel than the ship carries.
    constexpr std::size_t kMaxWindows = 4;
    constexpr std::size_t kMaxSeedsPerWindow = 2;
    // A refined window that just passed is flown now if the phase error is at most this.
    constexpr double kMaxPhaseSlackRad = 0.3;
    // ODE mass can undershoot dry mass by ~0.1-0.2% on max-burn arcs.
    constexpr double kDryMassSlack = 0.998;

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
    const Budget* chosen = nullptr;
    // Refining is the expensive part: try the two cheapest budgets at most.
    constexpr std::size_t kMaxBudgets = 2;
    for (std::size_t b = 0; b < std::min(budgets.size(), kMaxBudgets) && !found; ++b) {
        const auto& budget = budgets[b];
        const double kappa = budget.kappa;
        const variable_isp::CanonicalMissionConfig config =
            variable_isp::VariableIspIntegrator::canonical_config(rho, kappa);
        std::size_t budget_windows_tried = 0;
        for (const auto& window : budget.windows) {
            if (found || budget_windows_tried >= kMaxWindows) {
                break;
            }
            ++budget_windows_tried;
            ++windows_tried;
            const auto cells = neighborhood_cells(kappa, window.k);
            for (std::size_t c = 0; c < std::min(cells.size(), kMaxSeedsPerWindow) && !found; ++c) {
                const auto seed = atlas_.seed_at(cells[c].first, cells[c].second, window.k);
                try {
                    // The grid label is only valid mod 2π; the shooting keeps the branch the seed
                    // flies, and the launch wait below does not depend on whole turns.
                    const double theta_goal = theta_grid[window.k];
                    variable_isp::ShootingSettings shooting;
                    shooting.min_radius_m = kMinPerihelionM / r_scale;
                    shooting.fit_fuel_budget = true;
                    // The station pass below starts from the seed itself: a first pass onto the
                    // cell's own (rho, theta) target only moved the window a little and cost a
                    // second shooting (dropped in v35: 2% more plans found, 10% faster).
                    variable_isp::ShootingResult refined;
                    refined.seed = seed;
                    // Choose the launch wait from the seed's angle and transfer time (circular
                    // phase model). Co-orbiting bodies (e.g. Earth and the Moon) have no wait to
                    // absorb a phase mismatch; the targeting passes below fix it instead.
                    double phase_wait_s = 0.0;
                    if (std::abs(relative_rate) > 1.0e-15) {
                        const double T_s = refined.seed.transfer_time_days
                            * variable_isp::VariableIspIntegrator::kDayS * t_scale;
                        phase_wait_s = positive_mod(
                            (theta_goal - omega_dest * T_s - current_delta_phi) / relative_rate, synodic_period_s);
                    }
                    // Refining moves the window a little; if it moved just into the past, the
                    // phase wait wraps to almost a whole synodic period (7.5 years from Jupiter to
                    // Ceres). Leave now instead and let the station targeting absorb the small
                    // phase error; wait for the next window only if that does not converge.
                    std::vector<double> waits {phase_wait_s};
                    if ((synodic_period_s - phase_wait_s) * std::abs(relative_rate) < kMaxPhaseSlackRad) {
                        waits.insert(waits.begin(), 0.0);
                    }
                    // With departure fixed, aim the shooting at the destination *station's* actual
                    // position at departure + T (moon offsets, phase slack). The target moves with T,
                    // and the ship arrives co-moving with it, so it must sit inside the residual —
                    // an outer fixed-point loop over T does not contract.
                    // Likewise start from the origin station's actual position at departure:
                    // it sets the similarity scale and the frame rotation.
                    const auto departure_after = [&](double wait) {
                        const auto position = mechanics_.get_station_position(origin, current_time_s + wait);
                        const double r = std::hypot(position.x, position.z) / variable_isp::VariableIspIntegrator::kAstronomicalUnitM;
                        return Departure {wait, r, std::pow(r, 1.5), std::atan2(position.z, position.x)};
                    };
                    const double theta_branch = theta_goal;
                    const auto station_target_for = [&](const Departure& departure) {
                        return [&, departure](double transfer_time_days) {
                            const double arrival_s = current_time_s + departure.wait_s
                                + transfer_time_days * variable_isp::VariableIspIntegrator::kDayS * departure.t_scale;
                            const auto station = mechanics_.get_station_position(destination, arrival_s);
                            return std::pair {
                                std::hypot(station.x, station.z) / departure.r_scale,
                                theta_branch + std::remainder(std::atan2(station.z, station.x) - departure.phi - theta_branch, TAU),
                            };
                        };
                    };
                    // The geometric refinement is shared by all plans of this route, time and cell.
                    const auto key = std::format("{}|{}|{:a}|{}|{}|{}", origin.id, destination.id, current_time_s,
                        cells[c].first, cells[c].second, window.k);
                    std::shared_ptr<const Refinement> refinement;
                    {
                        const std::lock_guard lock(refinements_mutex_);
                        if (const auto it = refinements_.find(key); it != refinements_.end()) refinement = it->second;
                    }
                    if (!refinement) {
                        auto computed = std::make_shared<Refinement>();
                        variable_isp::ShootingSettings geometric = shooting;
                        geometric.fit_fuel_budget = false;
                        // The shared geometry is refined at the cell's own kappa, not the asking
                        // ship's: the path does not depend on kappa, but the integration does in
                        // the last digits, so the first ship to ask decided what every later one
                        // got, and a loaded game (empty cache) drifted from one never stopped.
                        const auto geometric_config = variable_isp::VariableIspIntegrator::canonical_config(
                            rho, kappa_grid[cells[c].second]);
                        for (const double wait : waits) {
                            const auto departure = departure_after(wait);
                            auto result = integrator_.refine_seed_stm(refined.seed, geometric_config,
                                station_target_for(departure), geometric);
                            const bool converged = result.converged;
                            computed->by_wait.emplace_back(departure, std::move(result));
                            if (converged) break;
                        }
                        const std::lock_guard lock(refinements_mutex_);
                        // Plans ask for "now" and a few bucket times: old entries are rarely reused.
                        if (refinements_.size() >= 50000) refinements_.clear();
                        refinement = refinements_.emplace(key, std::move(computed)).first->second;
                    }
                    // This ship's fuel: within 10% over its budget, refit T to the fuel (see
                    // ShootingSettings::fit_fuel_budget); a failed fit tries the next wait.
                    const double fuel_effort = 0.999 * kappa;  // canonical units: the budget is kappa
                    Departure departure {};
                    variable_isp::ShootingResult targeted;
                    // The path does not depend on the budget, so a path over this budget may fit a
                    // larger one the ship can load: the smallest such budget flies it unchanged, with
                    // no further shooting, ahead of the time fit (measured 2026-10-21: 40% fewer
                    // shootings than retrying the next window, and every plan flyable).
                    const Budget* flown = &budget;
                    const auto larger_budget_for = [&](double effort) -> const Budget* {
                        const Budget* best = nullptr;
                        for (const auto& other : budgets) {
                            if (other.kappa > kappa && 0.999 * other.kappa >= effort && (!best || other.kappa < best->kappa)) {
                                best = &other;
                            }
                        }
                        return best;
                    };
                    for (std::size_t w = 0; w < refinement->by_wait.size(); ++w) {
                        const auto& [option_departure, option] = refinement->by_wait[w];
                        if (!option.converged) continue;
                        departure = option_departure;
                        targeted = option;
                        flown = &budget;
                        const Budget* larger = option.effort > fuel_effort ? larger_budget_for(option.effort) : nullptr;
                        if (larger != nullptr) {
                            flown = larger;
                            const auto larger_config = variable_isp::VariableIspIntegrator::canonical_config(rho, larger->kappa);
                            targeted.end.mass_kg = 1.0 / (1.0 / larger_config.m0_kg
                                + option.effort / (2.0 * larger_config.power_w * kappa_scale_factor()));
                        } else if (option.effort > fuel_effort && option.effort < shooting.max_budget_overshoot * fuel_effort) {
                            const auto fit_key = std::format("{}|{}|{:a}|{:a}|{:a}", key, w, rho, kappa, shooting.min_radius_m);
                            bool cached = false;
                            {
                                const std::lock_guard lock(refinements_mutex_);
                                if (const auto it = fits_.find(fit_key); it != fits_.end()) {
                                    targeted = it->second;
                                    cached = true;
                                }
                            }
                            if (!cached) {
                                targeted = integrator_.refine_seed_stm(option.seed, config, station_target_for(departure), shooting);
                                const std::lock_guard lock(refinements_mutex_);
                                if (fits_.size() >= 50000) fits_.clear();
                                fits_.emplace(fit_key, targeted);
                            }
                            if (!targeted.converged) continue;
                        } else {
                            // The cached end mass belongs to the ship that first planned it.
                            // ∫a²dt in SI is the canonical effort / kappa_scale_factor.
                            targeted.end.mass_kg = 1.0 / (1.0 / config.m0_kg
                                + option.effort / (2.0 * config.power_w * kappa_scale_factor()));
                        }
                        break;
                    }
                    if (!targeted.converged) {
                        continue;
                    }
                    refined = std::move(targeted);
                    best_wait_s = departure.wait_s;
                    best_r_scale = departure.r_scale;
                    best_t_scale = departure.t_scale;
                    best_phi_depart = departure.phi;
                    // Integrate the winning canonical trajectory. We use many internal samples so
                    // that arc-length resampling (below) has enough source points to represent tight
                    // solar passes accurately.
                    // Dense enough that even a 0.1 AU perihelion on a multi-year transfer turns
                    // only a few degrees per sample before thinning.
                    const profiling::Scope path_scope(profiling::Phase::VariableIspPath);
                    // Without a path (scoring), the shooting's own end state, mass and closest
                    // approach suffice.
                    variable_isp::IntegrationSummary full;
                    const auto flown_config = flown == &budget
                        ? config : variable_isp::VariableIspIntegrator::canonical_config(rho, flown->kappa);
                    if (options.include_path) {
                        full = integrator_.integrate_fixed_time(refined.seed, flown_config, 4000);
                    } else {
                        full.samples = {
                            {0.0, flown_config.r0_m, 0.0, flown_config.vr0_mps, flown_config.vtheta0_mps, flown_config.m0_kg},
                            refined.end,
                            {0.0, refined.min_radius_m, 0.0, 0.0, 0.0, flown_config.m0_kg},  // closest approach
                        };
                    }
                    // Without a path the last sample is the closest-approach marker (at the start
                    // mass), so the end mass is the shooting's own end state, samples[1]. Until
                    // 2026-10-21 the marker was checked, a plan short of fuel scored as feasible,
                    // and its replan with the path then failed every day.
                    const double end_mass_kg = !options.include_path ? full.samples[1].mass_kg : full.samples.back().mass_kg;
                    if (end_mass_kg < kDryMassSlack * variable_isp::VariableIspIntegrator::kCanonicalDryMassKg) {
                        fuel_limited = true;
                        continue;
                    }
                    const auto closest = std::min_element(full.samples.begin(), full.samples.end(),
                        [](const auto& a, const auto& b) { return a.r_m < b.r_m; });
                    if (closest->r_m * departure.r_scale < kMinPerihelionM) {
                        continue;  // time-optimal variable-Isp arcs like to dive sunward; keep them survivable
                    }
                    best_seed = refined.seed;
                    if (!options.include_path) {
                        full.samples.pop_back();  // drop the closest-approach marker
                    }
                    result = std::move(full);
                    best_theta_f = theta_grid[window.k];
                    refine_iterations = refined.iterations;
                    chosen = flown;
                    found = true;
                } catch (const std::runtime_error&) {
                    // Seed dives into the Sun or similar: try the next seed/window.
                }
            }
        }
    }

    // Optional (PlanningOptions::direct_fallback): no atlas window converged (refinement failed,
    // too little fuel, or a Sun dive), so leave now on the fastest transfer the largest fuel
    // budget allows, solved directly between the stations (GeneralTransfer, no atlas). About
    // 60% of these requests have one (2026-10-12).
    const auto direct_transfer = [&](const Budget& budget) -> std::optional<domain::TrajectoryPlan> {
        namespace g = variable_isp::general;
        const double mu = variable_isp::VariableIspIntegrator::kMuSunSI;
        const double length = r_origin_m;
        const double unit_time = std::sqrt(length * length * length / mu);
        const double unit_speed = length / unit_time;
        const double unit_accel = mu / (length * length);
        // A station on its planet's circular heliocentric orbit (as the atlas assumes).
        const auto state_at = [&](const domain::StationDefinition& station, double time_s) {
            const auto p = mechanics_.get_station_position(station, time_s);
            const double r = std::max(1.0, std::hypot(p.x, p.z));
            const double v = std::sqrt(mu / r) / unit_speed;
            return g::State {{p.x / length, p.z / length}, {-p.z / r * v, p.x / r * v}};
        };
        const g::Target target = [&](double T) {
            const double h = 1e-5 * std::max(T, 1e-2);
            const double arrival = current_time_s + T * unit_time;
            const auto a = state_at(destination, arrival - h * unit_time);
            const auto b = state_at(destination, arrival + h * unit_time);
            return g::TargetPoint {state_at(destination, arrival), (0.5 / h) * (b.r - a.r), (0.5 / h) * (b.v - a.v)};
        };
        const g::Propagator propagator;
        const g::TransferRequest request {state_at(origin, current_time_s), target, g::EndCondition::Rendezvous,
            kMinPerihelionM / length};
        g::MinTimeOptions search;
        search.solve.fallbacks = false;  // the warm-started grid covers it; fallbacks cost 10x
        // Up to about 1.3 years at 1 AU (times scale with the origin's orbit).
        const auto solution = g::solve_min_time(propagator, request, 0.999 * budget.kappa, 0.05, 8.0, search);
        if (!solution.converged) {
            return std::nullopt;
        }
        domain::TrajectoryPlan direct;
        std::vector<g::Sample> dense = options.include_path
            ? propagator.sample(request.start, solution.primer, solution.T, 1500)
            : std::vector<g::Sample> {{0.0, request.start, solution.primer.p, 0.0},
                  {solution.T, solution.end.state, solution.end.primer.p, solution.end.effort}};
        if (dense.size() < 2) {
            return std::nullopt;
        }
        std::vector<std::size_t> keep;
        if (options.include_path) {
            std::vector<math::Vec3d> positions;
            positions.reserve(dense.size());
            for (const auto& sample : dense) positions.push_back({sample.state.r.x, 0.0, sample.state.r.y});
            keep = select_for_rendering(positions);
        } else {
            keep = {0, dense.size() - 1};
            direct.has_render_path = false;
        }
        // Mass from the effort: 1/m = 1/m0 + ∫a²dt / 2P.
        const auto mass_after = [&](double effort) {
            return 1.0 / (1.0 / budget.m0 + effort * unit_accel * unit_accel * unit_time / (2.0 * power_w));
        };
        for (const auto index : keep) {
            const auto& sample = dense[index];
            direct.sampled_path.push_back({sample.state.r.x * length, 0.0, sample.state.r.y * length});
            direct.sampled_times_s.push_back(current_time_s + sample.t * unit_time);
            direct.sampled_propellant_kg.push_back(std::max(0.0, mass_after(sample.effort) - laden_dry_kg));
        }
        const double transfer_s = solution.T * unit_time;
        direct.departure_time_s = current_time_s;
        direct.arrival_time_s = current_time_s + transfer_s;
        direct.wait_time_s = 0.0;
        direct.coast_time_s = transfer_s;
        direct.travel_time_s = transfer_s;
        direct.propellant_required_kg = budget.m0 - mass_after(solution.end.effort);
        direct.propellant_load_kg = budget.load_kg;
        direct.feasible = direct.propellant_required_kg <= budget.load_kg;
        direct.trajectory_type = "variable_isp";
        direct.summary = std::format("VariableISP {} -> {} direct (no atlas window) transfer={:.1f}d propellant={:.0f}kg ({})",
            origin.name, destination.name, transfer_s / variable_isp::VariableIspIntegrator::kDayS, direct.propellant_required_kg,
            direct.feasible ? "feasible" : "insufficient fuel");
        const auto origin_station_pos = mechanics_.get_station_position(origin, direct.departure_time_s);
        const auto dest_station_pos = mechanics_.get_station_position(destination, direct.arrival_time_s);
        direct.diagnostics = {
            .start_miss_m = (direct.sampled_path.front() - origin_station_pos).length(),
            .endpoint_miss_m = (direct.sampled_path.back() - dest_station_pos).length(),
            .rho = rho,
            .kappa = budget.kappa,
            .theta_target_rad = 0.0,
            .theta_actual_rad = solution.end.sweep_rad,
            .r_end_canonical_ratio = 1.0,
            .seed_source = "direct",
            .refine_iterations = solution.iterations,
            .windows_tried = windows_tried,
        };
        direct.sampled_path.front() = origin_station_pos;
        direct.sampled_path.back() = dest_station_pos;
        return direct;
    };
    if (!found && options.direct_fallback && !budgets.empty()) {
        const auto largest = std::max_element(budgets.begin(), budgets.end(),
            [](const Budget& a, const Budget& b) { return a.kappa < b.kappa; });
        if (auto direct = direct_transfer(*largest)) {
            return *direct;
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
    if (!options.include_path) {
        samples = {result.samples.front(), result.samples.back()};
        plan.has_render_path = false;
    } else {
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
    // I scales as (P_canonical/P_real) * r_scale^-2.5 = delta_inv_real/delta_inv_canonical
    // (kappa above includes the origin factor), where delta_inv = 1/m_dry - 1/m0 encodes
    // the ship's kappa fuel fraction.
    // Clamp m_final_canonical to m_dry_canonical: the ODE solver doesn't enforce
    // a mass floor, so trajectories at maximum burn can underflow by ~0.1-0.2%.
    const double m0_canonical = variable_isp::VariableIspIntegrator::kCanonicalM0Kg;
    const double m_dry_canonical = variable_isp::VariableIspIntegrator::kCanonicalDryMassKg;
    const double m_final_canonical = std::max(samples.back().mass_kg, m_dry_canonical);
    const double delta_inv_canonical = 1.0 / m_dry_canonical - 1.0 / m0_canonical;
    const double m0 = chosen->m0;
    const double m_floor = chosen->m_floor;
    const double kappa = chosen->kappa;
    const double delta_inv_real = 1.0 / m_floor - 1.0 / m0;
    const double I_canonical = 1.0 / m_final_canonical - 1.0 / m0_canonical;
    const double I_real = I_canonical * (delta_inv_real / delta_inv_canonical);
    const double m_f_real = 1.0 / (I_real + 1.0 / m0);
    plan.propellant_required_kg = m0 - m_f_real;
    plan.propellant_load_kg = chosen->load_kg;
    plan.feasible = plan.propellant_required_kg <= chosen->load_kg;

    // Per-sample propellant via same I-invariant scaling — used for continuous
    // propellant display during transit.
    plan.sampled_propellant_kg.reserve(samples.size());
    for (const auto& sample : samples) {
        const double m_s_can = std::max(sample.mass_kg, m_dry_canonical);
        const double I_s = 1.0 / m_s_can - 1.0 / m0_canonical;
        const double I_s_real = I_s * (delta_inv_real / delta_inv_canonical);
        const double m_s_real = 1.0 / (I_s_real + 1.0 / m0);
        plan.sampled_propellant_kg.push_back(std::max(0.0, m_s_real - laden_dry_kg));
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
