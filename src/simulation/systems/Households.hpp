#pragma once

// The economy overhaul's people and money (docs/plans/simulation_split.md, step 9): residents and
// their spending, the colonies' surface markets and imports, the labour market and migration,
// the stations' market responses, and the bridge's economy fields.

#include <string>

#include "domain/Types.hpp"
#include "simulation/World.hpp"

namespace spacetrains::simulation::systems {

// A background economy's account (Earth's is the outside economy's).
double& economy_account(World& w, const std::string& economy_id);
// A station's residents' savings per head (never below 0).
[[nodiscard]] double per_capita_savings(const domain::StationState& station);
// A traveller group's fares: from the residents' travel budget, the employer or the traveller's
// economy, to `payee`.
void pay_traveller_fare(World& w, domain::StationState& origin, const domain::PassengerGroup& group, double fares,
    int payee);
void step_households(World& w, double dt_s);
// Colonies: their people against the data population (sales and imports scale with it when
// colony_people_scaling is on).
[[nodiscard]] double colony_people_factor(const World& w, const std::string& economy_id);
// Affordability: 1 above the buffer, falling to 0 at minus the credit line (or the old hard stop).
[[nodiscard]] double colony_affordability(double account, double credit_line, double buffer);
// The share of a good's base imports a colony buys at affordability f (essentials first).
[[nodiscard]] double colony_import_share(double affordability, bool essential);
// Growth multiple from its import satisfaction (1 when off): 2 x satisfaction - 1.
[[nodiscard]] double colony_growth_factor(const World& w, const std::string& economy_id);
void step_surface_markets(World& w, double dt_s);
[[nodiscard]] double labour_migrants_per_day(const World& w, const domain::StationState& station);
[[nodiscard]] double labour_emigrants_per_day(const World& w, const domain::StationState& station);
[[nodiscard]] double settler_gain_tail(const World& w, const domain::StationState& destination, double gain_cr);
[[nodiscard]] double returner_gain_tail(const World& w, double gain_cr);
[[nodiscard]] double traveller_budget_tail(const World& w, const std::string& home_id, double cost_cr);
void step_labour(World& w, double dt_s);
// Phase C: supply follows price, demand follows price and income (StationState factors).
void update_market_responses(World& w);
// Economy overhaul bridge fields (households on): residents, wages, incomes, surface trade.
[[nodiscard]] std::string station_economy_json(const World& w, const domain::StationState& station);
void step_colony_labour(World& w, double dt_s);
// The background economies' accounts and people, and residents' savings (bridge "economy").
[[nodiscard]] std::string background_economies_json(const World& w);
[[nodiscard]] const domain::StationDefinition* yard_station_for(const World& w, const domain::StationDefinition& home);

}  // namespace spacetrains::simulation::systems
