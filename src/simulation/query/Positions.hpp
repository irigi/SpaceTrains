#pragma once

// Where things are (docs/plans/simulation_split.md, step 2): ships now, the Sun's mu, a
// station's planet, and whether a leg stays within one planet's system.

#include <string>

#include "domain/Types.hpp"
#include "math/Vec3d.hpp"
#include "simulation/World.hpp"

namespace spacetrains::simulation::query {

[[nodiscard]] math::Vec3d get_ship_render_position(const World& w, const domain::ShipState& ship);
[[nodiscard]] double sun_mu(const World& w);
// The planet (child of the Sun) a station's body belongs to.
[[nodiscard]] std::string primary_body_id(const World& w, const std::string& station_id);
[[nodiscard]] bool local_leg(const World& w, const domain::MissionAssignment& mission);

}  // namespace spacetrains::simulation::query
