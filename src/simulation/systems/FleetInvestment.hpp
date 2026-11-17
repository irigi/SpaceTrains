#pragma once

// Faction treasuries, credit, fleet investment, ship sales and commissioning
// (docs/plans/simulation_split.md, step 11; docs/modules/economy.md, docs/modules/ships_and_fleet.md).

#include "simulation/World.hpp"

namespace spacetrains::simulation::systems {

[[nodiscard]] double internal_money_supply(const World& w);
// Dividends, the faction treasuries' subsidies, taxes and interest, and the money-supply controller.
void step_treasuries(World& w, double dt_s);
// The cash above which a station pays its faction tax: lower while the faction is in debt.
[[nodiscard]] double station_tax_ceiling(const domain::OpenEconomyDefinition& open, double treasury);
// Fleet investment (docs/modules/ships_and_fleet.md): owners sell ships laid up for long,
// and every review the treasuries commission the ship with the best expected return.
void step_fleet_investment(World& w);

}  // namespace spacetrains::simulation::systems
