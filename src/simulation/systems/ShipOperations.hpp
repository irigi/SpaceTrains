#pragma once

// Running a ship (docs/plans/simulation_split.md, steps 5-6): crew costs, provisions, propellant,
// wear and repair, boil-off, radiation and particle events, and the crew's and the capital's pay.

#include <cstddef>
#include <string>
#include <vector>

#include "domain/Types.hpp"
#include "simulation/World.hpp"

namespace spacetrains::simulation::systems {

[[nodiscard]] double daily_crew_cost(const World& w, const domain::ShipClassDefinition& ship_class,
    const domain::StationState& market);
void buy_provisions(World& w, domain::ShipState& ship, domain::StationState& market, double days, bool departing);
// Overhauls a worn drive or holed radiators where the port has the spares; true if the
// ship went into the yard.
bool consider_repair(World& w, domain::ShipState& ship);
void step_particle_events(World& w, double dt_s);
void accrue_operating_costs(World& w, domain::ShipState& ship, double dt_s);
// Buys up to `kg` of propellant at the current port (missions fuel on departure).
void buy_propellant(World& w, domain::ShipState& ship, double kg);

}  // namespace spacetrains::simulation::systems
