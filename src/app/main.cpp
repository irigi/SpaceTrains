#include <cmath>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <vector>

#include "economy/EconomySystem.hpp"
#include "simulation/Simulation.hpp"

namespace {

constexpr double kAU = 1.495978707e11;
constexpr double kDayS = 86400.0;

void print_celestial_summary(const spacetrains::domain::UniverseDefinition& universe) {
    std::cout << "\n=== Celestial Mechanics ===\n";
    for (const auto& body : universe.bodies) {
        if (body.orbit.parent_id.empty()) {
            std::cout << std::format("  {:12s}  (root body)\n", body.name);
        } else {
            const double r_au = body.orbit.semi_major_axis_m / kAU;
            const double period_days = body.orbit.orbital_period_s / kDayS;
            std::cout << std::format(
                "  {:12s}  r={:.3f} AU  T={:.1f} days  parent={}\n",
                body.name, r_au, period_days, body.orbit.parent_id);
        }
    }
}

void print_ship_class_summary(const spacetrains::domain::UniverseDefinition& universe) {
    constexpr double kMuSun = 1.32712440018e20;
    const double kappa_scale = std::pow(kAU, 2.5) / std::pow(kMuSun, 1.5);

    std::cout << "\n=== Ship Classes ===\n";
    for (const auto& sc : universe.ship_classes) {
        if (sc.propulsion_type == "electric_ion") {
            const double m_dry = sc.dry_mass_kg;
            const double m0 = m_dry + sc.propellant_capacity_kg;
            const double P = sc.specific_engine_power_w_per_kg * m_dry;
            const double kappa = (m0 > m_dry && P > 0.0)
                ? 2.0 * P * (1.0 / m_dry - 1.0 / m0) * kappa_scale : 0.0;
            const double fuel_frac = sc.propellant_capacity_kg / m0;
            std::cout << std::format(
                "  {:20s}  [electric_ion]  m_dry={:.0f}kg  propellant={:.0f}kg  "
                "alpha={:.0f}W/kg  eps={:.2f}  kappa={:.3f}\n",
                sc.name, m_dry, sc.propellant_capacity_kg,
                sc.specific_engine_power_w_per_kg, fuel_frac, kappa);
        } else {
            std::cout << std::format(
                "  {:20s}  [chemical]      m_dry={:.0f}kg  propellant={:.0f}kg  "
                "dv={:.0f}m/s  accel={:.4f}m/s^2\n",
                sc.name, sc.dry_mass_kg, sc.propellant_capacity_kg,
                sc.max_delta_v_mps, sc.cruise_accel_mps2);
        }
    }
}

void print_economy_summary(
    const spacetrains::domain::UniverseDefinition& universe,
    const spacetrains::economy::EconomySystem& economy) {
    std::cout << "\n=== Station Economy (net rates, units/day) ===\n";
    for (const auto& station : universe.stations) {
        const auto rates = economy.get_profile_net_rates(station.economy_profile_id);
        std::cout << std::format("  {:30s}  profile={}\n", station.name, station.economy_profile_id);
        for (const auto& [commodity, rate] : rates) {
            if (std::abs(rate) > 0.001) {
                std::cout << std::format("    {:15s}  {:+.2f}/day\n", commodity, rate);
            }
        }
    }
}

void print_station_inventories(
    const spacetrains::domain::SimulationSnapshot& snap,
    const spacetrains::domain::UniverseDefinition& universe) {
    std::cout << "  Stations:\n";
    for (const auto& ss : snap.stations) {
        // Find station name
        std::string name = ss.station_id;
        for (const auto& sd : universe.stations) {
            if (sd.id == ss.station_id) { name = sd.name; break; }
        }
        std::cout << std::format("    {:30s}", name);
        for (const auto& [commodity, amount] : ss.inventory) {
            if (amount > 0.1) {
                std::cout << std::format("  {}={:.1f}", commodity, amount);
            }
        }
        std::cout << "\n";
    }
}

void print_ship_phases(
    const spacetrains::domain::SimulationSnapshot& snap,
    const spacetrains::domain::UniverseDefinition& universe) {
    int idle = 0, awaiting = 0, transit = 0, stranded = 0;
    std::cout << "  Ships:\n";
    for (const auto& ship : snap.ships) {
        std::string class_name = ship.class_id;
        std::string propulsion;
        for (const auto& sc : universe.ship_classes) {
            if (sc.id == ship.class_id) { class_name = sc.name; propulsion = sc.propulsion_type; break; }
        }
        const char* phase_str = "?";
        switch (ship.phase) {
            case spacetrains::domain::ShipMissionPhase::Idle:            phase_str = "idle"; ++idle; break;
            case spacetrains::domain::ShipMissionPhase::AwaitingDeparture: phase_str = "awaiting"; ++awaiting; break;
            case spacetrains::domain::ShipMissionPhase::InTransit:       phase_str = "in_transit"; ++transit; break;
            case spacetrains::domain::ShipMissionPhase::Stranded:        phase_str = "stranded"; ++stranded; break;
            case spacetrains::domain::ShipMissionPhase::Refueling:       phase_str = "refueling"; break;
        }
        std::cout << std::format(
            "    {:20s}  [{:12s}]  {:10s}  fuel={:.0f}kg",
            ship.name, propulsion, phase_str, ship.propellant_kg);
        if (ship.phase == spacetrains::domain::ShipMissionPhase::InTransit
            || ship.phase == spacetrains::domain::ShipMissionPhase::AwaitingDeparture) {
            std::cout << std::format("  -> {}  arr=day{:.1f}",
                ship.active_mission.destination_station_id,
                ship.active_mission.arrival_time_s / kDayS);
        }
        std::cout << "\n";
    }
    std::cout << std::format(
        "  Phase summary: idle={} awaiting={} in_transit={} stranded={}\n",
        idle, awaiting, transit, stranded);
}

}  // namespace

int main(int argc, char** argv) {
    std::string data_root_str;
    int sim_days = 365;
    bool verbose = false;
    int report_interval_days = 30;

    // Parse arguments
    std::vector<std::string> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--days" && i + 1 < args.size()) {
            sim_days = std::stoi(args[++i]);
        } else if (args[i] == "--report-interval" && i + 1 < args.size()) {
            report_interval_days = std::stoi(args[++i]);
        } else if (args[i] == "--verbose" || args[i] == "-v") {
            verbose = true;
        } else if (args[i][0] != '-') {
            data_root_str = args[i];
        }
    }
    if (data_root_str.empty()) {
        data_root_str = std::filesystem::current_path().string();
    }
    const std::filesystem::path data_root = std::filesystem::path(data_root_str) / "data";

    std::cout << "SpaceTrains Headless Simulation\n";
    std::cout << std::format("Data root: {}\n", data_root.string());
    std::cout << std::format("Simulating {} days, report every {} days\n", sim_days, report_interval_days);

    auto sim = spacetrains::simulation::Simulation::from_data_root(data_root.string());

    // Startup summaries
    print_celestial_summary(sim.universe());
    print_ship_class_summary(sim.universe());
    print_economy_summary(sim.universe(), sim.economy_system());

    std::cout << "\n=== Running Simulation ===\n";

    // Timewarp: each step() call advances one real second × timewarp.
    // Use 1-day steps for legible reports.
    sim.set_timewarp(kDayS);  // 1 real second = 1 simulated day per step

    double last_report_day = 0.0;
    int total_events = 0;

    for (int step = 0; step < sim_days; ++step) {
        sim.step(1.0);  // advance 1 simulated day

        const double game_day = sim.snapshot().game_time_s / kDayS;

        if (verbose) {
            for (const auto& event : sim.snapshot().recent_events) {
                // Only print events newer than the last step
                if (event.time_s > (step * kDayS) && event.time_s <= ((step + 1) * kDayS)) {
                    std::cout << std::format("[day {:7.1f}] {}\n", event.time_s / kDayS, event.text);
                    ++total_events;
                }
            }
        }

        if (game_day - last_report_day >= report_interval_days) {
            last_report_day = game_day;
            const auto snap = sim.snapshot();
            std::cout << std::format("\n--- Day {:.1f} ---\n", game_day);
            print_station_inventories(snap, sim.universe());
            print_ship_phases(snap, sim.universe());
            if (!verbose) {
                std::cout << "  Recent events:\n";
                for (const auto& event : snap.recent_events) {
                    std::cout << std::format("    [day {:7.1f}] {}\n", event.time_s / kDayS, event.text);
                }
            }
        }
    }

    std::cout << "\n=== Final Report ===\n";
    std::cout << sim.build_report();
    return 0;
}
