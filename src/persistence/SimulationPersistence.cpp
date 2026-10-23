// Save games: the whole mutable state of a Simulation as JSON. Definitions (the data
// files and the atlas) are not saved; a save records a fingerprint of them and is loaded
// into a Simulation built from the same data. Plan caches are not saved either: they are
// pure functions of their keys, so a loaded game continues exactly as one that never stopped
// (tests/test_main.cpp checks 100 days + save/load + 100 days against 200 days).
#include "persistence/Json.hpp"
#include "simulation/Simulation.hpp"

#include <format>
#include <sstream>
#include <stdexcept>

namespace spacetrains::simulation {

namespace {

using persistence::Json;

constexpr int kSaveVersion = 20;  // 20: colony welfare (satisfaction, income, wage); 19: money journal; 18: households and background economies; 2: mixed cargo (lots per mission); 3: a fleet review's probes; 4: probe runs; 5: contracts; 6: emergencies; 7: faction credit, import costs; 8: liner subsidies; 9: populations; 10: events; 11: order observations; 12: order prices; 13: state-based order bids (no raise timers); 14: ship fits (tier 2 modules); 15: passengers; 16: purchase spacing, cabin runs in probes; 17: passenger runs' return fuel

Json goods_to_json(const domain::Inventory& goods) {
    auto out = Json::object();
    for (const auto& [id, units] : goods) {
        out.set(id, units);
    }
    return out;
}

domain::Inventory goods_from_json(const Json& json) {
    domain::Inventory goods;
    for (const auto& [id, units] : json.fields()) {
        goods[id] = units.number();
    }
    return goods;
}

Json numbers_to_json(const std::vector<double>& values) {
    auto out = Json::array();
    for (const double value : values) {
        out.push(value);
    }
    return out;
}

std::vector<double> numbers_from_json(const Json& json) {
    std::vector<double> values;
    values.reserve(json.items().size());
    for (const auto& item : json.items()) {
        values.push_back(item.number());
    }
    return values;
}

// The money journal (MoneyJournal.hpp): flows by category id, only those that moved.
Json flows_to_json(const CategoryFlows& flows) {
    auto out = Json::object();
    for (std::size_t c = 0; c < flows.size(); ++c) {
        if (flows[c].in != 0.0 || flows[c].out != 0.0) {
            auto pair = Json::array();
            pair.push(flows[c].in);
            pair.push(flows[c].out);
            out.set(std::string(money_category_info(static_cast<MoneyCategory>(c)).id), std::move(pair));
        }
    }
    return out;
}

CategoryFlows flows_from_json(const Json& json) {
    CategoryFlows flows {};
    for (const auto& [id, pair] : json.fields()) {
        MoneyCategory category {};
        if (!money_category_from_id(id, category)) {
            throw std::runtime_error("save: unknown money category " + id);
        }
        flows[static_cast<std::size_t>(category)] = {.in = pair.items().at(0).number(), .out = pair.items().at(1).number()};
    }
    return flows;
}

Json journal_to_json(const MoneyJournal& journal) {
    auto out = Json::object();
    const auto& accounts = journal.accounts();
    auto keys = Json::array();
    auto seeds = Json::array();
    auto lifetime = Json::array();
    auto period = Json::array();
    for (int i = 0; i < static_cast<int>(accounts.size()); ++i) {
        keys.push(accounts[static_cast<std::size_t>(i)].key());
        seeds.push(journal.seed(i));
        lifetime.push(flows_to_json(journal.lifetime(i)));
        period.push(flows_to_json(journal.period(i)));
    }
    out.set("accounts", std::move(keys));
    out.set("seeds", std::move(seeds));
    out.set("lifetime", std::move(lifetime));
    out.set("period", std::move(period));
    out.set("next_close_day", journal.next_close_day());
    out.set("lifetime_sectors", numbers_to_json({journal.lifetime_sectors().begin(), journal.lifetime_sectors().end()}));
    out.set("period_sectors", numbers_to_json({journal.period_sectors().begin(), journal.period_sectors().end()}));
    auto history = Json::array();
    for (const auto& sample : journal.history()) {
        auto item = Json::object();
        item.set("day", sample.day);
        item.set("balances", numbers_to_json(sample.balances));
        auto flows = Json::array();
        for (const auto& account_flows : sample.flows) {
            flows.push(flows_to_json(account_flows));
        }
        item.set("flows", std::move(flows));
        auto stations = Json::array();
        for (const auto& people : sample.stations) {
            stations.push(numbers_to_json({people.population, people.workers, people.jobs, people.wage_cr_per_day}));
        }
        item.set("stations", std::move(stations));
        item.set("colony_people", numbers_to_json(sample.colony_people));
        item.set("station_profit", numbers_to_json(sample.station_profit));
        item.set("sectors", numbers_to_json({sample.sectors.begin(), sample.sectors.end()}));
        history.push(std::move(item));
    }
    out.set("history", std::move(history));
    return out;
}

// Restores into a journal whose accounts were set up from the same data (the data fingerprint).
void journal_from_json(const Json& json, MoneyJournal& journal) {
    const auto& keys = json.get("accounts").items();
    if (keys.size() != journal.accounts().size()) {
        throw std::runtime_error("save: the money journal's accounts differ from the data");
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (keys[i].string() != journal.accounts()[i].key()) {
            throw std::runtime_error("save: the money journal's accounts differ from the data");
        }
    }
    journal.seeds() = numbers_from_json(json.get("seeds"));
    auto& lifetime = journal.lifetime_mutable();
    auto& period = journal.period_mutable();
    for (std::size_t i = 0; i < keys.size(); ++i) {
        lifetime[i] = flows_from_json(json.get("lifetime").items().at(i));
        period[i] = flows_from_json(json.get("period").items().at(i));
    }
    journal.set_next_close_day(json.get("next_close_day").number());
    const auto matrix = [](const Json& j) {
        SectorMatrix m {};
        const auto values = numbers_from_json(j);
        if (values.size() != m.size()) {
            throw std::runtime_error("save: the money journal's sector matrix has the wrong size");
        }
        std::copy(values.begin(), values.end(), m.begin());
        return m;
    };
    journal.lifetime_sectors_mutable() = matrix(json.get("lifetime_sectors"));
    journal.period_sectors_mutable() = matrix(json.get("period_sectors"));
    auto& history = journal.history_mutable();
    history.clear();
    for (const auto& item : json.get("history").items()) {
        JournalSample sample;
        sample.day = item.get("day").number();
        sample.balances = numbers_from_json(item.get("balances"));
        for (const auto& account_flows : item.get("flows").items()) {
            sample.flows.push_back(flows_from_json(account_flows));
        }
        for (const auto& people : item.get("stations").items()) {
            const auto values = numbers_from_json(people);
            sample.stations.push_back({.population = values.at(0), .workers = values.at(1), .jobs = values.at(2),
                .wage_cr_per_day = values.at(3)});
        }
        sample.colony_people = numbers_from_json(item.get("colony_people"));
        sample.station_profit = numbers_from_json(item.get("station_profit"));
        sample.sectors = matrix(item.get("sectors"));
        history.push_back(std::move(sample));
    }
}

// Flat [x0, y0, z0, x1, ...].
Json points_to_json(const std::vector<math::Vec3d>& points) {
    auto out = Json::array();
    for (const auto& p : points) {
        out.push(p.x);
        out.push(p.y);
        out.push(p.z);
    }
    return out;
}

std::vector<math::Vec3d> points_from_json(const Json& json) {
    const auto& items = json.items();
    std::vector<math::Vec3d> points;
    points.reserve(items.size() / 3);
    for (std::size_t i = 0; i + 2 < items.size(); i += 3) {
        points.push_back({items[i].number(), items[i + 1].number(), items[i + 2].number()});
    }
    return points;
}

int phase_to_int(domain::ShipMissionPhase phase) {
    return static_cast<int>(phase);
}

domain::ShipMissionPhase phase_from_int(int value) {
    if (value < 0 || value > static_cast<int>(domain::ShipMissionPhase::Refitting)) {
        throw std::runtime_error(std::format("save: unknown ship phase {}", value));
    }
    return static_cast<domain::ShipMissionPhase>(value);
}

Json mission_to_json(const domain::MissionAssignment& m) {
    auto out = Json::object();
    out.set("origin_station_id", m.origin_station_id);
    out.set("destination_station_id", m.destination_station_id);
    auto cargo = Json::array();
    for (const auto& lot : m.cargo) {
        auto entry = Json::object();
        entry.set("commodity_id", lot.commodity_id);
        entry.set("units", lot.units);
        entry.set("contract_value", lot.contract_value);
        entry.set("emergency_premium", lot.emergency_premium);
        cargo.push(std::move(entry));
    }
    out.set("cargo", std::move(cargo));
    out.set("departure_time_s", m.departure_time_s);
    out.set("arrival_time_s", m.arrival_time_s);
    out.set("wait_time_s", m.wait_time_s);
    out.set("coast_time_s", m.coast_time_s);
    out.set("total_travel_time_s", m.total_travel_time_s);
    out.set("remaining_travel_time_s", m.remaining_travel_time_s);
    out.set("propellant_cost_kg", m.propellant_cost_kg);
    out.set("purchase_cost", m.purchase_cost);
    out.set("fuel_cost", m.fuel_cost);
    out.set("expected_revenue", m.expected_revenue);
    out.set("operating_cost", m.operating_cost);
    out.set("sampled_path", points_to_json(m.sampled_path));
    out.set("sampled_times_s", numbers_to_json(m.sampled_times_s));
    out.set("sampled_propellant_kg", numbers_to_json(m.sampled_propellant_kg));
    out.set("trajectory_type", m.trajectory_type);
    out.set("carried_propellant_kg", m.carried_propellant_kg);
    out.set("boiled_propellant_kg", m.boiled_propellant_kg);
    out.set("pickup_commodity_id", m.pickup_commodity_id);
    out.set("pickup_units", m.pickup_units);
    return out;
}

domain::MissionAssignment mission_from_json(const Json& j) {
    domain::MissionAssignment m;
    m.origin_station_id = j.get("origin_station_id").string();
    m.destination_station_id = j.get("destination_station_id").string();
    for (const auto& entry : j.get("cargo").items()) {
        m.cargo.push_back({.commodity_id = entry.get("commodity_id").string(), .units = entry.get("units").number(),
            .contract_value = entry.get("contract_value").number(),
            .emergency_premium = entry.get("emergency_premium").number()});
    }
    m.departure_time_s = j.get("departure_time_s").number();
    m.arrival_time_s = j.get("arrival_time_s").number();
    m.wait_time_s = j.get("wait_time_s").number();
    m.coast_time_s = j.get("coast_time_s").number();
    m.total_travel_time_s = j.get("total_travel_time_s").number();
    m.remaining_travel_time_s = j.get("remaining_travel_time_s").number();
    m.propellant_cost_kg = j.get("propellant_cost_kg").number();
    m.purchase_cost = j.get("purchase_cost").number();
    m.fuel_cost = j.get("fuel_cost").number();
    m.expected_revenue = j.get("expected_revenue").number();
    m.operating_cost = j.get("operating_cost").number();
    m.sampled_path = points_from_json(j.get("sampled_path"));
    m.sampled_times_s = numbers_from_json(j.get("sampled_times_s"));
    m.sampled_propellant_kg = numbers_from_json(j.get("sampled_propellant_kg"));
    m.trajectory_type = j.get("trajectory_type").string();
    m.carried_propellant_kg = j.get("carried_propellant_kg").number();
    m.boiled_propellant_kg = j.get("boiled_propellant_kg").number();
    m.pickup_commodity_id = j.get("pickup_commodity_id").string();
    m.pickup_units = j.get("pickup_units").number();
    return m;
}

// Passengers (save version 15).
Json passenger_group_to_json(const domain::PassengerGroup& g) {
    auto out = Json::object();
    out.set("kind", static_cast<double>(static_cast<int>(g.kind)));
    out.set("destination_id", g.destination_id);
    out.set("work_station_id", g.work_station_id);
    out.set("count", g.count);
    out.set("since_s", g.since_s);
    out.set("max_cost_cr", g.max_cost_cr);
    out.set("value_of_time_cr_per_day", g.value_of_time_cr_per_day);
    out.set("dose_msv", g.dose_msv);
    out.set("band", static_cast<double>(g.band));
    return out;
}

domain::PassengerGroup passenger_group_from_json(const Json& j) {
    return {
        .kind = static_cast<domain::PassengerKind>(static_cast<int>(j.get("kind").number())),
        .destination_id = j.get("destination_id").string(),
        .work_station_id = j.get("work_station_id").string(),
        .count = j.get("count").number(),
        .since_s = j.get("since_s").number(),
        .max_cost_cr = j.get("max_cost_cr").number(),
        .value_of_time_cr_per_day = j.get("value_of_time_cr_per_day").number(),
        .dose_msv = j.get("dose_msv").number(),
        .band = static_cast<int>(j.get("band").number()),
    };
}

Json passenger_lot_to_json(const domain::PassengerLot& lot) {
    auto out = Json::object();
    out.set("kind", static_cast<double>(static_cast<int>(lot.kind)));
    out.set("origin_id", lot.origin_id);
    out.set("destination_id", lot.destination_id);
    out.set("work_station_id", lot.work_station_id);
    out.set("count", lot.count);
    out.set("fare_cr", lot.fare_cr);
    out.set("value_of_time_cr_per_day", lot.value_of_time_cr_per_day);
    out.set("dose_msv", lot.dose_msv);
    out.set("boarded_s", lot.boarded_s);
    return out;
}

domain::PassengerLot passenger_lot_from_json(const Json& j) {
    return {
        .kind = static_cast<domain::PassengerKind>(static_cast<int>(j.get("kind").number())),
        .origin_id = j.get("origin_id").string(),
        .destination_id = j.get("destination_id").string(),
        .work_station_id = j.get("work_station_id").string(),
        .count = j.get("count").number(),
        .fare_cr = j.get("fare_cr").number(),
        .value_of_time_cr_per_day = j.get("value_of_time_cr_per_day").number(),
        .dose_msv = j.get("dose_msv").number(),
        .boarded_s = j.get("boarded_s").number(),
    };
}

Json ship_to_json(const domain::ShipState& s) {
    auto out = Json::object();
    out.set("id", s.id);
    out.set("name", s.name);
    out.set("faction_id", s.faction_id);
    out.set("class_id", s.class_id);
    auto fit = Json::array();
    for (const auto& module : s.fit) {
        fit.push(Json(module));
    }
    out.set("fit", std::move(fit));
    out.set("home_station_id", s.home_station_id);
    out.set("current_station_id", s.current_station_id);
    out.set("phase", phase_to_int(s.phase));
    out.set("propellant_kg", s.propellant_kg);
    out.set("credits", s.credits);
    out.set("lifetime_profit", s.lifetime_profit);
    out.set("active_mission", mission_to_json(s.active_mission));
    out.set("provisions", goods_to_json(s.provisions));
    auto ledger = Json::object();
    ledger.set("cargo_revenue", s.ledger.cargo_revenue);
    ledger.set("cargo_purchases", s.ledger.cargo_purchases);
    ledger.set("fuel", s.ledger.fuel);
    ledger.set("wages", s.ledger.wages);
    ledger.set("capital", s.ledger.capital);
    ledger.set("provisions", s.ledger.provisions);
    ledger.set("refits", s.ledger.refits);
    ledger.set("subsidies", s.ledger.subsidies);
    ledger.set("dividends", s.ledger.dividends);
    ledger.set("fares", s.ledger.fares);
    out.set("ledger", std::move(ledger));
    out.set("next_review_s", s.next_review_s);
    out.set("idle_since_s", s.idle_since_s);
    out.set("refit_class_id", s.refit_class_id);
    auto refit_fit = Json::array();
    for (const auto& module : s.refit_fit) {
        refit_fit.push(Json(module));
    }
    out.set("refit_fit", std::move(refit_fit));
    out.set("refit_done_s", s.refit_done_s);
    out.set("leg_dose_msv", s.leg_dose_msv);
    out.set("engine_wear", s.engine_wear);
    out.set("radiator_condition", s.radiator_condition);
    out.set("overhaul", s.overhaul);
    out.set("next_refit_review_s", s.next_refit_review_s);
    out.set("laid_up_since_s", s.laid_up_since_s);
    out.set("commissioned_s", s.commissioned_s);
    out.set("route_destination_id", s.route_destination_id);
    out.set("route_commodity_id", s.route_commodity_id);
    out.set("route_units_per_day", s.route_units_per_day);
    out.set("route_until_s", s.route_until_s);
    out.set("next_contract_review_s", s.next_contract_review_s);
    if (!s.contract.stops.empty()) {
        auto contract = Json::object();
        auto stops = Json::array();
        for (const auto& stop : s.contract.stops) {
            stops.push(Json(stop));
        }
        contract.set("stops", std::move(stops));
        auto legs = Json::array();
        for (const double days : s.contract.leg_days) {
            legs.push(Json(days));
        }
        contract.set("leg_days", std::move(legs));
        auto deliveries = Json::array();
        for (const auto& delivery : s.contract.deliveries) {
            auto entry = Json::object();
            entry.set("to_station_id", delivery.to_station_id);
            entry.set("commodity_id", delivery.commodity_id);
            entry.set("units", delivery.units);
            entry.set("unit_price", delivery.unit_price);
            deliveries.push(std::move(entry));
        }
        contract.set("deliveries", std::move(deliveries));
        contract.set("signed_s", s.contract.signed_s);
        contract.set("until_s", s.contract.until_s);
        contract.set("cycle_days", s.contract.cycle_days);
        out.set("contract", std::move(contract));
    }
    auto passengers = Json::array();
    for (const auto& lot : s.passengers) {
        passengers.push(passenger_lot_to_json(lot));
    }
    out.set("passengers", std::move(passengers));
    return out;
}

domain::ShipState ship_from_json(const Json& j) {
    domain::ShipState s;
    s.id = j.get("id").string();
    s.name = j.get("name").string();
    s.faction_id = j.get("faction_id").string();
    s.class_id = j.get("class_id").string();
    for (const auto& module : j.get("fit").items()) {
        s.fit.push_back(module.string());
    }
    s.home_station_id = j.get("home_station_id").string();
    s.current_station_id = j.get("current_station_id").string();
    s.phase = phase_from_int(static_cast<int>(j.get("phase").number()));
    s.propellant_kg = j.get("propellant_kg").number();
    s.credits = j.get("credits").number();
    s.lifetime_profit = j.get("lifetime_profit").number();
    s.active_mission = mission_from_json(j.get("active_mission"));
    s.provisions = goods_from_json(j.get("provisions"));
    const auto& ledger = j.get("ledger");
    s.ledger.cargo_revenue = ledger.get("cargo_revenue").number();
    s.ledger.cargo_purchases = ledger.get("cargo_purchases").number();
    s.ledger.fuel = ledger.get("fuel").number();
    s.ledger.wages = ledger.get("wages").number();
    s.ledger.capital = ledger.get("capital").number();
    s.ledger.provisions = ledger.get("provisions").number();
    s.ledger.refits = ledger.get("refits").number();
    s.ledger.subsidies = ledger.get("subsidies").number();
    s.ledger.dividends = ledger.get("dividends").number();
    s.ledger.fares = ledger.get("fares").number();
    s.next_review_s = j.get("next_review_s").number();
    s.idle_since_s = j.get("idle_since_s").number();
    s.refit_class_id = j.get("refit_class_id").string();
    for (const auto& module : j.get("refit_fit").items()) {
        s.refit_fit.push_back(module.string());
    }
    s.refit_done_s = j.get("refit_done_s").number();
    s.leg_dose_msv = j.get("leg_dose_msv").number();
    s.engine_wear = j.get("engine_wear").number();
    s.radiator_condition = j.get("radiator_condition").number();
    s.overhaul = j.get("overhaul").boolean();
    s.next_refit_review_s = j.get("next_refit_review_s").number();
    s.laid_up_since_s = j.get("laid_up_since_s").number();
    s.commissioned_s = j.get("commissioned_s").number();
    s.route_destination_id = j.get("route_destination_id").string();
    s.route_commodity_id = j.get("route_commodity_id").string();
    s.route_units_per_day = j.get("route_units_per_day").number();
    s.route_until_s = j.get("route_until_s").number();
    if (const auto* review = j.find("next_contract_review_s"); review != nullptr) {
        s.next_contract_review_s = review->number();
    }
    if (const auto* contract = j.find("contract"); contract != nullptr) {
        for (const auto& stop : contract->get("stops").items()) {
            s.contract.stops.push_back(stop.string());
        }
        for (const auto& days : contract->get("leg_days").items()) {
            s.contract.leg_days.push_back(days.number());
        }
        for (const auto& entry : contract->get("deliveries").items()) {
            s.contract.deliveries.push_back({
                .to_station_id = entry.get("to_station_id").string(),
                .commodity_id = entry.get("commodity_id").string(),
                .units = entry.get("units").number(),
                .unit_price = entry.get("unit_price").number(),
            });
        }
        s.contract.signed_s = contract->get("signed_s").number();
        s.contract.until_s = contract->get("until_s").number();
        s.contract.cycle_days = contract->get("cycle_days").number();
    }
    for (const auto& lot : j.get("passengers").items()) {
        s.passengers.push_back(passenger_lot_from_json(lot));
    }
    return s;
}

Json station_to_json(const domain::StationState& s) {
    auto out = Json::object();
    out.set("station_id", s.station_id);
    out.set("inventory", goods_to_json(s.inventory));
    out.set("credits", s.credits);
    auto ledger = Json::object();
    ledger.set("household_sales", s.ledger.household_sales);
    ledger.set("producer_purchases", s.ledger.producer_purchases);
    ledger.set("dividends", s.ledger.dividends);
    ledger.set("subsidies", s.ledger.subsidies);
    ledger.set("taxes", s.ledger.taxes);
    ledger.set("passages_paid", s.ledger.passages_paid);
    ledger.set("passages_recovered", s.ledger.passages_recovered);
    ledger.set("wages_paid", s.ledger.wages_paid);
    ledger.set("household_spending", s.ledger.household_spending);
    ledger.set("surface_bought", s.ledger.surface_bought);
    ledger.set("surface_sold", s.ledger.surface_sold);
    ledger.set("incomes", s.ledger.incomes);
    ledger.set("visitor_spending", s.ledger.visitor_spending);
    ledger.set("yard_sales", s.ledger.yard_sales);
    ledger.set("ship_sales", s.ledger.ship_sales);
    ledger.set("ship_purchases", s.ledger.ship_purchases);
    ledger.set("days_in_red", s.ledger.days_in_red);
    ledger.set("crew_wages", s.ledger.crew_wages);
    ledger.set("hired_from_surface", s.ledger.hired_from_surface);
    ledger.set("moved_to_surface", s.ledger.moved_to_surface);
    out.set("ledger", std::move(ledger));
    out.set("household_credits", s.household_credits);
    out.set("travel_budget_cr", s.travel_budget_cr);
    out.set("jobs", s.jobs);
    out.set("wage_cr_per_day", s.wage_cr_per_day);
    out.set("employed_share", s.employed_share);
    out.set("ship_fuel_units_per_day", s.ship_fuel_units_per_day);
    out.set("import_units_per_day", goods_to_json(s.import_units_per_day));
    out.set("export_units_per_day", goods_to_json(s.export_units_per_day));
    out.set("demand_units", goods_to_json(s.demand_units));
    out.set("unmet_units", goods_to_json(s.unmet_units));
    out.set("market_sold_units", goods_to_json(s.market_sold_units));
    out.set("import_unit_cost", goods_to_json(s.import_unit_cost));
    auto order_stats = Json::object();
    for (const auto& [commodity_id, stats] : s.order_stats) {
        auto entry = Json::object();
        entry.set("ship_sales_per_day", stats.ship_sales_per_day);
        entry.set("lead_days", stats.lead_days);
        entry.set("interval_days", stats.interval_days);
        entry.set("spread_days", stats.spread_days);
        entry.set("going_rate", stats.going_rate);
        entry.set("last_delivery_s", stats.last_delivery_s);
        entry.set("deliveries", static_cast<double>(stats.deliveries));
        entry.set("order_price", stats.order_price);
        entry.set("market_price", stats.market_price);
        entry.set("market_bidders", static_cast<double>(stats.market_bidders));
        order_stats.set(commodity_id, std::move(entry));
    }
    out.set("order_stats", std::move(order_stats));
    out.set("population", s.population);
    out.set("supply_index", s.supply_index);
    out.set("population_announced", s.population_announced);
    auto events = Json::array();
    for (const auto& active : s.events) {
        auto entry = Json::object();
        entry.set("event_id", active.event_id);
        entry.set("start_s", active.start_s);
        entry.set("end_s", active.end_s);
        events.push(std::move(entry));
    }
    out.set("events", std::move(events));
    auto waiting = Json::array();
    for (const auto& group : s.waiting) {
        waiting.push(passenger_group_to_json(group));
    }
    out.set("waiting", std::move(waiting));
    auto visiting = Json::array();
    for (const auto& group : s.visiting) {
        visiting.push(passenger_group_to_json(group));
    }
    out.set("visiting", std::move(visiting));
    out.set("passage_debt_cr", s.passage_debt_cr);
    return out;
}

domain::StationState station_from_json(const Json& j) {
    domain::StationState s;
    s.station_id = j.get("station_id").string();
    s.inventory = goods_from_json(j.get("inventory"));
    s.credits = j.get("credits").number();
    const auto& ledger = j.get("ledger");
    s.ledger.household_sales = ledger.get("household_sales").number();
    s.ledger.producer_purchases = ledger.get("producer_purchases").number();
    s.ledger.dividends = ledger.get("dividends").number();
    s.ledger.subsidies = ledger.get("subsidies").number();
    s.ledger.taxes = ledger.get("taxes").number();
    s.ledger.passages_paid = ledger.get("passages_paid").number();
    s.ledger.passages_recovered = ledger.get("passages_recovered").number();
    s.ledger.wages_paid = ledger.get("wages_paid").number();
    s.ledger.household_spending = ledger.get("household_spending").number();
    s.ledger.surface_bought = ledger.get("surface_bought").number();
    s.ledger.surface_sold = ledger.get("surface_sold").number();
    s.ledger.incomes = ledger.get("incomes").number();
    s.ledger.visitor_spending = ledger.get("visitor_spending").number();
    s.ledger.yard_sales = ledger.get("yard_sales").number();
    s.ledger.ship_sales = ledger.get("ship_sales").number();
    s.ledger.ship_purchases = ledger.get("ship_purchases").number();
    s.ledger.days_in_red = ledger.get("days_in_red").number();
    s.ledger.crew_wages = ledger.get("crew_wages").number();
    s.ledger.hired_from_surface = ledger.get("hired_from_surface").number();
    s.ledger.moved_to_surface = ledger.get("moved_to_surface").number();
    s.household_credits = j.get("household_credits").number();
    s.travel_budget_cr = j.get("travel_budget_cr").number();
    s.jobs = j.get("jobs").number();
    s.wage_cr_per_day = j.get("wage_cr_per_day").number();
    s.employed_share = j.get("employed_share").number();
    s.ship_fuel_units_per_day = j.get("ship_fuel_units_per_day").number();
    s.import_units_per_day = goods_from_json(j.get("import_units_per_day"));
    s.export_units_per_day = goods_from_json(j.get("export_units_per_day"));
    s.demand_units = goods_from_json(j.get("demand_units"));
    s.unmet_units = goods_from_json(j.get("unmet_units"));
    s.market_sold_units = goods_from_json(j.get("market_sold_units"));
    s.import_unit_cost = goods_from_json(j.get("import_unit_cost"));
    for (const auto& [commodity_id, entry] : j.get("order_stats").fields()) {
        s.order_stats[commodity_id] = {
            .ship_sales_per_day = entry.get("ship_sales_per_day").number(),
            .lead_days = entry.get("lead_days").number(),
            .interval_days = entry.get("interval_days").number(),
            .spread_days = entry.get("spread_days").number(),
            .going_rate = entry.get("going_rate").number(),
            .last_delivery_s = entry.get("last_delivery_s").number(),
            .deliveries = static_cast<int>(entry.get("deliveries").number()),
            .order_price = entry.get("order_price").number(),
        };
        if (const auto* market_price = entry.find("market_price"); market_price != nullptr) {
            s.order_stats[commodity_id].market_price = market_price->number();
            s.order_stats[commodity_id].market_bidders = static_cast<int>(entry.get("market_bidders").number());
        }
    }
    s.population = j.get("population").number();
    s.supply_index = j.get("supply_index").number();
    s.population_announced = j.get("population_announced").number();
    for (const auto& entry : j.get("events").items()) {
        s.events.push_back({
            .event_id = entry.get("event_id").string(),
            .start_s = entry.get("start_s").number(),
            .end_s = entry.get("end_s").number(),
        });
    }
    for (const auto& group : j.get("waiting").items()) {
        s.waiting.push_back(passenger_group_from_json(group));
    }
    for (const auto& group : j.get("visiting").items()) {
        s.visiting.push_back(passenger_group_from_json(group));
    }
    s.passage_debt_cr = j.get("passage_debt_cr").number();
    return s;
}

}  // namespace

std::string Simulation::data_fingerprint() const {
    // FNV-1a over the definitions a save depends on: ids and the numbers that shape play.
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&](const std::string& text) {
        for (const char ch : text) {
            hash ^= static_cast<unsigned char>(ch);
            hash *= 1099511628211ULL;
        }
    };
    for (const auto& station : universe_.stations) {
        mix(std::format("s{}|{}|{}|{};", station.id, station.parent_body_id, station.population, station.storage_capacity_units));
    }
    for (const auto& ship_class : universe_.ship_classes) {
        mix(std::format("c{}|{}|{}|{}|{}|{};", ship_class.id, ship_class.dry_mass_kg, ship_class.propellant_capacity_kg,
            ship_class.cargo_capacity_units, ship_class.crew_size, ship_class.berths));
    }
    {
        const auto& pax = universe_.passengers;
        mix(std::format("P{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{};", pax.enabled, pax.rotation, pax.migration, pax.discretionary,
            pax.body_luggage_kg, pax.consumables_kg_per_day, pax.recovery_days_per_flight_day, pax.rotation_share,
            pax.rotation_grade, pax.hardship_per_year, pax.resident_wage_multiple));
        mix(std::format("V{}|{}|{}|{}|{}|{};", pax.visit_trips_per_person_year, pax.earth_population_weight,
            pax.fare_elasticity, pax.demand_bands, pax.band_ratio, pax.visit_days));
        mix(std::format("S{}|{}|{}|{}|{}|{}|{};", pax.tank_wrap_mean_fill, pax.settlers_sponsored,
            pax.settler_payback_half_years, pax.settler_payback_exponent, pax.passage_recovery_days,
            pax.passage_max_markup, 0));
        mix(std::format("Q{}|{}|{};", pax.visitor_batch_days, pax.min_leg_passengers,
            universe_.fleet_investment.purchase_spacing_days));
        mix(std::format("R{}|{};", pax.rider_time_min_share, pax.rider_time_relative_step));
        for (const auto& gateway : pax.earth_gateways) {
            mix("G" + gateway + ";");
        }
    }
    {
        const auto& env = universe_.ship_environment;
        mix(std::format("e{}|{}|{}|{}|{}|{}|{}|{};", env.radiation, env.gcr_msv_per_day, env.gcr_cycle_amplitude,
            env.solar_minimum_day, env.spe_rate_per_year, env.spe_hull_msv, env.career_limit_msv, env.hazard_cr_per_msv));
        mix(std::format("b{}|{}|{}|{};", env.boiloff, env.mli_flux_w_m2, env.mli_flux_exponent, env.cold_storage));
        mix(std::format("w{}|{}|{}|{}|{}|{};", env.wear, env.ntr_core_life_h, env.plasma_drive_life_h, env.puncture_flux_m2_yr,
            env.overhaul_wear, env.initial_wear_max));
        mix(std::format("r{}|{}|{};", env.repair_team_size, env.repair_team_hours_per_day, env.min_repair_days));
        std::map<std::string, double> grades(universe_.labour.grades.begin(), universe_.labour.grades.end());
        mix(std::format("l{}|{};", universe_.labour.reference_wage_cr_per_day, universe_.labour.shift_hours));
        for (const auto& [grade, multiple] : grades) {
            mix(std::format("g{}|{};", grade, multiple));
        }
        std::map<std::string, domain::SparePartDefinition> spares(universe_.spare_parts.begin(), universe_.spare_parts.end());
        for (const auto& [kind, part] : spares) {
            mix(std::format("p{}|{}|{}|{}|{};", kind, part.wears_with, part.grade, part.fabrication_h_per_kg, part.install_h_per_kg));
            for (const auto& [good, share] : part.materials) {
                mix(std::format("q{}|{};", good, share));
            }
        }
        std::map<std::string, domain::StationRadiationDefinition> sorted(
            universe_.station_radiation.begin(), universe_.station_radiation.end());
        for (const auto& [station_id, radiation] : sorted) {
            mix(std::format("x{}|{}|{};", station_id, radiation.location_factor, radiation.shield_g_cm2));
        }
    }
    for (const auto& module : universe_.ship_modules) {
        mix(std::format("m{}|{}|{}|{}|{};", module.hull_id, module.id, module.slot, module.part.mass_kg, module.part.price_cr));
        std::map<std::string, double> effects(module.effects.begin(), module.effects.end());
        for (const auto& [effect, value] : effects) {
            mix(std::format("E{}|{};", effect, value));
        }
    }
    for (const auto& ship_class : universe_.ship_classes) {
        if (const auto slots = universe_.hull_slots.find(ship_class.id); slots != universe_.hull_slots.end()) {
            std::map<std::string, int> sorted(slots->second.begin(), slots->second.end());
            for (const auto& [slot, count] : sorted) {
                mix(std::format("h{}|{}|{};", ship_class.id, slot, count));
            }
        }
    }
    for (const auto& commodity : universe_.commodities) {
        mix(std::format("g{}|{};", commodity.id, commodity.base_price));
    }
    {
        // Economy overhaul: households, background economies and what they trade.
        const auto& h = universe_.households;
        mix(std::format("H{}|{}|{}|{}|{}|{}|{}|{}|{};", h.enabled, h.participation, h.savings_days, h.spend_days,
            h.migrant_savings_cr, h.visitor_spend_cr_per_day, h.credits_per_usd, h.yard_share, h.surface_depth_units));
        mix(std::format("Hc{}|{}|{}|{}|{}|{};", h.supply_price_elasticity, h.min_utilization, h.consume_income_elasticity,
            h.consume_price_elasticity, h.upkeep_price_elasticity, h.yard_materials_share));
        mix(std::format("Ht{}|{};", h.travel_share, h.science_price_premium));
        const std::map<std::string, double> wages(h.wage_multiple.begin(), h.wage_multiple.end());
        for (const auto& [station_id, multiple] : wages) {
            mix(std::format("Hw{}|{};", station_id, multiple));
        }
        const auto& l = universe_.labour_market;
        mix(std::format("L{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{};", l.enabled, l.earth_wage_median_cr_per_day, l.earth_wage_sigma,
            l.station_wage_sigma, l.stay_years, l.hardship_share, l.wage_adjust_per_year, l.min_wage_multiple, l.vacancy_fill_days,
            l.unemployed_leave_days, l.unemployment_benefit, l.visitor_budget_years, l.visitor_income_sigma));
        for (const auto& yard : h.yards) {
            mix("Hy" + yard + ";");
        }
        {
            const std::map<std::string, double> factory_prices(universe_.fuel_supply.factory_price_cr.begin(),
                universe_.fuel_supply.factory_price_cr.end());
            for (const auto& [station_id, price] : factory_prices) {
                mix(std::format("Fp{}|{};", station_id, price));
            }
        }
        for (const auto& economy : universe_.background_economies) {
            mix(std::format("B{}|{}|{}|{}|{}|{}|{}|{}|{}|{};", economy.id, economy.launch_cr_per_kg, economy.landing_cr_per_kg,
                economy.initial_credits, economy.income_cr_per_day, economy.population, economy.growth_per_year,
                economy.wage_median_cr_per_day, economy.wage_sigma, economy.search_per_year));
            mix(std::format("Bi{};", economy.immigration_share));
        }
        for (const auto& market : universe_.surface_markets) {
            mix(std::format("S{}|{}|{}|{}|{}|{}|{}|{};", market.economy_id, market.station_id, market.commodity_id, market.ask_cr,
                market.bid_cr, market.sell_units_per_day, market.buy_units_per_day, market.bid_share));
        }
        for (const auto& income : universe_.station_incomes) {
            mix(std::format("I{}|{}|{}|{}|{};", income.station_id, income.kind, income.payer_id, income.cr_per_day, income.need_based));
        }
    }
    const auto& orders = universe_.orders;
    mix(std::format("o{}|{}|{}|{}|{}|{}|{}|{}|{}|{};", orders.safety_factor,
        orders.ema_deliveries, orders.extra_discount, orders.max_extra_days, orders.resale_margin, orders.keep_share, orders.life_value, orders.consume_value,
        orders.depot_fuel_value, orders.input_cap));
    for (const auto& recipe : universe_.recipes) {
        mix(std::format("r{}|{}|{}|{}|{};", recipe.profile_id, recipe.station_id, recipe.commodity_id, recipe.units_per_day,
            static_cast<int>(recipe.role)));
        for (const auto& output : recipe.feeds) {
            mix(std::format("f{};", output));
        }
    }
    return std::format("{:016x}", hash);
}

std::string Simulation::save_state_json() const {
    auto root = Json::object();
    root.set("version", kSaveVersion);
    root.set("data_fingerprint", data_fingerprint());
    root.set("game_time_s", game_time_s_);
    root.set("untimed_s", untimed_s_);
    root.set("timewarp_factor", timewarp_factor_);
    root.set("outside_economy_credits", outside_economy_credits_);
    auto background = Json::object();
    for (const auto& [economy_id, balance] : background_credits_) {
        background.set(economy_id, balance);
    }
    root.set("background_credits", std::move(background));
    auto colonists = Json::object();
    for (const auto& [economy_id, people] : background_population_) {
        colonists.set(economy_id, people);
    }
    root.set("background_population", std::move(colonists));
    auto colony_states = Json::object();
    for (const auto& [economy_id, state] : colony_state_) {
        colony_states.set(economy_id, numbers_to_json({state.satisfaction, state.throttle, state.income_per_day,
            state.base_imports_per_day, state.wage_median}));
    }
    root.set("colony_state", std::move(colony_states));
    auto treasuries = Json::object();
    for (const auto& [faction_id, balance] : faction_treasuries_) {
        treasuries.set(faction_id, balance);
    }
    root.set("faction_treasuries", std::move(treasuries));
    auto taxes = Json::object();
    for (const auto& [faction_id, per_day] : faction_tax_per_day_) {
        taxes.set(faction_id, per_day);
    }
    root.set("faction_tax_per_day", std::move(taxes));
    root.set("faction_interest_paid", faction_interest_paid_);
    root.set("seeded_money_supply", seeded_money_supply_);
    root.set("next_investment_review_s", next_investment_review_s_);
    root.set("investment_purchases_left", investment_purchases_left_);
    root.set("next_purchase_s", next_purchase_s_);
    auto probes = Json::object();
    for (const auto& [key, runs] : review_probes_) {
        auto list = Json::array();
        for (const auto& run : runs) {
            auto entry = Json::object();
            entry.set("destination_id", run.destination_id);
            entry.set("commodity_id", run.commodity_id);
            entry.set("cargo_units", run.cargo_units);
            entry.set("travel_days", run.travel_days);
            entry.set("wait_days", run.wait_days);
            entry.set("fuel_cost", run.fuel_cost);
            entry.set("run", run.run);
            entry.set("module_id", run.module_id);
            entry.set("return_fuel_kg", run.return_fuel_kg);
            list.push(std::move(entry));
        }
        probes.set(key, std::move(list));
    }
    root.set("review_probes", std::move(probes));
    auto investment = Json::object();
    investment.set("ships_commissioned", investment_ledger_.ships_commissioned);
    investment.set("ships_sold", investment_ledger_.ships_sold);
    investment.set("hulls_bought", investment_ledger_.hulls_bought);
    investment.set("working_capital", investment_ledger_.working_capital);
    investment.set("salvage", investment_ledger_.salvage);
    root.set("investment_ledger", std::move(investment));
    auto emergencies = Json::array();
    for (const auto& emergency : emergencies_) {
        auto entry = Json::object();
        entry.set("station_id", emergency.station_id);
        entry.set("commodity_id", emergency.commodity_id);
        entry.set("premium", emergency.premium);
        entry.set("units_open", emergency.units_open);
        entry.set("opened_s", emergency.opened_s);
        entry.set("last_raise_s", emergency.last_raise_s);
        entry.set("faction_paid", emergency.faction_paid);
        emergencies.push(std::move(entry));
    }
    root.set("emergencies", std::move(emergencies));
    root.set("emergencies_opened", emergencies_opened_);
    {
        std::ostringstream rng;
        rng << event_rng_;
        root.set("event_rng", rng.str());
        std::ostringstream environment_rng;
        environment_rng << environment_rng_;
        root.set("environment_rng", environment_rng.str());
        auto started = Json::object();
        for (const auto& [event_id, count] : events_started_) {
            started.set(event_id, count);
        }
        root.set("events_started", std::move(started));
    }
    root.set("emergency_paid", emergency_paid_);
    auto stations = Json::array();
    for (const auto& station : stations_) {
        stations.push(station_to_json(station));
    }
    root.set("stations", std::move(stations));
    auto ships = Json::array();
    for (const auto& ship : ships_) {
        ships.push(ship_to_json(ship));
    }
    root.set("ships", std::move(ships));
    auto sold = Json::array();
    for (const auto& ship : sold_ships_) {
        sold.push(ship_to_json(ship));
    }
    root.set("sold_ships", std::move(sold));
    auto events = Json::array();
    for (const auto& event : recent_events_) {
        auto e = Json::object();
        e.set("time_s", event.time_s);
        e.set("text", event.text);
        e.set("category", event.category);
        events.push(std::move(e));
    }
    root.set("recent_events", std::move(events));
    auto trades = Json::array();
    for (const auto& trade : recent_trades_) {
        auto t = Json::object();
        t.set("time_s", trade.time_s);
        t.set("ship_id", trade.ship_id);
        t.set("station_id", trade.station_id);
        t.set("commodity_id", trade.commodity_id);
        t.set("kind", trade.kind);
        t.set("units", trade.units);
        t.set("unit_price", trade.unit_price);
        t.set("total", trade.total);
        trades.push(std::move(t));
    }
    root.set("recent_trades", std::move(trades));
    root.set("money_journal", journal_to_json(journal_));
    return root.dump();
}

void Simulation::load_state_json(const std::string& text) {
    const auto root = Json::parse(text);
    const int version = static_cast<int>(root.get("version").number());
    if (version != kSaveVersion) {
        throw std::runtime_error(std::format("save: version {} is not supported (expected {})", version, kSaveVersion));
    }
    if (root.get("data_fingerprint").string() != data_fingerprint()) {
        throw std::runtime_error("save: made with different data files (stations, ship classes, goods or recipes changed)");
    }
    // Parse everything before touching the simulation, so a bad file leaves it as it was.
    std::vector<domain::StationState> stations;
    for (const auto& item : root.get("stations").items()) {
        stations.push_back(station_from_json(item));
    }
    MoneyJournal journal = journal_;   // same accounts (same data)
    journal_from_json(root.get("money_journal"), journal);
    if (stations.size() != universe_.stations.size()) {
        throw std::runtime_error("save: station count does not match the data");
    }
    for (std::size_t i = 0; i < stations.size(); ++i) {
        if (stations[i].station_id != universe_.stations[i].id) {
            throw std::runtime_error(std::format("save: station {} is not {}", stations[i].station_id, universe_.stations[i].id));
        }
    }
    std::vector<domain::ShipState> ships;
    for (const auto& item : root.get("ships").items()) {
        ships.push_back(ship_from_json(item));
        (void)register_design(ships.back().class_id, ships.back().fit, radiator_bucket(ships.back()));  // throws on a bad class or fit
        if (!ships.back().refit_class_id.empty()) {
            (void)register_design(ships.back().refit_class_id, ships.back().refit_fit);
        }
    }
    std::vector<domain::ShipState> sold;
    for (const auto& item : root.get("sold_ships").items()) {
        sold.push_back(ship_from_json(item));
        (void)register_design(sold.back().class_id, sold.back().fit, radiator_bucket(sold.back()));
    }
    std::map<std::string, double> treasuries;
    for (const auto& [faction_id, balance] : root.get("faction_treasuries").fields()) {
        treasuries[faction_id] = balance.number();
    }
    std::map<std::string, double> tax_per_day;
    for (const auto& [faction_id, per_day] : root.get("faction_tax_per_day").fields()) {
        tax_per_day[faction_id] = per_day.number();
    }
    std::vector<domain::EventEntry> events;
    for (const auto& item : root.get("recent_events").items()) {
        events.push_back({.time_s = item.get("time_s").number(), .text = item.get("text").string(), .category = item.get("category").string()});
    }
    std::vector<domain::TradeEntry> trades;
    for (const auto& item : root.get("recent_trades").items()) {
        trades.push_back({
            .time_s = item.get("time_s").number(),
            .ship_id = item.get("ship_id").string(),
            .station_id = item.get("station_id").string(),
            .commodity_id = item.get("commodity_id").string(),
            .kind = item.get("kind").string(),
            .units = item.get("units").number(),
            .unit_price = item.get("unit_price").number(),
            .total = item.get("total").number(),
        });
    }
    const auto& investment = root.get("investment_ledger");
    std::vector<domain::Emergency> emergencies;
    for (const auto& entry : root.get("emergencies").items()) {
        emergencies.push_back({
            .station_id = entry.get("station_id").string(),
            .commodity_id = entry.get("commodity_id").string(),
            .premium = entry.get("premium").number(),
            .units_open = entry.get("units_open").number(),
            .opened_s = entry.get("opened_s").number(),
            .last_raise_s = entry.get("last_raise_s").number(),
            .faction_paid = entry.get("faction_paid").number(),
        });
        (void)get_station_definition(emergencies.back().station_id);  // throws on an unknown station
    }
    std::mt19937_64 event_rng;
    {
        std::istringstream rng(root.get("event_rng").string());
        rng >> event_rng;
        if (rng.fail()) {
            throw std::runtime_error("save: bad event_rng");
        }
    }
    std::mt19937_64 environment_rng;
    {
        std::istringstream rng(root.get("environment_rng").string());
        rng >> environment_rng;
        if (rng.fail()) {
            throw std::runtime_error("save: bad environment_rng");
        }
    }
    std::map<std::string, int> events_started;
    for (const auto& [event_id, count] : root.get("events_started").fields()) {
        events_started[event_id] = static_cast<int>(count.number());
    }
    std::map<std::string, std::vector<ProbedRun>> review_probes;
    for (const auto& [key, list] : root.get("review_probes").fields()) {
        auto& runs = review_probes[key];
        for (const auto& entry : list.items()) {
            runs.push_back({
                .destination_id = entry.get("destination_id").string(),
                .commodity_id = entry.get("commodity_id").string(),
                .cargo_units = entry.get("cargo_units").number(),
                .travel_days = entry.get("travel_days").number(),
                .wait_days = entry.get("wait_days").number(),
                .fuel_cost = entry.get("fuel_cost").number(),
                .run = static_cast<int>(entry.get("run").number()),
                .module_id = entry.get("module_id").string(),
                .return_fuel_kg = entry.get("return_fuel_kg").number(),
            });
            (void)get_station_definition(runs.back().destination_id);  // throws on an unknown station
        }
    }

    game_time_s_ = root.get("game_time_s").number();
    untimed_s_ = root.get("untimed_s").number();
    timewarp_factor_ = root.get("timewarp_factor").number();
    outside_economy_credits_ = root.get("outside_economy_credits").number();
    background_credits_.clear();
    for (const auto& [economy_id, balance] : root.get("background_credits").fields()) {
        background_credits_[economy_id] = balance.number();
    }
    background_population_.clear();
    for (const auto& [economy_id, people] : root.get("background_population").fields()) {
        background_population_[economy_id] = people.number();
    }
    colony_state_.clear();
    for (const auto& [economy_id, values] : root.get("colony_state").fields()) {
        const auto v = numbers_from_json(values);
        colony_state_[economy_id] = {.satisfaction = v.at(0), .throttle = v.at(1), .income_per_day = v.at(2),
            .base_imports_per_day = v.at(3), .wage_median = v.at(4)};
    }
    faction_treasuries_ = std::move(treasuries);
    faction_tax_per_day_ = std::move(tax_per_day);
    faction_interest_paid_ = root.get("faction_interest_paid").number();
    seeded_money_supply_ = root.get("seeded_money_supply").number();
    next_investment_review_s_ = root.get("next_investment_review_s").number();
    investment_purchases_left_ = static_cast<int>(root.get("investment_purchases_left").number());
    next_purchase_s_ = root.get("next_purchase_s").number();
    review_probes_ = std::move(review_probes);
    emergencies_ = std::move(emergencies);
    emergencies_opened_ = static_cast<int>(root.get("emergencies_opened").number());
    event_rng_ = event_rng;
    environment_rng_ = environment_rng;
    events_started_ = std::move(events_started);
    emergency_paid_ = root.get("emergency_paid").number();
    investment_ledger_.ships_commissioned = static_cast<int>(investment.get("ships_commissioned").number());
    investment_ledger_.ships_sold = static_cast<int>(investment.get("ships_sold").number());
    investment_ledger_.hulls_bought = investment.get("hulls_bought").number();
    investment_ledger_.working_capital = investment.get("working_capital").number();
    investment_ledger_.salvage = investment.get("salvage").number();
    stations_ = std::move(stations);
    journal_ = std::move(journal);
    for (const auto& station : stations_) {
        economy_.set_employment(station.station_id, station.employed_share);
        economy_.set_population(station.station_id, station.population);
    }
    ships_ = std::move(ships);
    sold_ships_ = std::move(sold);
    recent_events_ = std::move(events);
    recent_trades_ = std::move(trades);
    // Caches are pure functions of their keys; empty ones refill with the same values.
    leg_estimates_.clear();
    trajectory_audit_records_.clear();
}

}  // namespace spacetrains::simulation
