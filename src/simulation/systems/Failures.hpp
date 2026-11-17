#pragma once

// Ship failures, derelicts and salvage (docs/plans/simulation_split.md, step 10;
// docs/modules/ships_and_fleet.md, "Failures and salvage"): hulls fail at a Weibull-distributed age,
// the wreck coasts as a derelict, and idle ships may race to it for its cargo and scrap.

#include "simulation/World.hpp"

namespace spacetrains::simulation::systems {

// Every ship's failure age, drawn from its own stream (the starting fleet at a steady-state age).
void init_failure_clocks(World& w);
void step_failures(World& w);
// Dispatch hook: sets out on a salvage run if one pays more per day than `spot_per_day`.
bool consider_salvage(World& w, domain::ShipState& ship, double spot_per_day);
void reach_derelict(World& w, domain::ShipState& ship);

}  // namespace spacetrains::simulation::systems
