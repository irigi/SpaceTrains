#pragma once

// Save games: the whole mutable state of a World as JSON (see SimulationPersistence.cpp).

#include <string>

namespace spacetrains::simulation {
struct World;
}

namespace spacetrains::persistence {

// FNV-1a fingerprint of the definitions a save depends on; a save records it and loads
// only into a World built from data with the same fingerprint.
[[nodiscard]] std::string data_fingerprint(const simulation::World& w);
// The mutable state as JSON.
[[nodiscard]] std::string save_state_json(const simulation::World& w);
// Expects a World built from the same data files (checked by fingerprint) and throws,
// leaving the world unchanged, on a bad or foreign save.
void load_state_json(simulation::World& w, const std::string& text);

}  // namespace spacetrains::persistence
