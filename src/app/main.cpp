// Headless runs: simulate, print reports and audits, write side files (see README).

#include "app/Reports.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "domain/ShipDesign.hpp"
#include "celestial/CelestialMechanics.hpp"
#include "economy/EconomySystem.hpp"
#include "simulation/Simulation.hpp"
#include "trajectory/TrajectoryAudit.hpp"
#include "trajectory/TrajectoryPlanner.hpp"
#include "trajectory/VariableIspTrajectoryPlanner.hpp"
#include "util/Profiling.hpp"
#include "variable_isp/VariableIsp.hpp"

using namespace spacetrains::app;

int main(int argc, char** argv) {
    std::string data_root_str;
    int sim_days = 365;
    bool verbose = false;
    bool econ_audit = false;
    int report_interval_days = 30;
    bool trajectory_audit = false;
    std::string trajectory_dump_path;
    int sweep_step_days = 0;
    bool profile = false;
    double step_days = 1.0;
    std::string load_path;
    std::string save_path;
    double start_day = 0.0;
    std::string snapshot_json_path;
    std::string stats_json_path;
    bool no_events = false;
    int save_at_day = -1;
    std::string write_reference_prices_path;
    std::string write_reference_fares_path;

    // Parse arguments
    std::vector<std::string> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--days" && i + 1 < args.size()) {
            sim_days = std::stoi(args[++i]);
        } else if (args[i] == "--report-interval" && i + 1 < args.size()) {
            report_interval_days = std::stoi(args[++i]);
        } else if (args[i] == "--verbose" || args[i] == "-v") {
            verbose = true;
        } else if (args[i] == "--no-events") {
            no_events = true;
        } else if (args[i] == "--econ-audit") {
            econ_audit = true;
        } else if (args[i] == "--snapshot-json" && i + 1 < args.size()) {
            snapshot_json_path = args[++i];
        } else if (args[i] == "--stats-json" && i + 1 < args.size()) {
            stats_json_path = args[++i];
        } else if (args[i] == "--start-day" && i + 1 < args.size()) {
            start_day = std::stod(args[++i]);
        } else if (args[i] == "--load" && i + 1 < args.size()) {
            load_path = args[++i];
        } else if (args[i] == "--save-at" && i + 2 < args.size()) {
            save_at_day = std::stoi(args[++i]);
            save_path = args[++i];
        } else if (args[i] == "--write-reference-prices" && i + 1 < args.size()) {
            write_reference_prices_path = args[++i];
        } else if (args[i] == "--write-reference-fares" && i + 1 < args.size()) {
            write_reference_fares_path = args[++i];
        } else if (args[i] == "--profile") {
            profile = true;
        } else if (args[i] == "--step-days" && i + 1 < args.size()) {
            step_days = std::stod(args[++i]);
        } else if (args[i] == "--trajectory-audit") {
            trajectory_audit = true;
        } else if (args[i] == "--trajectory-sweep" && i + 1 < args.size()) {
            sweep_step_days = std::stoi(args[++i]);
        } else if (args[i] == "--trajectory-dump" && i + 1 < args.size()) {
            trajectory_audit = true;
            trajectory_dump_path = args[++i];
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

    if (profile) {
        spacetrains::profiling::set_enabled(true);
    }
    const auto load_start = std::chrono::steady_clock::now();
    auto sim = spacetrains::simulation::Simulation::from_data_root(data_root.string());
    sim.set_events_enabled(!no_events);
    if (!write_reference_prices_path.empty()) {
        // Plans the reference trips (step 15) and writes data/economy/reference_prices.csv.
        const auto prices = sim.compute_reference_prices();
        std::ofstream file(write_reference_prices_path);
        file << "station_id,commodity_id,reference_price,base_price,transport_per_unit,producer_id,class_id,round_trip_days\n";
        for (const auto& price : prices) {
            file << std::format("{},{},{:.2f},{:.2f},{:.2f},{},{},{:.1f}\n", price.station_id, price.commodity_id,
                price.reference_price, price.base_price, price.transport_per_unit, price.producer_id, price.class_id,
                price.round_trip_days);
            std::cout << std::format("{:16s} {:13s} {:9.2f} = {:7.2f} base + {:8.2f} carriage  from {:16s} by {:20s} ({:.0f} d round trip)\n",
                price.station_id, price.commodity_id, price.reference_price, price.base_price, price.transport_per_unit,
                price.producer_id, price.class_id, price.round_trip_days);
        }
        std::cout << std::format("Wrote {} reference prices to {}\n", prices.size(), write_reference_prices_path);
        return 0;
    }
    if (!write_reference_fares_path.empty()) {
        // Plans the reference passenger trips and writes data/economy/reference_fares.csv.
        const auto fares = sim.compute_reference_fares();
        std::ofstream file(write_reference_fares_path);
        file << "origin_id,destination_id,fare_cr,days,class_id,fuel_out_kg,fuel_back_kg\n";
        for (const auto& fare : fares) {
            file << std::format("{},{},{:.2f},{:.1f},{},{:.1f},{:.1f}\n", fare.origin_id, fare.destination_id, fare.fare_cr,
                fare.days, fare.class_id, fare.fuel_out_kg, fare.fuel_back_kg);
            std::cout << std::format("{:16s} -> {:16s} {:8.1f} cr  {:6.1f} d  by {}\n", fare.origin_id,
                fare.destination_id, fare.fare_cr, fare.days, fare.class_id);
        }
        std::cout << std::format("Wrote {} reference fares to {}\n", fares.size(), write_reference_fares_path);
        return 0;
    }
    const double load_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - load_start).count();
    if (start_day > 0.0) {
        sim.start_at(start_day * kDayS);
    }
    if (!load_path.empty()) {
        std::ifstream file(load_path, std::ios::binary);
        if (!file) {
            std::cerr << "Cannot read save " << load_path << "\n";
            return 1;
        }
        sim.load_state_json(std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()));
        std::cout << std::format("Loaded {} (day {:.1f}); simulating {} more days\n", load_path, sim.game_time_s() / kDayS, sim_days);
    }
    sim.set_trajectory_audit_enabled(trajectory_audit);
    if (sweep_step_days > 0) {
        spacetrains::celestial::CelestialMechanics sweep_mechanics(sim.universe());
        const auto records = run_trajectory_sweep(data_root, sim.universe(), sweep_mechanics, sim_days, sweep_step_days);
        print_trajectory_audit_summary(records);
        if (!trajectory_dump_path.empty()) {
            dump_flagged_trajectories(records, trajectory_dump_path);
        }
        return 0;
    }
    std::size_t audit_records_printed = 0;
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

    // Reports count from the start (a --start-day or a loaded game starts later than day 0).
    const double first_day = sim.game_time_s() / kDayS;
    double last_report_day = first_day;
    int total_events = 0;
    // Per-step wall time (profiling): the bridge shows a hitch whenever one step is slow.
    std::vector<double> step_wall_s;
    // and the running totals of its main phases (SPACETRAINS_PROFILE_STEPS).
    const std::array step_phases {spacetrains::profiling::Phase::FleetInvestment, spacetrains::profiling::Phase::ShipReview,
        spacetrains::profiling::Phase::ChooseMission, spacetrains::profiling::Phase::ConsiderRefit};
    std::array<std::vector<double>, 4> step_phase_s;
    const auto run_start = std::chrono::steady_clock::now();
    const int steps_per_day = std::max(1, static_cast<int>(std::lround(1.0 / step_days)));

    for (int step = 0; step < sim_days; ++step) {
        for (int sub = 0; sub < steps_per_day; ++sub) {
            const auto step_start = std::chrono::steady_clock::now();
            sim.step(1.0 / steps_per_day);  // advance 1 simulated day in steps_per_day steps
            if (profile) {
                step_wall_s.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now() - step_start).count());
                using spacetrains::profiling::Phase;
                for (std::size_t p = 0; p < step_phases.size(); ++p) {
                    step_phase_s[p].push_back(spacetrains::profiling::total_seconds(step_phases[p]));
                }
            }
        }

        const double game_day = sim.snapshot().game_time_s / kDayS;
        if (save_at_day >= 0 && step + 1 == save_at_day) {
            std::ofstream file(save_path, std::ios::binary | std::ios::trunc);
            file << sim.save_state_json();
            std::cout << std::format("Saved day {:.1f} to {}\n", game_day, save_path);
        }

        if (trajectory_audit) {
            const auto& records = sim.trajectory_audit_records();
            for (; audit_records_printed < records.size(); ++audit_records_printed) {
                if (!records[audit_records_printed].metrics.flags.empty()) {
                    print_trajectory_record(records[audit_records_printed]);
                }
            }
        }

        if (verbose) {
            for (const auto& event : sim.snapshot().recent_events) {
                // Only print events newer than the last step
                if (event.time_s > ((first_day + step) * kDayS) && event.time_s <= ((first_day + step + 1) * kDayS)) {
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
            if (econ_audit) {
                print_economy_audit(snap, sim.universe(), sim.economy_system(), sim.delivered_price_ratio(), sim.radiation_audit(),
                    [&sim](const auto& station) { return sim.station_lifetime_profit(station); });
                print_passenger_audit(snap, sim.universe(), sim.passenger_audit(), sim.rotation_tours());
                print_capacity_balance(sim.universe(), sim.economy_system(), snap);
                print_accounts_audit(sim);
            }
            if (!verbose) {
                std::cout << "  Recent events:\n";
                for (const auto& event : snap.recent_events) {
                    std::cout << std::format("    [day {:7.1f}] {}\n", event.time_s / kDayS, event.text);
                }
            }
        }
    }

    if (profile) {
        const double run_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - run_start).count();
        std::cout << std::format("\n=== Profile ===\nload {:.2f} s, run {:.2f} s ({:.1f} ms per simulated day), {} steps of {:.3f} d\n",
            load_s, run_s, 1000.0 * run_s / std::max(1, sim_days), step_wall_s.size(), 1.0 / steps_per_day);
        if (!step_wall_s.empty()) {
            auto sorted = step_wall_s;
            std::sort(sorted.begin(), sorted.end());
            const auto pct = [&](double q) { return 1000.0 * sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(q * static_cast<double>(sorted.size())))]; };
            std::size_t over_50ms = 0;
            for (const double s : step_wall_s) {
                over_50ms += s > 0.05 ? 1 : 0;
            }
            std::cout << std::format("step ms: p50 {:.1f}, p90 {:.1f}, p99 {:.1f}, max {:.1f} (first step {:.1f}); {} steps over 50 ms\n",
                pct(0.5), pct(0.9), pct(0.99), 1000.0 * sorted.back(), 1000.0 * step_wall_s.front(), over_50ms);
            // The slowest stretches of game time (after the opening step): the UI's top speed is
            // 5 days a second and the bridge carries up to 4 s of lag, so 5- and 20-day windows matter.
            for (const int days : {1, 5, 20}) {
                const auto window = static_cast<std::size_t>(days * steps_per_day);
                if (step_wall_s.size() <= window + 1) {
                    continue;
                }
                double sum = 0.0, worst = 0.0;
                std::size_t worst_end = 0;
                for (std::size_t i = 1; i < step_wall_s.size(); ++i) {
                    sum += step_wall_s[i] - (i > window ? step_wall_s[i - window] : 0.0);
                    if (i >= window && sum > worst) {
                        worst = sum;
                        worst_end = i;
                    }
                }
                std::cout << std::format("slowest {}-day stretch: {:.2f} s ({:.1f} days/s), from day {:.0f}\n", days, worst,
                    days / worst, static_cast<double>(worst_end + 1 - window) / steps_per_day);
            }
        }
        std::cout << spacetrains::profiling::report();
        // Debug aid: SPACETRAINS_PROFILE_STEPS=<file> writes each step's wall time and the time of
        // its fleet investment, ship reviews, mission choices and refit checks (ms), one step per line.
        if (const char* steps_path = std::getenv("SPACETRAINS_PROFILE_STEPS"); steps_path != nullptr) {
            std::ofstream steps_file(steps_path, std::ios::trunc);
            steps_file << "day wall invest review choose refit\n";
            for (std::size_t i = 0; i < step_wall_s.size(); ++i) {
                steps_file << std::format("{:.2f} {:.2f}", static_cast<double>(i) / steps_per_day, 1000.0 * step_wall_s[i]);
                for (const auto& totals : step_phase_s) {
                    steps_file << std::format(" {:.2f}", 1000.0 * (totals[i] - (i > 0 ? totals[i - 1] : 0.0)));
                }
                steps_file << "\n";
            }
        }
    }

    if (!snapshot_json_path.empty()) {
        std::ofstream file(snapshot_json_path, std::ios::binary | std::ios::trunc);
        file << sim.build_bridge_snapshot_json(false, 0, 0.0);
        // The planned paths, as the bridge writes them next to its snapshot.
        std::ofstream paths(snapshot_json_path + ".paths", std::ios::binary | std::ios::trunc);
        paths << sim.build_bridge_paths_json();
        std::ofstream designs(snapshot_json_path + ".designs", std::ios::binary | std::ios::trunc);
        designs << sim.build_bridge_designs_json();
    }

    if (!stats_json_path.empty()) {
        std::ofstream file(stats_json_path, std::ios::binary | std::ios::trunc);
        file << sim.build_stats_json();
    }

    std::cout << "\n=== Final Report ===\n";
    std::cout << sim.build_report();
    if (trajectory_audit) {
        print_trajectory_audit_summary(sim.trajectory_audit_records());
        if (!trajectory_dump_path.empty()) {
            dump_flagged_trajectories(sim.trajectory_audit_records(), trajectory_dump_path);
        }
    }
    return 0;
}
