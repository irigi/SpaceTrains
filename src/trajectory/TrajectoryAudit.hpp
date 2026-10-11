#pragma once

#include <string>
#include <vector>

#include "domain/Types.hpp"

namespace spacetrains::trajectory {

// Geometric sanity metrics for a planned trajectory. Only the transfer part
// (samples at or after departure) is measured; wait-period samples that track
// the origin station are skipped.
struct TrajectoryAuditMetrics {
    std::size_t sample_count {0};
    std::size_t transfer_sample_count {0};
    double revolutions {0.0};         // |unwrapped heliocentric angle swept| / 2π
    double max_step_deg {0.0};        // largest heliocentric angle between consecutive samples
    double min_radius_m {0.0};
    double max_radius_m {0.0};
    double last_segment_ratio {0.0};  // last segment length / median segment length
    double end_turn_deg {0.0};        // direction change at the penultimate sample
    double max_interior_turn_deg {0.0};  // largest direction change elsewhere
    double wait_revolutions {0.0};    // heliocentric angle swept by the wait-period prefix / 2π
    double wait_max_step_deg {0.0};   // coarsest step in the wait-period prefix
    bool times_monotonic {true};
    bool finite {true};
    std::vector<std::string> flags;   // empty = looks physical
};

struct TrajectoryAuditThresholds {
    double endpoint_miss_m {1.5e9};   // 0.01 AU
    double max_revolutions {1.25};
    // Continuous-thrust (variable-Isp) spirals: an inward spiral from Venus to Mercury
    // sweeps 1.3-1.4 turns around the Sun in ~245 days; that is the physics, not a fault.
    double max_revolutions_continuous {2.0};
    double max_step_deg {20.0};       // wait prefix: heliocentric angle per step
    double max_turn_deg {15.0};       // transfer: direction change at one point
    double end_turn_deg {30.0};
    double last_segment_ratio {3.0};
    double min_perihelion_m {1.495978707e10};  // 0.1 AU, same as kMinPerihelionM
};

[[nodiscard]] TrajectoryAuditMetrics audit_trajectory(
    const domain::TrajectoryPlan& plan,
    const TrajectoryAuditThresholds& thresholds = {});

// One accepted mission plan as seen by the audit.
struct TrajectoryAuditRecord {
    double planned_at_s {0.0};
    std::string ship_name;
    std::string class_id;
    std::string origin_station_id;
    std::string destination_station_id;
    std::string trajectory_type;
    double planning_propellant_kg {0.0};
    double wait_time_s {0.0};
    double coast_time_s {0.0};
    double r_origin_m {0.0};
    double r_dest_m {0.0};
    double plan_ms {0.0};             // wall time spent in plan_transfer (sweep only)
    domain::TrajectoryDiagnostics diagnostics;
    TrajectoryAuditMetrics metrics;
    // Kept only for flagged plans, so they can be dumped and plotted.
    std::vector<math::Vec3d> sampled_path;
    std::vector<double> sampled_times_s;
};

}  // namespace spacetrains::trajectory
