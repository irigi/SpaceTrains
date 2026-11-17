#pragma once

// The station systems view (display only; docs/plans/simulation_split.md, step 3): people,
// works and their jobs by kind, needs by tier, site and depot, for the stats and the snapshot.

#include <string>

#include "domain/Types.hpp"
#include "simulation/World.hpp"

namespace spacetrains::simulation::systems {

// The "systems":{...}, member of a station's JSON (with its trailing comma), or "" when the
// data has no station works.
[[nodiscard]] std::string station_systems_json(const World& world, const domain::StationDefinition& station,
    const domain::StationState& state);

}  // namespace spacetrains::simulation::systems
