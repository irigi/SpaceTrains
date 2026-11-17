#pragma once

// Ship contracts and liner routes (docs/plans/simulation_split.md, step 5): which ships run
// fixed routes, finding a contract for a ship, the parallel planning passes.

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "domain/Types.hpp"
#include "simulation/World.hpp"

namespace spacetrains::simulation::systems {

// The fleet's total hold, against max_fleet_hold_units (step 21).
[[nodiscard]] double fleet_hold_units(const World& w);
// Ships counted against max_fleet_size (liners are not).
[[nodiscard]] std::size_t fleet_ship_count(const World& w);
// Scheduled liners (step 22): a liner's loop (null for other ships), and its stop after `from_id`.
[[nodiscard]] const std::vector<std::string>* liner_stops(const World& w, const domain::ShipState& ship);
[[nodiscard]] std::string next_liner_stop(const World& w, const domain::ShipState& ship, const std::string& from_id);
[[nodiscard]] bool under_contract(const World& w, const domain::ShipState& ship);
// A liner's stops, or the loop of a ship under a rotation contract (nullptr: neither).
[[nodiscard]] const std::vector<std::string>* scheduled_stops(const World& w, const domain::ShipState& ship);
[[nodiscard]] std::string next_scheduled_stop(const World& w, const domain::ShipState& ship,
    const std::string& from_id);
// A rotation contract's wait in port at its first stop before each lap (its cycle minus its legs).
[[nodiscard]] double contract_dwell_days(const domain::ShipContract& contract);
[[nodiscard]] double contracted_flow(const World& w, const std::string& station_id, const std::string& commodity_id);
// Runs `pass` until it queues no plans, computing the queued plans in parallel in between.
void with_parallel_plans(World& w, const std::string& prefix, const std::function<void()>& pass);
// The best loop for a ship from its port, with its profit per day (0: none).
[[nodiscard]] std::pair<domain::ShipContract, double> find_contract(World& w, const domain::ShipState& ship,
    const domain::ShipClassDefinition& ship_class);

}  // namespace spacetrains::simulation::systems
