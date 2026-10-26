// The trajectory audit (--trajectory-audit) and sweep (--trajectory-sweep).

#include "app/Reports.hpp"

namespace spacetrains::app {

namespace {

std::string join_flags(const std::vector<std::string>& flags) {
    std::string out;
    for (const auto& flag : flags) {
        if (!out.empty()) out += ",";
        out += flag;
    }
    return out.empty() ? "ok" : out;
}

}  // namespace


void print_trajectory_record(const spacetrains::trajectory::TrajectoryAuditRecord& r) {
    const auto& d = r.diagnostics;
    const auto& m = r.metrics;
    std::cout << std::format(
        "[traj day {:7.1f}] {} ({}) {} -> {}  {}",
        r.planned_at_s / kDayS, r.ship_name, r.class_id,
        r.origin_station_id, r.destination_station_id, r.trajectory_type);
    if (r.trajectory_type == "variable_isp") {
        std::cout << std::format(
            "  seed={} iters={} windows={} rho={:.3f} kappa={:.3f} theta={:.3f}->{:.3f} rEnd/rho={:.3f}",
            d.seed_source, d.refine_iterations, d.windows_tried, d.rho, d.kappa, d.theta_target_rad,
            d.theta_actual_rad, d.r_end_canonical_ratio);
    }
    std::cout << std::format(
        "\n      miss={:.4f}AU start_miss={:.4f}AU rev={:.2f} max_step={:.1f}deg end_turn={:.1f}deg"
        " max_turn={:.1f}deg last_seg={:.1f}x wait_rev={:.2f} wait_step={:.1f}deg r=[{:.3f},{:.3f}]AU fuel={:.0f}kg wait={:.1f}d coast={:.1f}d samples={} plan={:.0f}ms\n"
        "      flags={}\n",
        d.endpoint_miss_m / kAU, d.start_miss_m / kAU, m.revolutions, m.max_step_deg, m.end_turn_deg,
        m.max_interior_turn_deg, m.last_segment_ratio, m.wait_revolutions, m.wait_max_step_deg, m.min_radius_m / kAU, m.max_radius_m / kAU,
        r.planning_propellant_kg, r.wait_time_s / kDayS, r.coast_time_s / kDayS, m.transfer_sample_count, r.plan_ms,
        join_flags(m.flags));
}

void print_trajectory_audit_summary(const std::vector<spacetrains::trajectory::TrajectoryAuditRecord>& records) {
    std::cout << "\n=== Trajectory Audit Summary ===\n";
    std::map<std::string, std::vector<const spacetrains::trajectory::TrajectoryAuditRecord*>> by_type;
    for (const auto& r : records) {
        std::string key = r.trajectory_type;
        if (key == "variable_isp") key += "/" + r.diagnostics.seed_source;
        by_type[key].push_back(&r);
    }
    for (const auto& [type, group] : by_type) {
        std::vector<double> misses;
        double max_rev = 0.0;
        std::size_t flagged = 0;
        std::map<std::string, int> flag_counts;
        for (const auto* r : group) {
            misses.push_back(r->diagnostics.endpoint_miss_m / kAU);
            max_rev = std::max(max_rev, r->metrics.revolutions);
            if (!r->metrics.flags.empty()) ++flagged;
            for (const auto& f : r->metrics.flags) ++flag_counts[f];
        }
        std::vector<double> plan_ms;
        for (const auto* r : group) plan_ms.push_back(r->plan_ms);
        std::sort(plan_ms.begin(), plan_ms.end());
        std::sort(misses.begin(), misses.end());
        const auto pct = [&](double q) { return misses[static_cast<std::size_t>(q * (misses.size() - 1))]; };
        std::cout << std::format(
            "  {:28s} plans={:5d} flagged={:5d}  miss AU p50={:.4f} p95={:.4f} max={:.4f}  max_rev={:.2f}"
            "  plan ms p50={:.1f} p99={:.1f} max={:.0f}\n",
            type, group.size(), flagged, pct(0.5), pct(0.95), misses.back(), max_rev,
            plan_ms[plan_ms.size() / 2], plan_ms[static_cast<std::size_t>(0.99 * (plan_ms.size() - 1))], plan_ms.back());
        for (const auto& [flag, count] : flag_counts) {
            std::cout << std::format("      {:20s} {}\n", flag, count);
        }
    }

    // Worst cases per planner family, so one family's outliers don't hide another's.
    const auto print_worst = [&](const char* title, auto key) {
        for (const char* family : {"keplerian", "variable_isp"}) {
            std::vector<const spacetrains::trajectory::TrajectoryAuditRecord*> sorted;
            for (const auto& r : records) {
                if (r.trajectory_type.starts_with(family)) sorted.push_back(&r);
            }
            std::sort(sorted.begin(), sorted.end(), [&](auto* a, auto* b) { return key(*a) > key(*b); });
            std::cout << std::format("\n  Worst {} by {}:\n", family, title);
            for (std::size_t i = 0; i < std::min<std::size_t>(5, sorted.size()); ++i) {
                print_trajectory_record(*sorted[i]);
            }
        }
    };
    print_worst("endpoint miss", [](const auto& r) { return r.diagnostics.endpoint_miss_m; });
    print_worst("revolutions", [](const auto& r) { return r.metrics.revolutions; });
    print_worst("end turn", [](const auto& r) { return r.metrics.end_turn_deg; });
    print_worst("wait revolutions", [](const auto& r) { return r.metrics.wait_revolutions; });
    print_worst("planning time", [](const auto& r) { return r.plan_ms; });
}

void dump_flagged_trajectories(
    const std::vector<spacetrains::trajectory::TrajectoryAuditRecord>& records,
    const std::string& path) {
    std::ofstream out(path);
    out << "record,planned_day,ship,type,seed,flags,sample,t_day,x_au,z_au\n";
    std::size_t dumped = 0;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        if (r.metrics.flags.empty()) continue;
        ++dumped;
        auto flags_field = join_flags(r.metrics.flags);
        std::replace(flags_field.begin(), flags_field.end(), ',', '|');
        for (std::size_t k = 0; k < r.sampled_path.size(); ++k) {
            const double t = k < r.sampled_times_s.size() ? r.sampled_times_s[k] : 0.0;
            out << std::format("{},{:.2f},{},{},{},{},{},{:.3f},{:.6f},{:.6f}\n",
                i, r.planned_at_s / kDayS, r.ship_name, r.trajectory_type, r.diagnostics.seed_source,
                flags_field, k, t / kDayS, r.sampled_path[k].x / kAU, r.sampled_path[k].z / kAU);
        }
    }
    std::cout << std::format("Dumped {} flagged trajectories to {}\n", dumped, path);
}

// Plan every interplanetary transfer over a grid of station pairs, fuel levels and
// departure days, bypassing the economy, so rare atlas regions get exercised.
std::vector<spacetrains::trajectory::TrajectoryAuditRecord> run_trajectory_sweep(
    const std::filesystem::path& data_root,
    const spacetrains::domain::UniverseDefinition& universe,
    const spacetrains::celestial::CelestialMechanics& mechanics,
    int sweep_days,
    int sweep_step_days) {
    spacetrains::variable_isp::VariableIspAtlas atlas;
    atlas.load_binary((data_root.parent_path() / "tests" / "data" / "variable_isp" / "variable_isp_atlas.bin").string());
    spacetrains::trajectory::VariableIspTrajectoryPlanner ion_planner(universe, mechanics, atlas);
    spacetrains::trajectory::KeplerTrajectoryPlanner kepler_planner(universe, mechanics);

    const std::vector<double> fuel_fractions {0.03, 0.06, 0.1, 0.2, 0.35, 0.5, 0.75, 1.0};
    struct SweepJob {
        const spacetrains::domain::ShipClassDefinition* ship_class;
        const spacetrains::domain::StationDefinition* origin;
        const spacetrains::domain::StationDefinition* destination;
        double fuel_fraction;
    };
    std::vector<SweepJob> jobs;
    for (const auto& ship_class : universe.ship_classes) {
        for (const auto& origin : universe.stations) {
            for (const auto& destination : universe.stations) {
                if (origin.parent_body_id == destination.parent_body_id) continue;
                for (const double fraction : fuel_fractions) {
                    jobs.push_back({&ship_class, &origin, &destination, fraction});
                }
            }
        }
    }

    // Planners are const and stateless, so jobs run on a simple worker pool;
    // results are merged in job order to keep the output deterministic.
    std::vector<std::vector<spacetrains::trajectory::TrajectoryAuditRecord>> job_records(jobs.size());
    std::atomic<std::size_t> next_job {0};
    std::atomic<std::size_t> attempted {0};
    std::atomic<std::size_t> integration_failures {0};
    // Watchdog: each worker publishes its current job and day; plans that run
    // for more than 10 s are reported (planner hangs are otherwise invisible).
    const unsigned thread_count = std::max(1u, std::thread::hardware_concurrency());
    struct InFlight {
        std::atomic<std::size_t> job {SIZE_MAX};
        std::atomic<int> day {0};
        std::atomic<std::int64_t> started_ms {0};
    };
    std::vector<InFlight> in_flight(thread_count);
    std::atomic<unsigned> next_slot {0};
    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    const auto worker = [&] {
        auto& slot = in_flight[next_slot++];
        for (std::size_t j = next_job++; j < jobs.size(); j = next_job++) {
            const auto& [ship_class, origin, destination, fraction] = jobs[j];
            const auto& planner = ship_class->propulsion_type == "variable_isp"
                ? static_cast<const spacetrains::trajectory::ITrajectoryPlanner&>(ion_planner)
                : static_cast<const spacetrains::trajectory::ITrajectoryPlanner&>(kepler_planner);
            spacetrains::domain::ShipState ship;
            ship.name = ship_class->id;
            ship.class_id = ship_class->id;
            ship.propellant_kg = fraction * ship_class->propellant_capacity_kg;
            for (int day = 0; day < sweep_days; day += sweep_step_days) {
                const double t = day * kDayS;
                ++attempted;
                slot.job = j;
                slot.day = day;
                slot.started_ms = now_ms();
                const auto started = std::chrono::steady_clock::now();
                const auto plan = planner.plan_transfer(*origin, *destination, ship, *ship_class, t);
                const double plan_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                if (plan_ms > 1000.0) {
                    std::cout << std::format("[slow plan {:.0f} ms] {} {} -> {} fuel={:.0f}% day={} feasible={} {}\n",
                        plan_ms, ship_class->id, origin->id, destination->id, fraction * 100.0, day, plan.feasible, plan.summary);
                }
                slot.job = SIZE_MAX;
                if (plan.summary.find("integration failed") != std::string::npos) {
                    if (integration_failures++ < 20) {
                        std::cout << std::format("[integration failure] {} fuel={:.0f}% day={} ({:.0f} ms): {}\n",
                            ship_class->id, fraction * 100.0, day, plan_ms, plan.summary);
                    }
                }
                if (!plan.feasible) continue;
                const double r_origin = mechanics.get_heliocentric_radius(origin->parent_body_id, plan.departure_time_s);
                const double r_dest = mechanics.get_heliocentric_radius(destination->parent_body_id, plan.arrival_time_s);
                auto metrics = spacetrains::trajectory::audit_trajectory(plan);
                const bool flagged = !metrics.flags.empty();
                job_records[j].push_back({
                    .planned_at_s = t,
                    .ship_name = std::format("{}@{:.0f}%", ship_class->id, fraction * 100.0),
                    .class_id = ship_class->id,
                    .origin_station_id = origin->id,
                    .destination_station_id = destination->id,
                    .trajectory_type = plan.trajectory_type,
                    .planning_propellant_kg = ship.propellant_kg,
                    .wait_time_s = plan.wait_time_s,
                    .coast_time_s = plan.coast_time_s,
                    .r_origin_m = r_origin,
                    .r_dest_m = r_dest,
                    .plan_ms = plan_ms,
                    .diagnostics = plan.diagnostics,
                    .metrics = std::move(metrics),
                    .sampled_path = flagged ? plan.sampled_path : std::vector<spacetrains::math::Vec3d>{},
                    .sampled_times_s = flagged ? plan.sampled_times_s : std::vector<double>{},
                });
            }
        }
    };
    {
        std::vector<std::jthread> threads;
        for (unsigned i = 0; i < thread_count; ++i) {
            threads.emplace_back(worker);
        }
        std::jthread watchdog([&](std::stop_token stop) {
            std::vector<std::pair<std::size_t, int>> reported;
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                for (auto& f : in_flight) {
                    const std::size_t j = f.job;
                    const int day = f.day;
                    if (j == SIZE_MAX || now_ms() - f.started_ms < 10000) continue;
                    if (std::find(reported.begin(), reported.end(), std::pair{j, day}) != reported.end()) continue;
                    reported.emplace_back(j, day);
                    const auto& job = jobs[j];
                    std::cout << std::format("[hung plan >10s] {} {} -> {} fuel={:.0f}% day={}\n",
                        job.ship_class->id, job.origin->id, job.destination->id, job.fuel_fraction * 100.0, day)
                              << std::flush;
                }
            }
        });
        for (auto& t : threads) t.join();
    }
    std::vector<spacetrains::trajectory::TrajectoryAuditRecord> records;
    for (auto& group : job_records) {
        std::move(group.begin(), group.end(), std::back_inserter(records));
    }
    std::cout << std::format("Sweep: {} plans attempted, {} feasible, {} integration failures\n",
        attempted.load(), records.size(), integration_failures.load());
    return records;
}

}  // namespace spacetrains::app
