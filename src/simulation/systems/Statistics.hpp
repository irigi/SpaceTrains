#pragma once

// Economy statistics (docs/plans/simulation_split.md, step 7): the money journal's accounts and
// monthly close, the flow record's stocks, and the statistics JSON for the bridge's side file and
// headless --stats-json (docs/modules/economy.md, "Money journal").

#include <string>
#include <vector>

#include "simulation/World.hpp"

namespace spacetrains::simulation::systems {

// Accounts in a fixed order (definitions order), seeded with today's balances; starts the
// journal's and the flow record's first period.
void setup_money_journal(World& w);
// Each station's stock of each of the flow record's goods (stations x goods).
[[nodiscard]] std::vector<std::vector<double>> flow_stocks(const World& w);
[[nodiscard]] double account_balance(const World& w, int account);
// Samples the balances, stations and colonies and closes the journal's and flow record's period
// (only reads the game state).
void close_journal_period(World& w);
[[nodiscard]] std::string build_stats_json(const World& w);

}  // namespace spacetrains::simulation::systems
