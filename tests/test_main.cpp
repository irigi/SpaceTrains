#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "celestial/CelestialMechanics.hpp"
#include "data_loader/DataLoader.hpp"
#include "simulation/Simulation.hpp"
#include "trajectory/TrajectoryAudit.hpp"
#include "trajectory/TrajectoryPlanner.hpp"
#include "trajectory/VariableIspTrajectoryPlanner.hpp"
#include "variable_isp/VariableIsp.hpp"

namespace {

constexpr double PI = 3.14159265358979323846;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_near(double actual, double expected, double tolerance, const char* message) {
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(message);
    }
}

double distance_between(const spacetrains::math::Vec3d& lhs, const spacetrains::math::Vec3d& rhs) {
    return (lhs - rhs).length();
}

const spacetrains::domain::StationDefinition& station_by_id(
    const spacetrains::domain::UniverseDefinition& universe,
    const std::string& id) {
    for (const auto& station : universe.stations) {
        if (station.id == id) {
            return station;
        }
    }
    throw std::runtime_error("missing station " + id);
}

const spacetrains::domain::ShipClassDefinition& ship_class_by_id(
    const spacetrains::domain::UniverseDefinition& universe,
    const std::string& id) {
    for (const auto& ship_class : universe.ship_classes) {
        if (ship_class.id == id) {
            return ship_class;
        }
    }
    throw std::runtime_error("missing ship class " + id);
}

const spacetrains::domain::CelestialBodyDefinition& body_by_id(
    const spacetrains::domain::UniverseDefinition& universe,
    const std::string& id) {
    for (const auto& body : universe.bodies) {
        if (body.id == id) {
            return body;
        }
    }
    throw std::runtime_error("missing body " + id);
}

spacetrains::math::Vec3d interpolate_path(const std::vector<spacetrains::math::Vec3d>& path, double progress) {
    const double scaled_index = std::clamp(progress, 0.0, 1.0) * static_cast<double>(path.size() - 1);
    const auto lower_index = static_cast<std::size_t>(std::floor(scaled_index));
    const auto upper_index = std::min(lower_index + 1, path.size() - 1);
    const double alpha = scaled_index - static_cast<double>(lower_index);
    return path[lower_index] * (1.0 - alpha) + path[upper_index] * alpha;
}

spacetrains::math::Vec3d interpolate_timed_path(
    const std::vector<spacetrains::math::Vec3d>& path,
    const std::vector<double>& sample_times_s,
    double time_s) {
    if (time_s <= sample_times_s.front()) {
        return path.front();
    }
    if (time_s >= sample_times_s.back()) {
        return path.back();
    }
    const auto upper = std::upper_bound(sample_times_s.begin(), sample_times_s.end(), time_s);
    const auto upper_index = static_cast<std::size_t>(std::distance(sample_times_s.begin(), upper));
    const auto lower_index = upper_index - 1;
    const double alpha = (time_s - sample_times_s[lower_index]) / (sample_times_s[upper_index] - sample_times_s[lower_index]);
    return path[lower_index] * (1.0 - alpha) + path[upper_index] * alpha;
}

}  // namespace

int main() {
    const auto repo_root = std::filesystem::current_path();
    spacetrains::data_loader::DataLoader loader;
    const auto universe = loader.load_universe(repo_root / "data");

    require(universe.bodies.size() >= 6, "expected seeded bodies");
    require(universe.stations.size() >= 6, "expected seeded stations");
    require(universe.ship_seeds.size() >= 3, "expected seeded ships");

    spacetrains::celestial::CelestialMechanics mechanics(universe);
    spacetrains::trajectory::KeplerTrajectoryPlanner planner(universe, mechanics);
    const auto& origin = station_by_id(universe, "earth_l1");
    const auto& destination = station_by_id(universe, "earth_orbit");
    const auto& mars_destination = station_by_id(universe, "mars_transfer");
    const auto& ship_class = ship_class_by_id(universe, "light_freighter");
    spacetrains::domain::ShipState ship {
        .id = "test_ship",
        .name = "Test Ship",
        .faction_id = "sol_fed",
        .class_id = ship_class.id,
        .home_station_id = origin.id,
        .current_station_id = origin.id,
        .phase = spacetrains::domain::ShipMissionPhase::Idle,
        .propellant_kg = ship_class.propellant_capacity_kg,
        .active_mission = {},
    };

    const auto local_plan = planner.plan_transfer(origin, destination, ship, ship_class, 0.0);
    require(local_plan.feasible, "fully fueled test ship should be feasible for local transfer");
    require(local_plan.wait_time_s == 0.0, "same-parent transfer should depart immediately");
    require(local_plan.travel_time_s <= 24.0 * 3600.0, "same-parent transfer should use local bounded timing");
    require(local_plan.sampled_path.size() >= 2, "local plan should include a sampled path");
    require(local_plan.sampled_times_s.size() == local_plan.sampled_path.size(), "local plan should include timed samples");
    require(distance_between(local_plan.sampled_path.front(), mechanics.get_station_position(origin, local_plan.departure_time_s)) < 1.0, "local path should start at departure station");
    require(distance_between(local_plan.sampled_path.back(), mechanics.get_station_position(destination, local_plan.arrival_time_s)) < 1.0, "local path should end at arrival station");

    const auto plan = planner.plan_transfer(origin, mars_destination, ship, ship_class, 0.0);
    require(plan.sampled_path.size() >= 2, "Kepler plan should include a sampled path");
    require(plan.sampled_times_s.size() == plan.sampled_path.size(), "Kepler plan should include timed samples");
    require(plan.wait_time_s >= 0.0, "Kepler launch wait should be non-negative");
    require(plan.coast_time_s > 0.0, "Kepler coast time should be positive");
    require_near(plan.travel_time_s, plan.wait_time_s + plan.coast_time_s, 1.0e-6, "Kepler travel time should include wait plus coast");
    // With wait-period samples prepended, path.front() is the origin at the first sample time
    // (current time when wait > 0, or departure time when wait == 0).
    require(distance_between(plan.sampled_path.front(),
        mechanics.get_station_position(origin, plan.sampled_times_s.front())) < 1.0,
        "sampled path should start at origin station at first sample time");
    require(distance_between(plan.sampled_path.back(), mechanics.get_station_position(mars_destination, plan.arrival_time_s)) < 1.0, "sampled path should end at arrival station");
    for (std::size_t i = 1; i < plan.sampled_times_s.size(); ++i) {
        require(plan.sampled_times_s[i] > plan.sampled_times_s[i - 1], "Kepler sample times should be monotonic");
    }
    require(plan.sampled_propellant_kg.size() == plan.sampled_path.size(),
        "Keplerian plan must have one propellant sample per path point");
    require_near(plan.sampled_propellant_kg.front(),
        ship.propellant_kg, 1.0,
        "Keplerian first propellant sample must equal initial propellant");
    require_near(plan.sampled_propellant_kg.back(),
        ship.propellant_kg - plan.propellant_required_kg, 1.0,
        "Keplerian last propellant sample must match remaining propellant after burns");
    for (std::size_t i = 1; i < plan.sampled_propellant_kg.size(); ++i) {
        require(plan.sampled_propellant_kg[i] <= plan.sampled_propellant_kg[i - 1] + 1.0,
            "Keplerian propellant samples must be monotonically non-increasing");
    }

    const auto& sun = body_by_id(universe, mechanics.get_root_body_id());
    const double r1 = std::max(1.0, mechanics.get_heliocentric_radius(origin.parent_body_id, 0.0));
    const double r2 = std::max(1.0, mechanics.get_heliocentric_radius(mars_destination.parent_body_id, 0.0));
    const double transfer_axis = (r1 + r2) * 0.5;
    const double expected_hohmann_time_s = PI * std::sqrt((transfer_axis * transfer_axis * transfer_axis) / sun.mu_m3_s2);
    // Lambert grid search picks the best transit from 0.3× to 1.5× Hohmann; allow the full range.
    require(plan.coast_time_s >= expected_hohmann_time_s * 0.28,
        "Kepler coast time must be at least 28% of Hohmann (Lambert lower bound)");
    require(plan.coast_time_s <= expected_hohmann_time_s * 1.52,
        "Kepler coast time must be at most 152% of Hohmann (Lambert upper bound)");
    require(plan.feasible, "fully fueled ship should find a feasible Earth-Mars Kepler plan");
    require(plan.propellant_required_kg > 0.0, "non-trivial Earth-Mars transfer must burn some propellant");

    ship.propellant_kg = 1.0;
    const auto impossible_plan = planner.plan_transfer(origin, mars_destination, ship, ship_class, 0.0);
    require(!impossible_plan.feasible, "planner should fail when propellant is insufficient");

    // --- Kepler moon-route endpoint regression (heliocentric rate bug) ---
    // Before the fix, omega_dest/omega_origin used each body's own orbital period instead
    // of the parent planet's heliocentric period.  For moons this gave the local
    // moon-around-planet rate, making phase calculations wrong:
    //   - Luna: old synodic period ≈ 29.5 days, fixed synodic period → ∞ (co-moving)
    //   - Ganymede: old synodic period ≈ 7.3 days, fixed synodic period ≈ 399 days
    ship.propellant_kg = ship_class.propellant_capacity_kg;

    // Earth L1 → Lunar Gateway: Moon co-moves heliocentrically with Earth.
    // Both bodies walk up to Earth's heliocentric rate → relative_rate == 0 → wait == 0.
    const auto& luna_station = station_by_id(universe, "luna_base");
    const auto luna_plan = planner.plan_transfer(origin, luna_station, ship, ship_class, 0.0);
    require(luna_plan.sampled_path.size() >= 2, "Kepler Earth->Luna should produce a sampled path");
    require(luna_plan.sampled_times_s.size() == luna_plan.sampled_path.size(),
        "Kepler Earth->Luna times and path sizes must match");
    require(luna_plan.wait_time_s == 0.0,
        "Kepler Earth->Luna wait must be zero: Moon has the same heliocentric rate as Earth");
    require(distance_between(luna_plan.sampled_path.front(),
            mechanics.get_station_position(origin, luna_plan.departure_time_s)) < 1.0,
        "Kepler Earth->Luna path start must match origin station at departure time");
    require(distance_between(luna_plan.sampled_path.back(),
            mechanics.get_station_position(luna_station, luna_plan.arrival_time_s)) < 1.0,
        "Kepler Earth->Luna path end must match destination station at arrival time");

    // Earth L1 → Ganymede Deep Mine: worst-case outer moon.
    // Old code used Ganymede's 7.2-day local period → synodic period ≈ 7.3 days.
    // Fixed code uses Jupiter's 11.9-year heliocentric period → synodic period ≈ 399 days.
    // We verify that wait_time_s is within the correct (large) synodic window, and that
    // the Hohmann coast time reflects the true Earth→Jupiter heliocentric distance.
    const auto& ganymede_station = station_by_id(universe, "ganymede_depot");
    const auto ganymede_plan = planner.plan_transfer(origin, ganymede_station, ship, ship_class, 0.0);
    require(ganymede_plan.sampled_path.size() >= 2, "Kepler Earth->Ganymede should produce a sampled path");
    require(ganymede_plan.sampled_times_s.size() == ganymede_plan.sampled_path.size(),
        "Kepler Earth->Ganymede times and path sizes must match");
    require(ganymede_plan.wait_time_s >= 0.0, "Kepler Earth->Ganymede wait must be non-negative");
    // Old buggy synodic period ≤ 7.3 days; correct synodic period ≈ 399 days.
    // Any wait value up to ~399 days is consistent with the fix.
    require(ganymede_plan.wait_time_s < 400.0 * 86400.0,
        "Kepler Earth->Ganymede wait must be within the Jupiter-Earth synodic period (~399 days)");
    // Hohmann coast for Earth→Jupiter distance ≈ 997 days — far larger than the buggy
    // ~7-day Ganymede synodic, so coast_time being large is indirect evidence of the fix.
    require(ganymede_plan.coast_time_s > 200.0 * 86400.0,
        "Kepler Earth->Ganymede coast must exceed 200 days (true heliocentric distance, not moon-period)");
    require(distance_between(ganymede_plan.sampled_path.front(),
            mechanics.get_station_position(origin, ganymede_plan.sampled_times_s.front())) < 1.0,
        "Kepler Earth->Ganymede path start must match origin station at first sample time");
    require(distance_between(ganymede_plan.sampled_path.back(),
            mechanics.get_station_position(ganymede_station, ganymede_plan.arrival_time_s)) < 1.0,
        "Kepler Earth->Ganymede path end must match destination station at arrival time");

    auto sim = spacetrains::simulation::Simulation::from_data_root((repo_root / "data").string());
    sim.set_timewarp(86400.0);
    const auto before = sim.snapshot();
    require(!before.stations.empty(), "snapshot should include stations");

    bool bridge_snapshot_included_trajectory_path = false;
    bool bridge_snapshot_included_destination_body = false;
    bool bridge_snapshot_included_ship_stats = false;
    bool checked_awaiting_departure_parked = false;
    bool checked_render_position_on_path = false;
    for (int i = 0; i < 90; ++i) {
        sim.step(1.0);
        const auto during = sim.snapshot();
        const auto bridge_during = sim.build_bridge_snapshot_json(false, static_cast<std::uint64_t>(i + 1), 0.1);
        if (bridge_during.find("\"trajectory_path\"") != std::string::npos) {
            bridge_snapshot_included_trajectory_path = true;
        }
        if (bridge_during.find("\"destination_body_at_arrival\"") != std::string::npos) {
            bridge_snapshot_included_destination_body = true;
        }
        if (bridge_during.find("\"dry_mass_kg\"") != std::string::npos
            && bridge_during.find("\"initial_mass_kg\"") != std::string::npos
            && bridge_during.find("\"current_mass_kg\"") != std::string::npos
            && bridge_during.find("\"commodity_id\"") != std::string::npos
            && bridge_during.find("\"departure_time_s\"") != std::string::npos) {
            bridge_snapshot_included_ship_stats = true;
        }
        if (checked_awaiting_departure_parked && checked_render_position_on_path) {
            continue;
        }
        for (const auto& active_ship : during.ships) {
            if ((active_ship.phase != spacetrains::domain::ShipMissionPhase::InTransit
                    && active_ship.phase != spacetrains::domain::ShipMissionPhase::AwaitingDeparture)
                || active_ship.active_mission.sampled_path.empty()
                || active_ship.active_mission.sampled_times_s.empty()) {
                continue;
            }
            const auto render_position = sim.get_ship_render_position(active_ship);
            require(distance_between(active_ship.active_mission.sampled_path.front(), interpolate_path(active_ship.active_mission.sampled_path, 0.0)) < 1.0, "path interpolation should return start point");
            require(distance_between(active_ship.active_mission.sampled_path.back(), interpolate_path(active_ship.active_mission.sampled_path, 1.0)) < 1.0, "path interpolation should return arrival point");
            if (active_ship.phase == spacetrains::domain::ShipMissionPhase::AwaitingDeparture
                && during.game_time_s < active_ship.active_mission.departure_time_s) {
                const auto& current_station = station_by_id(universe, active_ship.current_station_id);
                const auto expected_position = mechanics.get_station_position(current_station, during.game_time_s);
                require(distance_between(render_position, expected_position) < 1.0, "awaiting-departure ship should render parked at its current station before launch");
                checked_awaiting_departure_parked = true;
            } else {
                const auto expected_position = interpolate_timed_path(active_ship.active_mission.sampled_path, active_ship.active_mission.sampled_times_s, during.game_time_s);
                require(distance_between(render_position, expected_position) < 1.0, "ship render position should interpolate active mission path after departure");
                checked_render_position_on_path = true;
            }
            if (checked_awaiting_departure_parked && checked_render_position_on_path) {
                break;
            }
        }
    }

    const auto after = sim.snapshot();
    require(after.game_time_s > before.game_time_s, "time should advance");
    require(!after.recent_events.empty(), "simulation should emit events");
    const auto bridge_json = sim.build_bridge_snapshot_json(false, 1, 0.1);
    require(bridge_json.find("\"bodies\"") != std::string::npos, "bridge snapshot should include bodies");
    require(bridge_json.find("\"ships\"") != std::string::npos, "bridge snapshot should include ships");
    require(bridge_snapshot_included_trajectory_path, "bridge snapshot should include in-transit ship trajectory paths");
    require(bridge_snapshot_included_destination_body, "bridge snapshot should include destination body at arrival");
    require(bridge_snapshot_included_ship_stats, "bridge snapshot should include ship mass, cargo, and launch timing fields");

    bool found_in_transit_or_arrival_signal = false;
    for (const auto& event : after.recent_events) {
        if (event.text.find("departed") != std::string::npos || event.text.find("arrived") != std::string::npos) {
            found_in_transit_or_arrival_signal = true;
        }
    }
    require(found_in_transit_or_arrival_signal, "simulation should dispatch at least one mission");
    require(checked_awaiting_departure_parked, "simulation should keep an awaiting-departure ship parked before launch");
    require(checked_render_position_on_path, "simulation should keep an in-transit ship with a sampled path");

    // --- VariableISP planner integration test ---
    {
        const auto atlas_path = (repo_root / "tests" / "data" / "variable_isp" / "variable_isp_atlas.bin").string();
        spacetrains::variable_isp::VariableIspAtlas visp_atlas;
        visp_atlas.load_binary(atlas_path);
        require(!visp_atlas.rho_grid().empty(), "VariableISP atlas should load");

        spacetrains::trajectory::VariableIspTrajectoryPlanner visp_planner(universe, mechanics, visp_atlas);

        const auto& ion_class = ship_class_by_id(universe, "ion_freighter");
        const auto& earth_station = station_by_id(universe, "earth_l1");
        const auto& mars_station = station_by_id(universe, "mars_transfer");

        spacetrains::domain::ShipState ion_ship {
            .id = "test_ion_ship",
            .name = "Test Ion Ship",
            .faction_id = "sol_fed",
            .class_id = ion_class.id,
            .home_station_id = earth_station.id,
            .current_station_id = earth_station.id,
            .phase = spacetrains::domain::ShipMissionPhase::Idle,
            .propellant_kg = ion_class.propellant_capacity_kg,
            .active_mission = {},
        };

        const auto visp_plan = visp_planner.plan_transfer(
            earth_station, mars_station, ion_ship, ion_class, 0.0);

        require(visp_plan.feasible, "fully fueled ion ship should find a feasible Earth-Mars VariableISP plan");
        require(visp_plan.sampled_path.size() == 120, "VariableISP plan should have 120 sampled path points");
        require(visp_plan.sampled_times_s.size() == 120, "VariableISP plan should have 120 timed samples");
        require(visp_plan.coast_time_s > 0.0, "VariableISP transfer time should be positive");
        const double transfer_days = visp_plan.coast_time_s / 86400.0;
        require(transfer_days >= 30.0 && transfer_days <= 600.0,
            "VariableISP Earth-Mars transfer time should be physically plausible (30-600 days)");
        require(visp_plan.propellant_required_kg > 0.0 && visp_plan.propellant_required_kg <= ion_class.propellant_capacity_kg,
            "VariableISP propellant should be positive and within tank capacity");
        require(visp_plan.wait_time_s >= 0.0, "VariableISP launch wait should be non-negative");
        for (std::size_t i = 1; i < visp_plan.sampled_times_s.size(); ++i) {
            require(visp_plan.sampled_times_s[i] > visp_plan.sampled_times_s[i - 1],
                "VariableISP sample times should be strictly monotonically increasing");
        }
        // Departure point should be near Earth, arrival point near Mars (within ~15% of orbital radius).
        const double earth_r = mechanics.get_heliocentric_radius("earth", 0.0);
        const double mars_r = mechanics.get_heliocentric_radius("mars", visp_plan.arrival_time_s);
        const double depart_r = visp_plan.sampled_path.front().length();
        const double arrive_r = visp_plan.sampled_path.back().length();
        require(std::abs(depart_r - earth_r) < earth_r * 0.15,
            "VariableISP departure point should be near Earth orbit");
        require(std::abs(arrive_r - mars_r) < mars_r * 0.15,
            "VariableISP arrival point should be near Mars orbit");

        // --- VariableISP endpoint regression (atlas bilinear interpolation endpoint snap) ---
        // Before the fix, path endpoints came from atlas interpolation which could miss the
        // target body by up to 0.34 AU (radial error in bilinear seed interpolation).
        // Both endpoints are now snapped to exact station positions at departure/arrival time.
        require(distance_between(visp_plan.sampled_path.front(),
                mechanics.get_station_position(earth_station, visp_plan.departure_time_s)) < 1.0,
            "VariableISP departure point must match origin station position (endpoint snap)");
        require(distance_between(visp_plan.sampled_path.back(),
                mechanics.get_station_position(mars_station, visp_plan.arrival_time_s)) < 1.0,
            "VariableISP arrival point must match destination station position (endpoint snap)");

        // --- VariableISP propellant sample regression ---
        // Before the fix, sampled_propellant_kg was empty; ion ships showed ~0 propellant
        // throughout transit because propellant was deducted upfront at mission assignment.
        require(visp_plan.sampled_propellant_kg.size() == visp_plan.sampled_path.size(),
            "VariableISP plan must have one propellant sample per path point");
        require_near(visp_plan.sampled_propellant_kg.front(),
            ion_class.propellant_capacity_kg, 1.0,
            "VariableISP first propellant sample must be near full tank capacity");
        require_near(visp_plan.sampled_propellant_kg.back(),
            ion_class.propellant_capacity_kg - visp_plan.propellant_required_kg, 1.0,
            "VariableISP last propellant sample must match computed propellant cost");
        for (std::size_t i = 1; i < visp_plan.sampled_propellant_kg.size(); ++i) {
            require(visp_plan.sampled_propellant_kg[i] <= visp_plan.sampled_propellant_kg[i - 1] + 1.0,
                "VariableISP propellant samples must be monotonically non-increasing");
        }

        std::cout << std::format(
            "VariableISP Earth-Mars: wait={:.1f}d transfer={:.1f}d propellant={:.0f}kg\n",
            visp_plan.wait_time_s / 86400.0,
            transfer_days,
            visp_plan.propellant_required_kg);

        // --- VariableISP heliocentric rate regression (moon-station origin/destination) ---
        // Before the fix, omega for moon stations used the moon's local orbital period instead
        // of the parent planet's heliocentric period, producing wrong phase offsets.
        // Earth Orbit → Luna Base: both stations walk up to Earth's heliocentric rate,
        // so relative_rate == 0 and wait_s must be 0.0.  Old code gave wait ≈ 0-29 days
        // (Moon's synodic period) because it used Luna's 27.3-day local period for omega_dest.
        const auto& ion_courier_class = ship_class_by_id(universe, "ion_courier");
        const auto& earth_orbit_station = station_by_id(universe, "earth_orbit");
        const auto& luna_station_visp = station_by_id(universe, "luna_base");
        spacetrains::domain::ShipState ion_courier_ship {
            .id = "test_courier",
            .name = "Test Courier",
            .faction_id = "sol_fed",
            .class_id = ion_courier_class.id,
            .home_station_id = earth_orbit_station.id,
            .current_station_id = earth_orbit_station.id,
            .phase = spacetrains::domain::ShipMissionPhase::Idle,
            .propellant_kg = ion_courier_class.propellant_capacity_kg,
            .active_mission = {},
        };
        const auto visp_luna_plan = visp_planner.plan_transfer(
            earth_orbit_station, luna_station_visp, ion_courier_ship, ion_courier_class, 0.0);
        // rho ≈ 1.0 for Earth-Moon; plan may or may not be feasible (heliocentric model
        // doesn't apply well this close), but if a path is returned the checks below must hold.
        if (visp_luna_plan.sampled_path.size() >= 2 && visp_luna_plan.sampled_times_s.size() >= 2) {
            require(distance_between(visp_luna_plan.sampled_path.front(),
                    mechanics.get_station_position(earth_orbit_station, visp_luna_plan.departure_time_s)) < 1.0,
                "VariableISP Earth Orbit->Luna departure must match origin station (moon heliocentric rate fix)");
            require(distance_between(visp_luna_plan.sampled_path.back(),
                    mechanics.get_station_position(luna_station_visp, visp_luna_plan.arrival_time_s)) < 1.0,
                "VariableISP Earth Orbit->Luna arrival must match destination station (moon heliocentric rate fix)");
            // Moon co-moves with Earth heliocentrically → relative_rate == 0 → wait_s == 0.
            require(visp_luna_plan.wait_time_s == 0.0,
                "VariableISP Earth Orbit->Luna wait must be zero: Moon has the same heliocentric rate as Earth");
        }
    }

    // --- Trajectory audit: synthetic paths ---
    {
        constexpr double AU = 1.495978707e11;
        // Smooth quarter-arc from 1 AU to 1.5 AU: nothing flagged.
        spacetrains::domain::TrajectoryPlan smooth;
        smooth.departure_time_s = 0.0;
        for (int i = 0; i < 60; ++i) {
            const double a = 0.5 * PI * i / 59.0;
            const double r = AU * (1.0 + 0.5 * i / 59.0);
            smooth.sampled_path.push_back({r * std::cos(a), 0.0, r * std::sin(a)});
            smooth.sampled_times_s.push_back(i * 86400.0);
        }
        const auto smooth_audit = spacetrains::trajectory::audit_trajectory(smooth, AU, 1.5 * AU);
        require(smooth_audit.flags.empty(), "smooth arc should pass the trajectory audit");
        require_near(smooth_audit.revolutions, 0.25, 1e-6, "quarter arc should sweep 0.25 revolutions");

        // Bug 1 shape: path stops short and the last sample is snapped onto the station.
        auto dented = smooth;
        dented.sampled_path.back() = {0.3 * AU, 0.0, 1.9 * AU};
        dented.diagnostics.endpoint_miss_m = 0.4 * AU;
        const auto dented_audit = spacetrains::trajectory::audit_trajectory(dented, AU, 1.5 * AU);
        const auto has_flag = [](const auto& audit, const char* flag) {
            return std::find(audit.flags.begin(), audit.flags.end(), flag) != audit.flags.end();
        };
        require(has_flag(dented_audit, "end_dent"), "snapped far miss should be flagged as end_dent");
        require(has_flag(dented_audit, "endpoint_miss"), "pre-snap miss should be flagged as endpoint_miss");

        // Bug 2 shape: 11 wait samples spread over 2.5 origin revolutions before departure.
        spacetrains::domain::TrajectoryPlan waiting = smooth;
        waiting.departure_time_s = 1000.0 * 86400.0;
        std::vector<spacetrains::math::Vec3d> wait_path;
        std::vector<double> wait_times;
        for (int i = 0; i < 11; ++i) {
            const double a = 2.0 * PI * 2.5 * i / 11.0;
            wait_path.push_back({0.72 * AU * std::cos(a), 0.0, 0.72 * AU * std::sin(a)});
            wait_times.push_back(i * 90.0 * 86400.0);
        }
        for (std::size_t i = 0; i < smooth.sampled_path.size(); ++i) {
            wait_path.push_back(smooth.sampled_path[i]);
            wait_times.push_back(waiting.departure_time_s + smooth.sampled_times_s[i]);
        }
        waiting.sampled_path = wait_path;
        waiting.sampled_times_s = wait_times;
        const auto waiting_audit = spacetrains::trajectory::audit_trajectory(waiting, AU, 1.5 * AU);
        require(has_flag(waiting_audit, "coarse_wait"), "coarse multi-revolution wait prefix should be flagged");
        require_near(waiting_audit.revolutions, 0.25, 1e-6, "wait prefix must not count toward transfer revolutions");
    }

    // --- Kepler wait-prefix regression (bug: "trajectory loops around the Sun") ---
    // Found by --trajectory-sweep: a 573-day launch wait at Venus was drawn with a fixed 11
    // samples, i.e. 2.5 turns around the Sun as a star polygon with 83-degree steps.
    {
        const auto& freighter = ship_class_by_id(universe, "light_freighter");
        const auto& venus = station_by_id(universe, "venus_cloud");
        const auto& luna = station_by_id(universe, "luna_base");
        spacetrains::domain::ShipState waiting_ship;
        waiting_ship.class_id = freighter.id;
        waiting_ship.propellant_kg = 0.1 * freighter.propellant_capacity_kg;
        const double now_s = 45.0 * 86400.0;
        const auto plan = planner.plan_transfer(venus, luna, waiting_ship, freighter, now_s);
        require(plan.wait_time_s > 400.0 * 86400.0, "Venus->Luna regression case should still have a multi-orbit wait");
        const auto audit = spacetrains::trajectory::audit_trajectory(
            plan,
            mechanics.get_heliocentric_radius(venus.parent_body_id, plan.departure_time_s),
            mechanics.get_heliocentric_radius(luna.parent_body_id, plan.arrival_time_s));
        require(audit.wait_revolutions > 2.0, "wait prefix should follow Venus for more than two orbits");
        require(audit.wait_max_step_deg <= 5.0 + 1e-6, "wait prefix must be sampled at most 5 degrees per step");
        require(distance_between(plan.sampled_path.front(), mechanics.get_station_position(venus, now_s)) < 1.0,
            "wait prefix must start at the origin station's current position");
        for (std::size_t i = 1; i < plan.sampled_times_s.size(); ++i) {
            require(plan.sampled_times_s[i] > plan.sampled_times_s[i - 1], "wait prefix times must increase strictly");
        }
        require(plan.sampled_propellant_kg.size() == plan.sampled_path.size(),
            "propellant samples must cover the wait prefix too");
    }

    // --- VariableISP integration hang regression ---
    // Found by --trajectory-sweep: a low-fuel ion freighter planning Earth L1 -> Titan at
    // day 210 got an atlas seed that dives into the Sun, and RK45 shrank its step forever.
    // The integrator now has a step budget, and the planner refines a real atlas cell instead
    // of using the diverging seed, falling through to the next window when one fails.
    {
        const auto atlas_path = (repo_root / "tests" / "data" / "variable_isp" / "variable_isp_atlas.bin").string();
        spacetrains::variable_isp::VariableIspAtlas visp_atlas;
        visp_atlas.load_binary(atlas_path);
        spacetrains::trajectory::VariableIspTrajectoryPlanner visp_planner(universe, mechanics, visp_atlas);
        const auto& ion_class = ship_class_by_id(universe, "ion_freighter");
        spacetrains::domain::ShipState low_fuel_ship;
        low_fuel_ship.class_id = ion_class.id;
        low_fuel_ship.propellant_kg = 0.1 * ion_class.propellant_capacity_kg;
        const auto plan = visp_planner.plan_transfer(
            station_by_id(universe, "earth_l1"), station_by_id(universe, "titan_works"),
            low_fuel_ship, ion_class, 210.0 * 86400.0);
        if (plan.feasible) {
            require(plan.diagnostics.endpoint_miss_m < 1.5e9,
                "Earth L1 -> Titan low-fuel plan, if feasible, must end at Titan before the snap");
        }
    }

    // --- VariableISP endpoint regression (bug: trajectory ends in a sharp "dent") ---
    // Atlas seeds were used unrefined: blended corners mixed solution branches and
    // nearest-cell seeds were solved for a different rho, so paths missed the destination
    // (up to 13 AU) and the final snap drew a dent. Seeds are now shooting-refined onto the
    // stations' actual positions. Cases are from --trajectory-sweep; plus a small grid.
    {
        const auto atlas_path = (repo_root / "tests" / "data" / "variable_isp" / "variable_isp_atlas.bin").string();
        spacetrains::variable_isp::VariableIspAtlas visp_atlas;
        visp_atlas.load_binary(atlas_path);
        spacetrains::trajectory::VariableIspTrajectoryPlanner visp_planner(universe, mechanics, visp_atlas);
        constexpr double kMaxMissM = 7.5e8;  // 0.005 AU
        const auto check = [&](const char* class_id, const char* from, const char* to, double fuel_fraction, double day) {
            const auto& ship_class = ship_class_by_id(universe, class_id);
            spacetrains::domain::ShipState ship;
            ship.class_id = ship_class.id;
            ship.propellant_kg = fuel_fraction * ship_class.propellant_capacity_kg;
            const auto& origin = station_by_id(universe, from);
            const auto& destination = station_by_id(universe, to);
            const auto plan = visp_planner.plan_transfer(origin, destination, ship, ship_class, day * 86400.0);
            if (!plan.feasible) {
                return false;
            }
            const auto audit = spacetrains::trajectory::audit_trajectory(
                plan,
                mechanics.get_heliocentric_radius(origin.parent_body_id, plan.departure_time_s),
                mechanics.get_heliocentric_radius(destination.parent_body_id, plan.arrival_time_s));
            const auto message = std::format("VariableISP {} {}->{} fuel={:.0f}% day={:.0f}: miss={:.4f}AU start={:.4f}AU flags={}",
                class_id, from, to, fuel_fraction * 100.0, day,
                plan.diagnostics.endpoint_miss_m / 1.495978707e11, plan.diagnostics.start_miss_m / 1.495978707e11,
                audit.flags.size());
            require(plan.diagnostics.endpoint_miss_m < kMaxMissM, message.c_str());
            require(plan.diagnostics.start_miss_m < kMaxMissM, message.c_str());
            require(std::find(audit.flags.begin(), audit.flags.end(), "end_dent") == audit.flags.end(), message.c_str());
            return true;
        };
        // Worst cases before the fix: 0.088 AU dent, 13.3 AU overshoot, 0.07 AU dent.
        require(check("ion_courier", "venus_cloud", "earth_l1", 0.65, 185.0), "Venus->Earth L1 ion courier should be feasible");
        check("ion_freighter", "mars_transfer", "titan_works", 0.75, 540.0);
        check("ion_freighter", "earth_l1", "ceres_depot", 0.65, 384.0);
        // Moon stations at both ends (offset from the parent planet) and co-orbiting Earth/Moon.
        check("ion_courier", "ganymede_depot", "titan_works", 0.35, 480.0);
        check("ion_courier", "luna_base", "earth_orbit", 0.35, 15.0);
        int feasible = 0;
        for (const char* to : {"venus_cloud", "mars_transfer", "ceres_depot", "mercury_yard"}) {
            for (const double fuel : {0.2, 0.6, 1.0}) {
                for (const double day : {0.0, 200.0, 400.0}) {
                    feasible += check("ion_freighter", "earth_l1", to, fuel, day) ? 1 : 0;
                }
            }
        }
        require(feasible >= 20, "most Earth L1 ion freighter plans in the grid should be feasible");
    }

    std::cout << "All SpaceTrains tests passed.\n";
    return 0;
}
