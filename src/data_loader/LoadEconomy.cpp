// Economy data: labour, fuel, households and colonies, incomes, treasuries, investment, liners, growth, prices, orders and the market.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "data_loader/LoaderInternal.hpp"

namespace spacetrains::data_loader::detail {

void load_labour(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto labour_path = root / "economy" / "labour.csv";
    // The wage scale: key,value rows, grades as "grade.<name>" multiples of the reference.
    const auto rows = read_csv_rows(labour_path);
    require_header(rows.front(), {"key", "value"}, labour_path);
    std::unordered_set<std::string> seen_keys;
    auto& labour = universe.labour;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        require_field_count(row, 2, labour_path, i + 1);
        require_unique_id(row[0], seen_keys, labour_path, i + 1);
        const double value = parse_double(row[1], labour_path, i + 1, row[0].c_str());
        if (value <= 0.0) {
            throw std::runtime_error("'" + row[0] + "' must be positive in " + labour_path.string());
        }
        if (row[0] == "reference_wage_cr_per_day") {
            labour.reference_wage_cr_per_day = value;
        } else if (row[0] == "shift_hours") {
            labour.shift_hours = value;
        } else if (row[0].starts_with("grade.")) {
            labour.grades[row[0].substr(6)] = value;
        } else {
            throw std::runtime_error("Unknown key '" + row[0] + "' in " + labour_path.string());
        }
    }
    if (!labour.grades.contains("crew")) {
        throw std::runtime_error("Missing key 'grade.crew' in " + labour_path.string());
    }
    // Crews are paid at the crew grade.
    universe.ship_operations.wage_cr_per_crew_day = labour.wage_cr_per_day("crew");
}

void load_fuel_supply(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto fuel_supply_path = root / "economy" / "fuel_supply.csv";
    const auto rows = read_csv_rows(fuel_supply_path);
    require_header(rows.front(), {"key", "value"}, fuel_supply_path);
    std::unordered_set<std::string> seen_keys;
    auto& fuel_supply = universe.fuel_supply;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        require_field_count(row, 2, fuel_supply_path, i + 1);
        require_unique_id(row[0], seen_keys, fuel_supply_path, i + 1);
        const double value = parse_double(row[1], fuel_supply_path, i + 1, row[0].c_str());
        if (value < 0.0) {
            throw std::runtime_error("'" + row[0] + "' must not be negative in " + fuel_supply_path.string());
        }
        if (row[0] == "factory_buffer_days") {
            fuel_supply.factory_buffer_days = value;
        } else {
            throw std::runtime_error("Unknown key '" + row[0] + "' in " + fuel_supply_path.string());
        }
    }
    for (const char* required : {"factory_buffer_days"}) {
        if (!seen_keys.contains(required)) {
            throw std::runtime_error(std::string("Missing key '") + required + "' in " + fuel_supply_path.string());
        }
    }
}

// Economy overhaul: households, background economies, surface markets, station incomes
// (docs/modules/economy.md). All optional; without households.csv the economy runs as before.
void load_households(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    if (const auto households_path = root / "economy" / "households.csv"; std::filesystem::exists(households_path)) {
        const auto rows = read_csv_rows(households_path);
        require_header(rows.front(), {"key", "value"}, households_path);
        auto& households = universe.households;
        const std::unordered_map<std::string, double*> fields {
            {"enabled", &households.enabled},
            {"participation", &households.participation},
            {"savings_days", &households.savings_days},
            {"spend_days", &households.spend_days},
            {"migrant_savings_cr", &households.migrant_savings_cr},
            {"visitor_spend_cr_per_day", &households.visitor_spend_cr_per_day},
            {"credits_per_usd", &households.credits_per_usd},
            {"yard_share", &households.yard_share},
            {"yard_materials_share", &households.yard_materials_share},
            {"travel_share", &households.travel_share},
            {"science_price_premium", &households.science_price_premium},
            {"surface_depth_units", &households.surface_depth_units},
            {"supply_price_elasticity", &households.supply_price_elasticity},
            {"min_utilization", &households.min_utilization},
            {"consume_income_elasticity", &households.consume_income_elasticity},
            {"consume_price_elasticity", &households.consume_price_elasticity},
            {"upkeep_price_elasticity", &households.upkeep_price_elasticity},
            {"colony_centre_markup", &households.colony_centre_markup},
            {"colony_stock_days", &households.colony_stock_days},
            {"colony_import_buffer_days", &households.colony_import_buffer_days},
            {"colony_satisfaction_days", &households.colony_satisfaction_days},
            {"colony_people_scaling", &households.colony_people_scaling},
            {"colony_growth_follows_satisfaction", &households.colony_growth_follows_satisfaction},
            {"colony_wage_follows_income", &households.colony_wage_follows_income},
            {"colony_wage_relaxation_days", &households.colony_wage_relaxation_days},
        };
        std::unordered_set<std::string> seen_keys;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 2, households_path, i + 1);
            require_unique_id(row[0], seen_keys, households_path, i + 1);
            if (row[0] == "colony_essentials") {
                std::stringstream stream(row[1]);
                std::string commodity_id;
                while (std::getline(stream, commodity_id, '|')) {
                    require_commodity(universe, commodity_id, households_path);
                    households.colony_essentials.push_back(commodity_id);
                }
                continue;
            }
            if (row[0] == "yards") {
                std::stringstream stream(row[1]);
                std::string station_id;
                while (std::getline(stream, station_id, '|')) {
                    require_station(universe, station_id, households_path);
                    households.yards.push_back(station_id);
                }
                continue;
            }
            const double value = parse_double(row[1], households_path, i + 1, row[0].c_str());
            if (value < 0.0 && !row[0].ends_with("price_elasticity")) {
                throw std::runtime_error("'" + row[0] + "' must not be negative in " + households_path.string());
            }
            if (row[0].starts_with("colony_stock_days.")) {
                const auto commodity_id = row[0].substr(std::string("colony_stock_days.").size());
                require_commodity(universe, commodity_id, households_path);
                households.colony_stock_days_by_good[commodity_id] = value;
                continue;
            }
            if (row[0].starts_with("wage_multiple.")) {
                const auto station_id = row[0].substr(std::string("wage_multiple.").size());
                require_station(universe, station_id, households_path);
                households.wage_multiple[station_id] = value;
                continue;
            }
            const auto field = fields.find(row[0]);
            if (field == fields.end()) {
                throw std::runtime_error("Unknown key '" + row[0] + "' in " + households_path.string());
            }
            *field->second = value;
        }
        if (households.spend_days <= 0.0 || households.yard_share > 1.0) {
            throw std::runtime_error("spend_days must be positive and yard_share at most 1 in " + households_path.string());
        }
    }
}

void load_labour_market(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto labour_path = root / "economy" / "labour.csv";
    if (const auto labour_path = root / "economy" / "labour_market.csv"; std::filesystem::exists(labour_path)) {
        const auto rows = read_csv_rows(labour_path);
        require_header(rows.front(), {"key", "value"}, labour_path);
        auto& labour = universe.labour_market;
        const std::unordered_map<std::string, double*> fields {
            {"enabled", &labour.enabled},
            {"earth_wage_median_cr_per_day", &labour.earth_wage_median_cr_per_day},
            {"earth_wage_sigma", &labour.earth_wage_sigma},
            {"station_wage_sigma", &labour.station_wage_sigma},
            {"stay_years", &labour.stay_years},
            {"hardship_share", &labour.hardship_share},
            {"wage_adjust_per_year", &labour.wage_adjust_per_year},
            {"min_wage_multiple", &labour.min_wage_multiple},
            {"vacancy_fill_days", &labour.vacancy_fill_days},
            {"unemployed_leave_days", &labour.unemployed_leave_days},
            {"unemployment_benefit", &labour.unemployment_benefit},
            {"visitor_budget_years", &labour.visitor_budget_years},
            {"visitor_income_sigma", &labour.visitor_income_sigma},
        };
        std::unordered_set<std::string> seen_keys;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 2, labour_path, i + 1);
            require_unique_id(row[0], seen_keys, labour_path, i + 1);
            const auto field = fields.find(row[0]);
            if (field == fields.end()) {
                throw std::runtime_error("Unknown key '" + row[0] + "' in " + labour_path.string());
            }
            *field->second = parse_double(row[1], labour_path, i + 1, row[0].c_str());
            if (*field->second < 0.0) {
                throw std::runtime_error("'" + row[0] + "' must not be negative in " + labour_path.string());
            }
        }
        if (labour.vacancy_fill_days <= 0.0 || labour.unemployed_leave_days <= 0.0 || labour.stay_years <= 0.0
            || labour.earth_wage_sigma <= 0.0 || labour.visitor_income_sigma <= 0.0) {
            throw std::runtime_error("labour_market.csv: days, years and spreads must be positive");
        }
    }
}

void load_background_economies(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    if (const auto economies_path = root / "economy" / "background_economies.csv"; std::filesystem::exists(economies_path)) {
        const auto rows = read_csv_rows(economies_path);
        require_header(rows.front(),
            {"id", "name", "gateways", "launch_usd_per_kg", "landing_usd_per_kg", "initial_credits", "income_cr_per_year",
                "population", "growth_per_year", "wage_median_cr_per_day", "wage_sigma", "search_per_year", "immigration_share"},
            economies_path);
        std::unordered_set<std::string> seen_ids;
        const double scale = universe.households.credits_per_usd;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 13, economies_path, i + 1);
            require_unique_id(row[0], seen_ids, economies_path, i + 1);
            domain::BackgroundEconomyDefinition economy {
                .id = row[0],
                .name = row[1],
                .launch_cr_per_kg = parse_double(row[3], economies_path, i + 1, "launch_usd_per_kg") * scale,
                .landing_cr_per_kg = parse_double(row[4], economies_path, i + 1, "landing_usd_per_kg") * scale,
                .initial_credits = parse_double(row[5], economies_path, i + 1, "initial_credits"),
                .income_cr_per_day = parse_double(row[6], economies_path, i + 1, "income_cr_per_year") / 365.25,
                .population = parse_double(row[7], economies_path, i + 1, "population"),
                .growth_per_year = parse_double(row[8], economies_path, i + 1, "growth_per_year"),
                .wage_median_cr_per_day = parse_double(row[9], economies_path, i + 1, "wage_median_cr_per_day"),
                .wage_sigma = parse_double(row[10], economies_path, i + 1, "wage_sigma"),
                .search_per_year = parse_double(row[11], economies_path, i + 1, "search_per_year"),
                .immigration_share = parse_double(row[12], economies_path, i + 1, "immigration_share"),
            };
            if (economy.population > 0.0 && (economy.wage_median_cr_per_day <= 0.0 || economy.wage_sigma <= 0.0)) {
                throw std::runtime_error("A colony with people needs a wage median and spread in " + economies_path.string());
            }
            std::stringstream stream(row[2]);
            std::string station_id;
            while (std::getline(stream, station_id, '|')) {
                require_station(universe, station_id, economies_path);
                economy.gateways.push_back(station_id);
            }
            if (economy.gateways.empty()) {
                throw std::runtime_error("Background economy '" + row[0] + "' has no gateway in " + economies_path.string());
            }
            universe.background_economies.push_back(std::move(economy));
        }
    }
}

void load_surface_markets(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    if (const auto markets_path = root / "economy" / "surface_markets.csv"; std::filesystem::exists(markets_path)) {
        // Each row: what an economy trades at one gateway (station_id), or at all of them (empty).
        // Ask = (surface + launch) x unit mass, bid = (surface - landing) x unit mass, in credits;
        // a bid_share instead sets the bid from the station's reference price at run time.
        const auto rows = read_csv_rows(markets_path);
        require_header(rows.front(),
            {"economy_id", "station_id", "commodity_id", "surface_usd_per_kg", "sells", "buys", "sell_units_per_day",
                "buy_units_per_day", "bid_share"},
            markets_path);
        const double scale = universe.households.credits_per_usd;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 9, markets_path, i + 1);
            const auto economy = std::find_if(universe.background_economies.begin(), universe.background_economies.end(),
                [&](const domain::BackgroundEconomyDefinition& e) { return e.id == row[0]; });
            if (economy == universe.background_economies.end()) {
                throw std::runtime_error("Unknown background economy '" + row[0] + "' in " + markets_path.string());
            }
            require_commodity(universe, row[2], markets_path);
            const auto commodity = std::find_if(universe.commodities.begin(), universe.commodities.end(),
                [&](const domain::CommodityDefinition& c) { return c.id == row[2]; });
            const double surface = parse_double(row[3], markets_path, i + 1, "surface_usd_per_kg") * scale;
            const bool sells = row[4] == "1";
            const bool buys = row[5] == "1";
            const double sell_units = parse_double(row[6], markets_path, i + 1, "sell_units_per_day");
            const double buy_units = parse_double(row[7], markets_path, i + 1, "buy_units_per_day");
            const double bid_share = parse_double(row[8], markets_path, i + 1, "bid_share");
            std::vector<std::string> stations;
            if (row[1].empty()) {
                stations = economy->gateways;
            } else {
                require_station(universe, row[1], markets_path);
                stations.push_back(row[1]);
            }
            for (const auto& station_id : stations) {
                domain::SurfaceMarketDefinition market {
                    .economy_id = row[0],
                    .station_id = station_id,
                    .commodity_id = row[2],
                    .ask_cr = sells ? (surface + economy->launch_cr_per_kg) * commodity->mass_per_unit_kg : 0.0,
                    .bid_cr = buys && bid_share <= 0.0
                        ? std::max(0.0, surface - economy->landing_cr_per_kg) * commodity->mass_per_unit_kg : 0.0,
                    .sell_units_per_day = sell_units,
                    .buy_units_per_day = buy_units,
                    .bid_share = buys ? bid_share : 0.0,
                };
                if (buys && market.bid_cr <= 0.0 && market.bid_share <= 0.0) {
                    throw std::runtime_error("A surface market that buys needs a positive bid in " + markets_path.string());
                }
                universe.surface_markets.push_back(std::move(market));
            }
        }
    }
}

void load_station_incomes(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    if (const auto incomes_path = root / "economy" / "station_incomes.csv"; std::filesystem::exists(incomes_path)) {
        const auto rows = read_csv_rows(incomes_path);
        require_header(rows.front(), {"station_id", "kind", "payer_id", "cr_per_year", "need_based"}, incomes_path);
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 5, incomes_path, i + 1);
            require_station(universe, row[0], incomes_path);
            const bool payer_known = std::any_of(universe.background_economies.begin(), universe.background_economies.end(),
                [&](const domain::BackgroundEconomyDefinition& e) { return e.id == row[2]; });
            if (!payer_known) {
                throw std::runtime_error("Unknown payer '" + row[2] + "' in " + incomes_path.string());
            }
            universe.station_incomes.push_back({
                .station_id = row[0],
                .kind = row[1],
                .payer_id = row[2],
                .cr_per_day = parse_double(row[3], incomes_path, i + 1, "cr_per_year") / 365.25,
                .need_based = row[4] == "1",
            });
        }
    }
}

void load_fuel_factories(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto fuel_factories_path = root / "economy" / "fuel_factories.csv";
    const auto rows = read_csv_rows(fuel_factories_path);
    // Optional third column: the factory's own price per unit (economy overhaul).
    const bool priced = rows.front().size() >= 3;
    require_header(rows.front(), priced ? std::vector<std::string> {"station_id", "output_units_per_day", "price_cr"}
                                        : std::vector<std::string> {"station_id", "output_units_per_day"}, fuel_factories_path);
    std::unordered_set<std::string> seen_ids;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        require_field_count(row, priced ? 3 : 2, fuel_factories_path, i + 1);
        if (priced) {
            const double price = parse_double(row[2], fuel_factories_path, i + 1, "price_cr");
            if (price > 0.0) {
                universe.fuel_supply.factory_price_cr[row[0]] = price;
            }
        }
        require_unique_id(row[0], seen_ids, fuel_factories_path, i + 1);
        const bool known = std::any_of(universe.stations.begin(), universe.stations.end(),
            [&](const domain::StationDefinition& station) { return station.id == row[0]; });
        if (!known) {
            throw std::runtime_error("Fuel factory at unknown station '" + row[0] + "' in " + fuel_factories_path.string());
        }
        const double output = parse_double(row[1], fuel_factories_path, i + 1, "output_units_per_day");
        if (output <= 0.0) {
            throw std::runtime_error("output_units_per_day must be positive in " + fuel_factories_path.string());
        }
        universe.fuel_supply.factory_output_units_per_day[row[0]] = output;
    }
}

void load_open_economy(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto open_economy_path = root / "economy" / "open_economy.csv";
    const auto rows = read_csv_rows(open_economy_path);
    require_header(rows.front(), {"key", "value"}, open_economy_path);
    std::unordered_set<std::string> seen_keys;
    auto& open_economy = universe.open_economy;
    const std::unordered_map<std::string, double*> fields {
        {"station_credit_floor", &open_economy.station_credit_floor},
        {"station_credit_ceiling", &open_economy.station_credit_ceiling},
        {"station_credit_ceiling_in_debt", &open_economy.station_credit_ceiling_in_debt},
        {"station_balance_days", &open_economy.station_balance_days},
        {"ship_cash_reserve", &open_economy.ship_cash_reserve},
        {"dividend_days", &open_economy.dividend_days},
        {"money_supply_days", &open_economy.money_supply_days},
        {"core_crew_fraction", &open_economy.core_crew_fraction},
        {"faction_loan_to_value", &open_economy.faction_loan_to_value},
        {"faction_tax_years", &open_economy.faction_tax_years},
    };
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        require_field_count(row, 2, open_economy_path, i + 1);
        require_unique_id(row[0], seen_keys, open_economy_path, i + 1);
        const double value = parse_double(row[1], open_economy_path, i + 1, row[0].c_str());
        if (value < 0.0) {
            throw std::runtime_error("'" + row[0] + "' must not be negative in " + open_economy_path.string());
        }
        const auto field = fields.find(row[0]);
        if (field == fields.end()) {
            throw std::runtime_error("Unknown key '" + row[0] + "' in " + open_economy_path.string());
        }
        *field->second = value;
    }
    for (const auto& [key, field] : fields) {
        if (!seen_keys.contains(key)) {
            throw std::runtime_error("Missing key '" + key + "' in " + open_economy_path.string());
        }
    }
    if (open_economy.station_balance_days <= 0.0 || open_economy.dividend_days <= 0.0) {
        throw std::runtime_error("station_balance_days and dividend_days must be positive in " + open_economy_path.string());
    }
    if (open_economy.station_credit_ceiling > 0.0
        && open_economy.station_credit_ceiling < open_economy.station_credit_floor) {
        throw std::runtime_error("station_credit_ceiling must not be below the floor in " + open_economy_path.string());
    }
    if (open_economy.station_credit_ceiling_in_debt > 0.0
        && (open_economy.station_credit_ceiling_in_debt < open_economy.station_credit_floor
            || (open_economy.station_credit_ceiling > 0.0
                && open_economy.station_credit_ceiling_in_debt > open_economy.station_credit_ceiling))) {
        throw std::runtime_error("station_credit_ceiling_in_debt must lie between the floor and the ceiling in "
            + open_economy_path.string());
    }
}

void load_fleet_investment(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto fleet_investment_path = root / "economy" / "fleet_investment.csv";
    const auto rows = read_csv_rows(fleet_investment_path);
    require_header(rows.front(), {"key", "value"}, fleet_investment_path);
    std::unordered_set<std::string> seen_keys;
    auto& investment = universe.fleet_investment;
    const std::unordered_map<std::string, double*> fields {
        {"review_days", &investment.review_days},
        {"hurdle_return_per_year", &investment.hurdle_return_per_year},
        {"working_capital", &investment.working_capital},
        {"build_days", &investment.build_days},
        {"layup_sale_days", &investment.layup_sale_days},
        {"salvage_fraction", &investment.salvage_fraction},
        {"route_commitment_days", &investment.route_commitment_days},
        {"max_ships_per_review", &investment.max_ships_per_review},
        {"purchase_spacing_days", &investment.purchase_spacing_days},
        {"max_fleet_size", &investment.max_fleet_size},
        {"max_fleet_hold_units", &investment.max_fleet_hold_units},
    };
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        require_field_count(row, 2, fleet_investment_path, i + 1);
        require_unique_id(row[0], seen_keys, fleet_investment_path, i + 1);
        const double value = parse_double(row[1], fleet_investment_path, i + 1, row[0].c_str());
        if (value < 0.0) {
            throw std::runtime_error("'" + row[0] + "' must not be negative in " + fleet_investment_path.string());
        }
        const auto field = fields.find(row[0]);
        if (field == fields.end()) {
            throw std::runtime_error("Unknown key '" + row[0] + "' in " + fleet_investment_path.string());
        }
        *field->second = value;
    }
    for (const auto& [key, field] : fields) {
        if (!seen_keys.contains(key)) {
            throw std::runtime_error("Missing key '" + key + "' in " + fleet_investment_path.string());
        }
    }
    if (investment.salvage_fraction > 1.0) {
        throw std::runtime_error("salvage_fraction must not exceed 1 in " + fleet_investment_path.string());
    }
}

void load_liners(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto liners_path = root / "economy" / "liners.csv";
    const auto rows = read_csv_rows(liners_path);
    require_header(rows.front(), {"ship_id", "stops"}, liners_path);
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        require_field_count(row, 2, liners_path, i + 1);
        const auto seed = std::find_if(universe.ship_seeds.begin(), universe.ship_seeds.end(),
            [&](const domain::ShipSeedDefinition& s) { return s.id == row[0]; });
        if (seed == universe.ship_seeds.end()) {
            throw std::runtime_error("Liner '" + row[0] + "' is not a ship in ships.csv (" + liners_path.string() + ")");
        }
        std::vector<std::string> stops;
        std::size_t start = 0;
        while (start <= row[1].size()) {
            const auto end = std::min(row[1].find('|', start), row[1].size());
            stops.push_back(row[1].substr(start, end - start));
            require_station(universe, stops.back(), liners_path);
            start = end + 1;
        }
        if (stops.size() < 2 || std::unordered_set<std::string>(stops.begin(), stops.end()).size() != stops.size()) {
            throw std::runtime_error("A liner needs two or more different stops in " + liners_path.string());
        }
        if (std::find(stops.begin(), stops.end(), seed->start_station_id) == stops.end()) {
            throw std::runtime_error("Liner '" + row[0] + "' must start at one of its stops in " + liners_path.string());
        }
        universe.liners[row[0]] = std::move(stops);
    }
}

void load_growth(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto growth_path = root / "economy" / "growth.csv";
    const auto rows = read_csv_rows(growth_path);
    require_header(rows.front(), {"key", "value"}, growth_path);
    std::unordered_set<std::string> seen_keys;
    auto& growth = universe.growth;
    const std::unordered_map<std::string, double*> fields {
        {"growth_per_year", &growth.growth_per_year},
        {"decline_per_year", &growth.decline_per_year},
        {"max_population_factor", &growth.max_population_factor},
        {"window_days", &growth.window_days},
        {"grow_above", &growth.grow_above},
        {"decline_below", &growth.decline_below},
    };
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        require_field_count(row, 2, growth_path, i + 1);
        require_unique_id(row[0], seen_keys, growth_path, i + 1);
        const auto field = fields.find(row[0]);
        if (field == fields.end()) {
            throw std::runtime_error("Unknown key '" + row[0] + "' in " + growth_path.string());
        }
        *field->second = parse_double(row[1], growth_path, i + 1, row[0].c_str());
    }
    for (const auto& [key, field] : fields) {
        if (!seen_keys.contains(key)) {
            throw std::runtime_error("Missing key '" + key + "' in " + growth_path.string());
        }
    }
    if (growth.growth_per_year < 0.0 || growth.decline_per_year < 0.0 || growth.max_population_factor < 1.0
        || growth.window_days <= 0.0 || growth.decline_below > growth.grow_above) {
        throw std::runtime_error("growth rates must be >= 0, max_population_factor >= 1, window_days > 0 and "
            "decline_below <= grow_above in " + growth_path.string());
    }
}

void load_pricing(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto pricing_path = root / "economy" / "pricing.csv";
    const auto rows = read_csv_rows(pricing_path);
    require_header(rows.front(), {"key", "value"}, pricing_path);
    std::unordered_set<std::string> seen_keys;
    auto& pricing = universe.pricing;
    const std::unordered_map<std::string, double*> fields {
        {"price_cap", &pricing.price_cap},
        {"carrier_margin", &pricing.carrier_margin},
        {"reference_departures", &pricing.reference_departures},
        {"max_cover_days", &pricing.max_cover_days},
    };
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        require_field_count(row, 2, pricing_path, i + 1);
        require_unique_id(row[0], seen_keys, pricing_path, i + 1);
        const auto field = fields.find(row[0]);
        if (field == fields.end()) {
            throw std::runtime_error("Unknown key '" + row[0] + "' in " + pricing_path.string());
        }
        *field->second = parse_double(row[1], pricing_path, i + 1, row[0].c_str());
    }
    for (const auto& [key, field] : fields) {
        if (!seen_keys.contains(key)) {
            throw std::runtime_error("Missing key '" + key + "' in " + pricing_path.string());
        }
    }
    if (pricing.price_cap < 1.0 || pricing.carrier_margin < 0.0 || pricing.reference_departures < 1.0) {
        throw std::runtime_error("price_cap and reference_departures must be at least 1 in " + pricing_path.string());
    }
    if (pricing.max_cover_days < 21.0) {
        throw std::runtime_error("max_cover_days must be >= 21 in " + pricing_path.string());
    }
}

void load_orders(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    // Station purchase orders (step 29).
    const auto orders_path = root / "economy" / "orders.csv";
    const auto rows = read_csv_rows(orders_path);
    require_header(rows.front(), {"key", "value"}, orders_path);
    std::unordered_set<std::string> seen_keys;
    auto& orders = universe.orders;
    const std::unordered_map<std::string, double*> fields {
        {"safety_factor", &orders.safety_factor},
        {"ema_deliveries", &orders.ema_deliveries},
        {"extra_discount", &orders.extra_discount},
        {"max_extra_days", &orders.max_extra_days},
        {"resale_margin", &orders.resale_margin},
        {"keep_share", &orders.keep_share},
        {"life_value", &orders.life_value},
        {"consume_value", &orders.consume_value},
        {"depot_fuel_value", &orders.depot_fuel_value},
        {"input_cap", &orders.input_cap},
    };
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        require_field_count(row, 2, orders_path, i + 1);
        require_unique_id(row[0], seen_keys, orders_path, i + 1);
        const auto field = fields.find(row[0]);
        if (field == fields.end()) {
            throw std::runtime_error("Unknown key '" + row[0] + "' in " + orders_path.string());
        }
        *field->second = parse_double(row[1], orders_path, i + 1, row[0].c_str());
    }
    for (const auto& [key, field] : fields) {
        if (!seen_keys.contains(key)) {
            throw std::runtime_error("Missing key '" + key + "' in " + orders_path.string());
        }
    }
    if (orders.safety_factor < 0.0 || orders.ema_deliveries < 1.0 || orders.extra_discount < 0.0
        || orders.extra_discount >= 1.0 || orders.keep_share < 0.0 || orders.keep_share > 1.0
        || orders.max_extra_days < 0.0 || orders.resale_margin < 0.0) {
        throw std::runtime_error("safety_factor and extra_discount must be >= 0 (discount < 1), ema_deliveries >= 1 "
            "and keep_share in [0, 1], max_extra_days >= 0 and resale_margin >= 0 in " + orders_path.string());
    }
}

void load_market(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    // Market redesign switches: every key optional, defaults in MarketDefinition.
    const auto market_path = root / "economy" / "market.csv";
    if (std::filesystem::exists(market_path)) {
        const auto rows = read_csv_rows(market_path);
        require_header(rows.front(), {"key", "value"}, market_path);
        std::unordered_set<std::string> seen_keys;
        auto& market = universe.market;
        const std::unordered_map<std::string, double*> fields {
            {"competitive", &market.competitive},
            {"ask_markup", &market.ask_markup},
            {"return_share", &market.return_share},
            {"fill_ref", &market.fill_ref},
            {"ask_open_units", &market.ask_open_units},
            {"solo_premium", &market.solo_premium},
            {"available_days", &market.available_days},
            {"min_source_surplus", &market.min_source_surplus},
            {"urgency_elasticity", &market.urgency_elasticity},
            {"urgency_max", &market.urgency_max},
            {"contracts", &market.contracts},
            {"contract_max_stops", &market.contract_max_stops},
            {"contract_term_days", &market.contract_term_days},
            {"contract_review_days", &market.contract_review_days},
            {"contract_cover", &market.contract_cover},
            {"contract_accept", &market.contract_accept},
            {"contract_min_units", &market.contract_min_units},
            {"contract_refine", &market.contract_refine},
            {"contract_cost_plus", &market.contract_cost_plus},
            {"follow_up_horizon_days", &market.follow_up_horizon_days},
        };
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 2, market_path, i + 1);
            require_unique_id(row[0], seen_keys, market_path, i + 1);
            const auto field = fields.find(row[0]);
            if (field == fields.end()) {
                throw std::runtime_error("Unknown key '" + row[0] + "' in " + market_path.string());
            }
            *field->second = parse_double(row[1], market_path, i + 1, row[0].c_str());
        }
    }
}

void load_reference_prices(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    // Written by `spacetrains_headless --write-reference-prices` (step 15); the price and the
    // round-trip days are read, the rest documents where each price comes from.
    const auto reference_path = root / "economy" / "reference_prices.csv";
    const auto rows = read_csv_rows(reference_path);
    require_header(rows.front(), {"station_id", "commodity_id", "reference_price", "base_price", "transport_per_unit",
        "producer_id", "class_id", "round_trip_days"}, reference_path);
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        require_field_count(row, 8, reference_path, i + 1);
        require_station(universe, row[0], reference_path);
        require_commodity(universe, row[1], reference_path);
        const double price = parse_double(row[2], reference_path, i + 1, "reference_price");
        if (price <= 0.0) {
            throw std::runtime_error("reference_price must be positive in " + reference_path.string());
        }
        universe.pricing.reference_prices[row[0]][row[1]] = price;
        universe.pricing.round_trip_days[row[0]][row[1]] = parse_double(row[7], reference_path, i + 1, "round_trip_days");
    }
}

}  // namespace spacetrains::data_loader::detail
