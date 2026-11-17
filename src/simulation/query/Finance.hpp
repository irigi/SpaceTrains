#pragma once

// Money read-only leaves (docs/plans/simulation_split.md, step 4): background economies'
// balances and credit lines, colony state and wages, station profits, faction credit limits.

#include <string>

#include "domain/Types.hpp"
#include "simulation/World.hpp"

namespace spacetrains::simulation::query {

[[nodiscard]] inline const ColonyState* colony_state(const World& w, const std::string& economy_id) {
    const auto it = w.state.colony_state_.find(economy_id);
    return it == w.state.colony_state_.end() ? nullptr : &it->second;
}
[[nodiscard]] double faction_credit_limit(const World& w, const std::string& faction_id);
[[nodiscard]] double economy_balance(const World& w, const std::string& economy_id);
[[nodiscard]] double economy_credit_line(const World& w, const std::string& economy_id);
[[nodiscard]] double colony_wage_median(const World& w, const domain::BackgroundEconomyDefinition& economy);
[[nodiscard]] double station_lifetime_profit(const World& w, const domain::StationState& station);
[[nodiscard]] double station_profit_last_year(const World& w, const domain::StationState& station);

[[nodiscard]] double resident_wages_per_day(const World& w, const domain::StationState& station);
[[nodiscard]] double household_travel_budget(const World& w, const std::string& home_id, double window_days);

}  // namespace spacetrains::simulation::query
