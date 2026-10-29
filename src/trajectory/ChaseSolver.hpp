#pragma once

// Game-facing adapter for the general variable-Isp solver (variable_isp/GeneralTransfer.hpp):
// chase another ship's planned path, or a wreck's coast, from an arbitrary heliocentric state.
// Used by salvage runs and police cutters.

#include "domain/Types.hpp"
#include "math/Vec3d.hpp"
#include "variable_isp/GeneralTransfer.hpp"

#include <vector>

namespace spacetrains::trajectory {

// Normalisation of heliocentric SI states (the game's x-z plane) for the general solver: μ = 1
// and lengths in `length_m` (pick a scale near the chase, e.g. the chaser's solar distance).
struct ChaseUnits {
    double mu_m3_s2 {1.32712440018e20};
    double length_m {1.495978707e11};

    [[nodiscard]] double time_s() const;
    [[nodiscard]] double speed_mps() const;
    [[nodiscard]] double accel_mps2() const;
    [[nodiscard]] variable_isp::general::State to_normalised(const math::Vec3d& r_m, const math::Vec3d& v_mps) const;
    [[nodiscard]] math::Vec3d position_m(const variable_isp::general::Vec2& r) const;
};

// Target that follows a planned path (sampled_path / sampled_times_s) as a conic per segment
// (trajectory/PathMotion.hpp), starting `depart_time_s` (the chaser's departure). Past the
// path's ends it coasts on the end segments' conics.
variable_isp::general::Target path_target(const domain::TrajectoryPlan& plan, double depart_time_s, const ChaseUnits& units);

// Target coasting on a two-body orbit from the SI state (r, v) at the chaser's departure
// (analytic, trajectory/KeplerPropagation.hpp): a derelict.
variable_isp::general::Target kepler_target(const math::Vec3d& r_m, const math::Vec3d& v_mps, const ChaseUnits& units);

struct ChaseRequest {
    math::Vec3d position_m;
    math::Vec3d velocity_mps;
    double depart_time_s {0.0};
    double power_w {0.0};       // jet power
    double initial_mass_kg {0.0};
    double dry_mass_kg {0.0};   // mass at which the chase must be over
    variable_isp::general::EndCondition end_condition {variable_isp::general::EndCondition::Intercept};
    double min_transfer_s {86400.0};
    double max_transfer_s {365.0 * 86400.0};
    double min_perihelion_m {0.1 * 1.495978707e11};
};

struct ChasePlan {
    bool feasible {false};
    double transfer_time_s {0.0};
    double propellant_kg {0.0};
    std::vector<math::Vec3d> path_m;
    std::vector<double> times_s;
};

// Earliest interception (or rendezvous) the propellant allows (a race: the fastest, whatever it costs).
ChasePlan plan_chase(const ChaseRequest& request, const variable_isp::general::Target& target, const ChaseUnits& units,
    std::size_t path_samples = 200);

// Cheapest meeting (rendezvous or intercept at a fixed time, minimum effort for that time): the
// objective is propellant + `kg_per_s` × time, the owner's time cost in propellant at its fuel
// price, as the trade planners weigh a trip. The load (initial − dry mass) bounds the propellant.
struct MeetingCandidate {
    double transfer_time_s {0.0};
    double propellant_kg {0.0};
    double objective {0.0};
    double bracket_lo_s {0.0};   // the grid neighbours, for refinement
    double bracket_hi_s {0.0};
    variable_isp::general::Primer primer {};
};

// The feasible points of a grid of meeting times over [earliest_s, latest_s] (geometric over a
// wide window), warm-started in turn, cheapest first.
std::vector<MeetingCandidate> meeting_candidates(const ChaseRequest& request, const variable_isp::general::Target& target,
    const ChaseUnits& units, double earliest_s, double latest_s, double kg_per_s, std::size_t grid = 10);

// Golden-section refinement of a candidate between its grid neighbours, and its path.
ChasePlan refine_meeting(const ChaseRequest& request, const variable_isp::general::Target& target, const ChaseUnits& units,
    double kg_per_s, const MeetingCandidate& start, std::size_t iterations = 6, std::size_t path_samples = 60);

// Grid, then refinement of the cheapest point.
ChasePlan plan_cheapest(const ChaseRequest& request, const variable_isp::general::Target& target, const ChaseUnits& units,
    double earliest_s, double latest_s, double kg_per_s, std::size_t grid = 10, std::size_t refine_iterations = 6,
    std::size_t path_samples = 60);

// The best place on a target's path to meet it (police inspections): rendezvous at fixed times
// spread over [earliest_s, latest_s] after departure (`grid` of them, warm-started in turn),
// each at minimum effort; the one with the least propellant + `kg_per_s` × time that the load
// allows. `transfer_time_s` is the meeting time after departure.
ChasePlan plan_meeting(const ChaseRequest& request, const variable_isp::general::Target& target, const ChaseUnits& units,
    double earliest_s, double latest_s, double kg_per_s, std::size_t grid = 10, std::size_t path_samples = 60);

}  // namespace spacetrains::trajectory
