#include "simulation/Simulation.hpp"

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace spacetrains::simulation {

namespace {
constexpr double FUEL_UNITS_TO_KG = 100.0;
constexpr std::size_t MAX_EVENT_HISTORY = 24;
constexpr std::size_t MAX_TRADE_HISTORY = 24;
// A cargo run must clear this profit rate or the ship repositions toward demand
// instead. Without a floor, ships accept months-long near-zero-profit hauls that
// tie up the fleet (observed: 1u water, 14 cr, 183 d transit). Long interplanetary
// runs legitimately score ~15-25 cr/day, so the floor must sit well below that.
constexpr double MIN_MISSION_SCORE_PER_DAY = 2.0;
// Provisions are bought for the whole mission (wait + transit) plus this margin,
// and enough for a return leg (at least this many days): destinations are often
// starving stations that cannot spare food or oxygen, and a crew stuck there could
// never leave.
constexpr double PROVISION_MARGIN = 1.1;
constexpr double PROVISION_MIN_RETURN_DAYS = 120.0;
// Docked crews top up provisions from the local market when below the first
// threshold, buying enough for the second.
constexpr double DOCKED_PROVISION_LOW_DAYS = 7.0;
constexpr double DOCKED_PROVISION_TARGET_DAYS = 14.0;
// Crews keep two years of life support aboard where ports can spare it: outer-system
// legs take 400-1100 days and the outer stations themselves import their food.
constexpr double PROVISION_ENDURANCE_DAYS = 730.0;
// An idle crewed ship with nothing profitable to fly for this long lays up,
// whatever its balance: no owner pays an idle crew for months.
constexpr double IDLE_LAYUP_DAYS = 30.0;
// A laid-up ship has no crew; its owner looks for work for it every few days.
constexpr double LAYUP_REVIEW_DAYS = 5.0;
constexpr double IDLE_REVIEW_S = 6.0 * 3600.0;
// Ships load the propellant a mission burns plus this reserve, not a full tank.
constexpr double PROPELLANT_RESERVE_FRACTION = 0.1;

// Part loads dispatch scores: a full hold may be too heavy, and big lots move prices.
constexpr std::array<double, 3> LOAD_FRACTIONS {1.0, 0.5, 0.25};
// Tank refits (part C): a docked ship at its home base compares the next smaller and next
// larger tank variants this often (each comparison is a full dispatch pass, ~0.3 s), and
// refits when the better missions repay the yard within the payback period.
constexpr double REFIT_REVIEW_DAYS = 90.0;
constexpr double REFIT_PAYBACK_DAYS = 180.0;
// Ships carry their return fuel to ports that sell it at more than this multiple of the
// departure port's price.
constexpr double TANKERING_PRICE_RATIO = 2.0;
// Ships' fuel purchases and cargo trades at a station are averaged over about this many days.
constexpr double TRADE_FLOW_DAYS = 60.0;

math::Vec3d interpolate_sampled_path(const std::vector<math::Vec3d>& path, double progress) {
    if (path.empty()) {
        return {};
    }
    if (path.size() == 1) {
        return path.front();
    }

    const double clamped_progress = std::clamp(progress, 0.0, 1.0);
    const double scaled_index = clamped_progress * static_cast<double>(path.size() - 1);
    const auto lower_index = static_cast<std::size_t>(std::floor(scaled_index));
    const auto upper_index = std::min(lower_index + 1, path.size() - 1);
    const double segment_alpha = scaled_index - static_cast<double>(lower_index);
    return path[lower_index] * (1.0 - segment_alpha) + path[upper_index] * segment_alpha;
}

double interpolate_timed_scalar(
    const std::vector<double>& values,
    const std::vector<double>& sample_times_s,
    double time_s) {
    if (values.empty()) {
        return 0.0;
    }
    if (values.size() == 1 || sample_times_s.size() != values.size()) {
        return values.front();
    }
    if (time_s <= sample_times_s.front()) {
        return values.front();
    }
    if (time_s >= sample_times_s.back()) {
        return values.back();
    }
    const auto upper = std::upper_bound(sample_times_s.begin(), sample_times_s.end(), time_s);
    const auto upper_index = static_cast<std::size_t>(std::distance(sample_times_s.begin(), upper));
    const auto lower_index = upper_index - 1;
    const double span_s = std::max(1.0e-6, sample_times_s[upper_index] - sample_times_s[lower_index]);
    const double alpha = (time_s - sample_times_s[lower_index]) / span_s;
    return values[lower_index] * (1.0 - alpha) + values[upper_index] * alpha;
}

math::Vec3d interpolate_timed_sampled_path(
    const std::vector<math::Vec3d>& path,
    const std::vector<double>& sample_times_s,
    double time_s) {
    if (path.empty()) {
        return {};
    }
    if (path.size() == 1 || sample_times_s.size() != path.size()) {
        return path.front();
    }
    if (time_s <= sample_times_s.front()) {
        return path.front();
    }
    if (time_s >= sample_times_s.back()) {
        return path.back();
    }
    const auto upper = std::upper_bound(sample_times_s.begin(), sample_times_s.end(), time_s);
    const auto upper_index = static_cast<std::size_t>(std::distance(sample_times_s.begin(), upper));
    const auto lower_index = upper_index - 1;
    const double span_s = std::max(1.0e-6, sample_times_s[upper_index] - sample_times_s[lower_index]);
    const double alpha = (time_s - sample_times_s[lower_index]) / span_s;
    return path[lower_index] * (1.0 - alpha) + path[upper_index] * alpha;
}

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += ch;
                break;
        }
    }
    return out;
}
}

Simulation::Simulation(domain::UniverseDefinition universe)
    : universe_(std::move(universe)),
      mechanics_(universe_),
      economy_(universe_) {
    kepler_planner_ = std::make_unique<trajectory::KeplerTrajectoryPlanner>(universe_, mechanics_);
    for (const auto& station : universe_.stations) {
        station_defs_by_id_[station.id] = &station;
        stations_.push_back({.station_id = station.id, .inventory = station.initial_inventory, .credits = station.initial_credits, .ledger = {}});
    }
    for (const auto& ship_class : universe_.ship_classes) {
        ship_classes_by_id_[ship_class.id] = &ship_class;
    }
    for (const auto& faction : universe_.factions) {
        faction_treasuries_[faction.id] = 0.0;
    }
    for (const auto& seed : universe_.ship_seeds) {
        ships_.push_back({
            .id = seed.id,
            .name = seed.name,
            .faction_id = seed.faction_id,
            .class_id = seed.class_id,
            .home_station_id = seed.home_station_id,
            .current_station_id = seed.start_station_id,
            .phase = domain::ShipMissionPhase::Idle,
            .propellant_kg = seed.initial_propellant_kg,
            .credits = seed.initial_credits,
            .active_mission = {},
            .provisions = {},  // filled below: ships leave their yard provisioned
            .ledger = {},
            .next_review_s = 0.0,
            .idle_since_s = 0.0,
            .refit_class_id = {},
            .refit_done_s = 0.0,
            .next_refit_review_s = 0.0,
            .laid_up_since_s = 0.0,
            .commissioned_s = 0.0,
            .route_destination_id = {},
            .route_commodity_id = {},
            .route_units_per_day = 0.0,
            .route_until_s = 0.0,
        });
    }
    seeded_money_supply_ = internal_money_supply();
    next_investment_review_s_ = universe_.fleet_investment.review_days * 86400.0;
    // Ships start with their full life-support endurance aboard, as if fresh from the yard.
    constexpr double INITIAL_PROVISION_DAYS = PROVISION_ENDURANCE_DAYS;
    for (auto& ship : ships_) {
        const auto& ship_class = get_ship_class(ship.class_id);
        for (const auto& [commodity_id, units_per_crew_day] : universe_.ship_operations.life_support_units_per_crew_day) {
            ship.provisions[commodity_id] = ship_class.crew_size * units_per_crew_day * INITIAL_PROVISION_DAYS;
        }
    }
}

Simulation Simulation::from_data_root(const std::string& data_root) {
    data_loader::DataLoader loader;
    auto universe = loader.load_universe(data_root);
    Simulation sim(std::move(universe));

    // Try to load the VariableISP atlas. It lives next to the data root.
    const std::filesystem::path data_path(data_root);
    const std::filesystem::path atlas_path =
        data_path.parent_path() / "tests" / "data" / "variable_isp" / "variable_isp_atlas.bin";
    if (std::filesystem::exists(atlas_path)) {
        try {
            sim.atlas_.load_binary(atlas_path.string());
            sim.atlas_loaded_ = true;
            sim.variable_isp_planner_ = std::make_unique<trajectory::VariableIspTrajectoryPlanner>(
                sim.universe_, sim.mechanics_, sim.atlas_);
        } catch (const std::exception& ex) {
            std::cerr << "[VariableISP] Atlas load failed: " << ex.what() << " — electric ships will be stranded.\n";
        }
    } else {
        std::cerr << "[VariableISP] Atlas not found at " << atlas_path.string()
                  << " — electric ships will be stranded.\n";
    }

    return sim;
}

void Simulation::set_timewarp(double timewarp_factor) {
    timewarp_factor_ = std::max(1.0, timewarp_factor);
}

double Simulation::timewarp_factor() const {
    return timewarp_factor_;
}

const domain::UniverseDefinition& Simulation::universe() const {
    return universe_;
}

const economy::EconomySystem& Simulation::economy_system() const {
    return economy_;
}

const domain::ShipClassDefinition& Simulation::get_ship_class(const std::string& class_id) const {
    const auto it = ship_classes_by_id_.find(class_id);
    if (it == ship_classes_by_id_.end()) {
        throw std::runtime_error("Unknown ship class: " + class_id);
    }
    return *it->second;
}

const domain::CelestialBodyDefinition& Simulation::get_body_definition(const std::string& body_id) const {
    const auto it = std::find_if(
        universe_.bodies.begin(),
        universe_.bodies.end(),
        [&](const domain::CelestialBodyDefinition& body) { return body.id == body_id; });
    if (it == universe_.bodies.end()) {
        throw std::runtime_error("Unknown body: " + body_id);
    }
    return *it;
}

const domain::StationDefinition& Simulation::get_station_definition(const std::string& station_id) const {
    const auto it = station_defs_by_id_.find(station_id);
    if (it == station_defs_by_id_.end()) {
        throw std::runtime_error("Unknown station: " + station_id);
    }
    return *it->second;
}

domain::StationState& Simulation::get_station_state(const std::string& station_id) {
    const auto it = std::find_if(
        stations_.begin(),
        stations_.end(),
        [&](const domain::StationState& station) { return station.station_id == station_id; });
    if (it == stations_.end()) {
        throw std::runtime_error("Unknown station state: " + station_id);
    }
    return *it;
}

const domain::StationState& Simulation::get_station_state(const std::string& station_id) const {
    const auto it = std::find_if(
        stations_.begin(),
        stations_.end(),
        [&](const domain::StationState& station) { return station.station_id == station_id; });
    if (it == stations_.end()) {
        throw std::runtime_error("Unknown station state: " + station_id);
    }
    return *it;
}

void Simulation::add_event(std::string text, std::string category) {
    recent_events_.push_back({.time_s = game_time_s_, .text = std::move(text), .category = std::move(category)});
    if (recent_events_.size() > MAX_EVENT_HISTORY) {
        recent_events_.erase(recent_events_.begin(), recent_events_.begin() + static_cast<long>(recent_events_.size() - MAX_EVENT_HISTORY));
    }
}

void Simulation::record_trade(domain::TradeEntry trade) {
    recent_trades_.push_back(std::move(trade));
    if (recent_trades_.size() > MAX_TRADE_HISTORY) {
        recent_trades_.erase(recent_trades_.begin(), recent_trades_.begin() + static_cast<long>(recent_trades_.size() - MAX_TRADE_HISTORY));
    }
}

void Simulation::record_trajectory_audit(
    const domain::ShipState& ship,
    const domain::StationDefinition& origin,
    const domain::StationDefinition& destination,
    const domain::TrajectoryPlan& plan,
    double planning_propellant_kg) {
    const double r_origin_m = mechanics_.get_heliocentric_radius(origin.parent_body_id, plan.departure_time_s);
    const double r_dest_m = mechanics_.get_heliocentric_radius(destination.parent_body_id, plan.arrival_time_s);
    auto metrics = trajectory::audit_trajectory(plan);
    const bool flagged = !metrics.flags.empty();
    trajectory_audit_records_.push_back({
        .planned_at_s = game_time_s_,
        .ship_name = ship.name,
        .class_id = ship.class_id,
        .origin_station_id = origin.id,
        .destination_station_id = destination.id,
        .trajectory_type = plan.trajectory_type,
        .planning_propellant_kg = planning_propellant_kg,
        .wait_time_s = plan.wait_time_s,
        .coast_time_s = plan.coast_time_s,
        .r_origin_m = r_origin_m,
        .r_dest_m = r_dest_m,
        .diagnostics = plan.diagnostics,
        .metrics = std::move(metrics),
        .sampled_path = flagged ? plan.sampled_path : std::vector<math::Vec3d>{},
        .sampled_times_s = flagged ? plan.sampled_times_s : std::vector<double>{},
    });
}

const domain::CommodityDefinition& Simulation::get_commodity(const std::string& commodity_id) const {
    const auto it = std::find_if(
        universe_.commodities.begin(),
        universe_.commodities.end(),
        [&](const domain::CommodityDefinition& commodity) { return commodity.id == commodity_id; });
    if (it == universe_.commodities.end()) {
        throw std::runtime_error("Unknown commodity: " + commodity_id);
    }
    return *it;
}

double Simulation::station_price(const domain::StationState& state, const std::string& commodity_id) const {
    const auto& definition = get_station_definition(state.station_id);
    const auto stock_it = state.inventory.find(commodity_id);
    const double stock = stock_it == state.inventory.end() ? 0.0 : stock_it->second;
    return economy_.get_price(definition, commodity_id, stock, get_commodity(commodity_id).base_price);
}

double Simulation::trade_value(
    const domain::StationState& state, const std::string& commodity_id, double units_into_station) const {
    const auto& definition = get_station_definition(state.station_id);
    const auto stock_it = state.inventory.find(commodity_id);
    const double stock = stock_it == state.inventory.end() ? 0.0 : stock_it->second;
    return economy_.get_trade_value(
        definition, commodity_id, stock, units_into_station, get_commodity(commodity_id).base_price);
}

double Simulation::sale_value_on_arrival(const domain::StationState& state, const std::string& commodity_id,
    double units, double days_ahead, const std::string& seller_ship_id) const {
    // Forecast the stock the sale lands on: what the station holds, plus cargo of the
    // same kind other ships are already bringing, plus its own net production meanwhile.
    // Without the inbound cargo, every ship sent to a starving port expects its scarcity
    // price and the late arrivals sell at a loss.
    const auto& definition = get_station_definition(state.station_id);
    const auto stock_it = state.inventory.find(commodity_id);
    double stock = stock_it == state.inventory.end() ? 0.0 : stock_it->second;
    for (const auto& other : ships_) {
        if (other.id != seller_ship_id
            && (other.phase == domain::ShipMissionPhase::InTransit
                || other.phase == domain::ShipMissionPhase::AwaitingDeparture)
            && other.active_mission.destination_station_id == state.station_id
            && other.active_mission.commodity_id == commodity_id) {
            stock += other.active_mission.cargo_units;
        }
    }
    const auto rates = economy_.get_station_net_rates(definition);
    const double rate = rates.contains(commodity_id) ? rates.at(commodity_id) : 0.0;
    stock = std::max(0.0, stock + rate * std::max(0.0, days_ahead));
    return economy_.get_trade_value(definition, commodity_id, stock, units, get_commodity(commodity_id).base_price);
}

double Simulation::daily_capital_cost(const domain::ShipClassDefinition& ship_class) const {
    const auto& operations = universe_.ship_operations;
    return ship_class.ship_value_cr
        * (operations.interest_rate_per_year + 1.0 / operations.lifetime_years) / 365.0;
}

double Simulation::daily_crew_cost(
    const domain::ShipClassDefinition& ship_class, const domain::StationState& market) const {
    const auto& operations = universe_.ship_operations;
    double cost = ship_class.crew_size * operations.wage_cr_per_crew_day;
    for (const auto& [commodity_id, units_per_crew_day] : operations.life_support_units_per_crew_day) {
        cost += ship_class.crew_size * units_per_crew_day * station_price(market, commodity_id);
    }
    return cost;
}

double Simulation::credit_line(const domain::ShipClassDefinition& ship_class) const {
    // Owners lend against the hull: a ship may finance cargo down to -50% of its value.
    return 0.5 * ship_class.ship_value_cr;
}

namespace {

// Units a station will sell for provisions: consumers keep 14 days of their own
// use, everyone else a small working stock.
double sellable_reserve_units(
    const economy::EconomySystem& economy,
    const domain::StationDefinition& station_def,
    const std::string& commodity_id,
    bool departing) {
    const auto rates = economy.get_station_net_rates(station_def);
    const double rate = rates.contains(commodity_id) ? rates.at(commodity_id) : 0.0;
    return departing ? 0.0 : (rate < 0.0 ? std::abs(rate) * 14.0 : 5.0);
}

double sellable_units(
    const economy::EconomySystem& economy,
    const domain::StationDefinition& station_def,
    const domain::StationState& station_state,
    const std::string& commodity_id,
    bool departing) {
    const auto it = station_state.inventory.find(commodity_id);
    const double stock = it == station_state.inventory.end() ? 0.0 : it->second;
    return std::max(0.0, stock - sellable_reserve_units(economy, station_def, commodity_id, departing));
}

// Fuel a station can sell to a ship arriving in `days`, counting the depot's refills
// meanwhile (but not other ships' purchases).
double fuel_for_sale_on_arrival_kg(
    const economy::EconomySystem& economy,
    const domain::StationDefinition& station_def,
    const domain::StationState& station_state,
    double days) {
    const auto it = station_state.inventory.find(economy::FUEL_ID);
    const double stock = it == station_state.inventory.end() ? 0.0 : it->second;
    const double reserve = sellable_reserve_units(economy, station_def, economy::FUEL_ID, false);
    return std::max(0.0, economy.fuel_stock_after_days(station_def, stock, days) - reserve) * FUEL_UNITS_TO_KG;
}

}  // namespace

double Simulation::provisionable_days(const domain::ShipState& ship, const domain::StationState& market) const {
    const auto& ship_class = get_ship_class(ship.class_id);
    const auto& market_def = get_station_definition(market.station_id);
    double days = std::numeric_limits<double>::infinity();
    for (const auto& [commodity_id, units_per_crew_day] : universe_.ship_operations.life_support_units_per_crew_day) {
        const double need_per_day = ship_class.crew_size * units_per_crew_day;
        if (need_per_day <= 0.0) {
            continue;
        }
        const auto carried_it = ship.provisions.find(commodity_id);
        const double carried = carried_it == ship.provisions.end() ? 0.0 : carried_it->second;
        days = std::min(days, (carried + sellable_units(economy_, market_def, market, commodity_id, true)) / need_per_day);
    }
    return days;
}

void Simulation::buy_provisions(domain::ShipState& ship, domain::StationState& market, double days, bool departing) {
    const auto& ship_class = get_ship_class(ship.class_id);
    const auto& market_def = get_station_definition(market.station_id);
    for (const auto& [commodity_id, units_per_crew_day] : universe_.ship_operations.life_support_units_per_crew_day) {
        const double missing = ship_class.crew_size * units_per_crew_day * days - ship.provisions[commodity_id];
        const double units = std::min(missing, sellable_units(economy_, market_def, market, commodity_id, departing));
        if (units <= 0.0) {
            continue;
        }
        // Price on the stock before the transfer, like every other trade.
        const double unit_price = station_price(market, commodity_id);
        const double bill = units * unit_price;
        market.inventory[commodity_id] -= units;
        market.credits += bill;
        ship.provisions[commodity_id] += units;
        ship.credits -= bill;
        ship.lifetime_profit -= bill;
        ship.ledger.provisions += bill;
        record_trade({
            .time_s = game_time_s_,
            .ship_id = ship.id,
            .station_id = market.station_id,
            .commodity_id = commodity_id,
            .kind = "provisions",
            .units = units,
            .unit_price = unit_price,
            .total = bill,
        });
    }
}

void Simulation::pay_home_station(domain::ShipState& ship, double amount) {
    ship.credits -= amount;
    ship.lifetime_profit -= amount;
    get_station_state(ship.home_station_id).credits += amount;
}

void Simulation::accrue_operating_costs(domain::ShipState& ship, double dt_s) {
    const auto& ship_class = get_ship_class(ship.class_id);
    const double days = dt_s / 86400.0;

    // Capital (interest + amortisation) is owed whatever the ship does.
    const double capital = daily_capital_cost(ship_class) * days;
    pay_home_station(ship, capital);
    ship.ledger.capital += capital;
    if (ship.phase == domain::ShipMissionPhase::LaidUp || ship.phase == domain::ShipMissionPhase::Refitting) {
        return;  // crew discharged: no wages, no life support
    }

    const double wages = ship_class.crew_size * universe_.ship_operations.wage_cr_per_crew_day * days;
    pay_home_station(ship, wages);
    ship.ledger.wages += wages;

    const bool docked = ship.phase == domain::ShipMissionPhase::Idle
        || ship.phase == domain::ShipMissionPhase::Stranded
        || ship.phase == domain::ShipMissionPhase::Refueling;
    const auto& life_support = universe_.ship_operations.life_support_units_per_crew_day;
    if (docked) {
        // In port the crew lives off the station: keep the ship's stock topped up from the
        // market (respecting the station's own needs), up to the full endurance.
        bool below_buffer = false;
        for (const auto& [commodity_id, units_per_crew_day] : life_support) {
            const double use = ship_class.crew_size * units_per_crew_day;
            below_buffer = below_buffer
                || ship.provisions[commodity_id] < use * (PROVISION_MIN_RETURN_DAYS + DOCKED_PROVISION_LOW_DAYS);
        }
        if (below_buffer) {
            buy_provisions(ship, get_station_state(ship.current_station_id),
                std::max(PROVISION_ENDURANCE_DAYS, PROVISION_MIN_RETURN_DAYS + DOCKED_PROVISION_TARGET_DAYS), false);
        }
    }
    bool reserve_breached = false;
    for (const auto& [commodity_id, units_per_crew_day] : life_support) {
        const double use = ship_class.crew_size * units_per_crew_day;
        auto& carried = ship.provisions[commodity_id];
        carried = std::max(0.0, carried - use * days);
        reserve_breached = reserve_breached || carried < use * PROVISION_MIN_RETURN_DAYS;
    }
    // A crew in a port that cannot feed it must not eat the ship's return reserve, or the
    // ship could never leave (starving consumer stations became traps). Lay it off instead.
    if (ship.phase == domain::ShipMissionPhase::Idle && reserve_breached) {
        ship.phase = domain::ShipMissionPhase::LaidUp;
        ship.laid_up_since_s = game_time_s_;
        ship.next_review_s = game_time_s_ + LAYUP_REVIEW_DAYS * 86400.0;
        add_event(std::format("{} laid up at {} (port cannot provision the crew)",
            ship.name, get_station_definition(ship.current_station_id).name), "alert");
    }
}

double Simulation::purchasable_propellant_kg(const domain::ShipState& ship) const {
    const auto& station = get_station_state(ship.current_station_id);
    const auto& ship_class = get_ship_class(ship.class_id);
    // Leave the station its own working reserve (it may consume fuel too) — except for a
    // nearly-dry ship, which may tap the reserve to get unstuck.
    const bool emergency = ship.propellant_kg <= ship_class.propellant_capacity_kg * 0.05;
    const auto station_rates = economy_.get_station_net_rates(get_station_definition(ship.current_station_id));
    const double station_fuel_rate = station_rates.contains("fuel") ? station_rates.at("fuel") : 0.0;
    const double station_reserve = (station_fuel_rate < 0.0 && !emergency)
        ? std::abs(station_fuel_rate) * 14.0 : 0.0;
    const auto it = station.inventory.find("fuel");
    const double stock = it == station.inventory.end() ? 0.0 : it->second;
    const double for_sale_kg = std::max(0.0, stock - station_reserve) * FUEL_UNITS_TO_KG;
    return std::min(for_sale_kg, std::max(0.0, ship_class.propellant_capacity_kg - ship.propellant_kg));
}

double Simulation::provisions_mass_kg(const domain::ShipState& ship) const {
    double mass_kg = 0.0;
    for (const auto& [commodity_id, units] : ship.provisions) {
        mass_kg += std::max(0.0, units) * get_commodity(commodity_id).mass_per_unit_kg;
    }
    return mass_kg;
}

void Simulation::buy_propellant(domain::ShipState& ship, double kg) {
    const double transferable_kg = std::min(std::max(0.0, kg), purchasable_propellant_kg(ship));
    if (transferable_kg <= 0.0) {
        return;
    }
    auto& station = get_station_state(ship.current_station_id);
    const double transferred_units = transferable_kg / FUEL_UNITS_TO_KG;
    // Price the fuel on the stock before the transfer. Ships may go into debt for fuel
    // (but not for cargo) so an empty wallet never permanently strands a ship.
    const double fuel_bill = trade_value(station, "fuel", -transferred_units);
    const double fuel_unit_price = fuel_bill / transferred_units;
    station.inventory["fuel"] -= transferred_units;
    station.ship_fuel_units_per_day += transferred_units / TRADE_FLOW_DAYS;
    ship.propellant_kg += transferable_kg;
    ship.credits -= fuel_bill;
    ship.lifetime_profit -= fuel_bill;
    ship.ledger.fuel += fuel_bill;
    station.credits += fuel_bill;
    record_trade({
        .time_s = game_time_s_,
        .ship_id = ship.id,
        .station_id = station.station_id,
        .commodity_id = "fuel",
        .kind = "fuel",
        .units = transferred_units,
        .unit_price = fuel_unit_price,
        .total = fuel_bill,
    });
    add_event(std::format(
        "{} refueled {:.0f} kg at {} for {:.0f} cr",
        ship.name, transferable_kg, get_station_definition(ship.current_station_id).name, fuel_bill),
        "fuel");
}

Simulation::LegEstimate Simulation::estimate_leg(
    const domain::ShipClassDefinition& ship_class,
    const domain::StationDefinition& origin,
    const domain::StationDefinition& destination,
    double departure_time_s,
    double available_propellant_kg,
    double payload_kg) {
    // Follow-up legs are only estimates, so departures share 5-day buckets, available
    // fuel 10%-of-tank buckets and payloads 2 t buckets (fuel at the bucket's lower edge,
    // payload at its upper edge: conservative). Ships of one class then reuse each
    // other's plans across a whole bucket. The ship is assumed to buy what it needs.
    constexpr double BUCKET_S = 5.0 * 86400.0;
    const auto bucket = static_cast<std::int64_t>(std::floor(departure_time_s / BUCKET_S));
    // Plan with this class's typical costs (provision prices vary a little by port).
    const double fuel_step_kg = std::max(1.0, ship_class.propellant_capacity_kg * 0.1);
    const auto fuel_bucket = static_cast<std::int64_t>(std::floor(available_propellant_kg / fuel_step_kg));
    constexpr double PAYLOAD_STEP_KG = 2000.0;
    const auto payload_bucket = static_cast<std::int64_t>(std::ceil(std::max(0.0, payload_kg) / PAYLOAD_STEP_KG));
    const trajectory::PlanningOptions options {
        .propellant_cr_per_kg = get_commodity("fuel").base_price / FUEL_UNITS_TO_KG,
        .time_cr_per_day = daily_capital_cost(ship_class) + daily_crew_cost(ship_class, get_station_state(origin.id)),
        .payload_kg = static_cast<double>(payload_bucket) * PAYLOAD_STEP_KG,
        .purchasable_propellant_kg = static_cast<double>(fuel_bucket) * fuel_step_kg,
        .reserve_fraction = PROPELLANT_RESERVE_FRACTION,
    };
    leg_estimates_.erase(leg_estimates_.begin(),
        leg_estimates_.lower_bound(static_cast<std::int64_t>(std::floor(game_time_s_ / BUCKET_S))));
    auto& slot = leg_estimates_[bucket];
    const auto key = std::format("{}|{}|{}|{}|{}", ship_class.id, origin.id, destination.id, fuel_bucket, payload_bucket);
    if (const auto it = slot.find(key); it != slot.end()) {
        return it->second;
    }
    domain::ShipState probe;
    probe.class_id = ship_class.id;
    probe.current_station_id = origin.id;
    probe.propellant_kg = 0.0;
    const auto& planner = (ship_class.propulsion_type == "variable_isp" && variable_isp_planner_)
        ? static_cast<trajectory::ITrajectoryPlanner&>(*variable_isp_planner_)
        : static_cast<trajectory::ITrajectoryPlanner&>(*kepler_planner_);
    LegEstimate estimate;
    if (options.purchasable_propellant_kg > 0.0) {
        const auto plan = planner.plan_transfer(origin, destination, probe, ship_class,
            std::max(game_time_s_, static_cast<double>(bucket) * BUCKET_S), options);
        // Measured from the actual departure, not the bucket edge.
        estimate = {
            .feasible = plan.feasible,
            .travel_days = std::max(0.0, plan.arrival_time_s - departure_time_s) / 86400.0,
            .propellant_kg = plan.propellant_required_kg,
        };
    }
    slot.emplace(key, estimate);
    return estimate;
}

Simulation::MissionChoice Simulation::choose_mission(
    const domain::ShipState& ship, const domain::ShipClassDefinition& ship_class, bool trace,
    double earliest_departure_s, bool cargo_only) {
    // Plans start at the earliest departure; forecasts of stocks count from now.
    const double delay_days = std::max(0.0, earliest_departure_s - game_time_s_) / 86400.0;
    const auto trace_line = [&](const std::string& text) {
        if (trace) {
            std::cerr << std::format("[trace day {:.1f}] {} ({}, {}) at {}: {}\n", game_time_s_ / 86400.0, ship.name,
                mission_phase_name(ship.phase), ship_class.id, ship.current_station_id, text);
        }
    };

    auto& origin_state = get_station_state(ship.current_station_id);
    const auto& origin_def = get_station_definition(ship.current_station_id);

    // Ships fuel per mission: the planner loads what the transfer burns plus the reserve,
    // from what is aboard and what this port can sell.
    const double origin_fuel_for_sale_kg = purchasable_propellant_kg(ship);
    const double provisions_kg = provisions_mass_kg(ship);

    double best_score = MIN_MISSION_SCORE_PER_DAY;
    const domain::StationDefinition* best_destination = nullptr;
    std::string best_commodity;
    double best_cargo_units = 0.0;
    double best_cargo_margin = 0.0;
    const domain::TrajectoryPlan* best_plan = nullptr;
    double best_carried_kg = 0.0;
    std::vector<MissionChoice::CargoOption> cargo_options;

    // A newly commissioned ship works the route it was bought for: from home only to its route
    // destination, from anywhere else only home. Otherwise it left once the gap it was bought
    // for closed, the gap reopened and the treasuries ordered another ship for it.
    const bool on_route = !cargo_only && !ship.route_destination_id.empty() && game_time_s_ < ship.route_until_s;
    const auto route_leg_allowed = [&](const std::string& from_id, const std::string& to_id) {
        return !on_route || to_id == (from_id == ship.home_station_id ? ship.route_destination_id : ship.home_station_id);
    };

    // Every day of a mission (waiting for the window included) costs capital,
    // wages and provisions; missions are compared on profit after that cost.
    const double time_cost_per_day = daily_capital_cost(ship_class) + daily_crew_cost(ship_class, origin_state);
    // The crew must be provisionable from here for the whole mission.
    // (The return-leg reserve is bought too, but only if the station has it.)
    const double max_mission_days = provisionable_days(ship, origin_state) / PROVISION_MARGIN;
    // Cargo may be financed down to the credit line.
    const double spendable_credits = std::max(0.0, ship.credits + credit_line(ship_class));

    // The plan depends on the destination and the cargo mass, so plan each (destination,
    // cargo tonne) once per scoring pass rather than once per commodity. Plasma plans cost
    // ~8 ms each.
    const auto& planner = (ship_class.propulsion_type == "variable_isp" && variable_isp_planner_)
        ? static_cast<trajectory::ITrajectoryPlanner&>(*variable_isp_planner_)
        : static_cast<trajectory::ITrajectoryPlanner&>(*kepler_planner_);
    // A ship must be able to leave its destination again: the fuel it has left on
    // arrival plus what the station can sell must cover a leg as long as this one,
    // flown without cargo. Without this, ships flew into fuel-dry consumers (Mercury)
    // and stayed there. For a nuclear-thermal ship the same Δv takes
    // m_dry * (mass_ratio - 1), with the plan's mass ratio; plasma ships are close enough
    // with the plan's own burn.
    // Propellant is priced at what this port charges (never below base), so where fuel is
    // scarce the planners fly slower, cheaper transfers.
    const double planning_fuel_price = std::max(get_commodity("fuel").base_price, station_price(origin_state, "fuel"));
    const double exhaust_velocity_mps = trajectory::thermal_exhaust_velocity_mps(ship_class);
    // Returns the propellant missing for that, with `carried_kg` of return fuel aboard on top
    // of the plan's own load.
    const auto leave_shortfall_kg = [&](const domain::StationDefinition& destination,
                                        const domain::TrajectoryPlan& plan,
                                        double payload_kg,
                                        double carried_kg) {
        const auto& dest_state = get_station_state(destination.id);
        const double after_arrival_kg = std::max(0.0, plan.propellant_load_kg - plan.propellant_required_kg) + carried_kg;
        // Economic tankering: where the destination sells fuel at more than twice this port's
        // price, the ship carries its return fuel rather than buying it there.
        const double dest_fuel_kg = station_price(dest_state, "fuel") > TANKERING_PRICE_RATIO * planning_fuel_price
            ? 0.0
            : fuel_for_sale_on_arrival_kg(economy_, destination, dest_state,
                  delay_days + plan.wait_time_s / 86400.0 + plan.travel_time_s / 86400.0);
        double needed_kg = plan.propellant_required_kg;
        if (ship_class.propulsion_type != "variable_isp" && exhaust_velocity_mps > 0.0) {
            const double planned_mass_kg = ship_class.dry_mass_kg + payload_kg + plan.propellant_load_kg;
            const double mass_ratio = planned_mass_kg
                / std::max(1.0, planned_mass_kg - plan.propellant_required_kg);
            needed_kg = (ship_class.dry_mass_kg + provisions_kg) * (mass_ratio - 1.0);
        }
        return std::max(0.0, needed_kg * (1.0 + PROPELLANT_RESERVE_FRACTION) - after_arrival_kg - dest_fuel_kg);
    };
    // Planners trade travel time against propellant at the owner's prices, and load
    // propellant for the transfer plus the reserve.
    const auto planning_options = [&](double cargo_kg) {
        return trajectory::PlanningOptions {
            .propellant_cr_per_kg = planning_fuel_price / FUEL_UNITS_TO_KG,
            .time_cr_per_day = time_cost_per_day,
            .payload_kg = provisions_kg + cargo_kg,
            .purchasable_propellant_kg = origin_fuel_for_sale_kg,
            .reserve_fraction = PROPELLANT_RESERVE_FRACTION,
        };
    };
    std::unordered_map<std::string, domain::TrajectoryPlan> plans;
    const auto plan_to = [&](const domain::StationDefinition& destination, double cargo_kg) -> const domain::TrajectoryPlan& {
        // Cargo rounded up to whole tonnes keeps the cache small and the estimate safe.
        const double cargo_bucket_kg = std::ceil(std::max(0.0, cargo_kg) / 1000.0) * 1000.0;
        const auto key = std::format("{}|{:.0f}", destination.id, cargo_bucket_kg);
        auto it = plans.find(key);
        if (it == plans.end()) {
            it = plans.emplace(key, planner.plan_transfer(
                origin_def, destination, ship, ship_class, std::max(game_time_s_, earliest_departure_s),
                planning_options(cargo_bucket_kg))).first;
        }
        return it->second;
    };
    // Fuel-aware planning: where the destination's depot cannot refuel the ship for the next
    // leg, the ship carries the return fuel from here. That fuel rides as payload, so the leg
    // burns more; it must fit in the tanks and in what this port can sell. Depots without a
    // fuel factory only hold what tankers bring, so this is what keeps ships flying to them.
    struct FuelAwarePlan {
        const domain::TrajectoryPlan* plan {nullptr};
        double carried_kg {0.0};  // return fuel loaded on top of the plan's own load
        bool feasible {false};
    };
    const double fuel_limit_kg = std::min(ship_class.propellant_capacity_kg,
        std::max(0.0, ship.propellant_kg) + origin_fuel_for_sale_kg);
    const auto plan_with_return_fuel = [&](const domain::StationDefinition& destination, double cargo_kg) {
        const auto& plan = plan_to(destination, cargo_kg);
        if (!plan.feasible) {
            return FuelAwarePlan {&plan, 0.0, false};
        }
        double carried_kg = leave_shortfall_kg(destination, plan, cargo_kg, 0.0);
        if (carried_kg <= 0.0) {
            return FuelAwarePlan {&plan, 0.0, true};
        }
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto& laden = plan_to(destination, cargo_kg + carried_kg);
            if (!laden.feasible || laden.propellant_load_kg + carried_kg > fuel_limit_kg) {
                break;
            }
            const double missing_kg = leave_shortfall_kg(destination, laden, cargo_kg + carried_kg, carried_kg);
            if (missing_kg <= 0.0) {
                return FuelAwarePlan {&laden, carried_kg, true};
            }
            carried_kg += missing_kg;
        }
        return FuelAwarePlan {&plan, 0.0, false};
    };

    // Fuel burned on a mission comes out of the tank, already bought and not resalable,
    // so it is valued at the neutral base price, not the origin's scarcity price: a ship
    // holding 100 t of fuel at a fuel-starved port (16x) would otherwise never leave.
    const double origin_fuel_price = get_commodity("fuel").base_price;
    // But fuel bought here for a mission is paid at this port's price: since only fuel
    // factories make fuel, a drained depot sells at up to 16x base, and that premium is a
    // real cost of leaving from here (the tanks' contents are not).
    const double origin_fuel_premium = std::max(0.0, station_price(origin_state, "fuel") - origin_fuel_price);
    const auto fuel_premium = [&](const domain::TrajectoryPlan& plan, double carried_kg) {
        const double bought_kg = std::max(0.0, plan.propellant_load_kg + carried_kg - std::max(0.0, ship.propellant_kg));
        return bought_kg / FUEL_UNITS_TO_KG * origin_fuel_premium;
    };

    // Two-leg lookahead. A trip is only worth what the ship can do after it: a cargo run
    // into a port with nothing to carry away leaves the ship (and its costs) sitting
    // there. Every candidate leg is scored together with the best follow-up leg from its
    // destination, estimated from today's stocks and prices, or else with flying back
    // empty. Follow-ups are not urgency-weighted; they are only a forecast.
    struct FollowUp {
        double profit {0.0};
        double days {0.0};
        bool carries_cargo {false};
        std::string label;
    };
    std::unordered_map<std::string, std::vector<FollowUp>> follow_ups_by_destination;
    const auto follow_ups_from = [&](const domain::StationDefinition& destination,
                                     const domain::TrajectoryPlan& plan,
                                     double carried_kg) -> const std::vector<FollowUp>& {
        if (const auto it = follow_ups_by_destination.find(destination.id); it != follow_ups_by_destination.end()) {
            return it->second;
        }
        std::vector<FollowUp> options;
        const auto& dest_state = get_station_state(destination.id);
        const double arrival_kg = std::max(0.0, plan.propellant_load_kg - plan.propellant_required_kg) + carried_kg;
        const double follow_days = delay_days + (plan.wait_time_s + plan.travel_time_s) / 86400.0;
        const double departure_kg = std::min(ship_class.propellant_capacity_kg,
            arrival_kg + fuel_for_sale_on_arrival_kg(
                economy_, destination, dest_state, delay_days + plan.wait_time_s / 86400.0 + plan.travel_time_s / 86400.0));
        // Surplus left after the ships already docked there or inbound take their loads.
        // Each of them is assumed to fill its hold from the largest remaining surplus.
        const auto dest_rates = economy_.get_station_net_rates(destination);
        std::unordered_map<std::string, double> surplus_by_commodity;
        for (const auto& [commodity_id, stock] : dest_state.inventory) {
            const double rate = dest_rates.contains(commodity_id) ? dest_rates.at(commodity_id) : 0.0;
            if (rate > 0.0) {
                surplus_by_commodity[commodity_id] = stock - (8.0 + rate * 7.0);
            }
        }
        for (const auto& other : ships_) {
            if (other.id == ship.id) {
                continue;
            }
            const bool inbound = (other.phase == domain::ShipMissionPhase::InTransit
                || other.phase == domain::ShipMissionPhase::AwaitingDeparture)
                && other.active_mission.destination_station_id == destination.id;
            const bool docked = other.current_station_id == destination.id
                && (other.phase == domain::ShipMissionPhase::Idle || other.phase == domain::ShipMissionPhase::Refueling);
            if ((inbound || docked) && !surplus_by_commodity.empty()) {
                auto largest = std::max_element(surplus_by_commodity.begin(), surplus_by_commodity.end(),
                    [](const auto& a, const auto& b) { return a.second < b.second; });
                largest->second -= std::max(0.0, std::min(largest->second, get_ship_class(other.class_id).cargo_capacity_units));
            }
        }
        for (const auto& [commodity_id, surplus] : surplus_by_commodity) {
            const double full_units = std::min(surplus, ship_class.cargo_capacity_units);
            if (full_units <= 1.0) {
                continue;
            }
            const double decay_per_day = get_commodity(commodity_id).decay_fraction_per_day;
            for (const auto& next : universe_.stations) {
                if (next.id == destination.id || !route_leg_allowed(destination.id, next.id)) {
                    continue;
                }
                const auto next_rates = economy_.get_station_net_rates(next);
                const bool consumes = next_rates.contains(commodity_id) && next_rates.at(commodity_id) < 0.0;
                const auto& next_state = get_station_state(next.id);
                if (!consumes && station_price(next_state, commodity_id) <= get_commodity(commodity_id).base_price) {
                    continue;
                }
                // Best part load: a full hold may be too heavy, and big lots move prices.
                double units = 0.0;
                double best_profit = -std::numeric_limits<double>::infinity();
                LegEstimate leg;
                for (const double fraction : LOAD_FRACTIONS) {
                    const double lot = full_units * fraction;
                    if (lot <= 1.0) {
                        break;
                    }
                    const auto estimate = estimate_leg(ship_class, destination, next, plan.arrival_time_s, departure_kg,
                        provisions_kg + lot * get_commodity(commodity_id).mass_per_unit_kg);
                    if (!estimate.feasible) {
                        continue;
                    }
                    const double lot_profit = sale_value_on_arrival(next_state, commodity_id,
                            lot * std::pow(1.0 - decay_per_day, estimate.travel_days), follow_days + estimate.travel_days, ship.id)
                        - trade_value(dest_state, commodity_id, -lot)
                        - (estimate.propellant_kg / FUEL_UNITS_TO_KG) * origin_fuel_price
                        - time_cost_per_day * estimate.travel_days;
                    if (lot_profit > best_profit) {
                        best_profit = lot_profit;
                        units = lot;
                        leg = estimate;
                    }
                }
                if (!leg.feasible) {
                    continue;
                }
                const double revenue = sale_value_on_arrival(next_state, commodity_id,
                    units * std::pow(1.0 - decay_per_day, leg.travel_days), follow_days + leg.travel_days, ship.id);
                const double cost = trade_value(dest_state, commodity_id, -units) + (leg.propellant_kg / FUEL_UNITS_TO_KG) * origin_fuel_price
                    + time_cost_per_day * leg.travel_days;
                options.push_back({
                    .profit = revenue - cost,
                    .days = leg.travel_days,
                    .carries_cargo = true,
                    .label = std::format("then {:.0f}u {} -> {}", units, commodity_id, next.id),
                });
            }
        }
        // Dead end: fly back empty, or, if even that is impossible, sit out the lay-up delay.
        const auto back = estimate_leg(ship_class, destination, origin_def, plan.arrival_time_s, departure_kg, provisions_kg);
        if (back.feasible) {
            options.push_back({
                .profit = -((back.propellant_kg / FUEL_UNITS_TO_KG) * origin_fuel_price
                    + time_cost_per_day * back.travel_days),
                .days = back.travel_days,
                .label = "then back empty",
            });
        } else {
            options.push_back({.profit = -time_cost_per_day * IDLE_LAYUP_DAYS, .days = IDLE_LAYUP_DAYS, .label = "then idle"});
        }
        return follow_ups_by_destination.emplace(destination.id, std::move(options)).first->second;
    };
    // Profit per day over both legs, with the best follow-up for this leg.
    const auto two_leg_score = [&](const domain::StationDefinition& destination,
                                   const domain::TrajectoryPlan& plan,
                                   double carried_kg,
                                   double leg_profit,
                                   double leg_days,
                                   bool cargo_follow_up_only,
                                   std::string& follow_label) {
        double best = -std::numeric_limits<double>::infinity();
        for (const auto& option : follow_ups_from(destination, plan, carried_kg)) {
            if (cargo_follow_up_only && !option.carries_cargo) {
                continue;
            }
            const double score = (leg_profit + option.profit) / std::max(1.0, leg_days + option.days);
            if (score > best) {
                best = score;
                follow_label = option.label;
            }
        }
        return best;
    };

    const auto origin_rates = economy_.get_station_net_rates(origin_def);
    for (const auto& [commodity_id, stock] : origin_state.inventory) {
        const double origin_rate = origin_rates.contains(commodity_id) ? origin_rates.at(commodity_id) : 0.0;
        if (origin_rate <= 0.0) {
            continue;
        }
        const double origin_reserve = 8.0 + origin_rate * 7.0;
        const double surplus = std::max(0.0, stock - origin_reserve);
        if (surplus <= 1.0) {
            continue;
        }

        const double origin_price = station_price(origin_state, commodity_id);
        const double decay_per_day = get_commodity(commodity_id).decay_fraction_per_day;

        for (const auto& destination : universe_.stations) {
            if (destination.id == origin_def.id || !route_leg_allowed(origin_def.id, destination.id)) {
                continue;
            }
            const auto destination_rates = economy_.get_station_net_rates(destination);
            const double destination_rate = destination_rates.contains(commodity_id) ? destination_rates.at(commodity_id) : 0.0;
            const auto& destination_state = get_station_state(destination.id);
            // Follow the price signal, not just the recipe: a station short of something it
            // does not consume itself (water drunk by visiting crews) still bids it up.
            if (destination_rate >= 0.0
                && station_price(destination_state, commodity_id) <= get_commodity(commodity_id).base_price) {
                continue;
            }

            // Cap cargo by hold size, available surplus, what the ship can afford,
            // and the destination's free storage.
            double free_capacity = ship_class.cargo_capacity_units;
            if (destination.storage_capacity_units > 0.0) {
                free_capacity = std::max(0.0,
                    destination.storage_capacity_units - economy_.storage_used_units(destination_state.inventory));
            }
            const double affordable = origin_price > 0.0 ? spendable_credits / origin_price : surplus;
            double full_units = std::min({ship_class.cargo_capacity_units, surplus, affordable, free_capacity});
            // Buying moves the price up the curve; shrink the lot until it fits the credit.
            for (int i = 0; i < 3 && full_units > 1.0; ++i) {
                const double lot_cost = trade_value(origin_state, commodity_id, -full_units);
                if (lot_cost <= spendable_credits) {
                    break;
                }
                full_units *= 0.98 * spendable_credits / lot_cost;
            }
            if (full_units <= 1.0) {
                continue;
            }

            // Lot size: a full hold may be too heavy to fly or to leave again, and a big lot
            // moves both stations' prices along their curves, so score each part load.
            const double dest_stock = destination_state.inventory.count(commodity_id)
                ? destination_state.inventory.at(commodity_id) : 0.0;
            const double dest_days_left = destination_rate < 0.0
                ? dest_stock / std::abs(destination_rate)
                : std::numeric_limits<double>::infinity();
            // The price clamp saturates, so a starving station cannot bid any higher;
            // weight the score by how few days of stock the destination has left.
            const double urgency = std::clamp(14.0 / std::max(dest_days_left, 0.5), 1.0, 5.0);
            for (const double fraction : LOAD_FRACTIONS) {
                const double cargo_units = full_units * fraction;
                if (cargo_units <= 1.0) {
                    break;
                }
                const double cargo_kg = cargo_units * get_commodity(commodity_id).mass_per_unit_kg;
                const auto cargo_label = std::format("cargo {:.0f}u {} -> {}", cargo_units, commodity_id, destination.id);
                // Tankers too: a tanker that bought its return fuel from the cargo it just
                // delivered would drain the depot it supplies.
                const auto fuel_plan = plan_with_return_fuel(destination, cargo_kg);
                const auto& plan = *fuel_plan.plan;
                if (!plan.feasible) {
                    trace_line(cargo_label + ": no feasible trajectory (" + plan.summary + ")");
                    continue;
                }
                if (!fuel_plan.feasible) {
                    trace_line(cargo_label + ": could not refuel to leave again, nor carry the fuel");
                    continue;
                }
                const double travel_days = plan.travel_time_s / 86400.0;
                if (travel_days > max_mission_days) {
                    trace_line(std::format("{}: {:.0f} days exceeds provisionable {:.0f}", cargo_label, travel_days, max_mission_days));
                    continue;
                }
                const double surviving = cargo_units * std::pow(1.0 - decay_per_day, travel_days);
                const double revenue = sale_value_on_arrival(destination_state, commodity_id, surviving,
                    delay_days + plan.wait_time_s / 86400.0 + travel_days, ship.id);
                const double fuel_cost = (plan.propellant_required_kg / FUEL_UNITS_TO_KG) * origin_fuel_price
                    + fuel_premium(plan, fuel_plan.carried_kg);
                const double cost = trade_value(origin_state, commodity_id, -cargo_units) + fuel_cost
                    + time_cost_per_day * travel_days;
                if (cargo_only) {
                    cargo_options.push_back({.destination = &destination, .commodity_id = commodity_id,
                        .cargo_units = cargo_units, .travel_days = travel_days,
                        .wait_days = plan.wait_time_s / 86400.0, .fuel_cost = fuel_cost});
                }
                std::string follow_label;
                // Cargo-only probes (fleet investment) value the run on its own and skip the
                // follow-up forecast, the expensive part of a dispatch pass.
                const double score = cargo_only
                    ? urgency * (revenue - cost) / std::max(1.0, travel_days)
                    : two_leg_score(destination, plan, fuel_plan.carried_kg, urgency * (revenue - cost), travel_days, false,
                          follow_label);
                trace_line(std::format("{}: {:.0f} days revenue {:.0f} cost {:.0f} (time {:.0f}), {}: score {:.1f}{}",
                    cargo_label, travel_days, revenue, cost, time_cost_per_day * travel_days, follow_label, score,
                    score > best_score ? " (best so far)" : ""));
                if (score > best_score) {
                    best_score = score;
                    best_destination = &destination;
                    best_commodity = commodity_id;
                    best_cargo_units = cargo_units;
                    best_cargo_margin = revenue - trade_value(origin_state, commodity_id, -cargo_units) - fuel_cost;
                    best_plan = &plan;
                    best_carried_kg = fuel_plan.carried_kg;
                }
            }
        }
    }

    // Empty legs toward a better pickup compete with cargo runs on the same two-leg terms.
    for (const auto& destination : universe_.stations) {
        if (cargo_only || destination.id == origin_def.id || !route_leg_allowed(origin_def.id, destination.id)) {
            continue;
        }
        const auto fuel_plan = plan_with_return_fuel(destination, 0.0);
        if (!fuel_plan.feasible) {
            continue;
        }
        const auto& plan = *fuel_plan.plan;
        const double leg_days = plan.travel_time_s / 86400.0;
        if (leg_days > max_mission_days) {
            continue;
        }
        const double leg_cost = (plan.propellant_required_kg / FUEL_UNITS_TO_KG) * origin_fuel_price
            + fuel_premium(plan, fuel_plan.carried_kg)
            + time_cost_per_day * leg_days;
        std::string follow_label;
        const double score = two_leg_score(destination, plan, fuel_plan.carried_kg, -leg_cost, leg_days, true, follow_label);
        if (follow_label.empty()) {
            continue;
        }
        trace_line(std::format("empty -> {}: {:.0f} days cost {:.0f}, {}: score {:.1f}{}",
            destination.id, leg_days, leg_cost, follow_label, score, score > best_score ? " (best so far)" : ""));
        if (score > best_score) {
            best_score = score;
            best_destination = &destination;
            best_commodity.clear();
            best_cargo_units = 0.0;
            best_cargo_margin = 0.0;
            best_plan = &plan;
            best_carried_kg = fuel_plan.carried_kg;
        }
    }

    // A ship on its route away from home with nothing to carry back flies home empty.
    if (best_destination == nullptr && on_route && origin_def.id != ship.home_station_id) {
        const auto& home = get_station_definition(ship.home_station_id);
        const auto& plan = plan_to(home, 0.0);
        if (plan.feasible && plan.travel_time_s / 86400.0 <= max_mission_days) {
            trace_line("on route: back home empty to " + home.id);
            best_score = 0.0;
            best_destination = &home;
            best_plan = &plan;
            best_carried_kg = 0.0;
        }
    }

    bool repositioning = false;
    if (best_destination == nullptr && !cargo_only && !on_route) {
        repositioning = true;
        best_score = 0.0;  // repositioning scores are in urgency units, not credits/day

        // Sourcing score: does this station have surplus goods urgently needed
        // elsewhere? An empty ship is only useful where there is something to
        // pick up, so repositioning targets producers; starving stations are
        // served by the cargo loop's urgency weighting once a ship is loaded.
        const auto sourcing_score_for = [&](const domain::StationDefinition& station_def,
                                            const domain::StationState& station_state) {
            const auto rates = economy_.get_station_net_rates(station_def);
            double sourcing_score = 0.0;
            for (const auto& [commodity_id, rate] : rates) {
                if (rate <= 0.0) continue;
                const double stock = station_state.inventory.count(commodity_id)
                    ? station_state.inventory.at(commodity_id) : 0.0;
                const double reserve = 8.0 + rate * 7.0;
                const double surplus = std::max(0.0, stock - reserve);
                if (surplus <= 1.0) continue;
                // Sum up how urgently this commodity is needed across the whole system.
                double system_urgency = 0.0;
                for (const auto& other : universe_.stations) {
                    if (other.id == station_def.id) continue;
                    const auto other_rates = economy_.get_station_net_rates(other);
                    const double other_rate = other_rates.count(commodity_id) ? other_rates.at(commodity_id) : 0.0;
                    if (other_rate >= 0.0) continue;
                    const auto& other_state = get_station_state(other.id);
                    const double other_stock = other_state.inventory.count(commodity_id)
                        ? other_state.inventory.at(commodity_id) : 0.0;
                    const double days_rem = other_stock > 0.0 ? other_stock / std::abs(other_rate) : 0.0;
                    system_urgency += std::max(1.0, 30.0 / std::max(1.0, days_rem)) * std::abs(other_rate);
                }
                sourcing_score += std::min(surplus, ship_class.cargo_capacity_units) * system_urgency / 100.0;
            }
            return sourcing_score;
        };
        const double origin_sourcing_score = sourcing_score_for(origin_def, origin_state);

        // What one full load picked up at `station_def` could earn, in credits, at
        // today's prices (the best spread to any consumer). A repositioning trip
        // must at least pay for itself out of that.
        const auto sourcing_value_cr = [&](const domain::StationDefinition& station_def,
                                           const domain::StationState& station_state) {
            const auto rates = economy_.get_station_net_rates(station_def);
            double best_value = 0.0;
            for (const auto& [commodity_id, rate] : rates) {
                if (rate <= 0.0) continue;
                const double stock = station_state.inventory.count(commodity_id)
                    ? station_state.inventory.at(commodity_id) : 0.0;
                const double surplus = std::max(0.0, stock - (8.0 + rate * 7.0));
                const double load = std::min(surplus, ship_class.cargo_capacity_units);
                if (load <= 1.0) continue;
                const double buy_cost = trade_value(station_state, commodity_id, -load);
                for (const auto& other : universe_.stations) {
                    if (other.id == station_def.id) continue;
                    const auto other_rates = economy_.get_station_net_rates(other);
                    const bool consumes = other_rates.contains(commodity_id) && other_rates.at(commodity_id) < 0.0;
                    const auto& other_state = get_station_state(other.id);
                    if (!consumes && station_price(other_state, commodity_id) <= get_commodity(commodity_id).base_price) continue;
                    best_value = std::max(best_value, trade_value(other_state, commodity_id, load) - buy_cost);
                }
            }
            return best_value;
        };

        for (const auto& destination : universe_.stations) {
            if (destination.id == origin_def.id) {
                continue;
            }
            const auto& dest_state = get_station_state(destination.id);

            const double destination_score = sourcing_score_for(destination, dest_state);
            // Only burn propellant relocating if the destination is a clearly
            // better pickup spot than where the ship already sits — otherwise
            // the fleet thrashes between comparable producers, hauling nothing.
            if (destination_score <= 0.0 || destination_score < origin_sourcing_score * 1.5) {
                trace_line(std::format("reposition -> {}: sourcing score {:.1f} vs here {:.1f}",
                    destination.id, destination_score, origin_sourcing_score));
                continue;
            }
            const auto fuel_plan = plan_with_return_fuel(destination, 0.0);
            const auto& plan = *fuel_plan.plan;
            if (!plan.feasible) {
                trace_line("reposition -> " + destination.id + ": no feasible trajectory (" + plan.summary + ")");
                continue;
            }
            const double reposition_days = plan.travel_time_s / 86400.0;
            if (reposition_days > max_mission_days) {
                trace_line(std::format("reposition -> {}: {:.0f} days exceeds provisionable {:.0f}",
                    destination.id, reposition_days, max_mission_days));
                continue;
            }
            if (!fuel_plan.feasible) {
                trace_line(std::format("reposition -> {}: could not refuel to leave again nor carry the fuel (burns {:.0f} of {:.0f} kg, port sells {:.0f} kg)",
                    destination.id, plan.propellant_required_kg, plan.propellant_load_kg,
                    fuel_for_sale_on_arrival_kg(economy_, destination, get_station_state(destination.id),
                        delay_days + plan.wait_time_s / 86400.0 + reposition_days)));
                continue;
            }
            const double reposition_cost = (plan.propellant_required_kg / FUEL_UNITS_TO_KG) * origin_fuel_price
                + fuel_premium(plan, fuel_plan.carried_kg)
                + time_cost_per_day * reposition_days;
            if (sourcing_value_cr(destination, dest_state) <= reposition_cost) {
                trace_line(std::format("reposition -> {}: load worth {:.0f} does not cover trip cost {:.0f}",
                    destination.id, sourcing_value_cr(destination, dest_state), reposition_cost));
                continue;
            }
            // Crowd penalty: don't send the whole fleet to the same "best" station.
            int congestion = 0;
            for (const auto& other : ships_) {
                if (other.id == ship.id) {
                    continue;
                }
                const bool docked_there = other.current_station_id == destination.id
                    && other.phase != domain::ShipMissionPhase::InTransit;
                const bool inbound = (other.phase == domain::ShipMissionPhase::InTransit
                    || other.phase == domain::ShipMissionPhase::AwaitingDeparture)
                    && other.active_mission.destination_station_id == destination.id;
                if (docked_there || inbound) {
                    ++congestion;
                }
            }
            const double score = destination_score
                / std::max(1.0, plan.travel_time_s / 86400.0)
                / (1.0 + static_cast<double>(congestion));
            if (score > best_score) {
                best_score = score;
                best_destination = &destination;
                best_commodity.clear();
                best_cargo_units = 0.0;
                best_cargo_margin = 0.0;
                best_plan = &plan;
                best_carried_kg = fuel_plan.carried_kg;
            }
        }
    }

    MissionChoice choice;
    choice.cargo_options = std::move(cargo_options);
    if (best_destination == nullptr) {
        return choice;
    }
    choice.kind = repositioning ? MissionChoice::Kind::Reposition : MissionChoice::Kind::Mission;
    choice.score = best_score;
    choice.destination = best_destination;
    choice.commodity_id = best_commodity;
    choice.cargo_units = best_cargo_units;
    choice.cargo_margin = best_cargo_margin;
    choice.plan = *best_plan;
    choice.carried_propellant_kg = best_carried_kg;
    return choice;
}

void Simulation::step_idle_ship(domain::ShipState& ship) {
    // Planning is expensive, so docked ships look for work every few hours, and laid-up
    // ships (no crew) every few days, not on every tick.
    const bool laid_up = ship.phase == domain::ShipMissionPhase::LaidUp;
    if (game_time_s_ < ship.next_review_s) {
        return;
    }
    ship.next_review_s = game_time_s_ + (laid_up ? LAYUP_REVIEW_DAYS * 86400.0 : IDLE_REVIEW_S);

    // Debug aid: SPACETRAINS_TRACE_SHIP="<ship name>" logs every candidate mission of
    // that ship and why it was rejected (stderr).
    static const char* const trace_ship = std::getenv("SPACETRAINS_TRACE_SHIP");
    const bool trace = trace_ship != nullptr && ship.name == trace_ship;

    const auto& ship_class = get_ship_class(ship.class_id);
    const auto choice = choose_mission(ship, ship_class, trace, game_time_s_);
    if (consider_refit(ship, choice, trace)) {
        return;
    }

    auto& origin_state = get_station_state(ship.current_station_id);
    const auto& origin_def = get_station_definition(ship.current_station_id);
    if (choice.kind == MissionChoice::Kind::None) {
        if (ship.propellant_kg <= ship_class.propellant_capacity_kg * 0.01 && purchasable_propellant_kg(ship) <= 0.0) {
            ship.phase = domain::ShipMissionPhase::Stranded;
            add_event(std::format("{} is stranded at {} due to fuel shortage", ship.name, origin_def.name), "alert");
        } else if (!laid_up
            && (ship.credits < 0.0 || game_time_s_ - ship.idle_since_s > IDLE_LAYUP_DAYS * 86400.0)) {
            // Nothing worth flying and either in debt or idle for a month: stop paying the crew.
            ship.phase = domain::ShipMissionPhase::LaidUp;
            ship.laid_up_since_s = game_time_s_;
            ship.next_review_s = game_time_s_ + LAYUP_REVIEW_DAYS * 86400.0;
            add_event(ship.credits < 0.0
                ? std::format("{} laid up at {} ({:.0f} cr in debt)", ship.name, origin_def.name, -ship.credits)
                : std::format("{} laid up at {} (no profitable work for {:.0f} days)",
                      ship.name, origin_def.name, IDLE_LAYUP_DAYS),
                "alert");
        }
        return;
    }

    const auto* best_destination = choice.destination;
    const auto& best_commodity = choice.commodity_id;
    const double best_cargo_units = choice.cargo_units;
    const auto& plan = choice.plan;
    if (!plan.feasible) {
        return;
    }
    const double time_cost_per_day = daily_capital_cost(ship_class) + daily_crew_cost(ship_class, origin_state);
    if (trajectory_audit_enabled_) {
        record_trajectory_audit(ship, origin_def, *best_destination, plan, plan.propellant_load_kg);
    }
    // Load the mission's propellant (the plan's burn plus the reserve, and any return fuel).
    buy_propellant(ship, plan.propellant_load_kg + choice.carried_propellant_kg - ship.propellant_kg);

    if (laid_up) {
        add_event(std::format("{} rehired its crew at {}", ship.name, origin_def.name), "mission");
    }
    const double mission_days = plan.travel_time_s / 86400.0;
    // What the mission needs may come out of the whole stock; the rest of the endurance
    // only out of what the station can spare.
    buy_provisions(ship, origin_state,
        mission_days * PROVISION_MARGIN + std::max(PROVISION_MIN_RETURN_DAYS, mission_days), true);
    buy_provisions(ship, origin_state, PROVISION_ENDURANCE_DAYS, false);

    double purchase_cost = 0.0;
    double expected_revenue = 0.0;
    if (best_cargo_units > 0.0 && !best_commodity.empty()) {
        // Buy at origin along the price curve as the stock falls.
        purchase_cost = trade_value(origin_state, best_commodity, -best_cargo_units);
        const double unit_price = purchase_cost / best_cargo_units;
        origin_state.inventory[best_commodity] -= best_cargo_units;
        origin_state.export_units_per_day[best_commodity] += best_cargo_units / TRADE_FLOW_DAYS;
        ship.credits -= purchase_cost;
        ship.lifetime_profit -= purchase_cost;
        ship.ledger.cargo_purchases += purchase_cost;
        origin_state.credits += purchase_cost;
        record_trade({
            .time_s = game_time_s_,
            .ship_id = ship.id,
            .station_id = origin_state.station_id,
            .commodity_id = best_commodity,
            .kind = "buy",
            .units = best_cargo_units,
            .unit_price = unit_price,
            .total = purchase_cost,
        });
        const auto& destination_state = get_station_state(best_destination->id);
        expected_revenue = trade_value(destination_state, best_commodity, best_cargo_units);
    }
    // Nuclear-thermal ships: deduct propellant at mission start (instantaneous burns).
    // Variable-Isp ships: propellant is consumed continuously during transit and
    // tracked per-sample, so we don't deduct here — ship.propellant_kg is updated
    // in step_in_transit_ship from sampled_propellant_kg.
    if (ship_class.propulsion_type != "variable_isp") {
        ship.propellant_kg -= plan.propellant_required_kg;
    }
    auto sampled_propellant = plan.sampled_propellant_kg;
    ship.phase = plan.wait_time_s > 0.0 ? domain::ShipMissionPhase::AwaitingDeparture : domain::ShipMissionPhase::InTransit;
    ship.active_mission = {
        .origin_station_id = origin_def.id,
        .destination_station_id = best_destination->id,
        .commodity_id = best_commodity,
        .cargo_units = best_cargo_units,
        .departure_time_s = plan.departure_time_s,
        .arrival_time_s = plan.arrival_time_s,
        .wait_time_s = plan.wait_time_s,
        .coast_time_s = plan.coast_time_s,
        .total_travel_time_s = plan.travel_time_s,
        .remaining_travel_time_s = plan.travel_time_s,
        .propellant_cost_kg = plan.propellant_required_kg,
        .purchase_cost = purchase_cost,
        .fuel_cost = (plan.propellant_required_kg / FUEL_UNITS_TO_KG) * station_price(origin_state, "fuel"),
        .expected_revenue = expected_revenue,
        .operating_cost = time_cost_per_day * mission_days,
        .sampled_path = plan.sampled_path,
        .sampled_times_s = plan.sampled_times_s,
        .sampled_propellant_kg = std::move(sampled_propellant),
        .trajectory_type = plan.trajectory_type,
        .carried_propellant_kg = choice.carried_propellant_kg,
    };
    if (choice.carried_propellant_kg > 0.0) {
        add_event(std::format("{} carries {:.0f} kg of return fuel to {}", ship.name, choice.carried_propellant_kg,
            best_destination->name), "fuel");
    }
    if (plan.wait_time_s > 0.0) {
        if (best_cargo_units > 0.0) {
            add_event(std::format(
                "{} scheduled {}->{} ({}) in {:.1f}d  est. profit {:.0f} cr ({:.1f}u/{:.1f}d travel)",
                ship.name,
                origin_def.name,
                best_destination->name,
                best_commodity,
                plan.wait_time_s / 86400.0,
                expected_revenue - purchase_cost,
                best_cargo_units,
                plan.travel_time_s / 86400.0),
                "mission");
        } else {
            add_event(std::format(
                "{} repositioning {}->{} in {:.1f}d",
                ship.name,
                origin_def.name,
                best_destination->name,
                plan.wait_time_s / 86400.0),
                "mission");
        }
    } else if (best_cargo_units > 0.0) {
        add_event(std::format(
            "{} departed {}->{} ({}) {:.1f}u  est. profit {:.0f} cr ({:.1f}d transit, prop={:.0f}kg)",
            ship.name,
            origin_def.name,
            best_destination->name,
            best_commodity,
            best_cargo_units,
            expected_revenue - purchase_cost,
            plan.travel_time_s / 86400.0,
            plan.propellant_required_kg),
            "mission");
    } else {
        add_event(std::format("{} repositioned from {} to {}", ship.name, origin_def.name, best_destination->name), "mission");
    }
}

double Simulation::refit_bill(
    const domain::ShipClassDefinition& from, const domain::ShipClassDefinition& to) const {
    return universe_.ship_operations.refit_cost_fraction * std::abs(to.ship_value_cr - from.ship_value_cr);
}

bool Simulation::consider_refit(domain::ShipState& ship, const MissionChoice& current, bool trace) {
    if (ship.current_station_id != ship.home_station_id || game_time_s_ < ship.next_refit_review_s) {
        return false;
    }
    ship.next_refit_review_s = game_time_s_ + REFIT_REVIEW_DAYS * 86400.0;
    const auto& ship_class = get_ship_class(ship.class_id);
    const double refit_days = universe_.ship_operations.refit_days;
    // What the ship earns as it is: dispatch's profit rate, or nothing (a repositioning
    // score is not a profit, and such a ship has no paying work here either).
    const double current_score = current.kind == MissionChoice::Kind::Mission ? std::max(0.0, current.score) : 0.0;

    // Only the neighbouring tank sizes: larger steps take several refits.
    const domain::ShipClassDefinition* next_smaller = nullptr;
    const domain::ShipClassDefinition* next_larger = nullptr;
    for (const auto& other : universe_.ship_classes) {
        if (other.hull_id != ship_class.hull_id) {
            continue;
        }
        const double tank_kg = other.propellant_capacity_kg;
        if (tank_kg < ship_class.propellant_capacity_kg
            && (next_smaller == nullptr || tank_kg > next_smaller->propellant_capacity_kg)) {
            next_smaller = &other;
        }
        if (tank_kg > ship_class.propellant_capacity_kg
            && (next_larger == nullptr || tank_kg < next_larger->propellant_capacity_kg)) {
            next_larger = &other;
        }
    }

    const domain::ShipClassDefinition* best_class = nullptr;
    double best_gain = 0.0;
    for (const auto* candidate_ptr : {next_smaller, next_larger}) {
        if (candidate_ptr == nullptr) {
            continue;
        }
        const auto& candidate = *candidate_ptr;
        const double bill = refit_bill(ship_class, candidate);
        if (ship.credits - bill < -credit_line(candidate)) {
            continue;
        }
        domain::ShipState probe = ship;
        probe.class_id = candidate.id;
        probe.propellant_kg = std::min(ship.propellant_kg, candidate.propellant_capacity_kg);
        // Scored on departures after the yard is done: launch windows close meanwhile.
        const auto option = choose_mission(probe, candidate, trace, game_time_s_ + refit_days * 86400.0);
        if (option.kind != MissionChoice::Kind::Mission) {
            continue;
        }
        // The better missions must repay the yard bill and the earnings lost in the yard.
        // The scores already carry each variant's capital charge.
        const double gain = (option.score - current_score) * REFIT_PAYBACK_DAYS - bill - current_score * refit_days;
        if (trace) {
            std::cerr << std::format("[trace day {:.1f}] {}: refit to {}: score {:.1f} vs {:.1f}, bill {:.0f}, gain {:.0f}\n",
                game_time_s_ / 86400.0, ship.name, candidate.id, option.score, current_score, bill, gain);
        }
        if (gain > best_gain) {
            best_gain = gain;
            best_class = &candidate;
        }
    }
    if (best_class == nullptr) {
        return false;
    }

    auto& yard = get_station_state(ship.current_station_id);
    // Propellant that no longer fits goes back to the station's depot.
    const double excess_kg = ship.propellant_kg - best_class->propellant_capacity_kg;
    if (excess_kg > 0.0) {
        const double units = excess_kg / FUEL_UNITS_TO_KG;
        const double value = trade_value(yard, "fuel", units);
        yard.inventory["fuel"] += units;
        yard.credits -= value;
        ship.propellant_kg -= excess_kg;
        ship.credits += value;
        ship.lifetime_profit += value;
        ship.ledger.fuel -= value;
        record_trade({
            .time_s = game_time_s_,
            .ship_id = ship.id,
            .station_id = yard.station_id,
            .commodity_id = "fuel",
            .kind = "sell",
            .units = units,
            .unit_price = value / units,
            .total = value,
        });
    }
    const double bill = refit_bill(ship_class, *best_class);
    ship.credits -= bill;
    ship.lifetime_profit -= bill;
    ship.ledger.refits += bill;
    yard.credits += bill;
    ship.refit_class_id = best_class->id;
    ship.refit_done_s = game_time_s_ + refit_days * 86400.0;
    // The refit was justified by a payback period of better missions: give it that long
    // before reconsidering, or ships swap tanks back and forth on each new forecast.
    ship.next_refit_review_s = ship.refit_done_s + REFIT_PAYBACK_DAYS * 86400.0;
    ship.phase = domain::ShipMissionPhase::Refitting;
    add_event(std::format("{} refitting at {}: {} -> {} ({:.0f} cr, {:.0f} days)",
        ship.name, get_station_definition(ship.current_station_id).name, ship_class.name, best_class->name,
        bill, refit_days), "mission");
    return true;
}

void Simulation::step_refitting_ship(domain::ShipState& ship) {
    if (game_time_s_ < ship.refit_done_s) {
        return;
    }
    ship.class_id = ship.refit_class_id;
    ship.refit_class_id.clear();
    ship.phase = domain::ShipMissionPhase::Idle;
    ship.idle_since_s = game_time_s_;
    ship.next_review_s = game_time_s_;
    add_event(std::format("{} left the yard at {} as {}",
        ship.name, get_station_definition(ship.current_station_id).name, get_ship_class(ship.class_id).name), "mission");
}

void Simulation::step_awaiting_departure_ship(domain::ShipState& ship) {
    ship.active_mission.remaining_travel_time_s = std::max(0.0, ship.active_mission.arrival_time_s - game_time_s_);
    if (game_time_s_ < ship.active_mission.departure_time_s) {
        return;
    }
    ship.phase = domain::ShipMissionPhase::InTransit;
    const auto& origin = get_station_definition(ship.active_mission.origin_station_id);
    const auto& destination = get_station_definition(ship.active_mission.destination_station_id);
    if (ship.active_mission.cargo_units > 0.0) {
        add_event(std::format(
            "{} departed {} for {} carrying {:.1f} units of {}",
            ship.name,
            origin.name,
            destination.name,
            ship.active_mission.cargo_units,
            ship.active_mission.commodity_id),
            "mission");
    } else {
        add_event(std::format("{} departed {} for {}", ship.name, origin.name, destination.name), "mission");
    }
}

void Simulation::step_in_transit_ship(domain::ShipState& ship, double dt_s) {
    (void)dt_s;
    ship.active_mission.remaining_travel_time_s = std::max(0.0, ship.active_mission.arrival_time_s - game_time_s_);

    // Update propellant continuously from the pre-computed mass samples (variable-Isp drives only).
    // The samples run from initial propellant at departure to final propellant at arrival,
    // giving a smooth display rather than a step-change at mission assignment.
    if (!ship.active_mission.sampled_propellant_kg.empty()
        && ship.active_mission.sampled_times_s.size() == ship.active_mission.sampled_propellant_kg.size()) {
        ship.propellant_kg = interpolate_timed_scalar(
            ship.active_mission.sampled_propellant_kg,
            ship.active_mission.sampled_times_s,
            game_time_s_) + ship.active_mission.carried_propellant_kg;
    }

    if (ship.active_mission.remaining_travel_time_s > 0.0) {
        return;
    }

    ship.current_station_id = ship.active_mission.destination_station_id;
    ship.phase = domain::ShipMissionPhase::Idle;
    ship.idle_since_s = game_time_s_;
    auto& destination = get_station_state(ship.current_station_id);
    if (ship.active_mission.cargo_units > 0.0 && !ship.active_mission.commodity_id.empty()) {
        // Apply cargo decay: some goods (food, medicine) spoil in transit.
        double decay_per_day = 0.0;
        for (const auto& c : universe_.commodities) {
            if (c.id == ship.active_mission.commodity_id) {
                decay_per_day = c.decay_fraction_per_day;
                break;
            }
        }
        const double transit_days = ship.active_mission.total_travel_time_s / 86400.0;
        const double surviving = (decay_per_day > 0.0)
            ? std::max(0.0, std::pow(1.0 - decay_per_day, transit_days))
            : 1.0;
        double arrived = ship.active_mission.cargo_units * surviving;
        const double spoiled = ship.active_mission.cargo_units - arrived;

        // Clamp delivery to the destination's free storage; the overflow is jettisoned.
        const auto& destination_def = get_station_definition(ship.current_station_id);
        if (destination_def.storage_capacity_units > 0.0) {
            const double free_capacity = std::max(0.0,
                destination_def.storage_capacity_units - economy_.storage_used_units(destination.inventory));
            if (arrived > free_capacity) {
                add_event(std::format(
                    "{} jettisoned {:.1f}u {} at {} — storage full",
                    ship.name, arrived - free_capacity, ship.active_mission.commodity_id, destination_def.name),
                    "alert");
                arrived = free_capacity;
            }
        }

        // Sell at arrival along the price curve as the delivery lands.
        const double revenue = trade_value(destination, ship.active_mission.commodity_id, arrived);
        const double unit_price = arrived > 0.0 ? revenue / arrived : 0.0;
        destination.inventory[ship.active_mission.commodity_id] += arrived;
        destination.import_units_per_day[ship.active_mission.commodity_id] += arrived / TRADE_FLOW_DAYS;
        destination.credits -= revenue;
        ship.credits += revenue;
        ship.lifetime_profit += revenue;
        ship.ledger.cargo_revenue += revenue;
        if (arrived > 0.0) {
            record_trade({
                .time_s = game_time_s_,
                .ship_id = ship.id,
                .station_id = destination.station_id,
                .commodity_id = ship.active_mission.commodity_id,
                .kind = "sell",
                .units = arrived,
                .unit_price = unit_price,
                .total = revenue,
            });
        }

        if (spoiled > 0.1) {
            add_event(std::format(
                "{} arrived at {} with {:.1f}u {} for {:.0f} cr ({:.1f}u spoiled in {:.0f}d transit)",
                ship.name,
                get_station_definition(ship.current_station_id).name,
                arrived,
                ship.active_mission.commodity_id,
                revenue,
                spoiled,
                transit_days),
                "arrival");
        } else {
            add_event(std::format(
                "{} arrived at {} and sold {:.1f}u {} for {:.0f} cr",
                ship.name,
                get_station_definition(ship.current_station_id).name,
                arrived,
                ship.active_mission.commodity_id,
                revenue),
                "arrival");
        }
    } else {
        add_event(std::format("{} arrived at {}", ship.name, get_station_definition(ship.current_station_id).name), "arrival");
    }
    ship.active_mission = {};
}

void Simulation::step(double real_dt_s) {
    const double dt_s = real_dt_s * timewarp_factor_;
    game_time_s_ += dt_s;
    std::vector<domain::Inventory> stocks_before;
    stocks_before.reserve(stations_.size());
    for (const auto& station : stations_) {
        stocks_before.push_back(station.inventory);
    }
    economy_.step(stations_, dt_s);
    for (auto& station : stations_) {
        const double decay = std::exp(-dt_s / 86400.0 / TRADE_FLOW_DAYS);
        station.ship_fuel_units_per_day *= decay;
        for (auto* flows : {&station.import_units_per_day, &station.export_units_per_day}) {
            for (auto& [commodity_id, units_per_day] : *flows) {
                units_per_day *= decay;
            }
        }
    }
    settle_local_economy(stocks_before);
    step_treasuries(dt_s);
    step_fleet_investment();

    for (auto& ship : ships_) {
        accrue_operating_costs(ship, dt_s);
        switch (ship.phase) {
            case domain::ShipMissionPhase::Idle:
            case domain::ShipMissionPhase::LaidUp:
                step_idle_ship(ship);
                break;
            case domain::ShipMissionPhase::AwaitingDeparture:
                step_awaiting_departure_ship(ship);
                break;
            case domain::ShipMissionPhase::InTransit:
                step_in_transit_ship(ship, dt_s);
                break;
            case domain::ShipMissionPhase::Refueling:
                ship.phase = domain::ShipMissionPhase::Idle;
                break;
            case domain::ShipMissionPhase::Refitting:
                step_refitting_ship(ship);
                break;
            case domain::ShipMissionPhase::Stranded:
                // Fuel for sale here is enough: missions buy their own load.
                if (purchasable_propellant_kg(ship) > 0.0) {
                    ship.phase = domain::ShipMissionPhase::Idle;
                    add_event(std::format("{} recovered from stranded state at {}", ship.name, get_station_definition(ship.current_station_id).name), "alert");
                }
                break;
        }
    }
}

void Simulation::settle_local_economy(const std::vector<domain::Inventory>& stocks_before) {
    for (std::size_t i = 0; i < stations_.size(); ++i) {
        auto& station = stations_[i];
        const auto& definition = get_station_definition(station.station_id);
        for (const auto& [commodity_id, after] : station.inventory) {
            const auto before_it = stocks_before[i].find(commodity_id);
            const double before = before_it == stocks_before[i].end() ? 0.0 : before_it->second;
            const double delta = after - before;
            if (delta == 0.0) {
                continue;
            }
            const double base_price = get_commodity(commodity_id).base_price;
            double value = economy_.get_trade_value(definition, commodity_id, before, delta, base_price);
            if (delta > 0.0) {
                // Local producers are paid at most the base price: the scarcity premium is
                // for goods brought from elsewhere. Otherwise a producer short of its own
                // output (a fresh start) pays its producers up to 16x for stock it never sells.
                const double target = economy_.get_target_stock(definition, commodity_id);
                const double below_target = std::max(0.0, std::min(after, target) - before);
                if (below_target > 0.0) {
                    value += base_price * below_target
                        - economy_.get_trade_value(definition, commodity_id, before, below_target, base_price);
                }
            }
            if (delta < 0.0) {
                station.credits += value;
                station.ledger.household_sales += value;
                outside_economy_credits_ -= value;
            } else {
                station.credits -= value;
                station.ledger.producer_purchases += value;
                outside_economy_credits_ += value;
            }
        }
    }
}

double Simulation::internal_money_supply() const {
    double total = 0.0;
    for (const auto& station : stations_) {
        total += station.credits;
    }
    for (const auto& ship : ships_) {
        total += ship.credits;
    }
    return total;
}

void Simulation::step_treasuries(double dt_s) {
    const auto& open = universe_.open_economy;
    const double dt_days = dt_s / 86400.0;

    // Ships keep a working reserve; the rest goes to the owner, the home station.
    if (open.ship_cash_reserve > 0.0) {
        const double share = std::min(1.0, dt_days / open.dividend_days);
        for (auto& ship : ships_) {
            if (ship.credits <= open.ship_cash_reserve) {
                continue;
            }
            const double dividend = (ship.credits - open.ship_cash_reserve) * share;
            auto& home = get_station_state(ship.home_station_id);
            ship.credits -= dividend;
            ship.ledger.dividends += dividend;
            home.credits += dividend;
            home.ledger.dividends += dividend;
        }
    }

    // The slow controller hands the money-supply gap to the stations per head of population,
    // as subsidies (or taxes when there is too much money).
    double per_capita = 0.0;
    if (open.money_supply_days > 0.0) {
        double population = 0.0;
        for (const auto& definition : universe_.stations) {
            population += static_cast<double>(definition.population);
        }
        if (population > 0.0) {
            per_capita = (seeded_money_supply_ - internal_money_supply()) * std::min(1.0, dt_days / open.money_supply_days)
                / population;
        }
    }

    // Faction treasuries close the gap of stations outside the credit band.
    const double band_share = std::min(1.0, dt_days / open.station_balance_days);
    for (auto& station : stations_) {
        const auto& definition = get_station_definition(station.station_id);
        double transfer = 0.0;
        if (station.credits < open.station_credit_floor) {
            transfer = (open.station_credit_floor - station.credits) * band_share;
        } else if (open.station_credit_ceiling > 0.0 && station.credits > open.station_credit_ceiling) {
            transfer = -(station.credits - open.station_credit_ceiling) * band_share;
        }
        // The controller never pushes a station out of the band, or it would fight the band.
        const double after_band = station.credits + transfer;
        const double controller = per_capita * static_cast<double>(definition.population);
        if (controller < 0.0) {
            transfer -= std::min(-controller, std::max(0.0, after_band - open.station_credit_floor));
        } else if (open.station_credit_ceiling > 0.0) {
            transfer += std::min(controller, std::max(0.0, open.station_credit_ceiling - after_band));
        } else {
            transfer += controller;
        }
        station.credits += transfer;
        faction_treasuries_[definition.faction_id] -= transfer;
        if (transfer > 0.0) {
            station.ledger.subsidies += transfer;
        } else {
            station.ledger.taxes -= transfer;
        }
    }
}

void Simulation::step_fleet_investment() {
    const auto& investment = universe_.fleet_investment;
    if (investment.layup_sale_days > 0.0) {
        for (std::size_t i = ships_.size(); i-- > 0;) {
            const auto& ship = ships_[i];
            if (ship.phase == domain::ShipMissionPhase::LaidUp
                && game_time_s_ - ship.laid_up_since_s >= investment.layup_sale_days * 86400.0) {
                sell_ship(i);
            }
        }
    }
    if (investment.review_days > 0.0 && game_time_s_ >= next_investment_review_s_) {
        next_investment_review_s_ += investment.review_days * 86400.0;
        // Each purchase is a committed flow and keeps its yard busy, so the next one is
        // valued against the demand still open.
        for (int bought = 0; bought < static_cast<int>(investment.max_ships_per_review) && commission_best_ship(); ++bought) {
        }
    }
}

void Simulation::sell_ship(std::size_t index) {
    auto& ship = ships_[index];
    const auto& ship_class = get_ship_class(ship.class_id);
    // The outside economy buys the hull (with what is left in its tanks and stores) for its
    // salvage value, paid to the owner's treasury. The ship's cash, or its debt, goes to the
    // home station, like its dividends.
    const double salvage = universe_.fleet_investment.salvage_fraction * ship_class.ship_value_cr;
    outside_economy_credits_ -= salvage;
    faction_treasuries_[ship.faction_id] += salvage;
    investment_ledger_.salvage += salvage;
    ++investment_ledger_.ships_sold;
    auto& home = get_station_state(ship.home_station_id);
    home.credits += ship.credits;
    home.ledger.dividends += ship.credits;
    ship.ledger.dividends += ship.credits;
    ship.credits = 0.0;
    add_event(std::format("{} sold for salvage at {} ({:.0f} cr) after {:.0f} days laid up",
        ship.name, get_station_definition(ship.current_station_id).name, salvage,
        (game_time_s_ - ship.laid_up_since_s) / 86400.0), "alert");
    sold_ships_.push_back(std::move(ship));
    ships_.erase(ships_.begin() + static_cast<std::ptrdiff_t>(index));
}

bool Simulation::commission_best_ship() {
    const auto& investment = universe_.fleet_investment;
    // Debug aid: SPACETRAINS_TRACE_INVESTMENT=1 logs every candidate and the review's time (stderr).
    static const bool trace = std::getenv("SPACETRAINS_TRACE_INVESTMENT") != nullptr;
    const auto review_start = std::chrono::steady_clock::now();
    const auto new_ship = [&](const domain::ShipClassDefinition& ship_class, const domain::StationDefinition& yard) {
        domain::ShipState ship {
            .id = "probe",
            .name = "probe",
            .faction_id = yard.faction_id,
            .class_id = ship_class.id,
            .home_station_id = yard.id,
            .current_station_id = yard.id,
            .phase = domain::ShipMissionPhase::Idle,
            .propellant_kg = 0.0,
            .credits = investment.working_capital,
            .lifetime_profit = 0.0,
            .active_mission = {},
            .provisions = {},
            .ledger = {},
            .next_review_s = game_time_s_,
            .idle_since_s = game_time_s_,
            .refit_class_id = {},
            .refit_done_s = 0.0,
            .next_refit_review_s = 0.0,
            .laid_up_since_s = 0.0,
            .commissioned_s = game_time_s_,
            .route_destination_id = {},
            .route_commodity_id = {},
            .route_units_per_day = 0.0,
            .route_until_s = 0.0,
        };
        // New ships leave the yard with their full life-support endurance, like the starting fleet.
        for (const auto& [commodity_id, units_per_crew_day] : universe_.ship_operations.life_support_units_per_crew_day) {
            ship.provisions[commodity_id] = ship_class.crew_size * units_per_crew_day * PROVISION_ENDURANCE_DAYS;
        }
        return ship;
    };

    // Candidates: every hull in its standard tanks, built at every station; the winner's tank
    // variants are compared at the end. Dispatch's score is a one-shot rate (a 0.1-day hop with a one-off
    // price gap scores thousands of credits per day), so a candidate is valued by what its best
    // cargo run from the yard earns per day over the route commitment (sustained_profit_per_day),
    // minus its daily running costs.
    double richest_treasury = 0.0;
    for (const auto& [faction_id, balance] : faction_treasuries_) {
        richest_treasury = std::max(richest_treasury, balance);
    }
    // A depot's fuel is also bought by ships.
    const auto consumption_rate = [&](const domain::StationDefinition& station, const std::string& commodity_id) {
        const auto rates = economy_.get_station_net_rates(station);
        const auto it = rates.find(commodity_id);
        const double ship_demand = commodity_id == economy::FUEL_ID
            ? get_station_state(station.id).ship_fuel_units_per_day : 0.0;
        return std::max(0.0, (it == rates.end() ? 0.0 : -it->second) + ship_demand);
    };
    struct Candidate {
        const domain::ShipClassDefinition* ship_class {nullptr};
        const domain::StationDefinition* yard {nullptr};
        double return_bound {0.0};
    };
    std::vector<Candidate> candidates;
    // A yard builds one ship at a time. (Routes already served are discounted by the
    // committed flow, so a yard may build again as soon as it is free.)
    for (const auto& yard : universe_.stations) {
        bool yard_busy = false;
        for (const auto& ship : ships_) {
            yard_busy = yard_busy
                || (ship.home_station_id == yard.id && ship.commissioned_s > 0.0 && game_time_s_ < ship.refit_done_s
                    && ship.phase == domain::ShipMissionPhase::Refitting);
        }
        if (yard_busy) {
            continue;
        }
        // Margin per day each surplus good of the yard could make at its consumers' current
        // prices and consumption rates, a cheap upper bound on any ship's earnings.
        const auto& yard_state = get_station_state(yard.id);
        const auto yard_rates = economy_.get_station_net_rates(yard);
        struct Outlet {
            double unit_margin;
            double rate;
        };
        std::vector<Outlet> outlets;
        for (const auto& [commodity_id, stock] : yard_state.inventory) {
            const auto rate_it = yard_rates.find(commodity_id);
            if (rate_it == yard_rates.end() || rate_it->second <= 0.0 || stock - (8.0 + rate_it->second * 7.0) <= 1.0) {
                continue;
            }
            const double buy_price = station_price(yard_state, commodity_id);
            for (const auto& destination : universe_.stations) {
                const double unit_margin = station_price(get_station_state(destination.id), commodity_id) - buy_price;
                const double rate = consumption_rate(destination, commodity_id);
                if (unit_margin > 0.0 && rate > 0.0) {
                    outlets.push_back({unit_margin, rate});
                }
            }
        }
        for (const auto& ship_class : universe_.ship_classes) {
            if ((!ship_class.hull_id.empty() && ship_class.hull_id != ship_class.id) || ship_class.ship_value_cr <= 0.0
                || ship_class.ship_value_cr + investment.working_capital > richest_treasury) {
                continue;
            }
            // A ship of this hull docked idle or laid up here could already take the work.
            bool idle_here = false;
            for (const auto& ship : ships_) {
                const auto& other_class = get_ship_class(ship.class_id);
                const bool same_hull = (other_class.hull_id.empty() ? other_class.id : other_class.hull_id) == ship_class.id;
                idle_here = idle_here
                    || (same_hull && ship.current_station_id == yard.id
                        && (ship.phase == domain::ShipMissionPhase::Idle || ship.phase == domain::ShipMissionPhase::LaidUp));
            }
            if (idle_here) {
                continue;
            }
            // A trip takes at least a day, so a ship moves at most a hold per day.
            double margin_bound = 0.0;
            for (const auto& outlet : outlets) {
                margin_bound = std::max(margin_bound, outlet.unit_margin * std::min(outlet.rate, ship_class.cargo_capacity_units));
            }
            const double running_cost = daily_capital_cost(ship_class) + daily_crew_cost(ship_class, yard_state);
            const double return_bound = (margin_bound - running_cost) * 365.0 / ship_class.ship_value_cr;
            if (return_bound >= investment.hurdle_return_per_year) {
                candidates.push_back({.ship_class = &ship_class, .yard = &yard, .return_bound = return_bound});
            }
        }
    }
    // Probe the most promising first (plasma probes take seconds); stop once the best return
    // found beats every remaining bound. Sale prices are forecast at arrival, where a starving
    // consumer's price can be a little above today's, so the bound is a heuristic.
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) { return a.return_bound > b.return_bound; });
    // Ships still committed to a route deliver to it; only the rest of the destination's
    // demand is open to a new ship.
    // Ships still in the yard have delivered nothing yet, so the observed imports miss them.
    const auto committed_flow = [&](const std::string& destination_id, const std::string& commodity_id, bool in_yard) {
        double flow = 0.0;
        for (const auto& ship : ships_) {
            if (ship.route_destination_id == destination_id && ship.route_commodity_id == commodity_id
                && game_time_s_ < ship.route_until_s
                && in_yard == (game_time_s_ < ship.commissioned_s + investment.build_days * 86400.0)) {
                flow += ship.route_units_per_day;
            }
        }
        return flow;
    };
    struct Valuation {
        double annual_return {-std::numeric_limits<double>::infinity()};
        std::string label;
        std::string destination_id;
        std::string commodity_id;
        double units_per_day {0.0};
    };
    const auto flow_of = [](const domain::Inventory& flows, const std::string& commodity_id) {
        const auto it = flows.find(commodity_id);
        return it == flows.end() ? 0.0 : it->second;
    };
    // A cargo run is valued over the route commitment as the ship would fly it: a hold out
    // every round trip, bought and sold along the price curves of the stocks both stations
    // would hold by then. The yard keeps making the good and other ships keep taking their
    // share of it; the destination keeps consuming it and other ships (at least those committed
    // to the route) keep delivering it. A route already served thus sells near the base
    // price, not at today's scarcity price.
    const auto sustained_profit_per_day = [&](const domain::StationDefinition& yard,
                                              const MissionChoice::CargoOption& option, double& units_per_day) {
        const auto& commodity_id = option.commodity_id;
        const auto& commodity = get_commodity(commodity_id);
        const auto& destination = *option.destination;
        const auto& yard_state = get_station_state(yard.id);
        const auto& destination_state = get_station_state(destination.id);
        const auto yard_rates = economy_.get_station_net_rates(yard);
        const auto destination_rates = economy_.get_station_net_rates(destination);
        const bool fuel = commodity_id == economy::FUEL_ID;

        const double yard_rate = yard_rates.contains(commodity_id) ? yard_rates.at(commodity_id) : 0.0;
        const double yard_drift = yard_rate - flow_of(yard_state.export_units_per_day, commodity_id)
            - (fuel ? yard_state.ship_fuel_units_per_day : 0.0);
        const double yard_reserve = 8.0 + std::max(0.0, yard_rate) * 7.0;
        double yard_stock = flow_of(yard_state.inventory, commodity_id);
        const double yard_cap = std::max(yard_stock, economy_.production_cap_units(yard, commodity_id, yard_rate));
        const double destination_drift
            = (destination_rates.contains(commodity_id) ? destination_rates.at(commodity_id) : 0.0)
            - (fuel ? destination_state.ship_fuel_units_per_day : 0.0)
            + std::max(flow_of(destination_state.import_units_per_day, commodity_id),
                committed_flow(destination.id, commodity_id, false))
            + committed_flow(destination.id, commodity_id, true);
        double destination_stock = flow_of(destination_state.inventory, commodity_id);
        const auto advance = [&](double days) {
            yard_stock = std::clamp(yard_stock + yard_drift * days, 0.0, yard_cap);
            destination_stock = std::max(0.0, destination_stock + destination_drift * days);
        };

        // Whole round trips over the commitment, at least one: a run longer than the
        // commitment earns its margin over its own cycle.
        const double cycle_days = std::max(1.0, 2.0 * option.travel_days + option.wait_days);
        const double cycles = std::max(1.0, std::floor(investment.route_commitment_days / cycle_days));
        const double window_days = cycles * cycle_days;
        const double leg_days = std::min(cycle_days, option.travel_days + option.wait_days);
        advance(investment.build_days);
        double margin = 0.0;
        double delivered = 0.0;
        for (int cycle = 0; cycle < static_cast<int>(cycles); ++cycle) {
            const double units = std::min(option.cargo_units, yard_stock - yard_reserve);
            if (units <= 1.0) {
                advance(cycle_days);
                continue;
            }
            // The empty way back is lighter but is often fuelled at the dearer port: count it
            // as a second loaded leg.
            const double cost = economy_.get_trade_value(yard, commodity_id, yard_stock, -units, commodity.base_price)
                + 2.0 * option.fuel_cost;
            yard_stock -= units;
            advance(leg_days);
            const double surviving = units * std::pow(1.0 - commodity.decay_fraction_per_day, option.travel_days);
            margin += economy_.get_trade_value(destination, commodity_id, destination_stock, surviving, commodity.base_price)
                - cost;
            destination_stock += surviving;
            delivered += surviving;
            advance(cycle_days - leg_days);
        }
        units_per_day = delivered / window_days;
        return margin / window_days;
    };
    const auto value_candidate = [&](const domain::ShipClassDefinition& ship_class, const domain::StationDefinition& yard,
                                     double return_bound) {
        const auto probe = new_ship(ship_class, yard);
        const auto probe_start = std::chrono::steady_clock::now();
        const auto choice = choose_mission(probe, ship_class, false, game_time_s_ + investment.build_days * 86400.0, true);
        const double running_cost = daily_capital_cost(ship_class) + daily_crew_cost(ship_class, get_station_state(yard.id));
        Valuation valuation;
        valuation.label = "no cargo run";
        for (const auto& option : choice.cargo_options) {
            double units_per_day = 0.0;
            const double profit_per_day = sustained_profit_per_day(yard, option, units_per_day) - running_cost;
            const double annual_return = profit_per_day * 365.0 / ship_class.ship_value_cr;
            if (annual_return > valuation.annual_return) {
                valuation.annual_return = annual_return;
                valuation.label = std::format("{:.0f}u {} -> {} in {:.1f} d, {:.2f} u/d", option.cargo_units,
                    option.commodity_id, option.destination->name, option.travel_days, units_per_day);
                valuation.destination_id = option.destination->id;
                valuation.commodity_id = option.commodity_id;
                valuation.units_per_day = units_per_day;
            }
        }
        if (trace) {
            std::cerr << std::format("[invest day {:.0f}] {} at {}: {} of {} runs: {:.0f}%/yr (bound {:.0f}%) in {:.2f} s\n",
                game_time_s_ / 86400.0, ship_class.id, yard.id, valuation.label, choice.cargo_options.size(),
                100.0 * valuation.annual_return, 100.0 * return_bound,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - probe_start).count());
        }
        return valuation;
    };
    const domain::ShipClassDefinition* best_class = nullptr;
    const domain::StationDefinition* best_yard = nullptr;
    double best_return = investment.hurdle_return_per_year;
    Valuation best_valuation;
    int probes = 0;
    for (const auto& candidate : candidates) {
        if (candidate.return_bound <= best_return) {
            break;
        }
        const auto valuation = value_candidate(*candidate.ship_class, *candidate.yard, candidate.return_bound);
        ++probes;
        if (valuation.annual_return > best_return) {
            best_return = valuation.annual_return;
            best_class = candidate.ship_class;
            best_yard = candidate.yard;
            best_valuation = valuation;
        }
    }
    // The winning hull is built with the tanks that suit it best, judged like a refit (by
    // dispatch's full score, which carries each variant's capital charge); otherwise it would
    // go straight back into the yard.
    if (best_class != nullptr) {
        const auto* hull = best_class;
        double best_score = -std::numeric_limits<double>::infinity();
        for (const auto& variant : universe_.ship_classes) {
            if ((variant.hull_id.empty() ? variant.id : variant.hull_id) != hull->id
                || variant.ship_value_cr + investment.working_capital > richest_treasury) {
                continue;
            }
            const auto choice = choose_mission(
                new_ship(variant, *best_yard), variant, false, game_time_s_ + investment.build_days * 86400.0);
            ++probes;
            if (choice.kind == MissionChoice::Kind::Mission && choice.score > best_score) {
                best_score = choice.score;
                best_class = &variant;
            }
        }
    }
    if (trace) {
        std::cerr << std::format("[invest day {:.0f}] review: {} candidates, {} probed in {:.1f} s\n",
            game_time_s_ / 86400.0, candidates.size(), probes,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - review_start).count());
    }
    if (best_class == nullptr) {
        return false;
    }

    // The yard's own faction invests if it can pay for the ship and its working capital,
    // otherwise the richest faction (which can: candidates are priced against it).
    const double price = best_class->ship_value_cr + investment.working_capital;
    std::string investor = best_yard->faction_id;
    if (faction_treasuries_[investor] < price) {
        for (const auto& [faction_id, balance] : faction_treasuries_) {
            if (balance >= faction_treasuries_[investor]) {
                investor = faction_id;
            }
        }
    }
    // The treasury buys the hull from the outside economy and gives the ship its working
    // capital, which raises the money supply the controller holds.
    faction_treasuries_[investor] -= price;
    outside_economy_credits_ += best_class->ship_value_cr;
    seeded_money_supply_ += investment.working_capital;
    investment_ledger_.hulls_bought += best_class->ship_value_cr;
    investment_ledger_.working_capital += investment.working_capital;
    const int number = ++investment_ledger_.ships_commissioned;

    std::string initials;
    for (const auto& faction : universe_.factions) {
        if (faction.id != investor) {
            continue;
        }
        bool word_start = true;
        for (const char ch : faction.name) {
            if (word_start && ch != ' ') {
                initials += ch;
            }
            word_start = ch == ' ';
        }
    }
    auto ship = new_ship(*best_class, *best_yard);
    ship.id = std::format("commissioned_{:03d}", number);
    ship.name = std::format("{} {} {}", initials, get_ship_class(best_class->hull_id.empty() ? best_class->id : best_class->hull_id).name, number);
    ship.faction_id = investor;
    // Built in the home yard: the refit machinery launches it when the build is done.
    ship.phase = domain::ShipMissionPhase::Refitting;
    ship.refit_class_id = ship.class_id;
    ship.refit_done_s = game_time_s_ + investment.build_days * 86400.0;
    // Like a refit, the tank choice holds for the payback period before it is reconsidered.
    ship.next_refit_review_s = ship.refit_done_s + REFIT_PAYBACK_DAYS * 86400.0;
    // It works the route it was bought for until the commitment ends.
    ship.route_destination_id = best_valuation.destination_id;
    ship.route_commodity_id = best_valuation.commodity_id;
    ship.route_units_per_day = best_valuation.units_per_day;
    ship.route_until_s = ship.refit_done_s + investment.route_commitment_days * 86400.0;
    add_event(std::format("{} ordered at {} for {:.0f} cr: {:.0f}%/yr expected ({})",
        ship.name, best_yard->name, best_class->ship_value_cr, 100.0 * best_return, best_valuation.label), "mission");
    ships_.push_back(std::move(ship));
    return true;
}

domain::SimulationSnapshot Simulation::snapshot() const {
    return {
        .game_time_s = game_time_s_,
        .outside_economy_credits = outside_economy_credits_,
        .faction_treasuries = faction_treasuries_,
        .money_supply_target = seeded_money_supply_,
        .fleet_investment = investment_ledger_,
        .stations = stations_,
        .ships = ships_,
        .sold_ships = sold_ships_,
        .recent_events = recent_events_,
    };
}

std::string Simulation::mission_phase_name(domain::ShipMissionPhase phase) const {
    switch (phase) {
        case domain::ShipMissionPhase::Idle:
            return "idle";
        case domain::ShipMissionPhase::AwaitingDeparture:
            return "awaiting_departure";
        case domain::ShipMissionPhase::InTransit:
            return "in_transit";
        case domain::ShipMissionPhase::Refueling:
            return "refueling";
        case domain::ShipMissionPhase::Stranded:
            return "stranded";
        case domain::ShipMissionPhase::LaidUp:
            return "laid_up";
        case domain::ShipMissionPhase::Refitting:
            return "refitting";
    }
    return "unknown";
}

math::Vec3d Simulation::get_ship_render_position(const domain::ShipState& ship) const {
    if ((ship.phase != domain::ShipMissionPhase::InTransit && ship.phase != domain::ShipMissionPhase::AwaitingDeparture)
        || ship.active_mission.total_travel_time_s <= 0.0) {
        const auto& station = get_station_definition(ship.current_station_id);
        return mechanics_.get_station_position(station, game_time_s_);
    }

    if (ship.phase == domain::ShipMissionPhase::AwaitingDeparture
        && game_time_s_ < ship.active_mission.departure_time_s) {
        const auto& station = get_station_definition(ship.current_station_id);
        return mechanics_.get_station_position(station, game_time_s_);
    }

    if (!ship.active_mission.sampled_path.empty() && !ship.active_mission.sampled_times_s.empty()) {
        return interpolate_timed_sampled_path(ship.active_mission.sampled_path, ship.active_mission.sampled_times_s, game_time_s_);
    }

    const double progress = std::clamp(
        (game_time_s_ - ship.active_mission.departure_time_s) / ship.active_mission.total_travel_time_s,
        0.0,
        1.0);
    if (!ship.active_mission.sampled_path.empty()) {
        return interpolate_sampled_path(ship.active_mission.sampled_path, progress);
    }

    const auto& origin = get_station_definition(ship.active_mission.origin_station_id);
    const auto& destination = get_station_definition(ship.active_mission.destination_station_id);
    const auto origin_position = mechanics_.get_station_position(origin, ship.active_mission.departure_time_s);
    const auto destination_position = mechanics_.get_station_position(destination, ship.active_mission.arrival_time_s);
    return origin_position * (1.0 - progress) + destination_position * progress;
}

std::string Simulation::build_bridge_snapshot_json(bool paused, std::uint64_t snapshot_seq, double snapshot_real_time_s) const {
    auto inventory_value = [](const domain::Inventory& inventory, const std::string& commodity_id) {
        const auto it = inventory.find(commodity_id);
        return it == inventory.end() ? 0.0 : it->second;
    };

    auto station_inventory = [&](const std::string& station_id) -> const domain::Inventory& {
        return get_station_state(station_id).inventory;
    };

    std::ostringstream output;
    output << std::fixed << std::setprecision(6);
    output << "{";
    output << "\"snapshot_seq\":" << snapshot_seq << ",";
    output << "\"snapshot_real_time_s\":" << snapshot_real_time_s << ",";
    output << "\"game_time_s\":" << game_time_s_ << ",";
    output << "\"game_time_days\":" << (game_time_s_ / 86400.0) << ",";
    output << "\"timewarp_factor\":" << timewarp_factor_ << ",";
    output << "\"paused\":" << (paused ? "true" : "false") << ",";

    output << "\"factions\":[";
    for (std::size_t i = 0; i < universe_.factions.size(); ++i) {
        const auto& faction = universe_.factions[i];
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"id\":\"" << json_escape(faction.id) << "\","
               << "\"name\":\"" << json_escape(faction.name) << "\","
               << "\"color\":\"" << json_escape(faction.color_hex) << "\""
               << "}";
    }
    output << "],";

    output << "\"commodities\":[";
    for (std::size_t i = 0; i < universe_.commodities.size(); ++i) {
        const auto& commodity = universe_.commodities[i];
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"id\":\"" << json_escape(commodity.id) << "\","
               << "\"name\":\"" << json_escape(commodity.name) << "\","
               << "\"base_price\":" << commodity.base_price
               << "}";
    }
    output << "],";

    double total_credits = 0.0;
    for (const auto& station : stations_) {
        total_credits += station.credits;
    }
    for (const auto& ship : ships_) {
        total_credits += ship.credits;
    }
    output << "\"total_credits\":" << total_credits << ",";
    output << "\"outside_economy_credits\":" << outside_economy_credits_ << ",";
    output << "\"faction_treasuries\":{";
    for (std::size_t i = 0; i < universe_.factions.size(); ++i) {
        const auto& faction_id = universe_.factions[i].id;
        const auto it = faction_treasuries_.find(faction_id);
        output << (i > 0 ? "," : "") << "\"" << json_escape(faction_id) << "\":"
               << (it == faction_treasuries_.end() ? 0.0 : it->second);
    }
    output << "},";

    output << "\"recent_trades\":[";
    for (std::size_t i = 0; i < recent_trades_.size(); ++i) {
        const auto& trade = recent_trades_[i];
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"time_s\":" << trade.time_s << ","
               << "\"ship_id\":\"" << json_escape(trade.ship_id) << "\","
               << "\"station_id\":\"" << json_escape(trade.station_id) << "\","
               << "\"commodity_id\":\"" << json_escape(trade.commodity_id) << "\","
               << "\"kind\":\"" << json_escape(trade.kind) << "\","
               << "\"units\":" << trade.units << ","
               << "\"unit_price\":" << trade.unit_price << ","
               << "\"total\":" << trade.total
               << "}";
    }
    output << "],";

    output << "\"bodies\":[";
    for (std::size_t i = 0; i < universe_.bodies.size(); ++i) {
        const auto& body = universe_.bodies[i];
        const auto position = mechanics_.get_body_position(body.id, game_time_s_);
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"id\":\"" << json_escape(body.id) << "\","
               << "\"name\":\"" << json_escape(body.name) << "\","
               << "\"parent_id\":\"" << json_escape(body.orbit.parent_id) << "\","
               << "\"radius_m\":" << body.radius_m << ","
               << "\"x\":" << position.x << ","
               << "\"y\":" << position.y << ","
               << "\"z\":" << position.z
               << "}";
    }
    output << "],";

    output << "\"stations\":[";
    for (std::size_t i = 0; i < universe_.stations.size(); ++i) {
        const auto& station = universe_.stations[i];
        const auto position = mechanics_.get_station_position(station, game_time_s_);
        if (i > 0) {
            output << ",";
        }
        const auto& inventory = station_inventory(station.id);
        output << "{"
               << "\"id\":\"" << json_escape(station.id) << "\","
               << "\"name\":\"" << json_escape(station.name) << "\","
               << "\"faction_id\":\"" << json_escape(station.faction_id) << "\","
               << "\"parent_body_id\":\"" << json_escape(station.parent_body_id) << "\","
               << "\"population\":" << station.population << ","
               << "\"x\":" << position.x << ","
               << "\"y\":" << position.y << ","
               << "\"z\":" << position.z << ","
               // Legacy fields kept for Godot frontend compatibility
               << "\"food\":" << inventory_value(inventory, "food") << ","
               << "\"fuel\":" << inventory_value(inventory, "fuel") << ","
               << "\"metals\":" << inventory_value(inventory, "metals") << ","
               // Full inventory dict for all commodities
               << "\"inventory\":{";
        for (std::size_t ci = 0; ci < universe_.commodities.size(); ++ci) {
            if (ci > 0) output << ",";
            output << "\"" << json_escape(universe_.commodities[ci].id) << "\":"
                   << inventory_value(inventory, universe_.commodities[ci].id);
        }
        output << "},";

        const auto& station_state = get_station_state(station.id);
        double storage_used = 0.0;
        for (const auto& [_, units] : inventory) {
            storage_used += std::max(0.0, units);
        }
        output << "\"credits\":" << station_state.credits << ","
               << "\"storage_capacity\":" << station.storage_capacity_units << ","
               << "\"storage_used\":" << storage_used << ","
               << "\"prices\":{";
        for (std::size_t ci = 0; ci < universe_.commodities.size(); ++ci) {
            if (ci > 0) output << ",";
            output << "\"" << json_escape(universe_.commodities[ci].id) << "\":"
                   << station_price(station_state, universe_.commodities[ci].id);
        }
        output << "},"
               << "\"net_rates\":{";
        const auto net_rates = economy_.get_station_net_rates(station);
        bool first_rate = true;
        for (const auto& commodity : universe_.commodities) {
            const auto rate_it = net_rates.find(commodity.id);
            if (rate_it == net_rates.end()) {
                continue;
            }
            if (!first_rate) output << ",";
            first_rate = false;
            output << "\"" << json_escape(commodity.id) << "\":" << rate_it->second;
        }
        output << "}"
               << "}";
    }
    output << "],";

    output << "\"ships\":[";
    for (std::size_t i = 0; i < ships_.size(); ++i) {
        const auto& ship = ships_[i];
        const auto& ship_class = get_ship_class(ship.class_id);
        const auto position = get_ship_render_position(ship);
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"id\":\"" << json_escape(ship.id) << "\","
               << "\"name\":\"" << json_escape(ship.name) << "\","
               << "\"faction_id\":\"" << json_escape(ship.faction_id) << "\","
               << "\"class_id\":\"" << json_escape(ship.class_id) << "\","
               << "\"cargo_capacity_units\":" << ship_class.cargo_capacity_units << ","
               << "\"credits\":" << ship.credits << ","
               << "\"lifetime_profit\":" << ship.lifetime_profit << ","
               << "\"mission_value\":" << (ship.active_mission.expected_revenue - ship.active_mission.purchase_cost
                   - ship.active_mission.fuel_cost - ship.active_mission.operating_cost) << ","
               << "\"propulsion_type\":\"" << json_escape(ship_class.propulsion_type) << "\","
               << "\"trajectory_type\":\"" << json_escape(ship.active_mission.trajectory_type) << "\","
               << "\"phase\":\"" << json_escape(mission_phase_name(ship.phase)) << "\","
               << "\"current_station_id\":\"" << json_escape(ship.current_station_id) << "\","
               << "\"propellant_kg\":" << ship.propellant_kg << ","
               << "\"dry_mass_kg\":" << ship_class.dry_mass_kg << ","
               << "\"propellant_capacity_kg\":" << ship_class.propellant_capacity_kg << ","
               << "\"initial_mass_kg\":" << (ship_class.dry_mass_kg + ship_class.propellant_capacity_kg) << ","
               << "\"current_mass_kg\":" << (ship_class.dry_mass_kg + std::max(0.0, ship.propellant_kg)) << ","
               << "\"origin_station_id\":\"" << json_escape(ship.active_mission.origin_station_id) << "\","
               << "\"destination_station_id\":\"" << json_escape(ship.active_mission.destination_station_id) << "\","
               << "\"commodity_id\":\"" << json_escape(ship.active_mission.commodity_id) << "\","
               << "\"cargo_units\":" << ship.active_mission.cargo_units << ","
               << "\"departure_time_s\":" << ship.active_mission.departure_time_s << ","
               << "\"arrival_time_s\":" << ship.active_mission.arrival_time_s << ","
               << "\"wait_time_s\":" << ship.active_mission.wait_time_s << ","
               << "\"coast_time_s\":" << ship.active_mission.coast_time_s << ","
               << "\"total_travel_time_s\":" << ship.active_mission.total_travel_time_s << ","
               << "\"remaining_travel_time_s\":" << ship.active_mission.remaining_travel_time_s << ","
               << "\"x\":" << position.x << ","
               << "\"y\":" << position.y << ","
               << "\"z\":" << position.z;
        if (ship.phase == domain::ShipMissionPhase::InTransit || ship.phase == domain::ShipMissionPhase::AwaitingDeparture) {
            output << ",\"trajectory_path\":[";
            for (std::size_t path_index = 0; path_index < ship.active_mission.sampled_path.size(); ++path_index) {
                const auto& point = ship.active_mission.sampled_path[path_index];
                if (path_index > 0) {
                    output << ",";
                }
                output << "{"
                       << "\"t_s\":" << (path_index < ship.active_mission.sampled_times_s.size() ? ship.active_mission.sampled_times_s[path_index] : 0.0) << ","
                       << "\"x\":" << point.x << ","
                       << "\"y\":" << point.y << ","
                       << "\"z\":" << point.z
                       << "}";
            }
            output << "]";
            if (!ship.active_mission.destination_station_id.empty()) {
                const auto& destination_station = get_station_definition(ship.active_mission.destination_station_id);
                const auto& destination_body = get_body_definition(destination_station.parent_body_id);
                const auto destination_body_position = mechanics_.get_body_position(destination_body.id, ship.active_mission.arrival_time_s);
                output << ",\"destination_body_at_arrival\":{"
                       << "\"id\":\"" << json_escape(destination_body.id) << "\","
                       << "\"name\":\"" << json_escape(destination_body.name) << "\","
                       << "\"radius_m\":" << destination_body.radius_m << ","
                       << "\"x\":" << destination_body_position.x << ","
                       << "\"y\":" << destination_body_position.y << ","
                       << "\"z\":" << destination_body_position.z
                       << "}";
            }
        }
        output << "}";
    }
    output << "],";

    output << "\"recent_events\":[";
    for (std::size_t i = 0; i < recent_events_.size(); ++i) {
        const auto& event = recent_events_[i];
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"time_s\":" << event.time_s << ","
               << "\"category\":\"" << json_escape(event.category) << "\","
               << "\"text\":\"" << json_escape(event.text) << "\""
               << "}";
    }
    output << "]";
    output << "}";
    return output.str();
}

std::string Simulation::build_report() const {
    auto inventory_value = [](const domain::Inventory& inventory, const std::string& commodity_id) {
        const auto it = inventory.find(commodity_id);
        return it == inventory.end() ? 0.0 : it->second;
    };

    std::ostringstream output;
    output << "SpaceTrains snapshot at day " << (game_time_s_ / 86400.0) << "\n";
    output << "Stations:\n";
    for (const auto& station : stations_) {
        output << "  - " << get_station_definition(station.station_id).name;
        for (const auto& commodity : universe_.commodities) {
            const double val = inventory_value(station.inventory, commodity.id);
            if (val > 0.1) {
                output << " " << commodity.id << "=" << static_cast<int>(val);
            }
        }
        output << "\n";
    }
    output << "Ships:\n";
    for (const auto& ship : ships_) {
        output << "  - " << ship.name << " at " << get_station_definition(ship.current_station_id).name
               << " propellant=" << ship.propellant_kg
               << " phase=" << static_cast<int>(ship.phase) << "\n";
    }
    output << "Recent events:\n";
    for (const auto& event : recent_events_) {
        output << "  - [day " << (event.time_s / 86400.0) << "] " << event.text << "\n";
    }
    return output.str();
}

}  // namespace spacetrains::simulation
