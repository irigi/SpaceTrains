#pragma once

// The game clock's read-only questions (docs/plans/simulation_split.md, step 4).

#include <cmath>

#include "simulation/World.hpp"

namespace spacetrains::simulation::query {

// True on the tick that crosses into a new day (on the 0.1-day grid from midnight, the
// tick at midnight; from any other start, the first tick past it).
[[nodiscard]] inline bool first_tick_of_day(const World& w) {
    return std::floor(w.state.game_time_s / 86400.0) != std::floor((w.state.game_time_s - TICK_S) / 86400.0);
}

}  // namespace spacetrains::simulation::query
