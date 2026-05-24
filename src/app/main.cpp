#include <cmath>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <vector>

#include "celestial/CelestialMechanics.hpp"
#include "economy/EconomySystem.hpp"
#include "simulation/Simulation.hpp"
#include "trajectory/TrajectoryPlanner.hpp"
#include "trajectory/VariableIspTrajectoryPlanner.hpp"
#include "variable_isp/VariableIsp.hpp"

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
    constexpr double kG0 = 9.80665;
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
            const double m_wet = sc.dry_mass_kg + sc.propellant_capacity_kg;
            const double mass_ratio = (sc.dry_mass_kg > 0.0) ? m_wet / sc.dry_mass_kg : 0.0;
            const double ve = (mass_ratio > 1.0 && sc.max_delta_v_mps > 0.0)
                ? sc.max_delta_v_mps / std::log(mass_ratio) : 0.0;
            const double isp_s = ve / kG0;
            const double fuel_frac = (m_wet > 0.0) ? sc.propellant_capacity_kg / m_wet : 0.0;
            std::cout << std::format(
                "  {:20s}  [NTR/chemical]  m_dry={:.0f}kg  propellant={:.0f}kg  "
                "dv={:.0f}m/s  ISP={:.0f}s  MR={:.2f}  eps={:.2f}\n",
                sc.name, sc.dry_mass_kg, sc.propellant_capacity_kg,
                sc.max_delta_v_mps, isp_s, mass_ratio, fuel_frac);
        }
    }
}

void print_route_diagnosis(
    const spacetrains::domain::UniverseDefinition& universe,
    const spacetrains::celestial::CelestialMechanics& mechanics) {
    constexpr double kG0 = 9.80665;
    constexpr double PI = 3.14159265358979323846;

    // Find sun
    double mu_sun = 0.0;
    for (const auto& body : universe.bodies) {
        if (body.orbit.parent_id.empty()) { mu_sun = body.mu_m3_s2; break; }
    }

    std::cout << "\n=== Interplanetary Route Diagnosis (t=0) ===\n";
    std::cout << std::format("  {:8s}  {:8s}  {:>10s}  {:>10s}  {:>10s}  {:>12s}  {:>12s}\n",
        "From", "To", "Hohm.dv", "Hohm.trn", "Synodic", "Wait(t=0)", "Arr.(t=0)");

    const std::vector<std::pair<std::string,std::string>> routes = {
        {"earth","mars"}, {"earth","venus"}, {"earth","mercury"}, {"earth","ceres"},
        {"mars","earth"}, {"venus","earth"}, {"mars","ceres"}
    };

    for (const auto& [from, to] : routes) {
        const double r1 = mechanics.get_heliocentric_radius(from, 0.0);
        const double r2 = mechanics.get_heliocentric_radius(to, 0.0);
        if (r1 <= 0.0 || r2 <= 0.0 || mu_sun <= 0.0) continue;
        const double a = (r1 + r2) * 0.5;
        const double hohm_time_s = PI * std::sqrt(a * a * a / mu_sun);
        const double v1 = std::sqrt(mu_sun / r1);
        const double v2 = std::sqrt(mu_sun / r2);
        const double vt1 = std::sqrt(mu_sun * (2.0 / r1 - 1.0 / a));
        const double vt2 = std::sqrt(mu_sun * (2.0 / r2 - 1.0 / a));
        const double dv = std::abs(vt1 - v1) + std::abs(v2 - vt2);

        // Find orbital periods for synodic
        double T1 = 0.0, T2 = 0.0;
        for (const auto& body : universe.bodies) {
            if (body.id == from) T1 = body.orbit.orbital_period_s;
            if (body.id == to)   T2 = body.orbit.orbital_period_s;
        }
        double synodic_days = 0.0;
        if (T1 > 0.0 && T2 > 0.0 && std::abs(1.0/T1 - 1.0/T2) > 1e-20) {
            synodic_days = 1.0 / std::abs(1.0/T1 - 1.0/T2) / kDayS;
        }

        // Compute wait for a light_freighter (any chemical ship will use same Hohmann)
        double wait_days = 0.0;
        {
            const double TAU = 2.0 * PI;
            const double om1 = (T1 > 0.0) ? TAU / T1 : 0.0;
            const double om2 = (T2 > 0.0) ? TAU / T2 : 0.0;
            const double rel = om2 - om1;
            const double ang1 = std::atan2(mechanics.get_body_position(from, 0.0).z,
                                           mechanics.get_body_position(from, 0.0).x);
            const double ang2 = std::atan2(mechanics.get_body_position(to, 0.0).z,
                                           mechanics.get_body_position(to, 0.0).x);
            const double cur_phase = std::fmod(ang2 - ang1 + TAU * 10, TAU);
            const double req_phase = std::fmod(PI - om2 * hohm_time_s + TAU * 10, TAU);
            if (std::abs(rel) > 1e-12) {
                const double synodic_s = TAU / std::abs(rel);
                double w = std::fmod((req_phase - cur_phase) / rel, synodic_s);
                if (w < 0.0) w += synodic_s;
                wait_days = w / kDayS;
            }
        }

        std::cout << std::format("  {:8s}  {:8s}  {:>10.0f}  {:>10.1f}d  {:>10.1f}d  {:>12.1f}d  {:>12.1f}d\n",
            from, to, dv,
            hohm_time_s / kDayS,
            synodic_days,
            wait_days,
            wait_days + hohm_time_s / kDayS);
    }

    // Per ship class: can it do Earth-Mars? Show propellant fraction needed.
    std::cout << "\n=== NTR Ship Class Feasibility: Earth→Mars Hohmann (dv≈5591 m/s) ===\n";
    const double earth_mars_dv = 5591.0;  // m/s (Hohmann, no 250 fudge)
    for (const auto& sc : universe.ship_classes) {
        if (sc.propulsion_type == "electric_ion") continue;
        const double m_wet = sc.dry_mass_kg + sc.propellant_capacity_kg;
        const double mass_ratio = (sc.dry_mass_kg > 0.0) ? m_wet / sc.dry_mass_kg : 0.0;
        const double ve = (mass_ratio > 1.0 && sc.max_delta_v_mps > 0.0)
            ? sc.max_delta_v_mps / std::log(mass_ratio) : 0.0;
        const double isp_s = ve / kG0;
        if (ve <= 0.0) continue;
        const double prop_needed = m_wet * (1.0 - std::exp(-earth_mars_dv / ve));
        const double frac = prop_needed / sc.propellant_capacity_kg;
        std::cout << std::format(
            "  {:20s}  ISP={:.0f}s  MR={:.2f}  prop_needed={:.0f}kg/{:.0f}kg ({:.0f}%)  {}\n",
            sc.name, isp_s, mass_ratio,
            prop_needed, sc.propellant_capacity_kg, frac * 100.0,
            (frac <= 1.0) ? "FEASIBLE" : "INFEASIBLE");
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
    spacetrains::celestial::CelestialMechanics mechanics(sim.universe());

    // Startup summaries
    print_celestial_summary(sim.universe());
    print_ship_class_summary(sim.universe());
    print_economy_summary(sim.universe(), sim.economy_system());
    print_route_diagnosis(sim.universe(), mechanics);

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
