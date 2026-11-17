#pragma once

// Building a World (docs/plans/simulation_split.md, step 15).

#include <functional>
#include <memory>
#include <string>

#include "domain/Types.hpp"
#include "simulation/World.hpp"

namespace spacetrains::simulation {

// A new game's set-up on a World just built from its universe: lookups, stations and ships
// from the data, the money journal, residents' savings, rotation tours, order stats, failure
// clocks and patrols, in that order.
void init_world(World& w);
// The atlas in a source checkout: tests/data/variable_isp next to the data root.
[[nodiscard]] std::string default_atlas_path(const std::string& data_root);
// The universe from the data files, adjusted (tests: switches without data copies).
[[nodiscard]] domain::UniverseDefinition load_universe(const std::string& data_root,
    const std::function<void(domain::UniverseDefinition&)>& adjust);
// The thread pool and the VariableISP atlas (empty atlas_file: default_atlas_path). Without
// the atlas, electric ships are stranded (a warning on stderr).
void attach_engines(World& w, const std::string& data_root, const std::string& atlas_file);
// A started game from the data, as Simulation::from_data_root builds it: load_universe,
// init_world, attach_engines.
[[nodiscard]] std::unique_ptr<World> make_world(const std::string& data_root, const std::string& atlas_file = {},
    const std::function<void(domain::UniverseDefinition&)>& adjust = [](domain::UniverseDefinition&) {});

}  // namespace spacetrains::simulation
