#pragma once

// Police inspections (docs/plans/simulation_split.md, step 8; docs/modules/ships_and_fleet.md,
// "Police inspections"): cutters meet ships in flight, ride alongside for the inspection and go home.

#include "math/Vec3d.hpp"
#include "simulation/World.hpp"

namespace spacetrains::simulation::systems {

// The cutters of the data's patrol bases, docked at home with full tanks.
void init_patrols(World& w);
void step_patrols(World& w, double dt_s);
// Where a cutter is now (docked, alongside its target or on its path).
[[nodiscard]] math::Vec3d patrol_position(const World& w, const domain::PatrolState& patrol);

}  // namespace spacetrains::simulation::systems
