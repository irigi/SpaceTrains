// Economy statistics: the money journal's accounts and history, and the statistics JSON for the
// bridge's side file and headless --stats-json (docs/modules/economy.md, "Money journal").

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <stdexcept>

#include "simulation/Simulation.hpp"
#include "simulation/SimulationInternal.hpp"

namespace spacetrains::simulation {

using namespace detail;

void Simulation::setup_money_journal() {
    // Accounts in a fixed order (definitions order), so indices are the same in every run.
    std::vector<AccountInfo> accounts;
    accounts.push_back({.kind = AccountKind::Earth, .id = "earth", .name = "Earth", .faction_id = {}});
    for (const auto& economy : universe_.background_economies) {
        if (economy.id != "earth") {
            colony_accounts_[economy.id] = static_cast<int>(accounts.size());
            accounts.push_back({.kind = AccountKind::Colony, .id = economy.id, .name = economy.name, .faction_id = {}});
        }
    }
    for (const auto& faction : universe_.factions) {
        faction_accounts_[faction.id] = static_cast<int>(accounts.size());
        accounts.push_back({.kind = AccountKind::Faction, .id = faction.id, .name = faction.name, .faction_id = faction.id});
    }
    first_station_account_ = static_cast<int>(accounts.size());
    for (const auto& station : universe_.stations) {
        accounts.push_back({.kind = AccountKind::Station, .id = station.id, .name = station.name, .faction_id = station.faction_id});
    }
    first_household_account_ = static_cast<int>(accounts.size());
    for (const auto& station : universe_.stations) {
        accounts.push_back({.kind = AccountKind::Households, .id = station.id, .name = station.name + " residents",
            .faction_id = station.faction_id});
    }
    for (const auto& faction : universe_.factions) {
        fleet_accounts_[faction.id] = static_cast<int>(accounts.size());
        accounts.push_back({.kind = AccountKind::Fleet, .id = faction.id, .name = faction.name + " fleet", .faction_id = faction.id});
    }
    journal_.reset(std::move(accounts));
    for (int i = 0; i < static_cast<int>(journal_.accounts().size()); ++i) {
        journal_.set_seed(i, account_balance(i));
    }
    journal_.set_next_close_day(game_time_s_ / 86400.0 + MoneyJournal::kPeriodDays);
    // The flow record closes its months with the journal's.
    std::vector<std::string> station_ids;
    for (const auto& station : stations_) {
        station_ids.push_back(station.station_id);
    }
    std::vector<std::string> goods;
    for (const auto& commodity : universe_.commodities) {
        goods.push_back(commodity.id);
    }
    flows_.reset(std::move(station_ids), std::move(goods), {});
    flows_.start_stocks_mutable() = flow_stocks();
}

std::vector<std::vector<double>> Simulation::flow_stocks() const {
    std::vector<std::vector<double>> stocks;
    for (const auto& station : stations_) {
        std::vector<double> row;
        for (const auto& good : flows_.goods()) {
            const auto it = station.inventory.find(good);
            row.push_back(it == station.inventory.end() ? 0.0 : it->second);
        }
        stocks.push_back(std::move(row));
    }
    return stocks;
}

int Simulation::account_economy(const std::string& economy_id) const {
    if (economy_id == "earth") {
        return account_earth();
    }
    const auto it = colony_accounts_.find(economy_id);
    if (it == colony_accounts_.end()) {
        throw std::logic_error("money journal: no account for economy " + economy_id);
    }
    return it->second;
}

int Simulation::account_faction(const std::string& faction_id) const {
    const auto it = faction_accounts_.find(faction_id);
    if (it == faction_accounts_.end()) {
        throw std::logic_error("money journal: no account for faction " + faction_id);
    }
    return it->second;
}

int Simulation::account_fleet(const std::string& faction_id) const {
    const auto it = fleet_accounts_.find(faction_id);
    if (it == fleet_accounts_.end()) {
        throw std::logic_error("money journal: no fleet account for faction " + faction_id);
    }
    return it->second;
}

int Simulation::account_station(const domain::StationState& station) const {
    // stations_ is in definitions order (the constructor) and never resized.
    return first_station_account_ + static_cast<int>(&station - stations_.data());
}

int Simulation::account_households(const domain::StationState& station) const {
    return first_household_account_ + static_cast<int>(&station - stations_.data());
}

int Simulation::account_station(const std::string& station_id) const {
    return account_station(get_station_state(station_id));
}

int Simulation::account_households(const std::string& station_id) const {
    return account_households(get_station_state(station_id));
}

double Simulation::account_balance(int account) const {
    const auto& info = journal_.accounts().at(static_cast<std::size_t>(account));
    switch (info.kind) {
        case AccountKind::Earth:
        case AccountKind::Colony:
            return economy_balance(info.id);
        case AccountKind::Faction: {
            const auto it = faction_treasuries_.find(info.id);
            return it == faction_treasuries_.end() ? 0.0 : it->second;
        }
        case AccountKind::Station:
            return stations_.at(static_cast<std::size_t>(account - first_station_account_)).credits;
        case AccountKind::Households:
            return stations_.at(static_cast<std::size_t>(account - first_household_account_)).household_credits;
        case AccountKind::Fleet: {
            double cash = 0.0;
            for (const auto& ship : ships_) {
                if (ship.faction_id == info.id) {
                    cash += ship.credits;
                }
            }
            return cash;
        }
    }
    return 0.0;
}

double Simulation::station_profit_last_year(const domain::StationState& station) const {
    const auto& history = journal_.history();
    const auto index = static_cast<std::size_t>(&station - stations_.data());
    const double year_ago = history.size() > 12 ? history[history.size() - 13].station_profit.at(index) : 0.0;
    return station_lifetime_profit(station) - year_ago;
}

void Simulation::close_journal_period() {
    // Only reads the state: closing a period never changes the game.
    JournalSample sample;
    sample.day = journal_.next_close_day();
    const auto count = static_cast<int>(journal_.accounts().size());
    sample.balances.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        sample.balances.push_back(account_balance(i));
    }
    for (const auto& station : stations_) {
        const double workers = station_workers(station);
        sample.stations.push_back({
            .population = station.population,
            .workers = workers,
            .jobs = in_labour_market(station) ? station.jobs : workers,
            .wage_cr_per_day = station_wage_cr_per_day(station),
        });
        sample.station_profit.push_back(station_lifetime_profit(station));
    }
    for (const auto& info : journal_.accounts()) {
        if (info.kind == AccountKind::Colony) {
            const auto it = background_population_.find(info.id);
            sample.colony_people.push_back(it == background_population_.end() ? 0.0 : it->second);
        }
    }
    flows_.close_period(sample.day, flow_stocks());
    journal_.close_period(std::move(sample));
}

namespace {

std::string number(double value) {
    return std::isfinite(value) ? std::format("{:.2f}", value) : "0";
}

// Rates and shares, with three decimals; a non-finite value is written as 0 (valid JSON).
std::string fine(double value) {
    return std::isfinite(value) ? std::format("{:.3f}", value) : "0";
}

// {"cat":[in,out],...} for the categories that moved.
std::string flows_json(const CategoryFlows& flows) {
    std::string out = "{";
    for (std::size_t c = 0; c < flows.size(); ++c) {
        if (flows[c].in == 0.0 && flows[c].out == 0.0) {
            continue;
        }
        out += std::format("{}\"{}\":[{},{}]", out.size() > 1 ? "," : "", money_category_info(static_cast<MoneyCategory>(c)).id,
            number(flows[c].in), number(flows[c].out));
    }
    return out + "}";
}

void add_flows(CategoryFlows& total, const CategoryFlows& flows) {
    for (std::size_t c = 0; c < flows.size(); ++c) {
        total[c].in += flows[c].in;
        total[c].out += flows[c].out;
    }
}

std::string numbers_json(const std::vector<double>& values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        out += (i > 0 ? "," : "") + number(values[i]);
    }
    return out + "]";
}

}  // namespace

std::string Simulation::build_stats_json() const {
    const auto& accounts = journal_.accounts();
    const auto& history = journal_.history();
    const double today = game_time_s_ / 86400.0;
    std::string out = std::format("{{\"day\":{},\"period_days\":{},", number(today), number(MoneyJournal::kPeriodDays));

    out += "\"categories\":[";
    for (std::size_t c = 0; c < kMoneyCategories; ++c) {
        const auto& info = money_category_info(static_cast<MoneyCategory>(c));
        out += std::format("{}{{\"id\":\"{}\",\"label\":\"{}\",\"group\":\"{}\"}}", c > 0 ? "," : "", info.id, info.label, info.group);
    }
    out += "],";

    // Accounts: balance, lifetime flows, the last 12 closed periods (a year) and the last one (30 days).
    out += "\"accounts\":[";
    double total = 0.0;
    double seeds = 0.0;
    for (int i = 0; i < static_cast<int>(accounts.size()); ++i) {
        const auto& info = accounts[static_cast<std::size_t>(i)];
        CategoryFlows year {};
        const std::size_t first = history.size() > 12 ? history.size() - 12 : 0;
        for (std::size_t k = first; k < history.size(); ++k) {
            add_flows(year, history[k].flows[static_cast<std::size_t>(i)]);
        }
        const CategoryFlows month = history.empty() ? journal_.period(i) : history.back().flows[static_cast<std::size_t>(i)];
        const double balance = account_balance(i);
        total += balance;
        seeds += journal_.seed(i);
        out += std::format("{}{{\"key\":\"{}\",\"kind\":\"{}\",\"id\":\"{}\",\"name\":\"{}\",\"faction\":\"{}\",\"balance\":{},\"seed\":{},"
            "\"lifetime\":{},\"last_year\":{},\"last_30\":{},\"running\":{}}}",
            i > 0 ? "," : "", json_escape(info.key()), account_kind_id(info.kind), json_escape(info.id), json_escape(info.name),
            json_escape(info.faction_id), number(balance), number(journal_.seed(i)), flows_json(journal_.lifetime(i)),
            flows_json(year), flows_json(month), flows_json(journal_.period(i)));
    }
    out += std::format("],\"total\":{},\"seed_total\":{},", number(total), number(seeds));

    // Money between sectors (payer x payee, in account-kind order): since the start, last year, last period.
    {
        SectorMatrix year {};
        const std::size_t first = history.size() > 12 ? history.size() - 12 : 0;
        for (std::size_t k = first; k < history.size(); ++k) {
            for (std::size_t c = 0; c < year.size(); ++c) {
                year[c] += history[k].sectors[c];
            }
        }
        const SectorMatrix month = history.empty() ? journal_.period_sectors() : history.back().sectors;
        const auto matrix = [](const SectorMatrix& m) { return numbers_json({m.begin(), m.end()}); };
        out += "\"sectors\":{\"kinds\":[";
        for (std::size_t k = 0; k < kAccountKinds; ++k) {
            out += std::format("{}\"{}\"", k > 0 ? "," : "", account_kind_id(static_cast<AccountKind>(k)));
        }
        out += std::format("],\"lifetime\":{},\"last_year\":{},\"last_30\":{}}},", matrix(journal_.lifetime_sectors()),
            matrix(year), matrix(month));
    }

    // History: per sample day, balances, and each account's net flow by category.
    out += "\"history\":{\"days\":[";
    for (std::size_t k = 0; k < history.size(); ++k) {
        out += (k > 0 ? "," : "") + number(history[k].day);
    }
    out += "],\"balances\":{";
    for (std::size_t i = 0; i < accounts.size(); ++i) {
        std::vector<double> series;
        for (const auto& sample : history) {
            series.push_back(sample.balances[i]);
        }
        out += std::format("{}\"{}\":{}", i > 0 ? "," : "", json_escape(accounts[i].key()), numbers_json(series));
    }
    out += "},\"flows\":{";
    for (std::size_t i = 0; i < accounts.size(); ++i) {
        out += std::format("{}\"{}\":{{", i > 0 ? "," : "", json_escape(accounts[i].key()));
        bool first_category = true;
        for (std::size_t c = 0; c < kMoneyCategories; ++c) {
            std::vector<double> series;
            bool moved = false;
            for (const auto& sample : history) {
                const auto& flow = sample.flows[i][c];
                series.push_back(flow.in - flow.out);
                moved = moved || flow.in != 0.0 || flow.out != 0.0;
            }
            if (moved) {
                out += std::format("{}\"{}\":{}", first_category ? "" : ",", money_category_info(static_cast<MoneyCategory>(c)).id,
                    numbers_json(series));
                first_category = false;
            }
        }
        out += "}";
    }
    out += "},\"station_profit\":{";
    for (std::size_t s = 0; s < stations_.size(); ++s) {
        std::vector<double> series;
        for (const auto& sample : history) {
            series.push_back(s < sample.station_profit.size() ? sample.station_profit[s] : 0.0);
        }
        out += std::format("{}\"{}\":{}", s > 0 ? "," : "", json_escape(stations_[s].station_id), numbers_json(series));
    }
    out += "}},";

    // Stations: operating profit (before the faction's taxes and subsidies), red days.
    out += "\"stations\":[";
    for (std::size_t s = 0; s < stations_.size(); ++s) {
        const auto& station = stations_[s];
        const auto& definition = get_station_definition(station.station_id);
        const double profit = station_lifetime_profit(station);
        // People at each closed period, and the station systems view (display only) for the people charts.
        std::vector<double> people_series;
        for (const auto& sample : history) {
            people_series.push_back(s < sample.stations.size() ? sample.stations[s].population : 0.0);
        }
        out += std::format("{}{{\"id\":\"{}\",\"name\":\"{}\",\"faction\":\"{}\",\"credits\":{},\"profit\":{},\"profit_last_year\":{},"
            "\"people_history\":{},{}"   // station_systems_json writes "systems":{...}, (or nothing)
            "\"red_days\":{},\"population\":{},\"workers\":{},\"jobs\":{},\"wage\":{},\"labour_market\":{},\"household_credits\":{}}}",
            s > 0 ? "," : "", json_escape(station.station_id), json_escape(definition.name), json_escape(definition.faction_id),
            number(station.credits), number(profit), number(station_profit_last_year(station)), numbers_json(people_series),
            station_systems_json(definition, station), number(station.ledger.days_in_red),
            number(station.population), number(station_workers(station)),
            number(in_labour_market(station) ? station.jobs : station_workers(station)), fine(station_wage_cr_per_day(station)),
            in_labour_market(station) ? "true" : "false", number(station.household_credits));
    }
    out += "],";

    // Colonies: credit line, people and their wage distribution, flows up and down.
    out += "\"colonies\":[";
    bool first_colony = true;
    for (const auto& economy : universe_.background_economies) {
        if (economy.id == "earth") {
            continue;
        }
        double hired_up = 0.0;
        double gone_down = 0.0;
        for (const auto& gateway : economy.gateways) {
            const auto& station = get_station_state(gateway);
            hired_up += station.ledger.hired_from_surface;
            gone_down += station.ledger.moved_to_surface;
        }
        const auto people = background_population_.find(economy.id);
        std::vector<double> people_series;
        const int colony_index = account_economy(economy.id) - 1;   // colonies follow Earth in the account list
        for (const auto& sample : history) {
            people_series.push_back(colony_index >= 0 && static_cast<std::size_t>(colony_index) < sample.colony_people.size()
                ? sample.colony_people[static_cast<std::size_t>(colony_index)] : 0.0);
        }
        constexpr double Z90 = 1.2815515655446004;
        const double wage = colony_wage_median(economy);
        const auto* state = colony_state(economy.id);
        out += std::format("{}{{\"id\":\"{}\",\"name\":\"{}\",\"gateways\":\"{}\",\"credits\":{},\"credit_line\":{},\"people\":{},"
            "\"wage_p10\":{},\"wage_p50\":{},\"wage_p90\":{},\"hired_up\":{},\"gone_down\":{},\"earth_investment_per_year\":{},"
            "\"people_history\":{},\"wage_data\":{},\"satisfaction\":{},\"throttle\":{},\"income_per_year\":{},"
            "\"imports_per_year\":{}}}",
            first_colony ? "" : ",", json_escape(economy.id), json_escape(economy.name),
            json_escape(economy.gateways.empty() ? std::string() : economy.gateways.front()), number(economy_balance(economy.id)),
            number(economy_credit_line(economy.id)), number(people == background_population_.end() ? 0.0 : people->second),
            fine(wage * std::exp(-Z90 * economy.wage_sigma)), fine(wage), fine(wage * std::exp(Z90 * economy.wage_sigma)),
            number(hired_up), number(gone_down), number(economy.income_cr_per_day * 365.25), numbers_json(people_series),
            fine(economy.wage_median_cr_per_day), fine(state == nullptr ? 1.0 : state->satisfaction),
            fine(state == nullptr ? 1.0 : state->throttle),
            number(state == nullptr ? 0.0 : state->income_per_day * 365.25),
            number(state == nullptr ? 0.0 : state->base_imports_per_day * 365.25));
        first_colony = false;
    }
    out += "],";

    // Flows (FlowRecord.hpp): places, goods, and per period what moved and the stations' stocks at
    // its start and end (the goods balance: net made here = stock change - imported + exported).
    {
        out += "\"flows\":{\"nodes\":[";
        bool first_node = true;
        for (const auto& station : universe_.stations) {
            out += std::format("{}{{\"id\":\"{}\",\"name\":\"{}\",\"kind\":\"station\",\"faction\":\"{}\"}}",
                first_node ? "" : ",", json_escape(station.id), json_escape(station.name), json_escape(station.faction_id));
            first_node = false;
        }
        for (const auto& economy : universe_.background_economies) {
            out += std::format(",{{\"id\":\"surface:{}\",\"name\":\"{}\",\"kind\":\"surface\",\"faction\":\"\"}}",
                json_escape(economy.id), json_escape(economy.name));
        }
        out += "],\"goods\":[";
        for (std::size_t g = 0; g < flows_.goods().size(); ++g) {
            out += std::format("{}\"{}\"", g > 0 ? "," : "", json_escape(flows_.goods()[g]));
        }
        out += "],";
        const auto table_json = [](const std::vector<const FlowTable*>& tables) {
            FlowTable sum;
            for (const auto* table : tables) {
                for (const auto& [key, amount] : *table) {
                    auto& total = sum[key];
                    total.units += amount.units;
                    total.value += amount.value;
                    total.count += amount.count;
                }
            }
            std::string json = "[";
            bool first = true;
            for (const auto& [key, amount] : sum) {
                json += std::format("{}[\"{}\",\"{}\",\"{}\",\"{}\",{},{},{}]", first ? "" : ",", flow_kind_id(key.kind),
                    json_escape(key.from), json_escape(key.to), json_escape(key.item), fine(amount.units), fine(amount.value),
                    fine(amount.count));
                first = false;
            }
            return json + "]";
        };
        const auto stocks_json = [](const std::vector<std::vector<double>>& stocks) {
            std::string json = "[";
            for (std::size_t s = 0; s < stocks.size(); ++s) {
                json += (s > 0 ? "," : "") + numbers_json(stocks[s]);
            }
            return json + "]";
        };
        const auto& flow_history = flows_.history();
        const auto now = flow_stocks();
        // Since the start: every closed month and the running one.
        std::vector<const FlowTable*> all;
        for (const auto& sample : flow_history) {
            all.push_back(&sample.flows);
        }
        all.push_back(&flows_.period());
        // Last year: the last 12 closed months; last 30 days: the last closed month (as the journal).
        std::vector<const FlowTable*> year;
        for (std::size_t k = flow_history.size() > 12 ? flow_history.size() - 12 : 0; k < flow_history.size(); ++k) {
            year.push_back(&flow_history[k].flows);
        }
        std::vector<const FlowTable*> month;
        if (!flow_history.empty()) {
            month.push_back(&flow_history.back().flows);
        } else {
            month.push_back(&flows_.period());
        }
        const auto& start = flows_.start_stocks();
        const auto& year_start = flow_history.size() > 12 ? flow_history[flow_history.size() - 13].stocks : start;
        const auto& closed_end = flow_history.empty() ? now : flow_history.back().stocks;
        const auto& month_start = flow_history.size() > 1 ? flow_history[flow_history.size() - 2].stocks : start;
        out += std::format("\"lifetime\":{{\"entries\":{},\"stock_start\":{},\"stock_end\":{}}},",
            table_json(all), stocks_json(start), stocks_json(now));
        out += std::format("\"last_year\":{{\"entries\":{},\"stock_start\":{},\"stock_end\":{}}},",
            table_json(year), stocks_json(year_start), stocks_json(closed_end));
        out += std::format("\"last_30\":{{\"entries\":{},\"stock_start\":{},\"stock_end\":{}}}}},",
            table_json(month), stocks_json(flow_history.empty() ? start : month_start), stocks_json(closed_end));
    }

    // Factions: treasury and credit.
    out += "\"factions\":[";
    for (std::size_t f = 0; f < universe_.factions.size(); ++f) {
        const auto& faction = universe_.factions[f];
        const auto treasury = faction_treasuries_.find(faction.id);
        out += std::format("{}{{\"id\":\"{}\",\"name\":\"{}\",\"color\":\"{}\",\"treasury\":{},\"credit_limit\":{}}}",
            f > 0 ? "," : "", json_escape(faction.id), json_escape(faction.name), json_escape(faction.color_hex),
            number(treasury == faction_treasuries_.end() ? 0.0 : treasury->second), number(faction_credit_limit(faction.id)));
    }
    out += "],";

    // Fleet: every ship ever (sold ones too), by faction and by class, and the best and worst.
    struct FleetTally {
        int ships {0};
        int in_service {0};
        int profitable {0};
        domain::ShipLedger ledger;
        double profit {0.0};
    };
    std::map<std::string, FleetTally> by_faction;
    std::map<std::string, FleetTally> by_class;
    struct Ranked {
        const domain::ShipState* ship;
        bool sold;
    };
    std::vector<Ranked> ranked;
    const auto tally = [](FleetTally& t, const domain::ShipState& ship, bool sold) {
        ++t.ships;
        t.in_service += sold ? 0 : 1;
        t.profitable += ship.lifetime_profit > 0.0 ? 1 : 0;
        t.profit += ship.lifetime_profit;
        const auto& l = ship.ledger;
        t.ledger.cargo_revenue += l.cargo_revenue;
        t.ledger.cargo_purchases += l.cargo_purchases;
        t.ledger.fuel += l.fuel;
        t.ledger.wages += l.wages;
        t.ledger.capital += l.capital;
        t.ledger.provisions += l.provisions;
        t.ledger.refits += l.refits;
        t.ledger.fares += l.fares;
        t.ledger.dividends += l.dividends;
        t.ledger.subsidies += l.subsidies;
    };
    for (const auto* list : {&ships_, &sold_ships_}) {
        const bool sold = list == &sold_ships_;
        for (const auto& ship : *list) {
            tally(by_faction[ship.faction_id], ship, sold);
            tally(by_class[ship.class_id], ship, sold);
            ranked.push_back({&ship, sold});
        }
    }
    const auto tally_json = [](const std::string& key, const FleetTally& t) {
        const auto& l = t.ledger;
        return std::format("{{\"key\":\"{}\",\"ships\":{},\"in_service\":{},\"profitable\":{},\"cargo_revenue\":{},\"cargo_purchases\":{},"
            "\"fares\":{},\"fuel\":{},\"wages\":{},\"capital\":{},\"provisions\":{},\"refits\":{},\"dividends\":{},\"subsidies\":{},"
            "\"profit\":{}}}",
            json_escape(key), t.ships, t.in_service, t.profitable, number(l.cargo_revenue), number(l.cargo_purchases), number(l.fares),
            number(l.fuel), number(l.wages), number(l.capital), number(l.provisions), number(l.refits), number(l.dividends),
            number(l.subsidies), number(t.profit));
    };
    out += "\"fleet\":{\"by_faction\":[";
    bool first = true;
    for (const auto& [key, t] : by_faction) {
        out += (first ? "" : ",") + tally_json(key, t);
        first = false;
    }
    out += "],\"by_class\":[";
    first = true;
    for (const auto& [key, t] : by_class) {
        out += (first ? "" : ",") + tally_json(key, t);
        first = false;
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        return a.ship->lifetime_profit != b.ship->lifetime_profit ? a.ship->lifetime_profit > b.ship->lifetime_profit
                                                                  : a.ship->id < b.ship->id;
    });
    const auto ship_json = [](const Ranked& r) {
        return std::format("{{\"id\":\"{}\",\"name\":\"{}\",\"faction\":\"{}\",\"class\":\"{}\",\"profit\":{},\"sold\":{}}}",
            json_escape(r.ship->id), json_escape(r.ship->name), json_escape(r.ship->faction_id), json_escape(r.ship->class_id),
            number(r.ship->lifetime_profit), r.sold ? "true" : "false");
    };
    out += "],\"top\":[";
    for (std::size_t k = 0; k < std::min<std::size_t>(5, ranked.size()); ++k) {
        out += (k > 0 ? "," : "") + ship_json(ranked[k]);
    }
    out += "],\"bottom\":[";
    for (std::size_t k = 0; k < std::min<std::size_t>(5, ranked.size()); ++k) {
        out += (k > 0 ? "," : "") + ship_json(ranked[ranked.size() - 1 - k]);
    }
    out += "]}}";
    return out;
}

}  // namespace spacetrains::simulation
