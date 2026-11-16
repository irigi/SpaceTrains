#include "simulation/query/Finance.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>

#include "simulation/SimulationInternal.hpp"
#include "simulation/query/Labour.hpp"
#include "simulation/query/Lookups.hpp"

namespace spacetrains::simulation::query {

using namespace detail;

double faction_credit_limit(const World& w, const std::string& faction_id) {
    const auto& open = w.universe_.open_economy;
    double fleet_value = 0.0;
    for (const auto& ship : w.ships_) {
        if (ship.faction_id == faction_id) {
            fleet_value += query::ship_design(w, ship).ship_value_cr;
        }
    }
    // Its stations' stock is collateral too, at reference prices (step 26): after step 23 a
    // remote colony stocks a year or more of its imports, which its faction paid for; Mars
    // Corporation, with one station and few ships, went over its limit building that stock.
    double stock_value = 0.0;
    for (const auto& station : w.stations_) {
        const auto& definition = query::get_station_definition(w, station.station_id);
        if (definition.faction_id != faction_id) {
            continue;
        }
        for (const auto& [commodity_id, units] : station.inventory) {
            stock_value += std::max(0.0, units) * w.economy_.reference_price(definition, commodity_id);
        }
    }
    const auto tax = w.faction_tax_per_day_.find(faction_id);
    return open.faction_loan_to_value * (fleet_value + stock_value)
        + open.faction_tax_years * 365.0 * (tax == w.faction_tax_per_day_.end() ? 0.0 : tax->second);
}

double economy_balance(const World& w, const std::string& economy_id) {
    if (economy_id == EARTH_ECONOMY) {
        return w.outside_economy_credits_;
    }
    const auto it = w.background_credits_.find(economy_id);
    return it == w.background_credits_.end() ? 0.0 : it->second;
}

double economy_credit_line(const World& w, const std::string& economy_id) {
    // Earth's economy is too large to run out; a colony spends down to minus its credit line.
    if (economy_id == EARTH_ECONOMY) {
        return std::numeric_limits<double>::infinity();
    }
    for (const auto& economy : w.universe_.background_economies) {
        if (economy.id == economy_id) {
            return economy.initial_credits;
        }
    }
    return 0.0;
}

double colony_wage_median(const World& w, const domain::BackgroundEconomyDefinition& economy) {
    const auto* state = query::colony_state(w, economy.id);
    return state != nullptr && w.universe_.households.colony_wage_follows_income > 0.0 && state->wage_median > 0.0
        ? state->wage_median : economy.wage_median_cr_per_day;
}

double station_lifetime_profit(const World& w, const domain::StationState& station) {
    const auto& definition = query::get_station_definition(w, station.station_id);
    const auto stock_value = [&](const domain::Inventory& inventory) {
        double value = 0.0;
        for (const auto& commodity : w.universe_.commodities) {
            const auto it = inventory.find(commodity.id);
            const double stock = it == inventory.end() ? 0.0 : std::max(0.0, it->second);
            value += w.economy_.get_trade_value(definition, commodity.id, 0.0, stock, commodity.base_price);
        }
        return value;
    };
    return station.credits - definition.initial_credits + station.ledger.taxes - station.ledger.subsidies
        + stock_value(station.inventory) - stock_value(w.starting_inventories_.at(station.station_id));
}

double station_profit_last_year(const World& w, const domain::StationState& station) {
    const auto& history = w.journal_.history();
    const auto index = static_cast<std::size_t>(&station - w.stations_.data());
    const double year_ago = history.size() > 12 ? history[history.size() - 13].station_profit.at(index) : 0.0;
    return query::station_lifetime_profit(w, station) - year_ago;
}

double resident_wages_per_day(const World& w, const domain::StationState& station) {
    // Employed workers earn the station's wage; with the labour market on, the unemployed draw
    // a benefit from the station.
    const double workers = query::station_workers(w, station);
    const double wage = query::station_wage_cr_per_day(w, station);
    if (!query::in_labour_market(w, station)) {
        return workers * wage;
    }
    const double employed = std::min(workers, station.jobs);
    return wage * (employed + w.universe_.labour_market.unemployment_benefit * std::max(0.0, workers - employed));
}

double household_travel_budget(const World& w, const std::string& home_id, double window_days) {
    // What a station's residents may spend on trips over the next window_days (households on).
    const auto& home = query::get_station_state(w, home_id);
    const auto& households = w.universe_.households;
    if (households.travel_share > 0.0) {
        return home.travel_budget_cr + households.travel_share * query::resident_wages_per_day(w, home) * std::max(0.0, window_days);
    }
    // Without a travel share: their savings beyond half their buffer.
    return home.household_credits - 0.5 * households.savings_days * query::resident_wages_per_day(w, home);
}

}  // namespace spacetrains::simulation::query
