#pragma once

// Dispatch (docs/plans/simulation_split.md, step 5): the best mission for a ship, leg
// estimates and their cache, deferred plans computed in parallel.

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "domain/Types.hpp"
#include "simulation/World.hpp"
#include "trajectory/TrajectoryPlanner.hpp"

namespace spacetrains::simulation::systems {

// Dispatch's best mission for a ship flown as `ship_class` (its own class, or a tank
// variant it could refit to). Planning only: changes nothing but the plan caches.
struct MissionChoice {
    enum class Kind { None, Mission, Reposition };
    Kind kind {Kind::None};
    // Mission: two-leg profit per day (urgency-weighted for cargo runs);
    // Reposition: sourcing urgency, not credits.
    double score {0.0};
    const domain::StationDefinition* destination {nullptr};
    std::vector<domain::CargoLot> cargo;   // empty: an empty leg or repositioning
    // Cargo runs: sale value minus purchase and fuel, without urgency or time costs.
    double cargo_margin {0.0};
    domain::TrajectoryPlan plan;
    // What `plan` was planned with (without its path; step_idle_ship plans it again with it).
    trajectory::PlanningOptions plan_options {};
    double plan_departure_s {0.0};
    // Fuel-aware planning: return fuel loaded on top of the plan's load, for a port that
    // cannot refuel the ship.
    double carried_propellant_kg {0.0};
    // Missions: the follow-up load planned at the destination (see MissionAssignment).
    std::string pickup_commodity_id;
    double pickup_units {0.0};
    // Passengers who would board, by waiting group at the origin, and their fares.
    struct PassengerPick {
        std::size_t group {0};
        double count {0.0};
        double fare_cr {0.0};
    };
    std::vector<PassengerPick> passengers;
    // Cargo-only probes (fleet investment): every feasible cargo run, which the
    // investment review values at its sustained flow and prices.
    struct CargoOption {
        const domain::StationDefinition* destination {nullptr};
        std::string commodity_id;
        double cargo_units {0.0};
        double travel_days {0.0};
        double wait_days {0.0};
        double fuel_cost {0.0};
        int run {-1};  // lots of one mixed hold share a run number (its fuel is on the first lot)
        std::string module_id {};  // passengers: for the hull with this cabin module
        // Passengers: propellant (kg) for the way back, bought at the destination at its price;
        // -1 when the ship carries it from here (then the way back costs fuel_cost again).
        double return_fuel_kg {-1.0};
    };
    std::vector<CargoOption> cargo_options;
};

// choose_mission for several ships at once (fleet investment probes, refit variants):
// their plans are computed together, so the pool stays busy. Same results as one by one.
struct MissionRequest {
    const domain::ShipState* ship {nullptr};
    const domain::ShipClassDefinition* ship_class {nullptr};
    double earliest_departure_s {0.0};
    bool cargo_only {false};
};

// A follow-up leg's estimate (propellant, arrival, feasibility), cached by 5-day departure,
// 10%-of-tank fuel and 2 t payload buckets, planned from the bucket's start, with the travel
// days and the crew's dose limit measured from this departure. Inside a choose_mission pass
// an uncached leg is queued and an empty (infeasible) estimate returned for now.
[[nodiscard]] LegEstimate estimate_leg(World& w, const domain::ShipClassDefinition& ship_class,
    const domain::StationDefinition& origin, const domain::StationDefinition& destination, double departure_time_s,
    double available_propellant_kg, double payload_kg);
// Queues a plan once per key and pass.
void defer_plan(World& w, const std::string& key, DeferredPlan request);
// `cargo_only`: only cargo runs from the ship's port (no empty legs, no repositioning).
[[nodiscard]] MissionChoice choose_mission(World& w, const domain::ShipState& ship,
    const domain::ShipClassDefinition& ship_class, bool trace, double earliest_departure_s, bool cargo_only = false);
[[nodiscard]] std::vector<MissionChoice> choose_missions(World& w, const std::vector<MissionRequest>& requests);
// One pass of choose_mission. Plans it has no result for yet are queued in
// caches.deferred_plans and read as infeasible; choose_mission computes the queue in parallel
// and repeats the pass until nothing is missing, so the last pass is exactly the
// sequential result.
[[nodiscard]] MissionChoice choose_mission_pass(World& w, const domain::ShipState& ship,
    const domain::ShipClassDefinition& ship_class, bool trace, double earliest_departure_s, bool cargo_only,
    std::unordered_map<std::string, domain::TrajectoryPlan>& plans, std::string& trace_text);

}  // namespace spacetrains::simulation::systems
