#pragma once

// A planned transfer and its diagnostics.

#include <cstdint>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "math/Vec3d.hpp"

namespace spacetrains::domain {

// Planner-internal facts that the final path no longer shows (endpoints are
// snapped onto the stations). Used by the trajectory audit.
struct TrajectoryDiagnostics {
    double start_miss_m {0.0};      // unsnapped first sample -> origin station
    double endpoint_miss_m {0.0};   // unsnapped last sample -> destination station
    // VariableISP only.
    double rho {0.0};
    double kappa {0.0};
    double theta_target_rad {0.0};  // launch-window theta from the atlas grid
    double theta_actual_rad {0.0};  // theta reached by the integrated trajectory
    double r_end_canonical_ratio {0.0};  // integrated final radius / canonical rho
    std::string seed_source;        // refined (atlas cell seed after endpoint shooting)
    std::size_t refine_iterations {0};
    std::size_t windows_tried {0};  // launch windows attempted before one converged
};

struct TrajectoryPlan {
    bool feasible {false};
    double departure_time_s {0.0};
    double arrival_time_s {0.0};
    double wait_time_s {0.0};
    double coast_time_s {0.0};
    double travel_time_s {0.0};
    double propellant_required_kg {0.0};  // burned on the transfer
    double propellant_load_kg {0.0};      // aboard at departure (what was aboard plus any purchase)
    double propellant_boiled_kg {0.0};    // hydrogen expected to boil off in flight (in the load, not burned)
    std::vector<math::Vec3d> sampled_path;
    std::vector<double> sampled_times_s;
    std::vector<double> sampled_propellant_kg;
    std::string summary;
    std::string trajectory_type;  // keplerian_local | keplerian_lambert | keplerian_hohmann | variable_isp
    TrajectoryDiagnostics diagnostics;
    // False when planned without a rendering path (PlanningOptions::include_path off).
    bool has_render_path {true};
};

}  // namespace spacetrains::domain
