#pragma once

// Lookups on the World (docs/plans/simulation_split.md, step 2): stations, ship classes and their
// fitted designs, bodies, goods and modules. They read only, except register_design, which adds
// a design to the World's design cache (serial thread only).

#include <string>
#include <vector>

#include "domain/Types.hpp"
#include "simulation/World.hpp"

namespace spacetrains::simulation::query {

[[nodiscard]] const domain::StationDefinition& get_station_definition(const World& w, const std::string& station_id);
[[nodiscard]] domain::StationState& get_station_state(World& w, const std::string& station_id);
[[nodiscard]] const domain::StationState& get_station_state(const World& w, const std::string& station_id);
[[nodiscard]] const domain::ShipClassDefinition& get_ship_class(const World& w, const std::string& class_id);
[[nodiscard]] const domain::CelestialBodyDefinition& get_body_definition(const World& w, const std::string& body_id);
[[nodiscard]] const domain::CommodityDefinition& get_commodity(const World& w, const std::string& commodity_id);
[[nodiscard]] const domain::ShipModuleDefinition* find_module(const World& w, const std::string& hull_id,
    const std::string& module_id);
[[nodiscard]] bool module_has_active_effect(const World& w, const domain::ShipModuleDefinition& module);
// A plasma ship's radiator condition in tenths (1 when undamaged or not a plasma hull).
[[nodiscard]] double radiator_bucket(const World& w, const domain::ShipState& ship);
// The ship's design: its class with its fit and radiator damage (registered beforehand).
[[nodiscard]] const domain::ShipClassDefinition& ship_design(const World& w, const domain::ShipState& ship);
// Builds (once) and returns a class with a fit and a radiator bucket. Serial thread only.
const domain::ShipClassDefinition& register_design(World& w, const std::string& class_id,
    const std::vector<std::string>& fit, double radiator_bucket = 1.0);
// The bridge name of a mission phase ("in_transit", ...).
[[nodiscard]] std::string mission_phase_name(domain::ShipMissionPhase phase);

}  // namespace spacetrains::simulation::query
