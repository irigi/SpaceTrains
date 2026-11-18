// Households and background economies (docs/modules/economy.md).
//
// Residents are an account of their own: their station pays them wages, they pay for the goods
// they use up (settle_local_economy) and spend the rest of their income at the station, keeping
// a buffer of savings. Background economies (Earth's surface, the small colonies) trade with
// their gateway stations at surface price plus launch (they sell) or minus landing (they buy),
// and pay the stations' incomes (science budgets, services, subsidies). Every flow is a transfer
// between named accounts, so the money identity stays exact.

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

#include "simulation/SimulationInternal.hpp"
#include "simulation/query/Accounts.hpp"
#include "simulation/query/Events.hpp"
#include "simulation/query/Finance.hpp"
#include "simulation/query/Labour.hpp"
#include "simulation/query/Lookups.hpp"
#include "simulation/query/Passengers.hpp"
#include "simulation/query/Positions.hpp"
#include "simulation/query/ShipRates.hpp"
#include "simulation/systems/Households.hpp"
#include "simulation/systems/Market.hpp"
#include "simulation/systems/Passengers.hpp"
#include "simulation/systems/ShipOperations.hpp"

namespace spacetrains::simulation::systems {

using namespace detail;

namespace {

// Defined below, file-local.
// A boom or outage at a gateway scales what its colony sells there (events.csv).
[[nodiscard]] double colony_sales_factor(const World& w, const domain::StationState& station,
    const std::string& commodity_id);
[[nodiscard]] double colony_import_value_per_day(const World& w, const std::string& economy_id);

}  // namespace

namespace {
// A small colony's supply reaches its full rate once the station pays this multiple of its ask.
constexpr double SURFACE_SUPPLY_FULL_PRICE = 1.25;
// A colony buying at its gateway leaves the station this many days of its own use.
constexpr double SURFACE_RESERVE_DAYS = 14.0;
// A colony keeps its gateway stocked with at least this many days of the station's own use.
constexpr double SURFACE_COVER_DAYS = 30.0;
// A station adds jobs only while fewer than this share of its workers' jobs stand open.
constexpr double VACANCY_GROWTH_LIMIT = 0.05;
// A colony fills its gateway's vacancies, and takes its unemployed, over about this many days.
constexpr double COLONY_FILL_DAYS = 30.0;

// The journal category of a station income, by its kind in station_incomes.csv.
MoneyCategory grant_category(const std::string& kind) {
    if (kind == "science" || kind == "science_station") {
        return MoneyCategory::GrantScience;
    }
    if (kind == "settlement") {
        return MoneyCategory::GrantSettlement;
    }
    if (kind == "relay") {
        return MoneyCategory::GrantRelay;
    }
    if (kind.starts_with("tourism")) {
        return MoneyCategory::GrantTourism;
    }
    return MoneyCategory::GrantOther;
}
}  // namespace

double& economy_account(World& w, const std::string& economy_id) {
    return economy_id == EARTH_ECONOMY ? w.state.outside_economy_credits : w.state.background_credits[economy_id];
}

double per_capita_savings(const domain::StationState& station) {
    return station.population > 0.0 ? std::max(0.0, station.household_credits) / station.population : 0.0;
}

void pay_traveller_fare(World& w, domain::StationState& origin, const domain::PassengerGroup& group, double fares, int payee) {
    // A visitor going home pays from home; everyone else from where they board. Travellers from
    // an Earth gateway are Earth's people (Earth's account); a station's residents pay from
    // their households (the outside account before households existed).
    if (query::colony_employer(group.work_station_id)) {
        const auto economy_id = group.work_station_id.substr(std::string("colony:").size());
        economy_account(w, economy_id) -= fares;   // a colony's recruits
        w.state.journal.record(query::account_economy(w, economy_id), payee, MoneyCategory::FaresTraveller, fares);
        return;
    }
    if (!query::households_on(w)) {
        w.state.outside_economy_credits -= fares;
        w.state.journal.record(query::account_earth(), payee, MoneyCategory::FaresTraveller, fares);
        return;
    }
    const std::string& home = group.kind == domain::PassengerKind::VisitorHome ? group.destination_id : origin.station_id;
    if (query::is_earth_gateway(w, home)) {
        w.state.outside_economy_credits -= fares;
        w.state.journal.record(query::account_earth(), payee, MoneyCategory::FaresTraveller, fares);
    } else {
        auto& home_state = query::get_station_state(w, home);
        home_state.household_credits -= fares;
        w.state.journal.record(query::account_households(w, home_state), payee, MoneyCategory::FaresTraveller, fares);
        home_state.travel_budget_cr = std::max(0.0, home_state.travel_budget_cr - fares);
    }
}

namespace {

double colony_sales_factor(const World& w, const domain::StationState& station, const std::string& commodity_id) {
    // A boom (a rich seam) or an outage at the gateway changes what its colony mines (events.csv).
    double factor = 1.0;
    for (const auto& active : station.events) {
        const auto event = std::find_if(w.data.universe.events.begin(), w.data.universe.events.end(),
            [&](const auto& e) { return e.id == active.event_id; });
        if (event != w.data.universe.events.end() && event->commodity_id == commodity_id
            && (event->kind == domain::EventKind::Boom || event->kind == domain::EventKind::Outage)) {
            factor *= event->magnitude;
        }
    }
    return factor;
}

}  // namespace

void step_households(World& w, double dt_s) {
    if (!query::households_on(w)) {
        return;
    }
    const auto& households = w.data.universe.households;
    const double dt_days = dt_s / 86400.0;
    for (auto& station : w.state.stations) {
        if (station.credits < 0.0) {
            station.ledger.days_in_red += dt_days;
        }
        // Wages: the station employs its residents.
        const double wages = query::resident_wages_per_day(w, station) * dt_days;
        station.credits -= wages;
        station.household_credits += wages;
        station.ledger.wages_paid += wages;
        w.state.journal.record(query::account_station(w, station), query::account_households(w, station), MoneyCategory::Wages, wages);
        // A share of it is put aside for long trips (a year's worth at most).
        if (households.travel_share > 0.0) {
            station.travel_budget_cr = std::min(station.travel_budget_cr + households.travel_share * wages,
                households.travel_share * query::resident_wages_per_day(w, station) * 365.25);
        }
        // Spending beyond the goods they used up (local services, rent, taxes): savings above
        // the buffer go back to the station over spend_days.
        const double buffer = households.savings_days * query::resident_wages_per_day(w, station);
        if (station.household_credits > buffer) {
            const double spend = (station.household_credits - buffer) * std::min(1.0, dt_days / households.spend_days);
            station.household_credits -= spend;
            station.credits += spend;
            station.ledger.household_spending += spend;
            w.state.journal.record(query::account_households(w, station), query::account_station(w, station), MoneyCategory::HouseholdSpending, spend);
        }
        // Visitors spend during their stay, out of their home's account.
        if (households.visitor_spend_cr_per_day > 0.0) {
            for (const auto& stay : station.visiting) {
                const double spend = stay.count * households.visitor_spend_cr_per_day * dt_days;
                int payer = query::account_earth();
                if (query::is_earth_gateway(w, stay.destination_id)) {
                    w.state.outside_economy_credits -= spend;
                } else {
                    query::get_station_state(w, stay.destination_id).household_credits -= spend;
                    payer = query::account_households(w, stay.destination_id);
                }
                station.credits += spend;
                station.ledger.visitor_spending += spend;
                w.state.journal.record(payer, query::account_station(w, station), MoneyCategory::VisitorSpending, spend);
            }
        }
    }
    // Earth invests in the small colonies.
    for (const auto& economy : w.data.universe.background_economies) {
        if (economy.income_cr_per_day > 0.0) {
            const double amount = economy.income_cr_per_day * dt_days;
            w.state.outside_economy_credits -= amount;
            economy_account(w, economy.id) += amount;
            w.state.journal.record(query::account_earth(), query::account_economy(w, economy.id), MoneyCategory::ColonyInvestment, amount);
        }
    }
    // Incomes from the background economies: science budgets, services, subsidies.
    const double ceiling = w.data.universe.open_economy.station_credit_ceiling;
    for (const auto& income : w.data.universe.station_incomes) {
        auto& station = query::get_station_state(w, income.station_id);
        // A grant tapers off as the station's cash nears the credit ceiling (agencies fund need).
        const double taper = income.need_based && ceiling > 0.0
            ? std::clamp(2.0 * (1.0 - station.credits / ceiling), 0.0, 1.0) : 1.0;
        const double amount = income.cr_per_day * taper * dt_days;
        if (amount <= 0.0) {
            continue;
        }
        economy_account(w, income.payer_id) -= amount;
        station.credits += amount;
        station.ledger.incomes += amount;
        w.state.journal.record(query::account_economy(w, income.payer_id), query::account_station(w, station), grant_category(income.kind), amount);
    }
}

double colony_people_factor(const World& w, const std::string& economy_id) {
    if (w.data.universe.households.colony_people_scaling <= 0.0) {
        return 1.0;
    }
    for (const auto& economy : w.data.universe.background_economies) {
        if (economy.id == economy_id && economy.population > 0.0) {
            const auto people = w.state.background_population.find(economy_id);
            return people == w.state.background_population.end() ? 1.0 : people->second / economy.population;
        }
    }
    return 1.0;
}

double colony_affordability(double account, double credit_line, double buffer) {
    if (buffer <= 0.0) {
        return account > -credit_line ? 1.0 : 0.0;   // the old hard stop at the line
    }
    return std::clamp((account + credit_line) / (buffer + credit_line), 0.0, 1.0);
}

double colony_import_share(double affordability, bool essential) {
    // A colony short of money keeps buying food and medicine, and drops machinery and electronics first.
    return essential ? std::min(1.0, 2.0 * affordability) : std::max(0.0, 2.0 * affordability - 1.0);
}

double colony_growth_factor(const World& w, const std::string& economy_id) {
    const auto* state = query::colony_state(w, economy_id);
    if (state == nullptr || w.data.universe.households.colony_growth_follows_satisfaction <= 0.0) {
        return 1.0;
    }
    return 2.0 * state->satisfaction - 1.0;
}

namespace {

double colony_import_value_per_day(const World& w, const std::string& economy_id) {
    // A colony's base imports (people-scaled) at its gateways' bids.
    double value = 0.0;
    for (const auto& market : w.data.universe.surface_markets) {
        if (market.economy_id == economy_id && market.buy_units_per_day > 0.0 && !market.station_id.empty()) {
            value += market.buy_units_per_day * colony_people_factor(w, economy_id)
                * w.engines.economy.surface_bid(query::get_station_definition(w, market.station_id), market.commodity_id);
        }
    }
    return value;
}

}  // namespace

void step_surface_markets(World& w, double dt_s) {
    if (w.data.universe.surface_markets.empty()) {
        return;
    }
    const double dt_days = dt_s / 86400.0;
    const double depth = w.engines.economy.surface_depth_units();
    // Colonies (docs/modules/economy.md, "Colonies"): what each can afford this tick; what it buys and sells.
    const auto& households = w.data.universe.households;
    const bool colony_budgets = households.colony_import_buffer_days > 0.0;
    struct ColonyTick {
        double base {0.0};     // its base imports' value this tick
        double bought {0.0};
        double sold {0.0};
    };
    std::map<std::string, ColonyTick> colony_tick;
    if (colony_budgets) {
        for (auto& [economy_id, state] : w.state.colony_state) {
            state.base_imports_per_day = colony_import_value_per_day(w, economy_id);
            state.throttle = colony_affordability(query::economy_balance(w, economy_id), query::economy_credit_line(w, economy_id),
                households.colony_import_buffer_days * state.base_imports_per_day);
        }
    }
    for (auto& station : w.state.stations) {
        const auto& definition = query::get_station_definition(w, station.station_id);
        const auto& markets = w.engines.economy.surface_markets_at(station.station_id);
        if (markets.empty()) {
            continue;
        }
        const double capacity = definition.storage_capacity_units;
        for (const auto* market : markets) {
            const auto& commodity_id = market->commodity_id;
            double& account = economy_account(w, market->economy_id);
            const bool can_pay = account > -query::economy_credit_line(w, market->economy_id);
            double& stock = station.inventory[commodity_id];
            // The surface sells.
            if (market->ask_cr > 0.0) {
                double units = 0.0;
                if (market->sell_units_per_day <= 0.0) {
                    units = std::max(0.0, depth - stock);   // Earth keeps the gateway stocked
                } else {
                    // A colony sells while the station pays more than its ask, and the station has room;
                    // and always while the station holds less than SURFACE_COVER_DAYS of its own use (a
                    // station the colony supplies counts as a producer, whose curve alone would keep
                    // its stock below what its residents need).
                    const double price = systems::station_price(w, station, commodity_id);
                    double own_use = 0.0;
                    for (const auto* recipe : w.engines.economy.recipes_of(definition)) {
                        if (recipe->commodity_id == commodity_id && recipe->units_per_day < 0.0) {
                            own_use -= recipe->units_per_day * w.engines.economy.population_factor(definition);
                        }
                    }
                    const double eagerness = stock < SURFACE_COVER_DAYS * own_use
                        ? 1.0
                        : std::clamp((price / market->ask_cr - 1.0) / (SURFACE_SUPPLY_FULL_PRICE - 1.0), 0.0, 1.0);
                    const bool room = capacity <= 0.0 || w.engines.economy.storage_used_units(definition, station.inventory) < 0.85 * capacity;
                    units = room ? market->sell_units_per_day * colony_people_factor(w, market->economy_id)
                            * colony_sales_factor(w, station, commodity_id) * eagerness * dt_days
                        : 0.0;
                }
                if (units > 0.0) {
                    const double paid = units * market->ask_cr;
                    station.credits -= paid;
                    account += paid;
                    station.ledger.surface_bought += paid;
                    if (market->sell_units_per_day > 0.0) {
                        colony_tick[market->economy_id].sold += paid;
                    }
                    w.state.journal.record(query::account_station(w, station), query::account_economy(w, market->economy_id), MoneyCategory::SurfaceSales, paid);
                    w.state.flows.record(FlowKind::Delivered, "surface:" + market->economy_id, station.station_id, commodity_id, units,
                        paid, 0.0);
                    // Residents pay what the station paid for it (its import cost).
                    const auto& rates = w.engines.economy.get_station_net_rates(definition);
                    const auto rate = rates.find(commodity_id);
                    const double history = rate == rates.end() ? 0.0 : std::max(0.0, -rate->second) * IMPORT_COST_DAYS;
                    const auto known = station.import_unit_cost.find(commodity_id);
                    const double previous = known == station.import_unit_cost.end() ? market->ask_cr : known->second;
                    station.import_unit_cost[commodity_id] = (previous * history + paid) / (history + units);
                    stock += units;
                }
            }
            // The surface buys.
            const double bid = w.engines.economy.surface_bid(definition, commodity_id);
            // A colony's imports: its people's base rate, scaled by what it can afford (essentials first).
            const auto colony = colony_budgets && market->buy_units_per_day > 0.0 ? w.state.colony_state.find(market->economy_id)
                                                                                  : w.state.colony_state.end();
            const bool budgeted = colony != w.state.colony_state.end();
            double import_rate = market->buy_units_per_day;
            if (budgeted && bid > 0.0) {
                const double base_rate = import_rate * colony_people_factor(w, market->economy_id);
                const bool essential = std::find(households.colony_essentials.begin(), households.colony_essentials.end(),
                    commodity_id) != households.colony_essentials.end();
                import_rate = base_rate * colony_import_share(colony->second.throttle, essential);
                colony_tick[market->economy_id].base += base_rate * dt_days * bid;
            }
            if (bid > 0.0 && (budgeted || can_pay) && stock > 0.0) {
                double units = 0.0;
                if (market->ask_cr <= 0.0 && market->bid_share <= 0.0 && market->buy_units_per_day <= 0.0) {
                    units = stock;   // an export market takes it all
                } else {
                    // Keep the station's own use (and what the surface stocks here) for the station.
                    const auto& rates = w.engines.economy.get_station_net_rates(definition);
                    const auto rate = rates.find(commodity_id);
                    double own_use = rate == rates.end() ? 0.0 : std::max(0.0, -rate->second);
                    if (market->buy_units_per_day > 0.0) {
                        own_use = std::max(0.0, own_use - market->buy_units_per_day);
                    }
                    const double reserve = std::max(own_use * SURFACE_RESERVE_DAYS,
                        w.engines.economy.surface_supplied(definition, commodity_id) ? depth : 0.0);
                    units = std::clamp(stock - reserve, 0.0,
                        market->buy_units_per_day > 0.0 ? import_rate * dt_days : stock);
                }
                if (units > 0.0) {
                    const double paid = units * bid;
                    account -= paid;
                    station.credits += paid;
                    station.ledger.surface_sold += paid;
                    if (budgeted) {
                        colony_tick[market->economy_id].bought += paid;
                    }
                    w.state.journal.record(query::account_economy(w, market->economy_id), query::account_station(w, station), MoneyCategory::SurfacePurchases, paid);
                    w.state.flows.record(FlowKind::Delivered, station.station_id, "surface:" + market->economy_id, commodity_id, units,
                        paid, 0.0);
                    station.market_sold_units[commodity_id] += units;
                    stock -= units;
                }
            }
        }
    }
    // A colony's welfare: the share of its base imports it bought, and its income (sales and Earth's investment).
    if (colony_budgets) {
        const double blend = std::min(1.0, dt_days / std::max(1.0, households.colony_satisfaction_days));
        for (const auto& economy : w.data.universe.background_economies) {
            const auto state = w.state.colony_state.find(economy.id);
            if (state == w.state.colony_state.end()) {
                continue;
            }
            const auto& tick = colony_tick[economy.id];
            if (tick.base > 0.0) {
                state->second.satisfaction += (tick.bought / tick.base - state->second.satisfaction) * blend;
            }
            const double income = tick.sold / dt_days + economy.income_cr_per_day;
            state->second.income_per_day = state->second.income_per_day <= 0.0
                ? income : state->second.income_per_day + (income - state->second.income_per_day) * blend;
        }
    }
}

// ---- Labour market (phase B) ----------------------------------------------------------------

namespace {
double normal_cdf(double z) {
    return 0.5 * std::erfc(-z / std::sqrt(2.0));
}
// P(X >= x) for a lognormal X with this median and log-spread.
double lognormal_tail(double median, double sigma, double x) {
    if (x <= 0.0) {
        return 1.0;
    }
    if (median <= 0.0) {
        return 0.0;
    }
    return 1.0 - normal_cdf((std::log(x / median)) / sigma);
}
}  // namespace

double labour_migrants_per_day(const World& w, const domain::StationState& station) {
    // Vacancies go to Earth's job seekers over vacancy_fill_days; people already coming fill them.
    if (!query::in_labour_market(w, station) || !query::migration_by_ship(w)) {
        return 0.0;
    }
    const double participation = std::max(1e-6, w.data.universe.households.participation);
    const double people_needed = std::max(0.0, station.jobs - query::station_workers(w, station)) / participation
        - travellers_pending(w, station.station_id, domain::PassengerKind::Migrant);
    return std::max(0.0, people_needed) / w.data.universe.labour_market.vacancy_fill_days;
}

double labour_emigrants_per_day(const World& w, const domain::StationState& station) {
    // The unemployed look for a passage home (with their families).
    if (!query::in_labour_market(w, station) || !query::migration_by_ship(w)) {
        return 0.0;
    }
    const double participation = std::max(1e-6, w.data.universe.households.participation);
    const double people_out = std::max(0.0, query::station_workers(w, station) - station.jobs) / participation
        - travellers_pending(w, station.station_id, domain::PassengerKind::Emigrant);
    return std::max(0.0, people_out) / w.data.universe.labour_market.unemployed_leave_days;
}

double settler_gain_tail(const World& w, const domain::StationState& destination, double gain_cr) {
    // A job seeker on Earth earning w_E gains (w_S (1 - hardship) - w_E) x the stay; the share of
    // seekers who gain at least `gain_cr` is P(w_E <= w_S (1 - hardship) - gain / stay).
    const auto& labour = w.data.universe.labour_market;
    const double stay_days = 365.25 * labour.stay_years;
    const double ceiling = query::station_wage_cr_per_day(w, destination) * (1.0 - labour.hardship_share) - gain_cr / stay_days;
    if (ceiling <= 0.0) {
        return 0.0;
    }
    return normal_cdf(std::log(ceiling / labour.earth_wage_median_cr_per_day) / labour.earth_wage_sigma);
}

double returner_gain_tail(const World& w, double gain_cr) {
    // The unemployed going home gain an Earth wage over a stay.
    const auto& labour = w.data.universe.labour_market;
    return lognormal_tail(labour.earth_wage_median_cr_per_day * 365.25 * labour.stay_years, labour.earth_wage_sigma, gain_cr);
}

double traveller_budget_tail(const World& w, const std::string& home_id, double cost_cr) {
    // A trip's budget is a share of a year's income: Earth's income spread from an Earth
    // gateway, the station's wage (with its own spread) elsewhere.
    const auto& labour = w.data.universe.labour_market;
    const bool earth = query::is_earth_gateway(w, home_id);
    const double income = earth ? labour.earth_wage_median_cr_per_day : query::station_wage_cr_per_day(w, query::get_station_state(w, home_id));
    const double sigma = earth ? labour.visitor_income_sigma : labour.station_wage_sigma;
    return lognormal_tail(income * 365.25 * labour.visitor_budget_years, sigma, cost_cr);
}

void step_labour(World& w, double dt_s) {
    if (!query::labour_on(w)) {
        return;
    }
    const auto& labour = w.data.universe.labour_market;
    const auto& growth = w.data.universe.growth;
    const double dt_years = dt_s / 86400.0 / 365.25;
    const double floor_credits = w.data.universe.open_economy.station_credit_floor;
    for (auto& station : w.state.stations) {
        if (!query::in_labour_market(w, station)) {
            continue;
        }
        const auto& definition = query::get_station_definition(w, station.station_id);
        const double participation = w.data.universe.households.participation;
        const double seeded_workers = static_cast<double>(definition.population) * participation;
        if (station.wage_cr_per_day <= 0.0) {
            station.wage_cr_per_day = query::station_wage_cr_per_day(w, station);   // the data's wage level to start with
        }
        if (station.jobs <= 0.0) {
            station.jobs = query::station_workers(w, station);   // full employment to start with
        }
        // Jobs grow while the station is supplied, pays its way and its plants run near capacity;
        // they are cut when it is not supplied, is deep in the red or its plants stand idle (phase C:
        // output follows price, so idle plants are what a glut leaves).
        double output_value = 0.0;
        double running_value = 0.0;
        for (const auto* recipe : w.engines.economy.recipes_of(definition)) {
            if (recipe->units_per_day > 0.0) {
                const double value = recipe->units_per_day * w.engines.economy.reference_price(definition, recipe->commodity_id);
                const auto supply = station.supply_factor.find(recipe->commodity_id);
                output_value += value;
                running_value += value * (supply == station.supply_factor.end() ? 1.0 : supply->second);
            }
        }
        const double utilization = output_value > 0.0 ? running_value / output_value : 1.0;
        // New jobs only while the ones open get filled (under VACANCY_GROWTH_LIMIT unfilled).
        const double open_share = (station.jobs - query::station_workers(w, station)) / std::max(1.0, query::station_workers(w, station));
        double rate = 0.0;
        // A science station keeps the crew its research needs: its jobs do not grow.
        if (station.supply_index >= growth.grow_above && station.credits >= floor_credits && utilization >= 0.8
            && open_share < VACANCY_GROWTH_LIMIT && !w.engines.economy.science_station(station.station_id)) {
            rate = growth.growth_per_year;
        } else if (station.supply_index < growth.decline_below || station.credits < -floor_credits || utilization < 0.5) {
            rate = -growth.decline_per_year;
        }
        station.jobs = std::clamp(station.jobs * std::exp(rate * dt_years),
            w.data.universe.open_economy.core_crew_fraction * seeded_workers, growth.max_population_factor * seeded_workers);
        // Wages follow the vacancy rate (up where jobs go unfilled, down where people are idle).
        const double workers = std::max(1.0, query::station_workers(w, station));
        const double vacancy_rate = std::clamp((station.jobs - workers) / workers, -1.0, 1.0);
        station.wage_cr_per_day = std::max(labour.min_wage_multiple * w.data.universe.labour.reference_wage_cr_per_day,
            station.wage_cr_per_day * std::exp(labour.wage_adjust_per_year * vacancy_rate * dt_years));
        // Plants run with the workers employed (jobs filled / workers). The share the economy runs
        // on moves in steps of half a percent (or to full employment), so net rates are rebuilt
        // rarely; it is saved with the station.
        const double share = std::min(1.0, station.jobs / workers);
        if (std::abs(share - station.employed_share) >= 0.005 || (share == 1.0 && station.employed_share != 1.0)) {
            station.employed_share = share;
            w.engines.economy.set_employment(station.station_id, share);
        }
    }
}

void update_market_responses(World& w) {
    // Phase C: each tick's supply and demand multipliers from today's prices and wages.
    const auto& h = w.data.universe.households;
    const bool supply_on = h.supply_price_elasticity > 0.0;
    const bool demand_on = h.consume_income_elasticity != 0.0 || h.consume_price_elasticity != 0.0
        || h.upkeep_price_elasticity != 0.0;
    if (!query::households_on(w) || (!supply_on && !demand_on)) {
        return;
    }
    for (auto& station : w.state.stations) {
        const auto& definition = query::get_station_definition(w, station.station_id);
        station.supply_factor.clear();
        station.demand_factor.clear();
        const double starting_wage = [&] {
            const auto multiple = h.wage_multiple.find(station.station_id);
            return w.data.universe.labour.reference_wage_cr_per_day * (multiple == h.wage_multiple.end() ? 1.0 : multiple->second);
        }();
        const double income_ratio = starting_wage > 0.0 ? query::station_wage_cr_per_day(w, station) / starting_wage : 1.0;
        for (const auto* recipe : w.engines.economy.recipes_of(definition)) {
            const auto& commodity_id = recipe->commodity_id;
            const double reference = w.engines.economy.reference_price(definition, commodity_id);
            if (reference <= 0.0) {
                continue;
            }
            // A price the station gets from a surface economy or an export market is what it
            // is; elsewhere the station's own curve on its stock.
            const double price_ratio = systems::station_price(w, station, commodity_id) / reference;
            if (recipe->units_per_day > 0.0 && supply_on) {
                station.supply_factor[commodity_id] = std::clamp(std::pow(price_ratio, h.supply_price_elasticity), h.min_utilization, 1.0);
            } else if (recipe->units_per_day < 0.0 && demand_on && recipe->role != domain::RecipeRole::Input) {
                const bool upkeep = recipe->role == domain::RecipeRole::Upkeep;
                const double factor = upkeep ? std::pow(price_ratio, h.upkeep_price_elasticity)
                                             : std::pow(income_ratio, h.consume_income_elasticity) * std::pow(price_ratio, h.consume_price_elasticity);
                station.demand_factor[commodity_id] = std::clamp(factor, 0.1, 3.0);
            }
        }
    }
}

std::string station_economy_json(const World& w, const domain::StationState& station) {
    if (!query::households_on(w)) {
        return {};
    }
    const auto& definition = query::get_station_definition(w, station.station_id);
    std::string incomes = "[";
    for (const auto& income : w.data.universe.station_incomes) {
        if (income.station_id == station.station_id) {
            incomes += std::format("{}{{\"kind\":\"{}\",\"payer\":\"{}\",\"cr_per_year\":{:.0f}}}", incomes.size() > 1 ? "," : "",
                json_escape(income.kind), json_escape(income.payer_id), income.cr_per_day * 365.25);
        }
    }
    incomes += "]";
    std::string surface = "[";
    for (const auto* market : w.engines.economy.surface_markets_at(station.station_id)) {
        const auto economy = std::find_if(w.data.universe.background_economies.begin(), w.data.universe.background_economies.end(),
            [&](const auto& e) { return e.id == market->economy_id; });
        surface += std::format("{}{{\"economy\":\"{}\",\"good\":\"{}\",\"ask\":{:.2f},\"bid\":{:.2f},\"units_per_day\":{:.1f}}}",
            surface.size() > 1 ? "," : "", json_escape(economy == w.data.universe.background_economies.end() ? market->economy_id : economy->name),
            json_escape(market->commodity_id), market->ask_cr, w.engines.economy.surface_bid(definition, market->commodity_id),
            market->ask_cr > 0.0 ? market->sell_units_per_day : market->buy_units_per_day);
    }
    surface += "]";
    const double workers = query::station_workers(w, station);
    return std::format("\"household_credits\":{:.0f},\"wage_cr_per_day\":{:.3f},\"workers\":{:.0f},\"jobs\":{:.0f},"
        "\"labour_market\":{},\"incomes\":{},\"surface_trade\":{},\"days_in_red\":{:.0f},\"lifetime_profit\":{:.0f},\"profit_last_year\":{:.0f},",
        station.household_credits, query::station_wage_cr_per_day(w, station), workers,
        query::in_labour_market(w, station) ? station.jobs : workers, query::in_labour_market(w, station) ? "true" : "false", incomes, surface,
        station.ledger.days_in_red, query::station_lifetime_profit(w, station), query::station_profit_last_year(w, station));
}

void step_colony_labour(World& w, double dt_s) {
    // Local hiring (labour market): the people of a colony take its gateway station's jobs when the
    // wage gain over a stay beats the ascent (a seat to orbit at the colony's launch price), and the
    // station's unemployed go down to work on the surface. No ship is needed either way.
    if (!query::labour_on(w)) {
        return;
    }
    const auto& labour = w.data.universe.labour_market;
    const double dt_days = dt_s / 86400.0;
    const double participation = std::max(1e-6, w.data.universe.households.participation);
    for (const auto& economy : w.data.universe.background_economies) {
        const auto colony = w.state.background_population.find(economy.id);
        if (colony == w.state.background_population.end()) {
            continue;
        }
        double& colonists = colony->second;
        // Natural growth; the rest of its growth it recruits on Earth (they come by ship). A colony
        // short of its imports grows slower, and shrinks below half of them.
        colonists *= std::exp((1.0 - economy.immigration_share) * economy.growth_per_year * colony_growth_factor(w, economy.id)
            * dt_days / 365.25);
        // Its wage median follows its income per head (relative to the imports it was sized for).
        if (const auto state = w.state.colony_state.find(economy.id);
            state != w.state.colony_state.end() && w.data.universe.households.colony_wage_follows_income > 0.0
            && state->second.base_imports_per_day > 0.0) {
            const double ratio = std::clamp(state->second.income_per_day / state->second.base_imports_per_day, 0.5, 2.0);
            const double target = economy.wage_median_cr_per_day * std::sqrt(ratio);
            state->second.wage_median += (target - state->second.wage_median)
                * std::min(1.0, dt_days / std::max(1.0, w.data.universe.households.colony_wage_relaxation_days));
        }
        if (const double recruits = colony_recruits_per_day(w, economy) * dt_days; recruits > 0.0 && !economy.gateways.empty()) {
            const auto& gateway = economy.gateways.front();
            post_travellers(w, query::get_station_state(w, gateway_for(w, gateway, true)), domain::PassengerKind::Migrant, gateway,
                "colony:" + economy.id, recruits, true);
        }
        for (const auto& gateway_id : economy.gateways) {
            auto& station = query::get_station_state(w, gateway_id);
            if (!query::in_labour_market(w, station)) {
                continue;
            }
            const double workers = query::station_workers(w, station);
            // Up: those whose wage on the surface is low enough that the station's job, less the
            // hardship, repays the ascent over a stay.
            const double ascent_cr = economy.launch_cr_per_kg * query::passenger_kg(w, 0.0);
            const double ceiling = query::station_wage_cr_per_day(w, station) * (1.0 - labour.hardship_share)
                - ascent_cr / (365.25 * labour.stay_years);
            const double share = ceiling > 0.0
                ? 0.5 * std::erfc(-std::log(ceiling / query::colony_wage_median(w, economy)) / economy.wage_sigma / std::sqrt(2.0)) : 0.0;
            const double needed = std::max(0.0, station.jobs - workers) / participation;
            const double hires = std::min({needed * std::min(1.0, dt_days / COLONY_FILL_DAYS),
                colonists * economy.search_per_year * share * dt_days / 365.25, colonists});
            // Down: the unemployed take work on the surface.
            const double idle = std::max(0.0, workers - station.jobs) / participation;
            const double down = idle * std::min(1.0, dt_days / COLONY_FILL_DAYS);
            if (hires <= 0.0 && down <= 0.0) {
                continue;
            }
            // Savings follow the people: what a colonist brings, what a resident takes.
            const double brought = hires * w.data.universe.households.migrant_savings_cr;
            const double taken = down * per_capita_savings(station);
            economy_account(w, economy.id) -= brought - taken;
            station.household_credits += brought - taken;
            w.state.journal.record(query::account_economy(w, economy.id), query::account_households(w, station), MoneyCategory::MigrantSavings, brought - taken);
            colonists += down - hires;
            station.population += hires - down;
            station.ledger.hired_from_surface += hires;
            station.ledger.moved_to_surface += down;
            w.engines.economy.set_population(station.station_id, station.population);
        }
    }
}

std::string background_economies_json(const World& w) {
    double savings = 0.0;
    for (const auto& station : w.state.stations) {
        savings += station.household_credits;
    }
    std::string list = "[";
    for (const auto& economy : w.data.universe.background_economies) {
        const auto people = w.state.background_population.find(economy.id);
        list += std::format("{}{{\"id\":\"{}\",\"name\":\"{}\",\"credits\":{:.0f},\"population\":{:.0f}}}", list.size() > 1 ? "," : "",
            json_escape(economy.id), json_escape(economy.name), query::economy_balance(w, economy.id), people == w.state.background_population.end() ? 0.0 : people->second);
    }
    return std::format("\"residents_savings\":{:.0f},\"background_economies\":{}]", savings, list);
}

const domain::StationDefinition* yard_station_for(const World& w, const domain::StationDefinition& home) {
    const domain::StationDefinition* best = nullptr;
    double best_days = std::numeric_limits<double>::infinity();
    for (const auto& yard_id : w.data.universe.households.yards) {
        const auto& yard = query::get_station_definition(w, yard_id);
        const double days = yard.id == home.id ? 0.0 : w.engines.economy.transfer_days(yard, home);
        if (days < best_days) {
            best_days = days;
            best = &yard;
        }
    }
    return best;
}

}  // namespace spacetrains::simulation::systems
