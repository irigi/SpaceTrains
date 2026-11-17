#pragma once

// A ship's day by phase (docs/plans/simulation_split.md, step 12; docs/modules/ships_and_fleet.md):
// idle (dispatch, refits, salvage), refitting, awaiting departure, in transit, arrival.

#include "simulation/World.hpp"

namespace spacetrains::simulation::systems {

void step_idle_ship(World& w, domain::ShipState& ship);
void step_refitting_ship(World& w, domain::ShipState& ship);
void step_awaiting_departure_ship(World& w, domain::ShipState& ship);
void step_in_transit_ship(World& w, domain::ShipState& ship, double dt_s);

}  // namespace spacetrains::simulation::systems
