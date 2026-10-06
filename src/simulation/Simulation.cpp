#include "simulation/Simulation.hpp"

#include <algorithm>
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
        stations_.push_back({.station_id = station.id, .inventory = station.initial_inventory, .credits = station.initial_credits});
    }
    for (const auto& ship_class : universe_.ship_classes) {
        ship_classes_by_id_[ship_class.id] = &ship_class;
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
            .next_layup_review_s = 0.0,
            .idle_since_s = 0.0,
        });
    }
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
    return economy_.get_price(definition.economy_profile_id, commodity_id, stock, get_commodity(commodity_id).base_price);
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
double sellable_units(
    const economy::EconomySystem& economy,
    const domain::StationDefinition& station_def,
    const domain::StationState& station_state,
    const std::string& commodity_id,
    bool departing) {
    const auto rates = economy.get_profile_net_rates(station_def.economy_profile_id);
    const double rate = rates.contains(commodity_id) ? rates.at(commodity_id) : 0.0;
    const double reserve = departing ? 0.0 : (rate < 0.0 ? std::abs(rate) * 14.0 : 5.0);
    const auto it = station_state.inventory.find(commodity_id);
    const double stock = it == station_state.inventory.end() ? 0.0 : it->second;
    return std::max(0.0, stock - reserve);
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
    if (ship.phase == domain::ShipMissionPhase::LaidUp) {
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
        ship.next_layup_review_s = game_time_s_ + 86400.0;
        add_event(std::format("{} laid up at {} (port cannot provision the crew)",
            ship.name, get_station_definition(ship.current_station_id).name), "alert");
    }
}

bool Simulation::try_refuel(domain::ShipState& ship) {
    auto& station = get_station_state(ship.current_station_id);
    const auto& ship_class = get_ship_class(ship.class_id);
    // Refuel discipline: top up to 80% and only when below half tank. A fleet
    // that chases 100% every idle tick drinks entire stations dry and locks the
    // system's fuel supply inside ship tanks instead of the market.
    if (ship.propellant_kg >= ship_class.propellant_capacity_kg * 0.5) {
        return ship.propellant_kg >= ship_class.propellant_capacity_kg * 0.4;
    }
    const double missing_kg = std::max(0.0, ship_class.propellant_capacity_kg * 0.8 - ship.propellant_kg);

    // Leave the station its own working reserve (it may consume fuel too) —
    // except for a nearly-dry ship, which may tap the reserve to get unstuck.
    const bool emergency = ship.propellant_kg <= ship_class.propellant_capacity_kg * 0.05;
    const auto station_rates = economy_.get_profile_net_rates(
        get_station_definition(ship.current_station_id).economy_profile_id);
    const double station_fuel_rate = station_rates.contains("fuel") ? station_rates.at("fuel") : 0.0;
    const double station_reserve = (station_fuel_rate < 0.0 && !emergency)
        ? std::abs(station_fuel_rate) * 14.0 : 0.0;
    const double available_fuel_units = std::max(0.0, station.inventory["fuel"] - station_reserve);
    const double transferable_kg = std::min(missing_kg, available_fuel_units * FUEL_UNITS_TO_KG);
    const double transferred_units = transferable_kg / FUEL_UNITS_TO_KG;
    // Price the fuel on the stock before the transfer. Ships may go into debt for fuel
    // (but not for cargo) so an empty wallet never permanently strands a ship.
    const double fuel_unit_price = station_price(station, "fuel");
    const double fuel_bill = transferred_units * fuel_unit_price;
    station.inventory["fuel"] -= transferred_units;
    ship.propellant_kg += transferable_kg;
    ship.credits -= fuel_bill;
    ship.lifetime_profit -= fuel_bill;
    ship.ledger.fuel += fuel_bill;
    station.credits += fuel_bill;

    if (transferable_kg > 0.0) {
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
    return ship.propellant_kg >= ship_class.propellant_capacity_kg * 0.4;
}

Simulation::LegEstimate Simulation::estimate_leg(
    const domain::ShipClassDefinition& ship_class,
    const domain::StationDefinition& origin,
    const domain::StationDefinition& destination,
    double departure_time_s,
    double propellant_kg) {
    // Follow-up legs are only estimates, so departures share 5-day buckets and fuel
    // loads 10%-of-tank buckets (planned at the bucket's lower edge, conservatively).
    // Ships of one class then reuse each other's plans across a whole bucket.
    constexpr double BUCKET_S = 5.0 * 86400.0;
    const auto bucket = static_cast<std::int64_t>(std::floor(departure_time_s / BUCKET_S));
    // Plan with this class's typical costs (provision prices vary a little by port).
    const trajectory::PlanningCosts costs {
        .propellant_cr_per_kg = get_commodity("fuel").base_price / FUEL_UNITS_TO_KG,
        .time_cr_per_day = daily_capital_cost(ship_class) + daily_crew_cost(ship_class, get_station_state(origin.id)),
    };
    const double fuel_step_kg = std::max(1.0, ship_class.propellant_capacity_kg * 0.1);
    const auto fuel_bucket = static_cast<std::int64_t>(std::floor(propellant_kg / fuel_step_kg));
    leg_estimates_.erase(leg_estimates_.begin(),
        leg_estimates_.lower_bound(static_cast<std::int64_t>(std::floor(game_time_s_ / BUCKET_S))));
    auto& slot = leg_estimates_[bucket];
    const auto key = std::format("{}|{}|{}|{}", ship_class.id, origin.id, destination.id, fuel_bucket);
    if (const auto it = slot.find(key); it != slot.end()) {
        return it->second;
    }
    domain::ShipState probe;
    probe.class_id = ship_class.id;
    probe.current_station_id = origin.id;
    probe.propellant_kg = static_cast<double>(fuel_bucket) * fuel_step_kg;
    const auto& planner = (ship_class.propulsion_type == "electric_ion" && variable_isp_planner_)
        ? static_cast<trajectory::ITrajectoryPlanner&>(*variable_isp_planner_)
        : static_cast<trajectory::ITrajectoryPlanner&>(*kepler_planner_);
    LegEstimate estimate;
    if (probe.propellant_kg > 0.0) {
        const auto plan = planner.plan_transfer(origin, destination, probe, ship_class,
            std::max(game_time_s_, static_cast<double>(bucket) * BUCKET_S), costs);
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

void Simulation::step_idle_ship(domain::ShipState& ship) {
    // Laid-up ships (no crew) only look for work once a day.
    const bool laid_up = ship.phase == domain::ShipMissionPhase::LaidUp;
    if (laid_up) {
        if (game_time_s_ < ship.next_layup_review_s) {
            return;
        }
        ship.next_layup_review_s = game_time_s_ + 86400.0;
    }
    const bool has_refuel_reserve = try_refuel(ship);

    // Debug aid: SPACETRAINS_TRACE_SHIP="<ship name>" logs every candidate mission of
    // that ship and why it was rejected (stderr).
    static const char* const trace_ship = std::getenv("SPACETRAINS_TRACE_SHIP");
    const bool trace = trace_ship != nullptr && ship.name == trace_ship;
    const auto trace_line = [&](const std::string& text) {
        if (trace) {
            std::cerr << std::format("[trace day {:.1f}] {} ({}) at {}: {}\n", game_time_s_ / 86400.0, ship.name,
                mission_phase_name(ship.phase), ship.current_station_id, text);
        }
    };

    auto& origin_state = get_station_state(ship.current_station_id);
    const auto& origin_def = get_station_definition(ship.current_station_id);
    const auto& ship_class = get_ship_class(ship.class_id);

    // Ion ships fly max-burn trajectories that consume the whole planning budget,
    // so plan with a 15% captain's reserve held back — they then always arrive
    // with enough margin to leave again instead of gambling on destination stock.
    domain::ShipState planning_ship = ship;
    if (ship_class.propulsion_type == "electric_ion") {
        planning_ship.propellant_kg = std::max(
            0.0, ship.propellant_kg - ship_class.propellant_capacity_kg * 0.15);
    }

    double best_score = MIN_MISSION_SCORE_PER_DAY;
    const domain::StationDefinition* best_destination = nullptr;
    std::string best_commodity;
    double best_cargo_units = 0.0;

    // Every day of a mission (waiting for the window included) costs capital,
    // wages and provisions; missions are compared on profit after that cost.
    const double time_cost_per_day = daily_capital_cost(ship_class) + daily_crew_cost(ship_class, origin_state);
    // The crew must be provisionable from here for the whole mission.
    // (The return-leg reserve is bought too, but only if the station has it.)
    const double max_mission_days = provisionable_days(ship, origin_state) / PROVISION_MARGIN;
    // Cargo may be financed down to the credit line.
    const double spendable_credits = std::max(0.0, ship.credits + credit_line(ship_class));

    // The plan depends on the destination only (cargo mass is not part of the
    // trajectory model), so plan each destination once per scoring pass rather
    // than once per commodity. Ion plans cost ~8 ms each.
    const auto& planner = (ship_class.propulsion_type == "electric_ion" && variable_isp_planner_)
        ? static_cast<trajectory::ITrajectoryPlanner&>(*variable_isp_planner_)
        : static_cast<trajectory::ITrajectoryPlanner&>(*kepler_planner_);
    // A ship must be able to leave its destination again: the fuel it has left on
    // arrival plus what the station can sell must cover a leg as long as this one.
    // Without this, ships flew into fuel-dry consumers (Mercury) and stayed there.
    // A chemical ship planned with a full tank spends most of the burn accelerating its
    // own fuel; the leg's Δv flown with only the propellant it needs costs far less:
    // dry * (exp(Δv / ve) - 1), with exp(Δv / ve) the plan's mass ratio.
    const double exhaust_velocity_mps = trajectory::chemical_exhaust_velocity_mps(ship_class);
    const auto can_leave_again = [&](const domain::StationDefinition& destination,
                                     const domain::TrajectoryPlan& plan) {
        const auto& dest_state = get_station_state(destination.id);
        const double after_arrival_kg = std::max(0.0, ship.propellant_kg - plan.propellant_required_kg);
        const double dest_fuel_kg = sellable_units(economy_, destination, dest_state, "fuel", false) * FUEL_UNITS_TO_KG;
        double needed_kg = plan.propellant_required_kg;
        if (ship_class.propulsion_type != "electric_ion" && exhaust_velocity_mps > 0.0) {
            const double planned_mass_kg = ship_class.dry_mass_kg + planning_ship.propellant_kg;
            const double mass_ratio = planned_mass_kg
                / std::max(ship_class.dry_mass_kg, planned_mass_kg - plan.propellant_required_kg);
            needed_kg = ship_class.dry_mass_kg * (mass_ratio - 1.0);
        }
        return after_arrival_kg + dest_fuel_kg >= needed_kg;
    };
    // Planners trade travel time against propellant at the owner's prices.
    const trajectory::PlanningCosts planning_costs {
        .propellant_cr_per_kg = get_commodity("fuel").base_price / FUEL_UNITS_TO_KG,
        .time_cr_per_day = time_cost_per_day,
    };
    std::unordered_map<std::string, domain::TrajectoryPlan> plans_by_destination;
    const auto plan_to = [&](const domain::StationDefinition& destination) -> const domain::TrajectoryPlan& {
        auto it = plans_by_destination.find(destination.id);
        if (it == plans_by_destination.end()) {
            it = plans_by_destination.emplace(
                destination.id,
                planner.plan_transfer(origin_def, destination, planning_ship, ship_class, game_time_s_, planning_costs)).first;
        }
        return it->second;
    };

    // Fuel burned on a mission comes out of the tank, already bought and not resalable,
    // so it is valued at the neutral base price, not the origin's scarcity price: a ship
    // holding 100 t of fuel at a fuel-starved port (16x) would otherwise never leave.
    const double origin_fuel_price = get_commodity("fuel").base_price;

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
                                     const domain::TrajectoryPlan& plan) -> const std::vector<FollowUp>& {
        if (const auto it = follow_ups_by_destination.find(destination.id); it != follow_ups_by_destination.end()) {
            return it->second;
        }
        std::vector<FollowUp> options;
        const auto& dest_state = get_station_state(destination.id);
        const double arrival_kg = std::max(0.0, ship.propellant_kg - plan.propellant_required_kg);
        double departure_kg = std::min(ship_class.propellant_capacity_kg,
            arrival_kg + sellable_units(economy_, destination, dest_state, "fuel", false) * FUEL_UNITS_TO_KG);
        if (ship_class.propulsion_type == "electric_ion") {
            departure_kg = std::max(0.0, departure_kg - ship_class.propellant_capacity_kg * 0.15);
        }
        // Loads that other ships already docked there or inbound will take first.
        double claimed_units = 0.0;
        for (const auto& other : ships_) {
            if (other.id == ship.id) {
                continue;
            }
            const bool inbound = (other.phase == domain::ShipMissionPhase::InTransit
                || other.phase == domain::ShipMissionPhase::AwaitingDeparture)
                && other.active_mission.destination_station_id == destination.id;
            const bool docked = other.current_station_id == destination.id
                && (other.phase == domain::ShipMissionPhase::Idle || other.phase == domain::ShipMissionPhase::Refueling);
            if (inbound || docked) {
                claimed_units += get_ship_class(other.class_id).cargo_capacity_units;
            }
        }
        const auto dest_rates = economy_.get_profile_net_rates(destination.economy_profile_id);
        for (const auto& [commodity_id, stock] : dest_state.inventory) {
            const double rate = dest_rates.contains(commodity_id) ? dest_rates.at(commodity_id) : 0.0;
            if (rate <= 0.0) {
                continue;
            }
            const double surplus = stock - (8.0 + rate * 7.0) - claimed_units;
            const double units = std::min(surplus, ship_class.cargo_capacity_units);
            if (units <= 1.0) {
                continue;
            }
            const double buy_price = station_price(dest_state, commodity_id);
            const double decay_per_day = get_commodity(commodity_id).decay_fraction_per_day;
            for (const auto& next : universe_.stations) {
                if (next.id == destination.id) {
                    continue;
                }
                const auto next_rates = economy_.get_profile_net_rates(next.economy_profile_id);
                const bool consumes = next_rates.contains(commodity_id) && next_rates.at(commodity_id) < 0.0;
                const double sell_price = station_price(get_station_state(next.id), commodity_id);
                if (!consumes && sell_price <= get_commodity(commodity_id).base_price) {
                    continue;
                }
                const auto leg = estimate_leg(ship_class, destination, next, plan.arrival_time_s, departure_kg);
                if (!leg.feasible) {
                    continue;
                }
                const double revenue = units * std::pow(1.0 - decay_per_day, leg.travel_days) * sell_price;
                const double cost = units * buy_price + (leg.propellant_kg / FUEL_UNITS_TO_KG) * origin_fuel_price
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
        const auto back = estimate_leg(ship_class, destination, origin_def, plan.arrival_time_s, departure_kg);
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
                                   double leg_profit,
                                   double leg_days,
                                   bool cargo_follow_up_only,
                                   std::string& follow_label) {
        double best = -std::numeric_limits<double>::infinity();
        for (const auto& option : follow_ups_from(destination, plan)) {
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

    const auto origin_rates = economy_.get_profile_net_rates(origin_def.economy_profile_id);
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
            if (destination.id == origin_def.id) {
                continue;
            }
            const auto destination_rates = economy_.get_profile_net_rates(destination.economy_profile_id);
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
                double stored = 0.0;
                for (const auto& [_, units] : destination_state.inventory) {
                    stored += std::max(0.0, units);
                }
                free_capacity = std::max(0.0, destination.storage_capacity_units - stored);
            }
            const double affordable = origin_price > 0.0 ? spendable_credits / origin_price : surplus;
            const double cargo_units = std::min({ship_class.cargo_capacity_units, surplus, affordable, free_capacity});
            if (cargo_units <= 1.0) {
                continue;
            }

            const auto& plan = plan_to(destination);
            const auto cargo_label = std::format("cargo {:.0f}u {} -> {}", cargo_units, commodity_id, destination.id);
            if (!plan.feasible) {
                trace_line(cargo_label + ": no feasible trajectory (" + plan.summary + ")");
                continue;
            }

            // Expected profit per day. A starving destination prices the commodity at the
            // 4x scarcity clamp, so urgency is embedded in the price spread.
            const double travel_days = plan.travel_time_s / 86400.0;
            if (travel_days > max_mission_days) {
                trace_line(std::format("{}: {:.0f} days exceeds provisionable {:.0f}", cargo_label, travel_days, max_mission_days));
                continue;
            }
            if (commodity_id != "fuel" && !can_leave_again(destination, plan)) {
                trace_line(cargo_label + ": could not refuel to leave again");
                continue;
            }
            const double surviving = cargo_units * std::pow(1.0 - decay_per_day, travel_days);
            const double revenue = surviving * station_price(destination_state, commodity_id);
            const double fuel_cost = (plan.propellant_required_kg / FUEL_UNITS_TO_KG) * origin_fuel_price;
            const double cost = cargo_units * origin_price + fuel_cost + time_cost_per_day * travel_days;
            // Ion ships burn nearly their whole budget per leg, so don't send one
            // where it cannot refuel afterwards (unless the cargo itself is fuel).
            // Chemical ships keep large margins and are not restricted.
            if (ship_class.propulsion_type == "electric_ion" && commodity_id != "fuel") {
                const double after_arrival_kg = ship.propellant_kg - plan.propellant_required_kg;
                const double dest_fuel_kg = (destination_state.inventory.count("fuel")
                    ? destination_state.inventory.at("fuel") : 0.0) * FUEL_UNITS_TO_KG;
                if (after_arrival_kg + dest_fuel_kg < ship_class.propellant_capacity_kg * 0.25) {
                    trace_line(cargo_label + ": ion fuel reserve rule");
                    continue;
                }
            }

            // The 4x price clamp saturates, so a starving station cannot bid any
            // higher; weight the score by how few days of stock the destination
            // has left (this is what gets fuel hauled to fuel-dry stations).
            const double dest_stock = destination_state.inventory.count(commodity_id)
                ? destination_state.inventory.at(commodity_id) : 0.0;
            const double dest_days_left = destination_rate < 0.0
                ? dest_stock / std::abs(destination_rate)
                : std::numeric_limits<double>::infinity();
            const double urgency = std::clamp(14.0 / std::max(dest_days_left, 0.5), 1.0, 5.0);
            std::string follow_label;
            const double score = two_leg_score(destination, plan, urgency * (revenue - cost), travel_days, false, follow_label);
            trace_line(std::format("{}: {:.0f} days revenue {:.0f} cost {:.0f} (time {:.0f}), {}: score {:.1f}{}",
                cargo_label, travel_days, revenue, cost, time_cost_per_day * travel_days, follow_label, score,
                score > best_score ? " (best so far)" : ""));
            if (score > best_score) {
                best_score = score;
                best_destination = &destination;
                best_commodity = commodity_id;
                best_cargo_units = cargo_units;
            }
        }
    }

    // Empty legs toward a better pickup compete with cargo runs on the same two-leg terms.
    for (const auto& destination : universe_.stations) {
        if (destination.id == origin_def.id) {
            continue;
        }
        const auto& plan = plan_to(destination);
        if (!plan.feasible) {
            continue;
        }
        const double leg_days = plan.travel_time_s / 86400.0;
        if (leg_days > max_mission_days || !can_leave_again(destination, plan)) {
            continue;
        }
        if (ship_class.propulsion_type == "electric_ion") {
            const auto& dest_state = get_station_state(destination.id);
            const double after_arrival_kg = ship.propellant_kg - plan.propellant_required_kg;
            const double dest_fuel_kg = (dest_state.inventory.count("fuel")
                ? dest_state.inventory.at("fuel") : 0.0) * FUEL_UNITS_TO_KG;
            if (after_arrival_kg + dest_fuel_kg < ship_class.propellant_capacity_kg * 0.25) {
                continue;
            }
        }
        const double leg_cost = (plan.propellant_required_kg / FUEL_UNITS_TO_KG) * origin_fuel_price
            + time_cost_per_day * leg_days;
        std::string follow_label;
        const double score = two_leg_score(destination, plan, -leg_cost, leg_days, true, follow_label);
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
        }
    }

    if (best_destination == nullptr) {
        best_score = 0.0;  // repositioning scores are in urgency units, not credits/day

        // Sourcing score: does this station have surplus goods urgently needed
        // elsewhere? An empty ship is only useful where there is something to
        // pick up, so repositioning targets producers; starving stations are
        // served by the cargo loop's urgency weighting once a ship is loaded.
        const auto sourcing_score_for = [&](const domain::StationDefinition& station_def,
                                            const domain::StationState& station_state) {
            const auto rates = economy_.get_profile_net_rates(station_def.economy_profile_id);
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
                    const auto other_rates = economy_.get_profile_net_rates(other.economy_profile_id);
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
            const auto rates = economy_.get_profile_net_rates(station_def.economy_profile_id);
            double best_value = 0.0;
            for (const auto& [commodity_id, rate] : rates) {
                if (rate <= 0.0) continue;
                const double stock = station_state.inventory.count(commodity_id)
                    ? station_state.inventory.at(commodity_id) : 0.0;
                const double surplus = std::max(0.0, stock - (8.0 + rate * 7.0));
                const double load = std::min(surplus, ship_class.cargo_capacity_units);
                if (load <= 1.0) continue;
                const double buy_price = station_price(station_state, commodity_id);
                for (const auto& other : universe_.stations) {
                    if (other.id == station_def.id) continue;
                    const auto other_rates = economy_.get_profile_net_rates(other.economy_profile_id);
                    const bool consumes = other_rates.contains(commodity_id) && other_rates.at(commodity_id) < 0.0;
                    const double other_price = station_price(get_station_state(other.id), commodity_id);
                    if (!consumes && other_price <= get_commodity(commodity_id).base_price) continue;
                    const double spread = other_price - buy_price;
                    best_value = std::max(best_value, load * spread);
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
            const auto& plan = plan_to(destination);
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
            if (!can_leave_again(destination, plan)) {
                trace_line(std::format("reposition -> {}: could not refuel to leave again (burns {:.0f} of {:.0f} kg, port sells {:.0f} kg)",
                    destination.id, plan.propellant_required_kg, ship.propellant_kg,
                    sellable_units(economy_, destination, get_station_state(destination.id), "fuel", false) * FUEL_UNITS_TO_KG));
                continue;
            }
            const double reposition_cost = (plan.propellant_required_kg / FUEL_UNITS_TO_KG) * origin_fuel_price
                + time_cost_per_day * reposition_days;
            if (sourcing_value_cr(destination, dest_state) <= reposition_cost) {
                trace_line(std::format("reposition -> {}: load worth {:.0f} does not cover trip cost {:.0f}",
                    destination.id, sourcing_value_cr(destination, dest_state), reposition_cost));
                continue;
            }
            // Never reposition an ion ship into a fuel-dry trap it cannot leave from.
            if (ship_class.propulsion_type == "electric_ion") {
                const double after_arrival_kg = ship.propellant_kg - plan.propellant_required_kg;
                const double dest_fuel_kg = (dest_state.inventory.count("fuel")
                    ? dest_state.inventory.at("fuel") : 0.0) * FUEL_UNITS_TO_KG;
                if (after_arrival_kg + dest_fuel_kg < ship_class.propellant_capacity_kg * 0.25) {
                    continue;
                }
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
            }
        }
    }

    if (best_destination == nullptr) {
        if (!has_refuel_reserve && ship.propellant_kg <= 0.0) {
            ship.phase = domain::ShipMissionPhase::Stranded;
            add_event(std::format("{} is stranded at {} due to fuel shortage", ship.name, origin_def.name), "alert");
        } else if (!laid_up
            && (ship.credits < 0.0 || game_time_s_ - ship.idle_since_s > IDLE_LAYUP_DAYS * 86400.0)) {
            // Nothing worth flying and either in debt or idle for a month: stop paying the crew.
            ship.phase = domain::ShipMissionPhase::LaidUp;
            ship.next_layup_review_s = game_time_s_ + 86400.0;
            add_event(ship.credits < 0.0
                ? std::format("{} laid up at {} ({:.0f} cr in debt)", ship.name, origin_def.name, -ship.credits)
                : std::format("{} laid up at {} (no profitable work for {:.0f} days)",
                      ship.name, origin_def.name, IDLE_LAYUP_DAYS),
                "alert");
        }
        return;
    }

    const auto plan = plan_to(*best_destination);
    if (!plan.feasible) {
        return;
    }
    if (trajectory_audit_enabled_) {
        record_trajectory_audit(ship, origin_def, *best_destination, plan, planning_ship.propellant_kg);
    }

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
        // Buy at origin: price on the stock before the cargo is reserved.
        const double unit_price = station_price(origin_state, best_commodity);
        purchase_cost = best_cargo_units * unit_price;
        origin_state.inventory[best_commodity] -= best_cargo_units;
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
        expected_revenue = best_cargo_units * station_price(destination_state, best_commodity);
    }
    // Chemical ships: deduct propellant at mission start (instantaneous burns).
    // Electric ion ships: propellant is consumed continuously during transit and
    // tracked per-sample, so we don't deduct here — ship.propellant_kg is updated
    // in step_in_transit_ship from sampled_propellant_kg.
    if (ship_class.propulsion_type != "electric_ion") {
        ship.propellant_kg -= plan.propellant_required_kg;
    }
    // The plan was built from the reserve-reduced budget; shift the per-sample
    // propellant up by the held-back reserve so it stays in the tank instead of
    // being erased by the first in-transit interpolation.
    auto sampled_propellant = plan.sampled_propellant_kg;
    const double reserve_kg = ship.propellant_kg - planning_ship.propellant_kg;
    if (reserve_kg > 0.0) {
        for (double& sample_kg : sampled_propellant) {
            sample_kg += reserve_kg;
        }
    }
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
    };
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

    // Update propellant continuously from the pre-computed mass samples (ion drives only).
    // The samples run from initial propellant at departure to final propellant at arrival,
    // giving a smooth display rather than a step-change at mission assignment.
    if (!ship.active_mission.sampled_propellant_kg.empty()
        && ship.active_mission.sampled_times_s.size() == ship.active_mission.sampled_propellant_kg.size()) {
        ship.propellant_kg = interpolate_timed_scalar(
            ship.active_mission.sampled_propellant_kg,
            ship.active_mission.sampled_times_s,
            game_time_s_);
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
            double stored = 0.0;
            for (const auto& [_, units] : destination.inventory) {
                stored += std::max(0.0, units);
            }
            const double free_capacity = std::max(0.0, destination_def.storage_capacity_units - stored);
            if (arrived > free_capacity) {
                add_event(std::format(
                    "{} jettisoned {:.1f}u {} at {} — storage full",
                    ship.name, arrived - free_capacity, ship.active_mission.commodity_id, destination_def.name),
                    "alert");
                arrived = free_capacity;
            }
        }

        // Sell at arrival: price on the stock before the delivery lands.
        const double unit_price = station_price(destination, ship.active_mission.commodity_id);
        const double revenue = arrived * unit_price;
        destination.inventory[ship.active_mission.commodity_id] += arrived;
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
    economy_.step(stations_, dt_s);

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
            case domain::ShipMissionPhase::Stranded:
                // A partial fuel delivery is enough to get moving again; the 40%
                // bar inside try_refuel is the comfortable mission reserve, not
                // the recovery threshold.
                (void)try_refuel(ship);
                if (ship.propellant_kg >= get_ship_class(ship.class_id).propellant_capacity_kg * 0.15) {
                    ship.phase = domain::ShipMissionPhase::Idle;
                    add_event(std::format("{} recovered from stranded state at {}", ship.name, get_station_definition(ship.current_station_id).name), "alert");
                }
                break;
        }
    }
}

domain::SimulationSnapshot Simulation::snapshot() const {
    return {
        .game_time_s = game_time_s_,
        .stations = stations_,
        .ships = ships_,
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
        const auto net_rates = economy_.get_profile_net_rates(station.economy_profile_id);
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
