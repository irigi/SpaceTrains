#pragma once

// Households and the labour market, the read-only leaves (docs/plans/simulation_split.md,
// step 2): which stations hire from the labour market, their workers and their wage.

#include <string>

#include "domain/Types.hpp"
#include "simulation/World.hpp"

namespace spacetrains::simulation::query {

[[nodiscard]] inline bool households_on(const World& w) { return w.data.universe.households.enabled != 0.0; }
[[nodiscard]] inline bool labour_on(const World& w) {
    return households_on(w) && w.data.universe.labour_market.enabled != 0.0;
}
// Whether a worker's employer is a colony ("colony:<economy>") rather than a station.
[[nodiscard]] inline bool colony_employer(const std::string& work_station_id) {
    return work_station_id.starts_with("colony:");
}
[[nodiscard]] bool is_earth_gateway(const World& w, const std::string& station_id);
[[nodiscard]] bool in_labour_market(const World& w, const domain::StationState& station);
[[nodiscard]] double station_wage_cr_per_day(const World& w, const domain::StationState& station);
[[nodiscard]] double station_workers(const World& w, const domain::StationState& station);

}  // namespace spacetrains::simulation::query
