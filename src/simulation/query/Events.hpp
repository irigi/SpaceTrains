#pragma once

// The recent events and trades shown in the comms log (docs/plans/simulation_split.md, step 2).

#include <string>

#include "domain/Types.hpp"
#include "simulation/World.hpp"

namespace spacetrains::simulation::query {

void add_event(World& w, std::string text, std::string category = "general");
void record_trade(World& w, domain::TradeEntry trade);

}  // namespace spacetrains::simulation::query
