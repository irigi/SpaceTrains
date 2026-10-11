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

constexpr int kSaveVersion = 10;  // 2: mixed cargo (lots per mission); 3: a fleet review's probes; 4: probe runs; 5: contracts; 6: emergencies; 7: faction credit, import costs; 8: liner subsidies; 9: populations; 10: events

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
    m.pickup_commodity_id = j.get("pickup_commodity_id").string();
    m.pickup_units = j.get("pickup_units").number();
    return m;
}

Json ship_to_json(const domain::ShipState& s) {
    auto out = Json::object();
    out.set("id", s.id);
    out.set("name", s.name);
    out.set("faction_id", s.faction_id);
    out.set("class_id", s.class_id);
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
    out.set("ledger", std::move(ledger));
    out.set("next_review_s", s.next_review_s);
    out.set("idle_since_s", s.idle_since_s);
    out.set("refit_class_id", s.refit_class_id);
    out.set("refit_done_s", s.refit_done_s);
    out.set("next_refit_review_s", s.next_refit_review_s);
    out.set("laid_up_since_s", s.laid_up_since_s);
    out.set("commissioned_s", s.commissioned_s);
    out.set("route_destination_id", s.route_destination_id);
    out.set("route_commodity_id", s.route_commodity_id);
    out.set("route_units_per_day", s.route_units_per_day);
    out.set("route_until_s", s.route_until_s);
    return out;
}

domain::ShipState ship_from_json(const Json& j) {
    domain::ShipState s;
    s.id = j.get("id").string();
    s.name = j.get("name").string();
    s.faction_id = j.get("faction_id").string();
    s.class_id = j.get("class_id").string();
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
    s.next_review_s = j.get("next_review_s").number();
    s.idle_since_s = j.get("idle_since_s").number();
    s.refit_class_id = j.get("refit_class_id").string();
    s.refit_done_s = j.get("refit_done_s").number();
    s.next_refit_review_s = j.get("next_refit_review_s").number();
    s.laid_up_since_s = j.get("laid_up_since_s").number();
    s.commissioned_s = j.get("commissioned_s").number();
    s.route_destination_id = j.get("route_destination_id").string();
    s.route_commodity_id = j.get("route_commodity_id").string();
    s.route_units_per_day = j.get("route_units_per_day").number();
    s.route_until_s = j.get("route_until_s").number();
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
    out.set("ledger", std::move(ledger));
    out.set("ship_fuel_units_per_day", s.ship_fuel_units_per_day);
    out.set("import_units_per_day", goods_to_json(s.import_units_per_day));
    out.set("export_units_per_day", goods_to_json(s.export_units_per_day));
    out.set("demand_units", goods_to_json(s.demand_units));
    out.set("unmet_units", goods_to_json(s.unmet_units));
    out.set("market_sold_units", goods_to_json(s.market_sold_units));
    out.set("import_unit_cost", goods_to_json(s.import_unit_cost));
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
    s.ship_fuel_units_per_day = j.get("ship_fuel_units_per_day").number();
    s.import_units_per_day = goods_from_json(j.get("import_units_per_day"));
    s.export_units_per_day = goods_from_json(j.get("export_units_per_day"));
    s.demand_units = goods_from_json(j.get("demand_units"));
    s.unmet_units = goods_from_json(j.get("unmet_units"));
    s.market_sold_units = goods_from_json(j.get("market_sold_units"));
    s.import_unit_cost = goods_from_json(j.get("import_unit_cost"));
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
        mix(std::format("c{}|{}|{}|{};", ship_class.id, ship_class.dry_mass_kg, ship_class.propellant_capacity_kg, ship_class.cargo_capacity_units));
    }
    for (const auto& commodity : universe_.commodities) {
        mix(std::format("g{}|{};", commodity.id, commodity.base_price));
    }
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
        (void)get_ship_class(ships.back().class_id);  // throws on an unknown class
    }
    std::vector<domain::ShipState> sold;
    for (const auto& item : root.get("sold_ships").items()) {
        sold.push_back(ship_from_json(item));
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
            });
            (void)get_station_definition(runs.back().destination_id);  // throws on an unknown station
        }
    }

    game_time_s_ = root.get("game_time_s").number();
    untimed_s_ = root.get("untimed_s").number();
    timewarp_factor_ = root.get("timewarp_factor").number();
    outside_economy_credits_ = root.get("outside_economy_credits").number();
    faction_treasuries_ = std::move(treasuries);
    faction_tax_per_day_ = std::move(tax_per_day);
    faction_interest_paid_ = root.get("faction_interest_paid").number();
    seeded_money_supply_ = root.get("seeded_money_supply").number();
    next_investment_review_s_ = root.get("next_investment_review_s").number();
    investment_purchases_left_ = static_cast<int>(root.get("investment_purchases_left").number());
    review_probes_ = std::move(review_probes);
    emergencies_ = std::move(emergencies);
    emergencies_opened_ = static_cast<int>(root.get("emergencies_opened").number());
    event_rng_ = event_rng;
    events_started_ = std::move(events_started);
    emergency_paid_ = root.get("emergency_paid").number();
    investment_ledger_.ships_commissioned = static_cast<int>(investment.get("ships_commissioned").number());
    investment_ledger_.ships_sold = static_cast<int>(investment.get("ships_sold").number());
    investment_ledger_.hulls_bought = investment.get("hulls_bought").number();
    investment_ledger_.working_capital = investment.get("working_capital").number();
    investment_ledger_.salvage = investment.get("salvage").number();
    stations_ = std::move(stations);
    for (const auto& station : stations_) {
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
