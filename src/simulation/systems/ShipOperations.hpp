#pragma once

// Running a ship, the parts that price goods (docs/plans/simulation_split.md, step 5): crew
// costs. The rest of ShipOperations moves in step 6.

#include <cstddef>
#include <string>
#include <vector>

#include "domain/Types.hpp"
#include "simulation/World.hpp"

namespace spacetrains::simulation::systems {

[[nodiscard]] double daily_crew_cost(const World& w, const domain::ShipClassDefinition& ship_class,
    const domain::StationState& market);

}  // namespace spacetrains::simulation::systems
