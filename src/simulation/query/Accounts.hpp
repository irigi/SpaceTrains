#pragma once

// The money journal's account indices (docs/plans/simulation_split.md, step 2).

#include <string>

#include "domain/Types.hpp"
#include "simulation/World.hpp"

namespace spacetrains::simulation::query {

[[nodiscard]] constexpr int account_earth() { return 0; }
[[nodiscard]] int account_economy(const World& w, const std::string& economy_id);   // Earth or a colony
[[nodiscard]] int account_faction(const World& w, const std::string& faction_id);
[[nodiscard]] int account_fleet(const World& w, const std::string& faction_id);
[[nodiscard]] int account_station(const World& w, const domain::StationState& station);
[[nodiscard]] int account_households(const World& w, const domain::StationState& station);
[[nodiscard]] int account_station(const World& w, const std::string& station_id);
[[nodiscard]] int account_households(const World& w, const std::string& station_id);

}  // namespace spacetrains::simulation::query
