// Households and background economies (docs/plans/economy_overhaul.md).
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

#include "simulation/Simulation.hpp"

namespace spacetrains::simulation {

namespace {
constexpr const char* EARTH_ECONOMY = "earth";
// A small colony's supply reaches its full rate once the station pays this multiple of its ask.
constexpr double SURFACE_SUPPLY_FULL_PRICE = 1.25;
// A colony buying at its gateway leaves the station this many days of its own use.
constexpr double SURFACE_RESERVE_DAYS = 14.0;
// A colony keeps its gateway stocked with at least this many days of the station's own use.
constexpr double SURFACE_COVER_DAYS = 30.0;
// What a station paid per unit is averaged over about this many days of its use (as in Simulation.cpp).
constexpr double IMPORT_COST_DAYS = 30.0;
}  // namespace

double& Simulation::economy_account(const std::string& economy_id) {
    return economy_id == EARTH_ECONOMY ? outside_economy_credits_ : background_credits_[economy_id];
}

double Simulation::economy_balance(const std::string& economy_id) const {
    if (economy_id == EARTH_ECONOMY) {
        return outside_economy_credits_;
    }
    const auto it = background_credits_.find(economy_id);
    return it == background_credits_.end() ? 0.0 : it->second;
}

double Simulation::economy_credit_line(const std::string& economy_id) const {
    // Earth's economy is too large to run out; a colony spends down to minus its credit line.
    if (economy_id == EARTH_ECONOMY) {
        return std::numeric_limits<double>::infinity();
    }
    for (const auto& economy : universe_.background_economies) {
        if (economy.id == economy_id) {
            return economy.initial_credits;
        }
    }
    return 0.0;
}

double Simulation::resident_wages_per_day(const domain::StationState& station) const {
    // Employed workers earn the station's wage; with the labour market on, the unemployed draw
    // a benefit from the station.
    const double workers = station_workers(station);
    const double wage = station_wage_cr_per_day(station);
    if (!in_labour_market(station)) {
        return workers * wage;
    }
    const double employed = std::min(workers, station.jobs);
    return wage * (employed + universe_.labour_market.unemployment_benefit * std::max(0.0, workers - employed));
}

double Simulation::per_capita_savings(const domain::StationState& station) const {
    return station.population > 0.0 ? std::max(0.0, station.household_credits) / station.population : 0.0;
}

void Simulation::pay_traveller_fare(domain::StationState& origin, const domain::PassengerGroup& group, double fares) {
    // A visitor going home pays from home; everyone else from where they board. Travellers from
    // an Earth gateway are Earth's people (Earth's account); a station's residents pay from
    // their households (the outside account before households existed).
    if (!households_on()) {
        outside_economy_credits_ -= fares;
        return;
    }
    const std::string& home = group.kind == domain::PassengerKind::VisitorHome ? group.destination_id : origin.station_id;
    if (is_earth_gateway(home)) {
        outside_economy_credits_ -= fares;
    } else {
        auto& home_state = get_station_state(home);
        home_state.household_credits -= fares;
        home_state.travel_budget_cr = std::max(0.0, home_state.travel_budget_cr - fares);
    }
}

double Simulation::household_travel_budget(const std::string& home_id, double window_days) const {
    // What a station's residents may spend on trips over the next window_days (households on).
    const auto& home = get_station_state(home_id);
    const auto& households = universe_.households;
    if (households.travel_share > 0.0) {
        return home.travel_budget_cr + households.travel_share * resident_wages_per_day(home) * std::max(0.0, window_days);
    }
    // Without a travel share: their savings beyond half their buffer.
    return home.household_credits - 0.5 * households.savings_days * resident_wages_per_day(home);
}

void Simulation::step_households(double dt_s) {
    if (!households_on()) {
        return;
    }
    const auto& households = universe_.households;
    const double dt_days = dt_s / 86400.0;
    for (auto& station : stations_) {
        if (station.credits < 0.0) {
            station.ledger.days_in_red += dt_days;
        }
        // Wages: the station employs its residents.
        const double wages = resident_wages_per_day(station) * dt_days;
        station.credits -= wages;
        station.household_credits += wages;
        station.ledger.wages_paid += wages;
        // A share of it is put aside for long trips (a year's worth at most).
        if (households.travel_share > 0.0) {
            station.travel_budget_cr = std::min(station.travel_budget_cr + households.travel_share * wages,
                households.travel_share * resident_wages_per_day(station) * 365.25);
        }
        // Spending beyond the goods they used up (local services, rent, taxes): savings above
        // the buffer go back to the station over spend_days.
        const double buffer = households.savings_days * resident_wages_per_day(station);
        if (station.household_credits > buffer) {
            const double spend = (station.household_credits - buffer) * std::min(1.0, dt_days / households.spend_days);
            station.household_credits -= spend;
            station.credits += spend;
            station.ledger.household_spending += spend;
        }
        // Visitors spend during their stay, out of their home's account.
        if (households.visitor_spend_cr_per_day > 0.0) {
            for (const auto& stay : station.visiting) {
                const double spend = stay.count * households.visitor_spend_cr_per_day * dt_days;
                if (is_earth_gateway(stay.destination_id)) {
                    outside_economy_credits_ -= spend;
                } else {
                    get_station_state(stay.destination_id).household_credits -= spend;
                }
                station.credits += spend;
                station.ledger.visitor_spending += spend;
            }
        }
    }
    // Earth invests in the small colonies.
    for (const auto& economy : universe_.background_economies) {
        if (economy.income_cr_per_day > 0.0) {
            const double amount = economy.income_cr_per_day * dt_days;
            outside_economy_credits_ -= amount;
            economy_account(economy.id) += amount;
        }
    }
    // Incomes from the background economies: science budgets, services, subsidies.
    const double ceiling = universe_.open_economy.station_credit_ceiling;
    for (const auto& income : universe_.station_incomes) {
        auto& station = get_station_state(income.station_id);
        // A grant tapers off as the station's cash nears the credit ceiling (agencies fund need).
        const double taper = income.need_based && ceiling > 0.0
            ? std::clamp(2.0 * (1.0 - station.credits / ceiling), 0.0, 1.0) : 1.0;
        const double amount = income.cr_per_day * taper * dt_days;
        if (amount <= 0.0) {
            continue;
        }
        economy_account(income.payer_id) -= amount;
        station.credits += amount;
        station.ledger.incomes += amount;
    }
}

void Simulation::step_surface_markets(double dt_s) {
    if (universe_.surface_markets.empty()) {
        return;
    }
    const double dt_days = dt_s / 86400.0;
    const double depth = economy_.surface_depth_units();
    for (auto& station : stations_) {
        const auto& definition = get_station_definition(station.station_id);
        const auto& markets = economy_.surface_markets_at(station.station_id);
        if (markets.empty()) {
            continue;
        }
        const double capacity = definition.storage_capacity_units;
        for (const auto* market : markets) {
            const auto& commodity_id = market->commodity_id;
            double& account = economy_account(market->economy_id);
            const bool can_pay = account > -economy_credit_line(market->economy_id);
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
                    const double price = station_price(station, commodity_id);
                    double own_use = 0.0;
                    for (const auto* recipe : economy_.recipes_of(definition)) {
                        if (recipe->commodity_id == commodity_id && recipe->units_per_day < 0.0) {
                            own_use -= recipe->units_per_day * economy_.population_factor(definition);
                        }
                    }
                    const double eagerness = stock < SURFACE_COVER_DAYS * own_use
                        ? 1.0
                        : std::clamp((price / market->ask_cr - 1.0) / (SURFACE_SUPPLY_FULL_PRICE - 1.0), 0.0, 1.0);
                    const bool room = capacity <= 0.0 || economy_.storage_used_units(definition, station.inventory) < 0.85 * capacity;
                    units = room ? market->sell_units_per_day * eagerness * dt_days : 0.0;
                }
                if (units > 0.0) {
                    const double paid = units * market->ask_cr;
                    station.credits -= paid;
                    account += paid;
                    station.ledger.surface_bought += paid;
                    // Residents pay what the station paid for it (its import cost).
                    const auto& rates = economy_.get_station_net_rates(definition);
                    const auto rate = rates.find(commodity_id);
                    const double history = rate == rates.end() ? 0.0 : std::max(0.0, -rate->second) * IMPORT_COST_DAYS;
                    const auto known = station.import_unit_cost.find(commodity_id);
                    const double previous = known == station.import_unit_cost.end() ? market->ask_cr : known->second;
                    station.import_unit_cost[commodity_id] = (previous * history + paid) / (history + units);
                    stock += units;
                }
            }
            // The surface buys.
            const double bid = economy_.surface_bid(definition, commodity_id);
            if (bid > 0.0 && can_pay && stock > 0.0) {
                double units = 0.0;
                if (market->ask_cr <= 0.0 && market->bid_share <= 0.0 && market->buy_units_per_day <= 0.0) {
                    units = stock;   // an export market takes it all
                } else {
                    // Keep the station's own use (and what the surface stocks here) for the station.
                    const auto& rates = economy_.get_station_net_rates(definition);
                    const auto rate = rates.find(commodity_id);
                    double own_use = rate == rates.end() ? 0.0 : std::max(0.0, -rate->second);
                    if (market->buy_units_per_day > 0.0) {
                        own_use = std::max(0.0, own_use - market->buy_units_per_day);
                    }
                    const double reserve = std::max(own_use * SURFACE_RESERVE_DAYS,
                        economy_.surface_supplied(definition, commodity_id) ? depth : 0.0);
                    units = std::clamp(stock - reserve, 0.0,
                        market->buy_units_per_day > 0.0 ? market->buy_units_per_day * dt_days : stock);
                }
                if (units > 0.0) {
                    const double paid = units * bid;
                    account -= paid;
                    station.credits += paid;
                    station.ledger.surface_sold += paid;
                    station.market_sold_units[commodity_id] += units;
                    stock -= units;
                }
            }
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

bool Simulation::in_labour_market(const domain::StationState& station) const {
    return labour_on() && !is_earth_gateway(station.station_id);
}

double Simulation::station_wage_cr_per_day(const domain::StationState& station) const {
    if (in_labour_market(station) && station.wage_cr_per_day > 0.0) {
        return station.wage_cr_per_day;
    }
    const auto& households = universe_.households;
    const auto multiple = households.wage_multiple.find(station.station_id);
    return universe_.labour.reference_wage_cr_per_day * (multiple == households.wage_multiple.end() ? 1.0 : multiple->second);
}

double Simulation::station_workers(const domain::StationState& station) const {
    return station.population * universe_.households.participation;
}

double Simulation::labour_migrants_per_day(const domain::StationState& station) const {
    // Vacancies go to Earth's job seekers over vacancy_fill_days; people already coming fill them.
    if (!in_labour_market(station) || !migration_by_ship()) {
        return 0.0;
    }
    const double participation = std::max(1e-6, universe_.households.participation);
    const double people_needed = std::max(0.0, station.jobs - station_workers(station)) / participation
        - travellers_pending(station.station_id, domain::PassengerKind::Migrant);
    return std::max(0.0, people_needed) / universe_.labour_market.vacancy_fill_days;
}

double Simulation::labour_emigrants_per_day(const domain::StationState& station) const {
    // The unemployed look for a passage home (with their families).
    if (!in_labour_market(station) || !migration_by_ship()) {
        return 0.0;
    }
    const double participation = std::max(1e-6, universe_.households.participation);
    const double people_out = std::max(0.0, station_workers(station) - station.jobs) / participation
        - travellers_pending(station.station_id, domain::PassengerKind::Emigrant);
    return std::max(0.0, people_out) / universe_.labour_market.unemployed_leave_days;
}

double Simulation::settler_gain_tail(const domain::StationState& destination, double gain_cr) const {
    // A job seeker on Earth earning w_E gains (w_S (1 - hardship) - w_E) x the stay; the share of
    // seekers who gain at least `gain_cr` is P(w_E <= w_S (1 - hardship) - gain / stay).
    const auto& labour = universe_.labour_market;
    const double stay_days = 365.25 * labour.stay_years;
    const double ceiling = station_wage_cr_per_day(destination) * (1.0 - labour.hardship_share) - gain_cr / stay_days;
    if (ceiling <= 0.0) {
        return 0.0;
    }
    return normal_cdf(std::log(ceiling / labour.earth_wage_median_cr_per_day) / labour.earth_wage_sigma);
}

double Simulation::returner_gain_tail(double gain_cr) const {
    // The unemployed going home gain an Earth wage over a stay.
    const auto& labour = universe_.labour_market;
    return lognormal_tail(labour.earth_wage_median_cr_per_day * 365.25 * labour.stay_years, labour.earth_wage_sigma, gain_cr);
}

double Simulation::traveller_budget_tail(const std::string& home_id, double cost_cr) const {
    // A trip's budget is a share of a year's income: Earth's income spread from an Earth
    // gateway, the station's wage (with its own spread) elsewhere.
    const auto& labour = universe_.labour_market;
    const bool earth = is_earth_gateway(home_id);
    const double income = earth ? labour.earth_wage_median_cr_per_day : station_wage_cr_per_day(get_station_state(home_id));
    const double sigma = earth ? labour.visitor_income_sigma : labour.station_wage_sigma;
    return lognormal_tail(income * 365.25 * labour.visitor_budget_years, sigma, cost_cr);
}

void Simulation::step_labour(double dt_s) {
    if (!labour_on()) {
        return;
    }
    const auto& labour = universe_.labour_market;
    const auto& growth = universe_.growth;
    const double dt_years = dt_s / 86400.0 / 365.25;
    const double floor_credits = universe_.open_economy.station_credit_floor;
    for (auto& station : stations_) {
        if (!in_labour_market(station)) {
            continue;
        }
        const auto& definition = get_station_definition(station.station_id);
        const double participation = universe_.households.participation;
        const double seeded_workers = static_cast<double>(definition.population) * participation;
        if (station.wage_cr_per_day <= 0.0) {
            station.wage_cr_per_day = station_wage_cr_per_day(station);   // the data's wage level to start with
        }
        if (station.jobs <= 0.0) {
            station.jobs = station_workers(station);   // full employment to start with
        }
        // Jobs grow while the station is supplied, pays its way and its plants run near capacity;
        // they are cut when it is not supplied, is deep in the red or its plants stand idle (phase C:
        // output follows price, so idle plants are what a glut leaves).
        double output_value = 0.0;
        double running_value = 0.0;
        for (const auto* recipe : economy_.recipes_of(definition)) {
            if (recipe->units_per_day > 0.0) {
                const double value = recipe->units_per_day * economy_.reference_price(definition, recipe->commodity_id);
                const auto supply = station.supply_factor.find(recipe->commodity_id);
                output_value += value;
                running_value += value * (supply == station.supply_factor.end() ? 1.0 : supply->second);
            }
        }
        const double utilization = output_value > 0.0 ? running_value / output_value : 1.0;
        double rate = 0.0;
        if (station.supply_index >= growth.grow_above && station.credits >= floor_credits && utilization >= 0.8) {
            rate = growth.growth_per_year;
        } else if (station.supply_index < growth.decline_below || station.credits < -floor_credits || utilization < 0.5) {
            rate = -growth.decline_per_year;
        }
        station.jobs = std::clamp(station.jobs * std::exp(rate * dt_years),
            universe_.open_economy.core_crew_fraction * seeded_workers, growth.max_population_factor * seeded_workers);
        // Wages follow the vacancy rate (up where jobs go unfilled, down where people are idle).
        const double workers = std::max(1.0, station_workers(station));
        const double vacancy_rate = std::clamp((station.jobs - workers) / workers, -1.0, 1.0);
        station.wage_cr_per_day = std::max(labour.min_wage_multiple * universe_.labour.reference_wage_cr_per_day,
            station.wage_cr_per_day * std::exp(labour.wage_adjust_per_year * vacancy_rate * dt_years));
        // Plants run with the workers employed (jobs filled / workers). The share the economy runs
        // on moves in steps of half a percent (or to full employment), so net rates are rebuilt
        // rarely; it is saved with the station.
        const double share = std::min(1.0, station.jobs / workers);
        if (std::abs(share - station.employed_share) >= 0.005 || (share == 1.0 && station.employed_share != 1.0)) {
            station.employed_share = share;
            economy_.set_employment(station.station_id, share);
        }
    }
}

void Simulation::update_market_responses() {
    // Phase C: each tick's supply and demand multipliers from today's prices and wages.
    const auto& h = universe_.households;
    const bool supply_on = h.supply_price_elasticity > 0.0;
    const bool demand_on = h.consume_income_elasticity != 0.0 || h.consume_price_elasticity != 0.0
        || h.upkeep_price_elasticity != 0.0;
    if (!households_on() || (!supply_on && !demand_on)) {
        return;
    }
    for (auto& station : stations_) {
        const auto& definition = get_station_definition(station.station_id);
        station.supply_factor.clear();
        station.demand_factor.clear();
        const double starting_wage = [&] {
            const auto multiple = h.wage_multiple.find(station.station_id);
            return universe_.labour.reference_wage_cr_per_day * (multiple == h.wage_multiple.end() ? 1.0 : multiple->second);
        }();
        const double income_ratio = starting_wage > 0.0 ? station_wage_cr_per_day(station) / starting_wage : 1.0;
        for (const auto* recipe : economy_.recipes_of(definition)) {
            const auto& commodity_id = recipe->commodity_id;
            const double reference = economy_.reference_price(definition, commodity_id);
            if (reference <= 0.0) {
                continue;
            }
            // A price the station gets from a surface economy or an export market is what it
            // is; elsewhere the station's own curve on its stock.
            const double price_ratio = station_price(station, commodity_id) / reference;
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

std::string Simulation::station_economy_json(const domain::StationState& station) const {
    if (!households_on()) {
        return {};
    }
    const auto& definition = get_station_definition(station.station_id);
    const auto escape = [](const std::string& text) {
        std::string out;
        for (const char c : text) {
            if (c == '"' || c == '\\') {
                out += '\\';
            }
            out += c;
        }
        return out;
    };
    std::string incomes = "[";
    for (const auto& income : universe_.station_incomes) {
        if (income.station_id == station.station_id) {
            incomes += std::format("{}{{\"kind\":\"{}\",\"payer\":\"{}\",\"cr_per_year\":{:.0f}}}", incomes.size() > 1 ? "," : "",
                escape(income.kind), escape(income.payer_id), income.cr_per_day * 365.25);
        }
    }
    incomes += "]";
    std::string surface = "[";
    for (const auto* market : economy_.surface_markets_at(station.station_id)) {
        const auto economy = std::find_if(universe_.background_economies.begin(), universe_.background_economies.end(),
            [&](const auto& e) { return e.id == market->economy_id; });
        surface += std::format("{}{{\"economy\":\"{}\",\"good\":\"{}\",\"ask\":{:.2f},\"bid\":{:.2f},\"units_per_day\":{:.1f}}}",
            surface.size() > 1 ? "," : "", escape(economy == universe_.background_economies.end() ? market->economy_id : economy->name),
            escape(market->commodity_id), market->ask_cr, economy_.surface_bid(definition, market->commodity_id),
            market->ask_cr > 0.0 ? market->sell_units_per_day : market->buy_units_per_day);
    }
    surface += "]";
    const double workers = station_workers(station);
    return std::format("\"household_credits\":{:.0f},\"wage_cr_per_day\":{:.3f},\"workers\":{:.0f},\"jobs\":{:.0f},"
        "\"labour_market\":{},\"incomes\":{},\"surface_trade\":{},\"days_in_red\":{:.0f},",
        station.household_credits, station_wage_cr_per_day(station), workers,
        in_labour_market(station) ? station.jobs : workers, in_labour_market(station) ? "true" : "false", incomes, surface,
        station.ledger.days_in_red);
}

const domain::StationDefinition* Simulation::yard_station_for(const domain::StationDefinition& home) const {
    const domain::StationDefinition* best = nullptr;
    double best_days = std::numeric_limits<double>::infinity();
    for (const auto& yard_id : universe_.households.yards) {
        const auto& yard = get_station_definition(yard_id);
        const double days = yard.id == home.id ? 0.0 : economy_.transfer_days(yard, home);
        if (days < best_days) {
            best_days = days;
            best = &yard;
        }
    }
    return best;
}

}  // namespace spacetrains::simulation
