#include "simulation/Simulation.hpp"

#include "util/Profiling.hpp"

#include <algorithm>
#include <chrono>
#include <array>
#include <tuple>
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
// Emergency deliveries (step 14): opened when a life-support good's forecast stock runs out
// within the horizon, closed when it covers EMERGENCY_CLOSE_DAYS again. The floor price covers
// one delivery of EMERGENCY_COVER_DAYS of use (shared by the ships that take it); its premium
// (x the reference price) grows by EMERGENCY_RAISE_FACTOR every EMERGENCY_RAISE_DAYS that no
// ship takes a delivery: a small station's month of water is worth little at 3x, less than
// a trip to it costs, so the offer grows until a ship comes (Mercury's water stayed short for
// two years at 8x; doubling to 96x paid 2M cr in two years).
// A station's import cost (what its residents pay) averages deliveries over this many days of its use.
constexpr double IMPORT_COST_DAYS = 30.0;
constexpr double EMERGENCY_HORIZON_DAYS = 21.0;
constexpr double EMERGENCY_CLOSE_DAYS = 14.0;
constexpr double EMERGENCY_COVER_DAYS = 30.0;
constexpr double EMERGENCY_START_PREMIUM = 3.0;
constexpr double EMERGENCY_RAISE_FACTOR = 1.5;
constexpr double EMERGENCY_MAX_PREMIUM = 24.0;
constexpr double EMERGENCY_RAISE_DAYS = 15.0;

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
        stations_.push_back({.station_id = station.id, .inventory = starting_inventory(station), .credits = station.initial_credits, .ledger = {}});
        stations_.back().population = static_cast<double>(station.population);
        stations_.back().population_announced = stations_.back().population;
    }
    for (const auto& ship_class : universe_.ship_classes) {
        ship_classes_by_id_[ship_class.id] = &ship_class;
    }
    event_rng_.seed(universe_.event_seed);
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

namespace {

// The largest lot's good (the UI's one-line summary of a hold).
std::string largest_lot(const std::vector<domain::CargoLot>& cargo) {
    std::string largest;
    double units = 0.0;
    for (const auto& lot : cargo) {
        if (lot.units > units) {
            units = lot.units;
            largest = lot.commodity_id;
        }
    }
    return largest;
}

std::string cargo_json(const std::vector<domain::CargoLot>& cargo) {
    std::string text = "[";
    for (const auto& lot : cargo) {
        text += std::format("{}{{\"commodity_id\":\"{}\",\"units\":{:.6f},\"contract_value\":{:.2f}}}", text.size() > 1 ? "," : "",
            json_escape(lot.commodity_id), lot.units, lot.contract_value);
    }
    return text + "]";
}

// "30u food + 20u water" (the largest lot first).
std::string describe_cargo(std::vector<domain::CargoLot> cargo) {
    std::sort(cargo.begin(), cargo.end(), [](const auto& a, const auto& b) { return a.units > b.units; });
    std::string text;
    for (const auto& lot : cargo) {
        text += std::format("{}{:.0f}u {}", text.empty() ? "" : " + ", lot.units, lot.commodity_id);
    }
    return text;
}

}  // namespace

Simulation Simulation::from_data_root(const std::string& data_root) {
    data_loader::DataLoader loader;
    auto universe = loader.load_universe(data_root);
    Simulation sim(std::move(universe));
    sim.thread_pool_ = std::make_unique<util::ThreadPool>();

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

void Simulation::start_at(double time_s) {
    game_time_s_ = time_s;
    event_rng_.seed(universe_.event_seed ^ static_cast<std::uint64_t>(time_s));
    next_investment_review_s_ = time_s;
    for (auto& ship : ships_) {
        ship.next_review_s = time_s;
        ship.idle_since_s = time_s;
        ship.next_refit_review_s = time_s;
    }
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

domain::Inventory Simulation::starting_inventory(const domain::StationDefinition& station) const {
    // A station starts with what it would hold if it had been supplied all along: each good it
    // consumes at least at its target stock (its resupply cover: three weeks, or 1.4x the
    // transfer from the nearest producer, at most a year). With the few weeks of the data
    // files, Ceres and Ganymede ran out of oxygen on day 20 and the outposts' first emergencies
    // stayed open for 450-660 days while the first freighters were on their way. The raise is
    // scaled down to fit 85% of the station's storage.
    domain::Inventory inventory = station.initial_inventory;
    domain::Inventory raises;
    for (const auto& [commodity_id, rate] : economy_.get_station_net_rates(station)) {
        if (rate >= 0.0 || economy_.is_export_market(station, commodity_id)) {
            continue;
        }
        const double stock = inventory.contains(commodity_id) ? inventory.at(commodity_id) : 0.0;
        const double raise = economy_.get_target_stock(station, commodity_id) - stock;
        if (raise > 0.0) {
            raises[commodity_id] = raise;
        }
    }
    double raised_storage = 0.0;
    for (const auto& [commodity_id, raise] : raises) {
        raised_storage += economy_.storage_used_units({{commodity_id, raise}});
    }
    const double room = 0.85 * station.storage_capacity_units - economy_.storage_used_units(inventory);
    const double scale = station.storage_capacity_units > 0.0 && raised_storage > room
        ? std::max(0.0, room) / raised_storage : 1.0;
    for (const auto& [commodity_id, raise] : raises) {
        inventory[commodity_id] += raise * scale;
    }
    return inventory;
}

std::vector<Simulation::ReferencePrice> Simulation::compute_reference_prices() const {
    constexpr double MIN_HOLD_UNITS = 150.0;
    constexpr std::size_t PRODUCERS_PER_GOOD = 2;
    const auto& pricing = universe_.pricing;
    const auto departures = static_cast<int>(pricing.reference_departures);
    std::vector<const domain::ShipClassDefinition*> hulls;
    for (const auto& ship_class : universe_.ship_classes) {
        if (ship_class.id == ship_class.hull_id && ship_class.cargo_capacity_units >= MIN_HOLD_UNITS) {
            hulls.push_back(&ship_class);
        }
    }
    const double fuel_cr_per_kg = get_commodity(economy::FUEL_ID).base_price / FUEL_UNITS_TO_KG;
    const auto daily_cost = [&](const domain::ShipClassDefinition& ship_class) {
        double cost = daily_capital_cost(ship_class) + ship_class.crew_size * universe_.ship_operations.wage_cr_per_crew_day;
        for (const auto& [commodity_id, units_per_crew_day] : universe_.ship_operations.life_support_units_per_crew_day) {
            cost += ship_class.crew_size * units_per_crew_day * get_commodity(commodity_id).base_price;
        }
        return cost;
    };

    // What to price: every consumer's net-consumed goods (export markets keep their flat price),
    // with its nearest producers; and fuel at every depot without a factory (step 20: Mars, 250
    // days from the nearest factory, got almost no fuel while it was priced like a factory's).
    struct Route {
        const domain::StationDefinition* consumer {nullptr};
        const domain::CommodityDefinition* commodity {nullptr};
        std::vector<const domain::StationDefinition*> producers;
    };
    std::vector<Route> routes;
    for (const auto& consumer : universe_.stations) {
        auto goods = economy_.get_station_net_rates(consumer);
        const bool fuel_depot = universe_.fuel_supply.depot_buffer_units > 0.0 && economy_.fuel_factory_output(consumer) <= 0.0;
        if (fuel_depot) {
            goods[economy::FUEL_ID] = std::min(goods[economy::FUEL_ID], -1.0);
        } else {
            goods.erase(economy::FUEL_ID);
        }
        for (const auto& [commodity_id, rate] : goods) {
            if (rate >= 0.0 || economy_.is_export_market(consumer, commodity_id)) {
                continue;
            }
            Route route {.consumer = &consumer, .commodity = &get_commodity(commodity_id)};
            for (const auto& producer : universe_.stations) {
                const auto rates = economy_.get_station_net_rates(producer);
                const auto it = rates.find(commodity_id);
                if (producer.id != consumer.id && it != rates.end() && it->second > 0.0) {
                    route.producers.push_back(&producer);
                }
            }
            std::stable_sort(route.producers.begin(), route.producers.end(), [&](const auto* a, const auto* b) {
                return economy_.transfer_days(*a, consumer) < economy_.transfer_days(*b, consumer);
            });
            if (route.producers.size() > PRODUCERS_PER_GOOD) {
                route.producers.resize(PRODUCERS_PER_GOOD);
            }
            if (!route.producers.empty()) {
                routes.push_back(std::move(route));
            }
        }
    }
    std::sort(routes.begin(), routes.end(), [](const Route& a, const Route& b) {
        return std::tie(a.consumer->id, a.commodity->id) < std::tie(b.consumer->id, b.commodity->id);
    });

    // Every leg to plan: (from, to, hull, departure, payload); the empty returns are shared.
    struct Leg {
        const domain::StationDefinition* from {nullptr};
        const domain::StationDefinition* to {nullptr};
        const domain::ShipClassDefinition* hull {nullptr};
        double departure_s {0.0};
        double payload_kg {0.0};
    };
    std::vector<Leg> legs;
    std::map<std::string, std::size_t> leg_index;
    const auto add_leg = [&](const Leg& leg) {
        const auto key = std::format("{}|{}|{}|{:.0f}|{:.0f}", leg.from->id, leg.to->id, leg.hull->id, leg.departure_s, leg.payload_kg);
        if (const auto it = leg_index.find(key); it != leg_index.end()) {
            return it->second;
        }
        legs.push_back(leg);
        return leg_index[key] = legs.size() - 1;
    };
    const auto departure_s = [&](int d) { return d * 365.25 * 86400.0 / departures; };
    // A full hold, or a part of it where a full one is too heavy for the transfer (Mercury).
    constexpr std::array<double, 3> LOADS {1.0, 0.5, 0.25};
    for (const auto& route : routes) {
        for (const auto* producer : route.producers) {
            for (const auto* hull : hulls) {
                for (const double load : LOADS) {
                    for (int d = 0; d < departures; ++d) {
                        add_leg({producer, route.consumer, hull, departure_s(d),
                            load * hull->cargo_capacity_units * route.commodity->mass_per_unit_kg});
                    }
                }
            }
        }
    }
    const auto plan_leg = [&](const Leg& leg) {
        domain::ShipState probe;
        probe.class_id = leg.hull->id;
        probe.current_station_id = leg.from->id;
        const trajectory::PlanningOptions options {
            .propellant_cr_per_kg = fuel_cr_per_kg,
            .time_cr_per_day = daily_cost(*leg.hull),
            .payload_kg = leg.payload_kg,
            .purchasable_propellant_kg = leg.hull->propellant_capacity_kg,
            .reserve_fraction = PROPELLANT_RESERVE_FRACTION,
            .include_path = false,
        };
        const auto& planner = (leg.hull->propulsion_type == "variable_isp" && variable_isp_planner_)
            ? static_cast<const trajectory::ITrajectoryPlanner&>(*variable_isp_planner_)
            : static_cast<const trajectory::ITrajectoryPlanner&>(*kepler_planner_);
        return planner.plan_transfer(*leg.from, *leg.to, probe, *leg.hull, leg.departure_s, options);
    };
    std::vector<domain::TrajectoryPlan> outbound(legs.size());
    thread_pool_->parallel_for(legs.size(), [&](std::size_t i) { outbound[i] = plan_leg(legs[i]); });
    // The empty returns leave when the loaded legs arrive.
    std::vector<Leg> returns;
    std::map<std::string, std::size_t> return_index;
    std::vector<std::size_t> return_of(legs.size(), std::numeric_limits<std::size_t>::max());
    for (std::size_t i = 0; i < legs.size(); ++i) {
        if (!outbound[i].feasible) {
            continue;
        }
        const Leg back {legs[i].to, legs[i].from, legs[i].hull, outbound[i].arrival_time_s, 0.0};
        const auto key = std::format("{}|{}|{}|{:.0f}", back.from->id, back.to->id, back.hull->id, back.departure_s);
        auto it = return_index.find(key);
        if (it == return_index.end()) {
            returns.push_back(back);
            it = return_index.emplace(key, returns.size() - 1).first;
        }
        return_of[i] = it->second;
    }
    std::vector<domain::TrajectoryPlan> inbound(returns.size());
    thread_pool_->parallel_for(returns.size(), [&](std::size_t i) { inbound[i] = plan_leg(returns[i]); });

    std::vector<ReferencePrice> prices;
    for (const auto& route : routes) {
        ReferencePrice best {
            .station_id = route.consumer->id,
            .commodity_id = route.commodity->id,
            .base_price = route.commodity->base_price,
            .transport_per_unit = std::numeric_limits<double>::infinity(),
        };
        for (const auto* producer : route.producers) {
            for (const auto* hull : hulls) {
              for (const double load : LOADS) {
                const double units = load * hull->cargo_capacity_units;
                std::vector<std::pair<double, double>> trips;  // (cost per unit, round-trip days)
                for (int d = 0; d < departures; ++d) {
                    const auto key = std::format("{}|{}|{}|{:.0f}|{:.0f}", producer->id, route.consumer->id, hull->id,
                        departure_s(d), units * route.commodity->mass_per_unit_kg);
                    const auto i = leg_index.at(key);
                    if (return_of[i] == std::numeric_limits<std::size_t>::max() || !inbound[return_of[i]].feasible) {
                        continue;
                    }
                    const auto& out = outbound[i];
                    const auto& back = inbound[return_of[i]];
                    const double days = (back.arrival_time_s - departure_s(d)) / 86400.0;
                    const double cost = daily_cost(*hull) * days
                        + (out.propellant_required_kg + back.propellant_required_kg) * fuel_cr_per_kg;
                    trips.emplace_back(cost / units, days);
                }
                // A hull that cannot fly the route at most dates does not set its price.
                if (trips.size() * 2 <= static_cast<std::size_t>(departures)) {
                    continue;
                }
                std::sort(trips.begin(), trips.end());
                const auto& median = trips[trips.size() / 2];
                if (median.first < best.transport_per_unit) {
                    best.transport_per_unit = median.first;
                    best.round_trip_days = median.second;
                    best.producer_id = producer->id;
                    best.class_id = load < 1.0 ? std::format("{}@{:.0f}%", hull->id, 100.0 * load) : hull->id;
                }
              }
            }
        }
        if (std::isfinite(best.transport_per_unit)) {
            best.reference_price = best.base_price + pricing.carrier_margin * best.transport_per_unit;
            prices.push_back(std::move(best));
        }
    }
    return prices;
}

void Simulation::set_station_stock(const std::string& station_id, const std::string& commodity_id, double units) {
    get_station_state(station_id).inventory[commodity_id] = units;
}

std::string Simulation::faction_name(const std::string& faction_id) const {
    for (const auto& faction : universe_.factions) {
        if (faction.id == faction_id) {
            return faction.name;
        }
    }
    return faction_id;
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
    return sale_split_on_arrival(state, commodity_id, units, days_ahead, seller_ship_id).total;
}

Simulation::SaleSplit Simulation::sale_split_on_arrival(const domain::StationState& state, const std::string& commodity_id,
    double units, double days_ahead, const std::string& seller_ship_id) const {
    return sale_split(get_station_definition(state.station_id), commodity_id,
        forecast_stock_on_arrival(state, commodity_id, days_ahead, seller_ship_id), units);
}

const domain::Emergency* Simulation::find_emergency(const std::string& station_id, const std::string& commodity_id) const {
    for (const auto& emergency : emergencies_) {
        if (emergency.station_id == station_id && emergency.commodity_id == commodity_id) {
            return &emergency;
        }
    }
    return nullptr;
}

Simulation::SaleSplit Simulation::sale_split(const domain::StationDefinition& station, const std::string& commodity_id,
    double stock, double units) const {
    SaleSplit split {.total = economy_.get_trade_value(station, commodity_id, stock, units, get_commodity(commodity_id).base_price)};
    // A station that cannot pay offers less (step 17); Earth's export markets always pay.
    if (units > 0.0 && !economy_.is_export_market(station, commodity_id)) {
        split.total *= station_affordability(station);
    }
    const auto* emergency = units > 0.0 ? find_emergency(station.id, commodity_id) : nullptr;
    if (emergency == nullptr) {
        return split;
    }
    const auto rates = economy_.get_station_net_rates(station);
    const auto rate = rates.find(commodity_id);
    if (rate == rates.end() || rate->second >= 0.0) {
        return split;
    }
    // The faction pays the floor price for the units that land below the emergency cover,
    // where the station's own curve is below the floor. The curve falls with the stock, so
    // that is the stock range above the floor's point on the curve (all of it when the floor
    // is above the curve's cap).
    const double cover = -rate->second * EMERGENCY_COVER_DAYS;
    const double floor_from = emergency->premium >= economy_.price_cap(station, commodity_id)
        ? 0.0 : economy_.stock_at_multiplier(station, commodity_id, emergency->premium);
    const double lo = std::max(stock, floor_from);
    const double hi = std::min({stock + units, cover, lo + emergency->units_open});
    if (hi <= lo) {
        return split;
    }
    const double floor_price = emergency->premium * economy_.reference_price(station, commodity_id);
    const double premium = floor_price * (hi - lo)
        - economy_.get_trade_value(station, commodity_id, lo, hi - lo, get_commodity(commodity_id).base_price);
    if (premium > 0.0) {
        split.faction = premium;
        split.total += premium;
        split.emergency_units = hi - lo;
    }
    return split;
}

void Simulation::step_emergencies() {
    for (const auto& state : stations_) {
        const auto& definition = get_station_definition(state.station_id);
        const auto rates = economy_.get_station_net_rates(definition);
        for (const auto& commodity_id : universe_.emergency_goods) {
            const auto rate = rates.find(commodity_id);
            if (rate == rates.end() || rate->second >= 0.0 || !economy_.is_upkeep(definition, commodity_id)) {
                continue;
            }
            const double use_per_day = -rate->second;
            const double forecast = forecast_stock_on_arrival(state, commodity_id, EMERGENCY_HORIZON_DAYS, {});
            const auto open = std::find_if(emergencies_.begin(), emergencies_.end(), [&](const domain::Emergency& e) {
                return e.station_id == state.station_id && e.commodity_id == commodity_id;
            });
            if (open == emergencies_.end()) {
                if (forecast <= 0.0) {
                    emergencies_.push_back({
                        .station_id = state.station_id,
                        .commodity_id = commodity_id,
                        .premium = EMERGENCY_START_PREMIUM,
                        .units_open = use_per_day * EMERGENCY_COVER_DAYS,
                        .opened_s = game_time_s_,
                        .last_raise_s = game_time_s_,
                    });
                    ++emergencies_opened_;
                    add_event(std::format("EMERGENCY: {} will run out of {} — {} pays {:.0f}x for deliveries",
                        definition.name, get_commodity(commodity_id).name, faction_name(definition.faction_id),
                        EMERGENCY_START_PREMIUM), "alert");
                }
                continue;
            }
            if (forecast >= use_per_day * EMERGENCY_CLOSE_DAYS) {
                add_event(std::format("Emergency over: {} has enough {} coming ({:.0f} days after it began)",
                    definition.name, get_commodity(commodity_id).name, (game_time_s_ - open->opened_s) / 86400.0), "alert");
                emergencies_.erase(open);
            } else if (game_time_s_ - open->last_raise_s >= EMERGENCY_RAISE_DAYS * 86400.0
                && open->premium < EMERGENCY_MAX_PREMIUM && open->units_open > 0.0) {
                open->premium = std::min(EMERGENCY_MAX_PREMIUM, open->premium * EMERGENCY_RAISE_FACTOR);
                open->last_raise_s = game_time_s_;
                add_event(std::format("EMERGENCY: no ship for {} at {} — {} raises its offer to {:.1f}x",
                    get_commodity(commodity_id).name, definition.name, faction_name(definition.faction_id), open->premium),
                    "alert");
            }
        }
    }
}

double Simulation::forecast_stock_on_arrival(const domain::StationState& state, const std::string& commodity_id,
    double days_ahead, const std::string& seller_ship_id) const {
    // Forecast the stock the sale lands on: today's stock run forward to this ship's
    // arrival at the station's net rate (never below empty), plus the cargo of the same
    // kind other ships deliver before then. Without the inbound cargo, every ship sent to a
    // starving port expects its scarcity price and the late arrivals sell at a loss. Cargo
    // that arrives after this ship does not lower its price: before v36 all inbound cargo
    // counted, so a hop of hours to a starving station looked worthless whenever a slow
    // ship was months out with the same good (Low Earth Logistics waited for Venus food
    // with 10,000 units at Earth L1).
    const auto& definition = get_station_definition(state.station_id);
    const auto stock_it = state.inventory.find(commodity_id);
    double stock = stock_it == state.inventory.end() ? 0.0 : stock_it->second;
    const double arrival_s = game_time_s_ + std::max(0.0, days_ahead) * 86400.0;
    std::vector<std::pair<double, double>> deliveries;
    for (const auto& other : ships_) {
        if (other.id != seller_ship_id
            && (other.phase == domain::ShipMissionPhase::InTransit
                || other.phase == domain::ShipMissionPhase::AwaitingDeparture)
            && other.active_mission.destination_station_id == state.station_id
            && other.active_mission.arrival_time_s <= arrival_s) {
            if (const double units_aboard = domain::units_of(other.active_mission.cargo, commodity_id); units_aboard > 0.0) {
                deliveries.emplace_back(other.active_mission.arrival_time_s, units_aboard);
            }
        }
    }
    std::sort(deliveries.begin(), deliveries.end());
    const auto rates = economy_.get_station_net_rates(definition);
    const double rate = rates.contains(commodity_id) ? rates.at(commodity_id) : 0.0;
    double time_s = game_time_s_;
    for (const auto& [delivery_s, delivered] : deliveries) {
        stock = std::max(0.0, stock + rate * std::max(0.0, delivery_s - time_s) / 86400.0) + delivered;
        time_s = std::max(time_s, delivery_s);
    }
    return std::max(0.0, stock + rate * std::max(0.0, arrival_s - time_s) / 86400.0);
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
    const profiling::Scope profile_scope(profiling::Phase::EstimateLeg);
    // Follow-up legs are only estimates, so departures share 5-day buckets, available
    // fuel 10%-of-tank buckets and payloads 2 t buckets (fuel at the bucket's lower edge,
    // payload at its upper edge: conservative). Ships of one class then reuse each
    // other's plans across a whole bucket. The ship is assumed to buy what it needs.
    constexpr double BUCKET_S = 5.0 * 86400.0;
    const auto bucket = static_cast<std::int64_t>(std::floor(departure_time_s / BUCKET_S));
    // Plan with this class's typical costs: provisions at base prices, so an estimate does
    // not depend on the port's prices on the day the bucket was first planned.
    double typical_crew_cost = ship_class.crew_size * universe_.ship_operations.wage_cr_per_crew_day;
    for (const auto& [commodity_id, units_per_crew_day] : universe_.ship_operations.life_support_units_per_crew_day) {
        typical_crew_cost += ship_class.crew_size * units_per_crew_day * get_commodity(commodity_id).base_price;
    }
    const double fuel_step_kg = std::max(1.0, ship_class.propellant_capacity_kg * 0.1);
    const auto fuel_bucket = static_cast<std::int64_t>(std::floor(available_propellant_kg / fuel_step_kg));
    constexpr double PAYLOAD_STEP_KG = 2000.0;
    const auto payload_bucket = static_cast<std::int64_t>(std::ceil(std::max(0.0, payload_kg) / PAYLOAD_STEP_KG));
    const trajectory::PlanningOptions options {
        .propellant_cr_per_kg = get_commodity("fuel").base_price / FUEL_UNITS_TO_KG,
        .time_cr_per_day = daily_capital_cost(ship_class) + typical_crew_cost,
        .payload_kg = static_cast<double>(payload_bucket) * PAYLOAD_STEP_KG,
        .purchasable_propellant_kg = static_cast<double>(fuel_bucket) * fuel_step_kg,
        .reserve_fraction = PROPELLANT_RESERVE_FRACTION,
        .include_path = false,
    };
    leg_estimates_.erase(leg_estimates_.begin(),
        leg_estimates_.lower_bound(static_cast<std::int64_t>(std::floor(game_time_s_ / BUCKET_S))));
    auto& slot = leg_estimates_[bucket];
    const auto key = std::format("{}|{}|{}|{}|{}", ship_class.id, origin.id, destination.id, fuel_bucket, payload_bucket);
    // Measured from this caller's departure, not the bucket edge.
    const auto for_caller = [departure_time_s](LegEstimate estimate) {
        estimate.travel_days = std::max(0.0, estimate.arrival_time_s - departure_time_s) / 86400.0;
        return estimate;
    };
    if (const auto it = slot.find(key); it != slot.end()) {
        return it->second.feasible ? for_caller(it->second) : it->second;
    }
    domain::ShipState probe;
    probe.class_id = ship_class.id;
    probe.current_station_id = origin.id;
    probe.propellant_kg = 0.0;
    const auto& planner = (ship_class.propulsion_type == "variable_isp" && variable_isp_planner_)
        ? static_cast<trajectory::ITrajectoryPlanner&>(*variable_isp_planner_)
        : static_cast<trajectory::ITrajectoryPlanner&>(*kepler_planner_);
    if (options.purchasable_propellant_kg <= 0.0) {
        slot.emplace(key, LegEstimate {});
        return {};
    }
    // Planned from the bucket's start, whenever the bucket is first asked for, so an
    // estimate does not depend on which ship asked first (or on a save and load).
    const double plan_departure_s = static_cast<double>(bucket) * BUCKET_S;
    const auto to_estimate = [](const domain::TrajectoryPlan& plan) {
        return LegEstimate {
            .feasible = plan.feasible,
            .propellant_kg = plan.propellant_required_kg,
            .arrival_time_s = plan.arrival_time_s,
        };
    };
    if (deferred_plans_ != nullptr) {
        // Inside a choose_mission pass: plan later, in parallel with the others.
        defer_plan(std::format("leg|{}|{}", bucket, key), {
            .compute = [&planner, &origin, &destination, probe, &ship_class, plan_departure_s, options] {
                return planner.plan_transfer(origin, destination, probe, ship_class, plan_departure_s, options);
            },
            .commit = [this, bucket, key, to_estimate](domain::TrajectoryPlan&& plan) {
                leg_estimates_[bucket].emplace(key, to_estimate(plan));
            },
        });
        return {};
    }
    const auto estimate = to_estimate(planner.plan_transfer(origin, destination, probe, ship_class, plan_departure_s, options));
    slot.emplace(key, estimate);
    return estimate.feasible ? for_caller(estimate) : estimate;
}

void Simulation::defer_plan(const std::string& key, DeferredPlan request) {
    ++missing_plans_;
    if (deferred_keys_.insert(key).second) {
        deferred_plans_->push_back(std::move(request));
    }
}

Simulation::MissionChoice Simulation::choose_mission(
    const domain::ShipState& ship, const domain::ShipClassDefinition& ship_class, bool trace,
    double earliest_departure_s, bool cargo_only) {
    const profiling::Scope profile_scope(profiling::Phase::ChooseMission);
    std::unordered_map<std::string, domain::TrajectoryPlan> plans;
    for (;;) {
        std::vector<DeferredPlan> deferred;
        deferred_plans_ = &deferred;
        deferred_keys_.clear();
        deferred_prefix_.clear();
        std::string trace_text;
        auto choice = choose_mission_pass(ship, ship_class, trace, earliest_departure_s, cargo_only, plans, trace_text);
        deferred_plans_ = nullptr;
        if (deferred.empty()) {
            std::cerr << trace_text;
            return choice;
        }
        std::vector<domain::TrajectoryPlan> results(deferred.size());
        thread_pool_->parallel_for(deferred.size(), [&](std::size_t i) { results[i] = deferred[i].compute(); });
        for (std::size_t i = 0; i < deferred.size(); ++i) {
            deferred[i].commit(std::move(results[i]));
        }
    }
}

std::vector<Simulation::MissionChoice> Simulation::choose_missions(const std::vector<MissionRequest>& requests) {
    const profiling::Scope profile_scope(profiling::Phase::ChooseMission);
    std::vector<std::unordered_map<std::string, domain::TrajectoryPlan>> plans(requests.size());
    std::vector<MissionChoice> choices(requests.size());
    std::vector<bool> done(requests.size(), false);
    for (bool all_done = requests.empty(); !all_done;) {
        std::vector<DeferredPlan> deferred;
        deferred_plans_ = &deferred;
        deferred_keys_.clear();
        all_done = true;
        for (std::size_t r = 0; r < requests.size(); ++r) {
            if (done[r]) {
                continue;
            }
            const auto missing_before = missing_plans_;
            deferred_prefix_ = std::format("{}#", r);
            std::string trace_text;
            const auto& request = requests[r];
            choices[r] = choose_mission_pass(*request.ship, *request.ship_class, false, request.earliest_departure_s,
                request.cargo_only, plans[r], trace_text);
            // A pass that found every plan is final (a plan another request queued first in
            // this pass still counts as missing).
            done[r] = missing_plans_ == missing_before;
            all_done = all_done && done[r];
        }
        deferred_plans_ = nullptr;
        deferred_prefix_.clear();
        std::vector<domain::TrajectoryPlan> results(deferred.size());
        thread_pool_->parallel_for(deferred.size(), [&](std::size_t i) { results[i] = deferred[i].compute(); });
        for (std::size_t i = 0; i < deferred.size(); ++i) {
            deferred[i].commit(std::move(results[i]));
        }
    }
    return choices;
}

Simulation::MissionChoice Simulation::choose_mission_pass(
    const domain::ShipState& ship, const domain::ShipClassDefinition& ship_class, bool trace,
    double earliest_departure_s, bool cargo_only, std::unordered_map<std::string, domain::TrajectoryPlan>& plans,
    std::string& trace_text) {
    // Plans start at the earliest departure; forecasts of stocks count from now.
    const double delay_days = std::max(0.0, earliest_departure_s - game_time_s_) / 86400.0;
    // Traces are kept until the pass that has every plan, so each line is printed once.
    const auto trace_line = [&](const std::string& text) {
        if (trace) {
            trace_text += std::format("[trace day {:.1f}] {} ({}, {}) at {}: {}\n", game_time_s_ / 86400.0, ship.name,
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
    std::vector<domain::CargoLot> best_cargo;
    double best_cargo_margin = 0.0;
    const domain::TrajectoryPlan* best_plan = nullptr;
    double best_carried_kg = 0.0;
    std::string best_pickup_commodity;
    double best_pickup_units = 0.0;
    std::vector<MissionChoice::CargoOption> cargo_options;

    // A newly commissioned ship works the route it was bought for: from home only to its route
    // destination, from anywhere else only home. Otherwise it left once the gap it was bought
    // for closed, the gap reopened and the treasuries ordered another ship for it.
    // A liner (step 22) flies only to the stop after the one it is at, forever.
    const bool liner = !cargo_only && liner_stops(ship) != nullptr;
    const bool on_route = liner || (!cargo_only && !ship.route_destination_id.empty() && game_time_s_ < ship.route_until_s);
    const auto route_leg_allowed = [&](const std::string& from_id, const std::string& to_id) {
        if (liner) {
            return to_id == next_liner_stop(ship, from_id);
        }
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
                  delay_days + plan.travel_time_s / 86400.0);
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
            .include_path = false,  // the chosen mission is planned again with its path
        };
    };
    // What each plan was planned with, so the chosen one can be planned again with its path.
    std::unordered_map<const domain::TrajectoryPlan*, std::pair<trajectory::PlanningOptions, double>> plan_inputs;
    const auto plan_to = [&](const domain::StationDefinition& destination, double cargo_kg) -> const domain::TrajectoryPlan& {
        // Cargo rounded up to whole tonnes keeps the cache small and the estimate safe.
        const double cargo_bucket_kg = std::ceil(std::max(0.0, cargo_kg) / 1000.0) * 1000.0;
        auto key = std::format("{}|{:.0f}", destination.id, cargo_bucket_kg);
        if (const auto it = plans.find(key); it != plans.end()) {
            plan_inputs[&it->second] = {planning_options(cargo_bucket_kg), std::max(game_time_s_, earliest_departure_s)};
            return it->second;
        }
        // Not planned yet: queue it and read it as infeasible for this pass.
        static const domain::TrajectoryPlan pending = [] {
            domain::TrajectoryPlan plan;
            plan.summary = "pending";
            return plan;
        }();
        defer_plan(deferred_prefix_ + "plan|" + key, {
            .compute = [&planner, &origin_def, &destination, &ship, &ship_class,
                           departure_s = std::max(game_time_s_, earliest_departure_s),
                           options = planning_options(cargo_bucket_kg)] {
                return planner.plan_transfer(origin_def, destination, ship, ship_class, departure_s, options);
            },
            .commit = [&plans, key](domain::TrajectoryPlan&& plan) { plans.emplace(key, std::move(plan)); },
        });
        return pending;
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

    // A station's surplus left after the ships already docked there or inbound take their
    // loads, `days_ahead` from now (the producer keeps adding up to its stockpile limit).
    // An inbound ship takes the follow-up load it planned; the others are assumed to fill
    // their holds from the largest remaining surplus. (Before v33 every ship was assumed to
    // take the largest surplus: at Ceres a bulk good, so the platinum looked open to all.)
    const auto open_surplus = [&](const domain::StationDefinition& station_def, const domain::StationState& station_state,
                                  double days_ahead = 0.0) {
        const auto rates = economy_.get_station_net_rates(station_def);
        std::unordered_map<std::string, double> surplus_by_commodity;
        for (const auto& [commodity_id, stock] : station_state.inventory) {
            const double rate = rates.contains(commodity_id) ? rates.at(commodity_id) : 0.0;
            if (rate > 0.0) {
                const double cap = economy_.production_cap_units(station_def, commodity_id, rate);
                const double forecast = std::max(stock, std::min(cap, stock + rate * days_ahead));
                surplus_by_commodity[commodity_id] = forecast - (8.0 + rate * 7.0);
            }
        }
        std::vector<const domain::ShipState*> unplanned;
        for (const auto& other : ships_) {
            if (other.id == ship.id) {
                continue;
            }
            const bool inbound = (other.phase == domain::ShipMissionPhase::InTransit
                || other.phase == domain::ShipMissionPhase::AwaitingDeparture)
                && other.active_mission.destination_station_id == station_def.id;
            const bool docked = other.current_station_id == station_def.id
                && (other.phase == domain::ShipMissionPhase::Idle || other.phase == domain::ShipMissionPhase::Refueling);
            if (inbound && other.active_mission.pickup_units > 0.0) {
                if (const auto it = surplus_by_commodity.find(other.active_mission.pickup_commodity_id);
                    it != surplus_by_commodity.end()) {
                    it->second -= std::max(0.0, std::min(it->second, other.active_mission.pickup_units));
                }
            } else if (inbound || docked) {
                unplanned.push_back(&other);
            }
        }
        for (const auto* other : unplanned) {
            if (surplus_by_commodity.empty()) {
                break;
            }
            auto largest = std::max_element(surplus_by_commodity.begin(), surplus_by_commodity.end(),
                [](const auto& a, const auto& b) { return a.second < b.second; });
            largest->second -= std::max(0.0, std::min(largest->second, get_ship_class(other->class_id).cargo_capacity_units));
        }
        return surplus_by_commodity;
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
        std::string label {};
        std::string commodity_id {};
        double units {0.0};
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
        // A plan's travel time includes its wait for the launch window (until 2026-10-08 the wait
        // was added a second time to every arrival forecast).
        const double follow_days = delay_days + plan.travel_time_s / 86400.0;
        const double departure_kg = std::min(ship_class.propellant_capacity_kg,
            arrival_kg + fuel_for_sale_on_arrival_kg(
                economy_, destination, dest_state, delay_days + plan.travel_time_s / 86400.0));
        auto surplus_by_commodity = open_surplus(destination, dest_state, follow_days);
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
                    .commodity_id = commodity_id,
                    .units = units,
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
                                   FollowUp& follow) {
        double best = -std::numeric_limits<double>::infinity();
        for (const auto& option : follow_ups_from(destination, plan, carried_kg)) {
            if (cargo_follow_up_only && !option.carries_cargo) {
                continue;
            }
            const double score = (leg_profit + option.profit) / std::max(1.0, leg_days + option.days);
            if (score > best) {
                best = score;
                follow = option;
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
            // An export market never starves: Earth's economy takes what arrives.
            const double dest_days_left = destination_rate < 0.0 && !economy_.is_export_market(destination, commodity_id)
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
                    delay_days + travel_days, ship.id);
                const double fuel_cost = (plan.propellant_required_kg / FUEL_UNITS_TO_KG) * origin_fuel_price
                    + fuel_premium(plan, fuel_plan.carried_kg);
                const double cost = trade_value(origin_state, commodity_id, -cargo_units) + fuel_cost
                    + time_cost_per_day * travel_days;
                if (cargo_only) {
                    cargo_options.push_back({.destination = &destination, .commodity_id = commodity_id,
                        .cargo_units = cargo_units, .travel_days = travel_days,
                        .wait_days = plan.wait_time_s / 86400.0, .fuel_cost = fuel_cost});
                }
                FollowUp follow;
                // Cargo-only probes (fleet investment) value the run on its own and skip the
                // follow-up forecast, the expensive part of a dispatch pass.
                const double score = cargo_only
                    ? urgency * (revenue - cost) / std::max(1.0, travel_days)
                    : two_leg_score(destination, plan, fuel_plan.carried_kg, urgency * (revenue - cost), travel_days, false,
                          follow);
                trace_line(std::format("{}: {:.0f} days revenue {:.0f} cost {:.0f} (time {:.0f}), {}: score {:.1f}{}",
                    cargo_label, travel_days, revenue, cost, time_cost_per_day * travel_days, follow.label, score,
                    score > best_score ? " (best so far)" : ""));
                if (score > best_score) {
                    best_score = score;
                    best_destination = &destination;
                    best_cargo = {{.commodity_id = commodity_id, .units = cargo_units}};
                    best_cargo_margin = revenue - trade_value(origin_state, commodity_id, -cargo_units) - fuel_cost;
                    best_plan = &plan;
                    best_carried_kg = fuel_plan.carried_kg;
                    best_pickup_commodity = follow.commodity_id;
                    best_pickup_units = follow.units;
                }
            }
        }
    }

    // Mixed cargo (v37): a hold carries the goods the destination pays most for, chunk by
    // chunk. Each chunk goes to the good whose next units earn the most along both stations'
    // price curves (weighted by how short the destination is of it), so once fuel's price
    // has fallen below food's the next chunk is food, and a big hold is not filled with one
    // good that floods its price. Single-good runs are the candidates above. Fleet
    // investment probes (cargo_only) get each manifest as lots sharing a run number.
    int next_run = 0;
    {
        struct Offer {
            std::string commodity_id;
            double surplus {0.0};
        };
        std::vector<Offer> offers;
        for (const auto& [commodity_id, stock] : origin_state.inventory) {
            const double rate = origin_rates.contains(commodity_id) ? origin_rates.at(commodity_id) : 0.0;
            const double surplus = rate > 0.0 ? std::max(0.0, stock - (8.0 + rate * 7.0)) : 0.0;
            if (surplus > 1.0) {
                offers.push_back({commodity_id, surplus});
            }
        }
        for (const auto& destination : universe_.stations) {
            if (offers.size() < 2) {
                break;
            }
            if (destination.id == origin_def.id || !route_leg_allowed(origin_def.id, destination.id)) {
                continue;
            }
            // The empty-hold transfer dates the forecasts (a laden one differs a little).
            const auto& probe = plan_to(destination, 0.0);
            if (!probe.feasible) {
                continue;
            }
            const double days_ahead = delay_days + probe.travel_time_s / 86400.0;
            const auto& destination_state = get_station_state(destination.id);
            const auto destination_rates = economy_.get_station_net_rates(destination);
            struct Good {
                const Offer* offer {nullptr};
                double stock_on_arrival {0.0};
                double urgency {1.0};
                double loaded {0.0};
                double buy_cost {0.0};  // of `loaded`, along the origin's curve
            };
            std::vector<Good> goods;
            for (const auto& offer : offers) {
                const auto& commodity = get_commodity(offer.commodity_id);
                const double rate = destination_rates.contains(offer.commodity_id) ? destination_rates.at(offer.commodity_id) : 0.0;
                if (rate >= 0.0 && station_price(destination_state, offer.commodity_id) <= commodity.base_price) {
                    continue;
                }
                const auto stock_it = destination_state.inventory.find(offer.commodity_id);
                const double stock = stock_it == destination_state.inventory.end() ? 0.0 : stock_it->second;
                const double days_left = rate < 0.0 && !economy_.is_export_market(destination, offer.commodity_id)
                    ? stock / std::abs(rate) : std::numeric_limits<double>::infinity();
                goods.push_back({
                    .offer = &offer,
                    .stock_on_arrival = forecast_stock_on_arrival(destination_state, offer.commodity_id, days_ahead, ship.id),
                    .urgency = std::clamp(14.0 / std::max(days_left, 0.5), 1.0, 5.0),
                });
            }
            if (goods.size() < 2) {
                continue;
            }
            double hold_left = ship_class.cargo_capacity_units;
            if (destination.storage_capacity_units > 0.0) {
                hold_left = std::min(hold_left, std::max(0.0,
                    destination.storage_capacity_units - economy_.storage_used_units(destination_state.inventory)));
            }
            double credits_left = spendable_credits;
            const double chunk = std::max(1.0, ship_class.cargo_capacity_units / 40.0);
            std::vector<std::pair<std::size_t, double>> picks;  // (good, units) in the order loaded
            while (hold_left >= 0.5) {
                std::size_t best_good = goods.size();
                double best_gain = 0.0;
                double best_units = 0.0;
                double best_buy = 0.0;
                for (std::size_t g = 0; g < goods.size(); ++g) {
                    auto& good = goods[g];
                    const double units = std::min({chunk, good.offer->surplus - good.loaded, hold_left});
                    if (units < 0.5) {
                        continue;
                    }
                    const auto& commodity = get_commodity(good.offer->commodity_id);
                    const double sale = sale_split(destination, commodity.id, good.stock_on_arrival + good.loaded, units).total;
                    const double buy = trade_value(origin_state, commodity.id, -(good.loaded + units)) - good.buy_cost;
                    const double gain = good.urgency * (sale - buy) / units;
                    if (sale > buy && gain > best_gain && buy <= credits_left) {
                        best_good = g;
                        best_gain = gain;
                        best_units = units;
                        best_buy = buy;
                    }
                }
                if (best_good == goods.size()) {
                    break;
                }
                goods[best_good].loaded += best_units;
                goods[best_good].buy_cost += best_buy;
                hold_left -= best_units;
                credits_left -= best_buy;
                picks.emplace_back(best_good, best_units);
            }
            // The whole manifest, and its first half and quarter: a full hold may be too heavy.
            for (const double fraction : LOAD_FRACTIONS) {
                const auto count = static_cast<std::size_t>(std::ceil(static_cast<double>(picks.size()) * fraction));
                std::vector<double> units_by_good(goods.size(), 0.0);
                for (std::size_t i = 0; i < count; ++i) {
                    units_by_good[picks[i].first] += picks[i].second;
                }
                std::vector<domain::CargoLot> lots;
                double cargo_kg = 0.0;
                for (std::size_t g = 0; g < goods.size(); ++g) {
                    if (units_by_good[g] > 0.0) {
                        lots.push_back({.commodity_id = goods[g].offer->commodity_id, .units = units_by_good[g]});
                        cargo_kg += units_by_good[g] * get_commodity(goods[g].offer->commodity_id).mass_per_unit_kg;
                    }
                }
                if (lots.size() < 2) {
                    continue;
                }
                const auto cargo_label = std::format("mixed {} -> {}", describe_cargo(lots), destination.id);
                const auto fuel_plan = plan_with_return_fuel(destination, cargo_kg);
                const auto& plan = *fuel_plan.plan;
                if (!plan.feasible || !fuel_plan.feasible) {
                    trace_line(cargo_label + ": no feasible trajectory, or no fuel to leave again");
                    continue;
                }
                const double travel_days = plan.travel_time_s / 86400.0;
                if (travel_days > max_mission_days) {
                    continue;
                }
                const double fuel_cost = (plan.propellant_required_kg / FUEL_UNITS_TO_KG) * origin_fuel_price
                    + fuel_premium(plan, fuel_plan.carried_kg);
                if (cargo_only) {
                    const int run = next_run++;
                    for (std::size_t l = 0; l < lots.size(); ++l) {
                        cargo_options.push_back({.destination = &destination, .commodity_id = lots[l].commodity_id,
                            .cargo_units = lots[l].units, .travel_days = travel_days,
                            .wait_days = plan.wait_time_s / 86400.0, .fuel_cost = l == 0 ? fuel_cost : 0.0, .run = run});
                    }
                    continue;
                }
                const double trip_cost = fuel_cost + time_cost_per_day * travel_days;
                const double total_units = domain::total_units(lots);
                double revenue = 0.0;
                double purchases = 0.0;
                double weighted = 0.0;
                for (const auto& lot : lots) {
                    const auto& commodity = get_commodity(lot.commodity_id);
                    const double surviving = lot.units * std::pow(1.0 - commodity.decay_fraction_per_day, travel_days);
                    const double lot_revenue = sale_value_on_arrival(destination_state, lot.commodity_id, surviving,
                        delay_days + travel_days, ship.id);
                    const double lot_cost = trade_value(origin_state, lot.commodity_id, -lot.units);
                    revenue += lot_revenue;
                    purchases += lot_cost;
                    // As for one good: its margin, less its share of the trip, weighted by urgency.
                    double urgency = 1.0;
                    for (const auto& good : goods) {
                        if (good.offer->commodity_id == lot.commodity_id) {
                            urgency = good.urgency;
                        }
                    }
                    weighted += urgency * (lot_revenue - lot_cost - trip_cost * lot.units / total_units);
                }
                FollowUp follow;
                const double score = two_leg_score(destination, plan, fuel_plan.carried_kg, weighted, travel_days, false, follow);
                trace_line(std::format("{}: {:.0f} days revenue {:.0f} cost {:.0f} (trip {:.0f}), {}: score {:.1f}{}",
                    cargo_label, travel_days, revenue, purchases + trip_cost, trip_cost, follow.label, score,
                    score > best_score ? " (best so far)" : ""));
                if (score > best_score) {
                    best_score = score;
                    best_destination = &destination;
                    best_cargo = lots;
                    best_cargo_margin = revenue - purchases - fuel_cost;
                    best_plan = &plan;
                    best_carried_kg = fuel_plan.carried_kg;
                    best_pickup_commodity = follow.commodity_id;
                    best_pickup_units = follow.units;
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
        FollowUp follow;
        const double score = two_leg_score(destination, plan, fuel_plan.carried_kg, -leg_cost, leg_days, true, follow);
        if (follow.label.empty()) {
            continue;
        }
        trace_line(std::format("empty -> {}: {:.0f} days cost {:.0f}, {}: score {:.1f}{}",
            destination.id, leg_days, leg_cost, follow.label, score, score > best_score ? " (best so far)" : ""));
        if (score > best_score) {
            best_score = score;
            best_destination = &destination;
            best_cargo.clear();
            best_cargo_margin = 0.0;
            best_plan = &plan;
            best_carried_kg = fuel_plan.carried_kg;
            best_pickup_commodity = follow.commodity_id;
            best_pickup_units = follow.units;
        }
    }

    // A liner with nothing that pays for its next leg flies it empty: its value is the schedule.
    if (best_destination == nullptr && liner) {
        const auto& next = get_station_definition(next_liner_stop(ship, origin_def.id));
        const auto fuel_plan = plan_with_return_fuel(next, 0.0);
        if (fuel_plan.plan->feasible && fuel_plan.feasible && fuel_plan.plan->travel_time_s / 86400.0 <= max_mission_days) {
            trace_line("liner: next stop empty to " + next.id);
            best_pickup_commodity.clear();
            best_pickup_units = 0.0;
            best_score = 0.0;
            best_destination = &next;
            best_plan = fuel_plan.plan;
            best_carried_kg = fuel_plan.carried_kg;
        }
    }

    // A ship on its route away from home with nothing to carry back flies home empty.
    if (best_destination == nullptr && on_route && !liner && origin_def.id != ship.home_station_id) {
        const auto& home = get_station_definition(ship.home_station_id);
        const auto& plan = plan_to(home, 0.0);
        if (plan.feasible && plan.travel_time_s / 86400.0 <= max_mission_days) {
            trace_line("on route: back home empty to " + home.id);
            best_pickup_commodity.clear();
            best_pickup_units = 0.0;
            best_score = 0.0;
            best_destination = &home;
            best_plan = &plan;
            best_carried_kg = 0.0;
        }
    }

    bool repositioning = false;
    if (best_destination == nullptr && !cargo_only && !on_route) {
        repositioning = true;
        best_pickup_commodity.clear();
        best_pickup_units = 0.0;
        best_score = 0.0;  // repositioning scores are in urgency units, not credits/day

        // Sourcing score: does this station have surplus goods urgently needed
        // elsewhere? An empty ship is only useful where there is something to
        // pick up, so repositioning targets producers; starving stations are
        // served by the cargo loop's urgency weighting once a ship is loaded.
        const auto sourcing_score_for = [&](const domain::StationDefinition& station_def,
                                            const domain::StationState& station_state) {
            double sourcing_score = 0.0;
            for (const auto& [commodity_id, surplus] : open_surplus(station_def, station_state)) {
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
                    const double days_rem = economy_.is_export_market(other, commodity_id) ? 30.0
                        : other_stock > 0.0 ? other_stock / std::abs(other_rate) : 0.0;
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
            // What the ships already there or on their way leave of it.
            double best_value = 0.0;
            for (const auto& [commodity_id, surplus] : open_surplus(station_def, station_state)) {
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
                        delay_days + reposition_days)));
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
                best_cargo.clear();
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
    choice.cargo = best_cargo;
    choice.cargo_margin = best_cargo_margin;
    choice.plan = *best_plan;
    if (const auto it = plan_inputs.find(best_plan); it != plan_inputs.end()) {
        choice.plan_options = it->second.first;
        choice.plan_departure_s = it->second.second;
    }
    choice.carried_propellant_kg = best_carried_kg;
    choice.pickup_commodity_id = best_pickup_commodity;
    choice.pickup_units = best_pickup_units;
    return choice;
}

void Simulation::step_idle_ship(domain::ShipState& ship) {
    const profiling::Scope profile_scope(profiling::Phase::ShipReview);
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
    const auto review_start = std::chrono::steady_clock::now();
    const auto choice = choose_mission(ship, ship_class, trace, game_time_s_);
    // Debug aid: with SPACETRAINS_PROFILE, reviews over half a second are reported (stderr).
    if (profiling::enabled()) {
        const double review_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - review_start).count();
        if (review_s > 0.5) {
            std::cerr << std::format("[slow review day {:.1f}] {} ({}) at {}: {:.2f} s\n", game_time_s_ / 86400.0, ship.name,
                ship_class.id, ship.current_station_id, review_s);
        }
    }
    if (liner_stops(ship) == nullptr && consider_refit(ship, choice, trace)) {
        return;
    }

    auto& origin_state = get_station_state(ship.current_station_id);
    const auto& origin_def = get_station_definition(ship.current_station_id);
    if (choice.kind == MissionChoice::Kind::None) {
        if (ship.propellant_kg <= ship_class.propellant_capacity_kg * 0.01 && purchasable_propellant_kg(ship) <= 0.0) {
            ship.phase = domain::ShipMissionPhase::Stranded;
            add_event(std::format("{} is stranded at {} due to fuel shortage", ship.name, origin_def.name), "alert");
        } else if (!laid_up && liner_stops(ship) == nullptr
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
    const auto& cargo = choice.cargo;
    const double best_cargo_units = domain::total_units(cargo);
    // Candidates are planned without a rendering path; plan the chosen one again with it
    // (same inputs, so the same transfer; a variable-Isp check on the denser path can still
    // reject it, and the ship then waits for its next review).
    domain::TrajectoryPlan plan = choice.plan;
    if (!plan.has_render_path) {
        auto options = choice.plan_options;
        options.include_path = true;
        const auto& planner = (ship_class.propulsion_type == "variable_isp" && variable_isp_planner_)
            ? static_cast<trajectory::ITrajectoryPlanner&>(*variable_isp_planner_)
            : static_cast<trajectory::ITrajectoryPlanner&>(*kepler_planner_);
        plan = planner.plan_transfer(origin_def, *best_destination, ship, ship_class, choice.plan_departure_s, options);
    }
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
    const auto& destination_state = get_station_state(best_destination->id);
    // Contracts: each lot's price is agreed now, from the destination's forecast stock on
    // arrival (other ships' earlier deliveries included), for the units that will survive the
    // trip. The ship knows what it will earn; the station bears the forecast's error.
    auto contracted_cargo = cargo;
    const double arrival_days = std::max(0.0, plan.arrival_time_s - game_time_s_) / 86400.0;
    for (auto& lot : contracted_cargo) {
        const double surviving = lot.units
            * std::pow(1.0 - get_commodity(lot.commodity_id).decay_fraction_per_day, plan.travel_time_s / 86400.0);
        const auto split = sale_split_on_arrival(destination_state, lot.commodity_id, surviving, arrival_days, ship.id);
        lot.contract_value = split.total;
        lot.emergency_premium = split.faction;
        station_payable_[best_destination->id] -= contract_reserve_share(plan.arrival_time_s) * (split.total - split.faction);
        if (split.faction > 0.0) {
            for (auto& emergency : emergencies_) {
                if (emergency.station_id == best_destination->id && emergency.commodity_id == lot.commodity_id) {
                    emergency.last_raise_s = game_time_s_;
                    emergency.units_open = std::max(0.0, emergency.units_open - split.emergency_units);
                }
            }
            add_event(std::format("{} takes an emergency delivery of {:.0f}u {} to {} ({:.0f} cr from {})",
                ship.name, surviving, get_commodity(lot.commodity_id).name, best_destination->name, split.faction,
                faction_name(best_destination->faction_id)), "alert");
        }
    }
    for (const auto& lot : cargo) {
        // Buy at origin along the price curve as the stock falls.
        const double cost = trade_value(origin_state, lot.commodity_id, -lot.units);
        origin_state.inventory[lot.commodity_id] -= lot.units;
        origin_state.export_units_per_day[lot.commodity_id] += lot.units / TRADE_FLOW_DAYS;
        ship.credits -= cost;
        ship.lifetime_profit -= cost;
        ship.ledger.cargo_purchases += cost;
        origin_state.credits += cost;
        purchase_cost += cost;
        record_trade({
            .time_s = game_time_s_,
            .ship_id = ship.id,
            .station_id = origin_state.station_id,
            .commodity_id = lot.commodity_id,
            .kind = "buy",
            .units = lot.units,
            .unit_price = cost / lot.units,
            .total = cost,
        });
    }
    for (const auto& lot : contracted_cargo) {
        expected_revenue += lot.contract_value;
    }
    if (liner_stops(ship) != nullptr) {
        add_event(std::format("{} (liner) leaves {} on schedule for {}, departing day {:.0f}, arriving day {:.0f}{}",
            ship.name, origin_def.name, best_destination->name, plan.departure_time_s / 86400.0,
            plan.arrival_time_s / 86400.0, cargo.empty() ? " (empty)" : ""), "mission");
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
        .cargo = contracted_cargo,
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
        .pickup_commodity_id = choice.pickup_commodity_id,
        .pickup_units = choice.pickup_units,
    };
    if (choice.carried_propellant_kg > 0.0) {
        add_event(std::format("{} carries {:.0f} kg of return fuel to {}", ship.name, choice.carried_propellant_kg,
            best_destination->name), "fuel");
    }
    if (plan.wait_time_s > 0.0) {
        if (best_cargo_units > 0.0) {
            add_event(std::format(
                "{} scheduled {}->{} ({}) in {:.1f}d  est. profit {:.0f} cr ({:.1f}d travel)",
                ship.name,
                origin_def.name,
                best_destination->name,
                describe_cargo(cargo),
                plan.wait_time_s / 86400.0,
                expected_revenue - purchase_cost,
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
            "{} departed {}->{} ({})  est. profit {:.0f} cr ({:.1f}d transit, prop={:.0f}kg)",
            ship.name,
            origin_def.name,
            best_destination->name,
            describe_cargo(cargo),
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
    const profiling::Scope profile_scope(profiling::Phase::ConsiderRefit);
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
    if (!ship.active_mission.cargo.empty()) {
        add_event(std::format("{} departed {} for {} carrying {}", ship.name, origin.name, destination.name,
            describe_cargo(ship.active_mission.cargo)), "mission");
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
    if (!ship.active_mission.cargo.empty()) {
        const auto& destination_def = get_station_definition(ship.current_station_id);
        const double transit_days = ship.active_mission.total_travel_time_s / 86400.0;
        // Deliveries beyond the destination's free storage are jettisoned.
        double free_capacity = destination_def.storage_capacity_units > 0.0
            ? std::max(0.0, destination_def.storage_capacity_units - economy_.storage_used_units(destination.inventory))
            : std::numeric_limits<double>::infinity();
        double total_revenue = 0.0;
        double total_spoiled = 0.0;
        std::vector<domain::CargoLot> sold;
        for (const auto& lot : ship.active_mission.cargo) {
            // Some goods (food, medicine) spoil in transit.
            const double decay_per_day = get_commodity(lot.commodity_id).decay_fraction_per_day;
            const double surviving = decay_per_day > 0.0 ? std::max(0.0, std::pow(1.0 - decay_per_day, transit_days)) : 1.0;
            double arrived = lot.units * surviving;
            total_spoiled += lot.units - arrived;
            if (arrived > free_capacity) {
                add_event(std::format("{} jettisoned {:.1f}u {} at {} — storage full",
                    ship.name, arrived - free_capacity, lot.commodity_id, destination_def.name), "alert");
                arrived = free_capacity;
            }
            free_capacity -= arrived;
            // The agreed price for what arrives (less if storage forced a jettison), or, without a
            // contract, the market's along the price curve as the delivery lands.
            const double expected_units = lot.units * surviving;
            const double delivered_share = expected_units > 0.0 ? std::min(1.0, arrived / expected_units) : 0.0;
            const double revenue = lot.contract_value > 0.0 && expected_units > 0.0
                ? lot.contract_value * delivered_share
                : trade_value(destination, lot.commodity_id, arrived);
            // An emergency premium comes from the station's faction, the rest from the station.
            const double faction_share = lot.emergency_premium * delivered_share;
            if (faction_share > 0.0) {
                faction_treasuries_[destination_def.faction_id] -= faction_share;
                emergency_paid_ += faction_share;
                for (auto& emergency : emergencies_) {
                    if (emergency.station_id == destination.station_id && emergency.commodity_id == lot.commodity_id) {
                        emergency.faction_paid += faction_share;
                    }
                }
            }
            // The station's import cost: what it paid per unit, averaged over about 30 days of its use.
            if (arrived > 0.0 && !economy_.is_export_market(destination_def, lot.commodity_id)) {
                const auto rates = economy_.get_station_net_rates(destination_def);
                const auto rate = rates.find(lot.commodity_id);
                const double history = rate == rates.end() ? 0.0 : std::max(0.0, -rate->second) * IMPORT_COST_DAYS;
                const auto known = destination.import_unit_cost.find(lot.commodity_id);
                const double previous = known == destination.import_unit_cost.end()
                    ? economy_.reference_price(destination_def, lot.commodity_id) : known->second;
                destination.import_unit_cost[lot.commodity_id] =
                    (previous * history + (revenue - faction_share)) / (history + arrived);
            }
            destination.inventory[lot.commodity_id] += arrived;
            destination.import_units_per_day[lot.commodity_id] += arrived / TRADE_FLOW_DAYS;
            destination.credits -= revenue - faction_share;
            ship.credits += revenue;
            ship.lifetime_profit += revenue;
            ship.ledger.cargo_revenue += revenue;
            total_revenue += revenue;
            if (arrived > 0.0) {
                record_trade({
                    .time_s = game_time_s_,
                    .ship_id = ship.id,
                    .station_id = destination.station_id,
                    .commodity_id = lot.commodity_id,
                    .kind = "sell",
                    .units = arrived,
                    .unit_price = revenue / arrived,
                    .total = revenue,
                });
                sold.push_back({.commodity_id = lot.commodity_id, .units = arrived});
            }
        }
        add_event(total_spoiled > 0.1
            ? std::format("{} arrived at {} with {} for {:.0f} cr ({:.1f}u spoiled in {:.0f}d transit)",
                  ship.name, destination_def.name, describe_cargo(sold), total_revenue, total_spoiled, transit_days)
            : std::format("{} arrived at {} and sold {} for {:.0f} cr", ship.name, destination_def.name,
                  describe_cargo(sold), total_revenue),
            "arrival");
    } else {
        add_event(std::format("{} arrived at {}", ship.name, get_station_definition(ship.current_station_id).name), "arrival");
    }
    ship.active_mission = {};
}

void Simulation::step(double real_dt_s) {
    untimed_s_ += real_dt_s * timewarp_factor_;
    // A millisecond of slack absorbs rounding (0.1 s x 86400 is not exactly 8640 s).
    while (untimed_s_ >= TICK_S - 1e-3) {
        untimed_s_ = std::max(0.0, untimed_s_ - TICK_S);
        tick();
    }
}

void Simulation::tick() {
    const profiling::Scope profile_scope(profiling::Phase::Step);
    const double dt_s = TICK_S;
    game_time_s_ += dt_s;
    std::vector<domain::Inventory> stocks_before;
    stocks_before.reserve(stations_.size());
    for (const auto& station : stations_) {
        stocks_before.push_back(station.inventory);
    }
    {
        const profiling::Scope economy_scope(profiling::Phase::EconomyStep);
        economy_.step(stations_, dt_s);
    }
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
    step_population(dt_s);
    step_events(dt_s);
    step_treasuries(dt_s);
    step_emergencies();
    refresh_station_payable();
    record_affordability();
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
    // A station and its residents are one account (step 17): local production costs the station
    // nothing (before, it paid local producers up to the base price for new output, and a
    // producer such as Venus that sold its glut to ships for less went broke). Residents pay for
    // what they use up, with income from outside like wages, at what their station paid ships
    // for it (the reference price until a delivery): before step 17 they paid the scarcity
    // curve, up to 16x for goods nobody had delivered, the main source of new money; paying
    // only the reference price left every chronically short station losing money on each import.
    for (std::size_t i = 0; i < stations_.size(); ++i) {
        auto& station = stations_[i];
        const auto& definition = get_station_definition(station.station_id);
        for (const auto& [commodity_id, after] : station.inventory) {
            const auto before_it = stocks_before[i].find(commodity_id);
            const double before = before_it == stocks_before[i].end() ? 0.0 : before_it->second;
            const double used = before - after;
            if (used <= 0.0) {
                continue;
            }
            const auto cost = station.import_unit_cost.find(commodity_id);
            const double retail = cost == station.import_unit_cost.end()
                ? economy_.reference_price(definition, commodity_id) : cost->second;
            const double value = retail * used;
            station.credits += value;
            station.ledger.household_sales += value;
            outside_economy_credits_ -= value;
        }
    }
}

// Step 27: residents come while their station keeps them supplied and pays its way, and leave
// while it does not (see GrowthDefinition). Rates follow the population, so a grown station
// needs more and makes more; growth stops by itself where supply cannot keep up.
void Simulation::step_population(double dt_s) {
    const auto& growth = universe_.growth;
    const double dt_days = dt_s / 86400.0;
    for (auto& station : stations_) {
        const auto& definition = get_station_definition(station.station_id);
        double worst = 1.0;
        for (const auto& [commodity_id, available] : station.upkeep_availability) {
            worst = std::min(worst, available);
        }
        station.supply_index += (worst - station.supply_index) * std::min(1.0, dt_days / growth.window_days);
        double rate = 0.0;
        if (station.supply_index >= growth.grow_above && station.credits >= universe_.open_economy.station_credit_floor) {
            rate = growth.growth_per_year;
        } else if (station.supply_index < growth.decline_below) {
            rate = -growth.decline_per_year;
        }
        const double seeded = static_cast<double>(definition.population);
        const double floor = std::max(universe_.open_economy.core_crew_fraction, 0.0) * seeded;
        const double before = station.population;
        station.population = std::clamp(station.population * std::exp(rate * dt_days / 365.25),
            std::min(floor, seeded), growth.max_population_factor * seeded);
        if (station.population != before) {
            economy_.set_population(station.station_id, station.population);
        }
        const double announced = station.population_announced > 0.0 ? station.population_announced : seeded;
        if (std::abs(station.population / announced - 1.0) >= 0.05) {
            add_event(station.population > announced
                    ? std::format("{} grows to {:.0f} residents: its people are well supplied", definition.name, station.population)
                    : std::format("{} shrinks to {:.0f} residents as people leave for better-supplied stations",
                        definition.name, station.population),
                "news");
            station.population_announced = station.population;
        }
    }
}

// Step 28: events end when due, and each idle one strikes its station with probability
// rate x dt (one draw per event per tick, so the sequence depends only on the seed).
void Simulation::step_events(double dt_s) {
    for (auto& station : stations_) {
        std::erase_if(station.events, [&](const domain::ActiveEvent& active) {
            if (active.end_s > game_time_s_) {
                return false;
            }
            const auto it = std::find_if(universe_.events.begin(), universe_.events.end(),
                [&](const auto& event) { return event.id == active.event_id; });
            if (it != universe_.events.end() && !it->end_headline.empty()) {
                add_event(it->end_headline, "news");
            }
            return true;
        });
    }
    if (!events_enabled_) {
        return;
    }
    const double dt_years = dt_s / 86400.0 / 365.25;
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    for (const auto& event : universe_.events) {
        const double roll = uniform(event_rng_);
        const double length = uniform(event_rng_);
        if (roll >= event.rate_per_year * dt_years) {
            continue;
        }
        auto& station = get_station_state(event.station_id);
        if (std::any_of(station.events.begin(), station.events.end(),
                [&](const auto& active) { return active.event_id == event.id; })) {
            continue;
        }
        ++events_started_[event.id];
        add_event(event.headline, "news");
        if (event.kind == domain::EventKind::Migration) {
            const auto& definition = get_station_definition(station.station_id);
            station.population = std::min(station.population * (1.0 + event.magnitude),
                universe_.growth.max_population_factor * static_cast<double>(definition.population));
            station.population_announced = station.population;
            economy_.set_population(station.station_id, station.population);
            continue;
        }
        const double days = event.min_days + (event.max_days - event.min_days) * length;
        station.events.push_back({.event_id = event.id, .start_s = game_time_s_, .end_s = game_time_s_ + days * 86400.0});
    }
}

double Simulation::fleet_hold_units() const {
    double units = 0.0;
    for (const auto& ship : ships_) {
        if (liner_stops(ship) == nullptr) {  // liners are a public service, outside the cap
            units += get_ship_class(ship.class_id).cargo_capacity_units;
        }
    }
    return units;
}

std::size_t Simulation::fleet_ship_count() const {
    return static_cast<std::size_t>(std::count_if(ships_.begin(), ships_.end(),
        [&](const domain::ShipState& ship) { return liner_stops(ship) == nullptr; }));
}

const std::vector<std::string>* Simulation::liner_stops(const domain::ShipState& ship) const {
    const auto it = universe_.liners.find(ship.id);
    return it == universe_.liners.end() ? nullptr : &it->second;
}

std::string Simulation::next_liner_stop(const domain::ShipState& ship, const std::string& from_id) const {
    const auto* stops = liner_stops(ship);
    if (stops == nullptr) {
        return {};
    }
    const auto at = std::find(stops->begin(), stops->end(), from_id);
    return at == stops->end() ? stops->front() : stops->at(static_cast<std::size_t>(at - stops->begin() + 1) % stops->size());
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
    const profiling::Scope profile_scope(profiling::Phase::Treasuries);
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

    // A liner keeps its schedule whatever it earns: its faction tops its cash up to the working
    // reserve when it runs out (step 22).
    for (auto& ship : ships_) {
        if (ship.credits < 0.0 && liner_stops(ship) != nullptr) {
            const double top_up = open.ship_cash_reserve - ship.credits;
            ship.credits += top_up;
            ship.ledger.subsidies += top_up;
            faction_treasuries_[ship.faction_id] -= top_up;
        }
    }

    // The slow controller hands the money-supply gap to the stations per head of population,
    // as subsidies (or taxes when there is too much money).
    double per_capita = 0.0;
    if (open.money_supply_days > 0.0) {
        double population = 0.0;
        for (const auto& station : stations_) {
            population += station.population;
        }
        if (population > 0.0) {
            per_capita = (seeded_money_supply_ - internal_money_supply()) * std::min(1.0, dt_days / open.money_supply_days)
                / population;
        }
    }

    // Faction treasuries close the gap of stations outside the credit band: they tax the excess
    // above the ceiling, and top up a station below the floor: while the faction borrows, by at
    // most the upkeep of its core crew (step 17), so a station that cannot pay its way gets less
    // than it spends; out of its savings, by the whole gap (step 26).
    const double band_share = std::min(1.0, dt_days / open.station_balance_days);
    const double tax_decay = std::exp(-dt_days / 365.0);
    for (auto& [faction_id, per_day] : faction_tax_per_day_) {
        per_day *= tax_decay;
    }
    for (auto& station : stations_) {
        const auto& definition = get_station_definition(station.station_id);
        double transfer = 0.0;
        if (station.credits < open.station_credit_floor) {
            transfer = (open.station_credit_floor - station.credits) * band_share;
            if (open.core_crew_fraction > 0.0) {
                // A faction in surplus covers the whole gap out of its savings (step 26): the
                // core-crew cap left outposts deep in debt for the stock step 23 has them hold,
                // while their faction's taxes piled up unused.
                const double savings = std::max(0.0, faction_treasuries_[definition.faction_id]);
                transfer = std::min(transfer,
                    std::max(open.core_crew_fraction * consumption_value_per_day(definition, true) * dt_days, savings));
            }
        } else if (open.station_credit_ceiling > 0.0 && station.credits > open.station_credit_ceiling) {
            transfer = -(station.credits - open.station_credit_ceiling) * band_share;
        }
        // The controller never pushes a station out of the band, or it would fight the band.
        const double after_band = station.credits + transfer;
        const double controller = per_capita * station.population;
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
            faction_tax_per_day_[definition.faction_id] += -transfer / 365.0;
        }
    }

    // A faction in debt pays interest to the outside economy (Earth's lenders).
    const double rate = universe_.ship_operations.interest_rate_per_year;
    for (auto& [faction_id, balance] : faction_treasuries_) {
        if (balance < 0.0 && rate > 0.0) {
            const double interest = -balance * rate * dt_days / 365.0;
            balance -= interest;
            outside_economy_credits_ += interest;
            faction_interest_paid_ += interest;
        }
    }
}

double Simulation::consumption_value_per_day(const domain::StationDefinition& station, bool upkeep_only) const {
    double value = 0.0;
    for (const auto& [commodity_id, rate] : economy_.get_station_net_rates(station)) {
        if (rate < 0.0 && !economy_.is_export_market(station, commodity_id)
            && (!upkeep_only || economy_.is_upkeep(station, commodity_id))) {
            value += -rate * economy_.reference_price(station, commodity_id);
        }
    }
    return value;
}

double Simulation::faction_credit_limit(const std::string& faction_id) const {
    const auto& open = universe_.open_economy;
    double fleet_value = 0.0;
    for (const auto& ship : ships_) {
        if (ship.faction_id == faction_id) {
            fleet_value += get_ship_class(ship.class_id).ship_value_cr;
        }
    }
    // Its stations' stock is collateral too, at reference prices (step 26): after step 23 a
    // remote colony stocks a year or more of its imports, which its faction paid for; Mars
    // Corporation, with one station and few ships, went over its limit building that stock.
    double stock_value = 0.0;
    for (const auto& station : stations_) {
        const auto& definition = get_station_definition(station.station_id);
        if (definition.faction_id != faction_id) {
            continue;
        }
        for (const auto& [commodity_id, units] : station.inventory) {
            stock_value += std::max(0.0, units) * economy_.reference_price(definition, commodity_id);
        }
    }
    const auto tax = faction_tax_per_day_.find(faction_id);
    return open.faction_loan_to_value * (fleet_value + stock_value)
        + open.faction_tax_years * 365.0 * (tax == faction_tax_per_day_.end() ? 0.0 : tax->second);
}

double Simulation::faction_spendable(const std::string& faction_id) const {
    const auto it = faction_treasuries_.find(faction_id);
    return (it == faction_treasuries_.end() ? 0.0 : it->second) + faction_credit_limit(faction_id);
}

void Simulation::refresh_station_payable() {
    const auto& open = universe_.open_economy;
    station_payable_.clear();
    for (const auto& station : stations_) {
        const auto& definition = get_station_definition(station.station_id);
        station_payable_[station.station_id] = station.credits
            + open.station_credit_days * consumption_value_per_day(definition, false);
    }
    // Contracts on their way reserve the station's money in full when due within the credit
    // window, and in proportion (window / days to arrival) when later: those are paid partly
    // out of the income until then. (Counting every contract in full left Low Earth Logistics
    // unable to pay for food from Earth L1, 0.1 days away, because freighters from Ganymede
    // were booked to arrive in years; counting only those due within the window let Mars,
    // 200 days from its suppliers, order 1.3M cr of goods it could not pay for.)
    for (const auto& ship : ships_) {
        if (ship.phase != domain::ShipMissionPhase::InTransit && ship.phase != domain::ShipMissionPhase::AwaitingDeparture) {
            continue;
        }
        const double share = contract_reserve_share(ship.active_mission.arrival_time_s);
        for (const auto& lot : ship.active_mission.cargo) {
            station_payable_[ship.active_mission.destination_station_id] -= share * (lot.contract_value - lot.emergency_premium);
        }
    }
}

void Simulation::record_affordability() {
    for (auto& station : stations_) {
        station.affordability = station_affordability(get_station_definition(station.station_id));
    }
}

double Simulation::contract_reserve_share(double arrival_time_s) const {
    const double window_days = universe_.open_economy.station_credit_days;
    const double days = (arrival_time_s - game_time_s_) / 86400.0;
    return days <= window_days ? 1.0 : window_days / days;
}

double Simulation::station_affordability(const domain::StationDefinition& station) const {
    const double need = universe_.open_economy.affordability_days * consumption_value_per_day(station, false);
    const auto it = station_payable_.find(station.id);
    if (need <= 0.0 || it == station_payable_.end()) {
        return 1.0;
    }
    return std::clamp(it->second / need, 0.0, 1.0);
}

void Simulation::step_fleet_investment() {
    const profiling::Scope profile_scope(profiling::Phase::FleetInvestment);
    const auto& investment = universe_.fleet_investment;
    if (investment.layup_sale_days > 0.0) {
        for (std::size_t i = ships_.size(); i-- > 0;) {
            const auto& ship = ships_[i];
            if (ship.phase == domain::ShipMissionPhase::LaidUp && liner_stops(ship) == nullptr
                && game_time_s_ - ship.laid_up_since_s >= investment.layup_sale_days * 86400.0) {
                sell_ship(i);
            }
        }
    }
    if (investment.review_days > 0.0 && game_time_s_ >= next_investment_review_s_) {
        next_investment_review_s_ += investment.review_days * 86400.0;
        investment_purchases_left_ = static_cast<int>(investment.max_ships_per_review);
        review_probes_.clear();
    }
    // One purchase per tick (each takes a second or two of probing), so a review never
    // stalls the simulation for long. Each purchase is a committed flow and keeps its yard
    // busy, so the next one is valued against the demand still open.
    // The fleet is limited by its total hold (step 21: a cap on ship numbers made a cheap local
    // shuttle cost a place as much as a 2,000 u freighter, so none was bought), with a ship
    // count as a backstop for run time. Laid-up ships sold for salvage free their places.
    if ((investment.max_fleet_size > 0.0 && static_cast<double>(fleet_ship_count()) >= investment.max_fleet_size)
        || (investment.max_fleet_hold_units > 0.0 && fleet_hold_units() >= investment.max_fleet_hold_units)) {
        investment_purchases_left_ = 0;
    }
    // One batch of probes or one purchase per tick: a review never stalls the simulation for
    // long (probing every candidate at once took up to six seconds with ninety ships).
    if (investment_purchases_left_ > 0) {
        switch (commission_step()) {
            case CommissionStep::Probed:
                break;
            case CommissionStep::Bought:
                --investment_purchases_left_;
                break;
            case CommissionStep::NothingToBuy:
                investment_purchases_left_ = 0;
                break;
        }
    }
    if (investment_purchases_left_ <= 0) {
        review_probes_.clear();
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

Simulation::CommissionStep Simulation::commission_step() {
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
    // Treasuries pay for hulls with their cash and what they may borrow (step 17).
    double richest_treasury = 0.0;
    for (const auto& [faction_id, balance] : faction_treasuries_) {
        richest_treasury = std::max(richest_treasury, faction_spendable(faction_id));
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
        double rank_bound {0.0};  // in the review's ranking: return per year, or profit per day
    };
    // Far from the fleet limit the treasuries rank candidates by return on their price; from
    // half the limit on, hold space in the fleet is the scarce thing, so by profit per day per
    // unit of hold (above the hurdle): what a ship earns for the share of the cap it takes.
    const double fleet_hold = fleet_hold_units();
    // Under a hold cap a candidate competes per unit of hold it takes; under a ship cap, per ship.
    const bool per_hold_unit = investment.max_fleet_hold_units > 0.0;
    const bool rank_by_profit = investment.max_fleet_hold_units > 0.0
        ? fleet_hold >= 0.5 * investment.max_fleet_hold_units
        : investment.max_fleet_size > 0.0 && static_cast<double>(fleet_ship_count()) >= 0.5 * investment.max_fleet_size;
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
                || ship_class.ship_value_cr + investment.working_capital > richest_treasury
                || (investment.max_fleet_hold_units > 0.0
                    && fleet_hold + ship_class.cargo_capacity_units > investment.max_fleet_hold_units)) {
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
                candidates.push_back({.ship_class = &ship_class, .yard = &yard, .return_bound = return_bound,
                    .rank_bound = rank_by_profit
                        ? (margin_bound - running_cost) / (per_hold_unit ? std::max(1.0, ship_class.cargo_capacity_units) : 1.0)
                        : return_bound});
            }
        }
    }
    // Probe the most promising first (plasma probes take seconds); stop once the best return
    // found beats every remaining bound. Sale prices are forecast at arrival, where a starving
    // consumer's price can be a little above today's, so the bound is a heuristic.
    std::sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) { return a.rank_bound > b.rank_bound; });
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
        // travel_days includes the wait for the launch window: a cycle is the wait plus the
        // transfer out and back (until 2026-10-08 the wait was counted three times).
        const double transfer_days = std::max(0.0, option.travel_days - option.wait_days);
        const double cycle_days = std::max(1.0, 2.0 * transfer_days + option.wait_days);
        const double cycles = std::max(1.0, std::floor(investment.route_commitment_days / cycle_days));
        const double window_days = cycles * cycle_days;
        const double leg_days = std::min(cycle_days, option.travel_days);
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
                                     double return_bound, const MissionChoice& choice) {
        const double running_cost = daily_capital_cost(ship_class) + daily_crew_cost(ship_class, get_station_state(yard.id));
        Valuation valuation;
        valuation.label = "no cargo run";
        // A single-good run is valued alone; the lots of a mixed hold together (each along its
        // own good's stocks), less the running costs once.
        struct RunValue {
            double profit_per_day {0.0};
            std::string label;
            const MissionChoice::CargoOption* main_lot {nullptr};
            double main_units_per_day {0.0};
        };
        std::map<int, RunValue> mixed_runs;
        const auto consider = [&](const RunValue& run) {
            const double annual_return = (run.profit_per_day - running_cost) * 365.0 / ship_class.ship_value_cr;
            if (annual_return > valuation.annual_return) {
                valuation.annual_return = annual_return;
                valuation.label = run.label;
                valuation.destination_id = run.main_lot->destination->id;
                valuation.commodity_id = run.main_lot->commodity_id;
                valuation.units_per_day = run.main_units_per_day;
            }
        };
        for (const auto& option : choice.cargo_options) {
            double units_per_day = 0.0;
            const double profit_per_day = sustained_profit_per_day(yard, option, units_per_day);
            const auto label = std::format("{:.0f}u {} -> {} in {:.1f} d, {:.2f} u/d", option.cargo_units,
                option.commodity_id, option.destination->name, option.travel_days, units_per_day);
            if (option.run < 0) {
                consider({.profit_per_day = profit_per_day, .label = label, .main_lot = &option, .main_units_per_day = units_per_day});
                continue;
            }
            auto& run = mixed_runs[option.run];
            run.profit_per_day += profit_per_day;
            run.label += (run.label.empty() ? "mixed: " : " + ") + label;
            if (run.main_lot == nullptr || units_per_day > run.main_units_per_day) {
                run.main_lot = &option;
                run.main_units_per_day = units_per_day;
            }
        }
        for (const auto& [run_number, run] : mixed_runs) {
            consider(run);
        }
        if (trace) {
            std::cerr << std::format("[invest day {:.0f}] {} at {}: {} of {} runs: {:.0f}%/yr (bound {:.0f}%)\n",
                game_time_s_ / 86400.0, ship_class.id, yard.id, valuation.label, choice.cargo_options.size(),
                100.0 * valuation.annual_return, 100.0 * return_bound);
        }
        return valuation;
    };
    const domain::ShipClassDefinition* best_class = nullptr;
    const domain::StationDefinition* best_yard = nullptr;
    double best_return = investment.hurdle_return_per_year;
    double best_rank = rank_by_profit ? 0.0 : investment.hurdle_return_per_year;
    Valuation best_valuation;
    int probes = 0;
    // Candidates are valued in order of their bound, so the early stop picks the same ship
    // as probing one by one. A candidate not probed yet in this review stops the scan: the
    // next batch is probed (this tick) and the purchase waits for a later tick.
    constexpr std::size_t kProbeBatch = 2;
    const auto probe_key = [](const Candidate& candidate) {
        return candidate.ship_class->id + "|" + candidate.yard->id;
    };
    std::vector<std::size_t> to_probe;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];
        if (candidate.rank_bound <= best_rank) {
            break;
        }
        const auto probed = review_probes_.find(probe_key(candidate));
        if (probed == review_probes_.end()) {
            for (std::size_t j = i; j < candidates.size() && to_probe.size() < kProbeBatch; ++j) {
                if (!review_probes_.contains(probe_key(candidates[j]))) {
                    to_probe.push_back(j);
                }
            }
            break;
        }
        MissionChoice choice;
        for (const auto& run : probed->second) {
            choice.cargo_options.push_back({.destination = &get_station_definition(run.destination_id),
                .commodity_id = run.commodity_id, .cargo_units = run.cargo_units, .travel_days = run.travel_days,
                .wait_days = run.wait_days, .fuel_cost = run.fuel_cost, .run = run.run});
        }
        const auto valuation = value_candidate(*candidate.ship_class, *candidate.yard, candidate.return_bound, choice);
        ++probes;
        const double rank = rank_by_profit
            ? valuation.annual_return * candidate.ship_class->ship_value_cr / 365.0
                / (per_hold_unit ? std::max(1.0, candidate.ship_class->cargo_capacity_units) : 1.0)
            : valuation.annual_return;
        if (valuation.annual_return > investment.hurdle_return_per_year && rank > best_rank) {
            best_rank = rank;
            best_return = valuation.annual_return;
            best_class = candidate.ship_class;
            best_yard = candidate.yard;
            best_valuation = valuation;
        }
    }
    if (!to_probe.empty()) {
        std::vector<domain::ShipState> probe_ships;
        probe_ships.reserve(to_probe.size());
        std::vector<MissionRequest> requests;
        for (const auto index : to_probe) {
            probe_ships.push_back(new_ship(*candidates[index].ship_class, *candidates[index].yard));
            requests.push_back({.ship = &probe_ships.back(), .ship_class = candidates[index].ship_class,
                .earliest_departure_s = game_time_s_ + investment.build_days * 86400.0, .cargo_only = true});
        }
        const auto choices = choose_missions(requests);
        for (std::size_t k = 0; k < to_probe.size(); ++k) {
            auto& runs = review_probes_[probe_key(candidates[to_probe[k]])];
            for (const auto& option : choices[k].cargo_options) {
                runs.push_back({.destination_id = option.destination->id, .commodity_id = option.commodity_id,
                    .cargo_units = option.cargo_units, .travel_days = option.travel_days,
                    .wait_days = option.wait_days, .fuel_cost = option.fuel_cost, .run = option.run});
            }
        }
        if (trace) {
            std::cerr << std::format("[invest day {:.1f}] probed {} candidates in {:.2f} s\n", game_time_s_ / 86400.0,
                to_probe.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - review_start).count());
        }
        return CommissionStep::Probed;
    }
    // The winning hull is built with the tanks that suit it best, judged like a refit (by
    // dispatch's full score, which carries each variant's capital charge); otherwise it would
    // go straight back into the yard.
    if (best_class != nullptr) {
        const auto* hull = best_class;
        std::vector<const domain::ShipClassDefinition*> variants;
        std::vector<domain::ShipState> variant_ships;
        for (const auto& variant : universe_.ship_classes) {
            if ((variant.hull_id.empty() ? variant.id : variant.hull_id) == hull->id
                && variant.ship_value_cr + investment.working_capital <= richest_treasury) {
                variants.push_back(&variant);
            }
        }
        variant_ships.reserve(variants.size());
        std::vector<MissionRequest> requests;
        for (const auto* variant : variants) {
            variant_ships.push_back(new_ship(*variant, *best_yard));
            requests.push_back({.ship = &variant_ships.back(), .ship_class = variant,
                .earliest_departure_s = game_time_s_ + investment.build_days * 86400.0, .cargo_only = false});
        }
        const auto choices = choose_missions(requests);
        double best_score = -std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < variants.size(); ++i) {
            ++probes;
            if (choices[i].kind == MissionChoice::Kind::Mission && choices[i].score > best_score) {
                best_score = choices[i].score;
                best_class = variants[i];
            }
        }
    }
    if (trace) {
        std::cerr << std::format("[invest day {:.0f}] review: {} candidates, {} probed in {:.1f} s\n",
            game_time_s_ / 86400.0, candidates.size(), probes,
            std::chrono::duration<double>(std::chrono::steady_clock::now() - review_start).count());
    }
    if (best_class == nullptr) {
        return CommissionStep::NothingToBuy;
    }

    // The yard's own faction invests if it can pay for the ship and its working capital (with
    // what it may borrow), otherwise the faction that can spend most (which can: candidates
    // are priced against it).
    const double price = best_class->ship_value_cr + investment.working_capital;
    std::string investor = best_yard->faction_id;
    if (faction_spendable(investor) < price) {
        for (const auto& [faction_id, balance] : faction_treasuries_) {
            if (faction_spendable(faction_id) >= faction_spendable(investor)) {
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
    return CommissionStep::Bought;
}

domain::SimulationSnapshot Simulation::snapshot() const {
    return {
        .game_time_s = game_time_s_,
        .outside_economy_credits = outside_economy_credits_,
        .faction_treasuries = faction_treasuries_,
        .faction_credit_limits = [&] {
            std::map<std::string, double> limits;
            for (const auto& faction : universe_.factions) {
                limits[faction.id] = faction_credit_limit(faction.id);
            }
            return limits;
        }(),
        .faction_interest_paid = faction_interest_paid_,
        .money_supply_target = seeded_money_supply_,
        .fleet_investment = investment_ledger_,
        .stations = stations_,
        .ships = ships_,
        .sold_ships = sold_ships_,
        .recent_events = recent_events_,
        .emergencies = emergencies_,
        .emergencies_opened = emergencies_opened_,
        .events_started = events_started_,
        .emergency_paid = emergency_paid_,
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

std::string Simulation::bridge_path_id(const domain::ShipState& ship) const {
    const auto& mission = ship.active_mission;
    return std::format("{}|{}|{:.3f}|{}", mission.destination_station_id, mission.trajectory_type, mission.departure_time_s,
        mission.sampled_path.size());
}

void Simulation::write_ship_path_json(std::ostream& output, const domain::ShipState& ship) const {
    output << "\"trajectory_path\":[";
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

std::string Simulation::bridge_paths_signature() const {
    std::string signature;
    for (const auto& ship : ships_) {
        if (ship.phase == domain::ShipMissionPhase::InTransit || ship.phase == domain::ShipMissionPhase::AwaitingDeparture) {
            signature += ship.id + "=" + bridge_path_id(ship) + ";";
        }
    }
    return signature;
}

std::string Simulation::build_bridge_paths_json() const {
    std::ostringstream output;
    output << std::fixed << std::setprecision(6);
    output << "{\"paths\":{";
    bool first = true;
    for (const auto& ship : ships_) {
        if (ship.phase != domain::ShipMissionPhase::InTransit && ship.phase != domain::ShipMissionPhase::AwaitingDeparture) {
            continue;
        }
        output << (first ? "" : ",") << "\"" << json_escape(ship.id) << "\":{\"path_id\":\"" << json_escape(bridge_path_id(ship)) << "\",";
        write_ship_path_json(output, ship);
        output << "}";
        first = false;
    }
    output << "}}";
    return output.str();
}

std::string Simulation::build_bridge_snapshot_json(bool paused, std::uint64_t snapshot_seq, double snapshot_real_time_s, bool with_paths) const {
    const profiling::Scope profile_scope(profiling::Phase::Snapshot);
    auto inventory_value = [](const domain::Inventory& inventory, const std::string& commodity_id) {
        const auto it = inventory.find(commodity_id);
        return it == inventory.end() ? 0.0 : it->second;
    };

    auto station_inventory = [&](const std::string& station_id) -> const domain::Inventory& {
        return get_station_state(station_id).inventory;
    };

    std::ostringstream output;
    output << std::fixed << std::setprecision(6);
    // {"commodity": units, ...} for the non-zero entries, in the commodity table's order.
    const auto write_goods = [&](const domain::Inventory& goods) {
        output << "{";
        bool first = true;
        for (const auto& commodity : universe_.commodities) {
            const auto it = goods.find(commodity.id);
            if (it == goods.end() || std::abs(it->second) < 1e-9) {
                continue;
            }
            output << (first ? "" : ",") << "\"" << json_escape(commodity.id) << "\":" << it->second;
            first = false;
        }
        output << "}";
    };
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
    output << "},\"faction_credit_limits\":{";
    for (std::size_t i = 0; i < universe_.factions.size(); ++i) {
        const auto& faction_id = universe_.factions[i].id;
        output << (i > 0 ? "," : "") << "\"" << json_escape(faction_id) << "\":" << faction_credit_limit(faction_id);
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
               // Circular orbits: the UI places bodies at any time from these.
               << "\"semi_major_axis_m\":" << body.orbit.semi_major_axis_m << ","
               << "\"orbital_period_s\":" << body.orbit.orbital_period_s << ","
               << "\"phase_at_epoch_rad\":" << body.orbit.phase_at_epoch_rad << ","
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
               << "\"altitude_m\":" << station.altitude_m << ","
               << "\"theta_rad\":" << station.theta_rad << ","
               << "\"population\":" << std::llround(get_station_state(station.id).population) << ","
               << "\"seeded_population\":" << station.population << ","
               << "\"supply_index\":" << get_station_state(station.id).supply_index << ","
               << "\"events\":" << [&] {
                      std::string list = "[";
                      for (const auto& active : get_station_state(station.id).events) {
                          const auto it = std::find_if(universe_.events.begin(), universe_.events.end(),
                              [&](const auto& event) { return event.id == active.event_id; });
                          if (it == universe_.events.end()) {
                              continue;
                          }
                          list += std::format("{}{{\"headline\":\"{}\",\"days_left\":{:.1f}}}", list.size() > 1 ? "," : "",
                              json_escape(it->headline), (active.end_s - game_time_s_) / 86400.0);
                      }
                      return list + "]";
                  }() << ","
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
        output << "},\"target_stock\":{";
        bool first_target = true;
        for (const auto& commodity : universe_.commodities) {
            if (!net_rates.contains(commodity.id) && !(commodity.id == economy::FUEL_ID)) {
                continue;
            }
            output << (first_target ? "" : ",") << "\"" << json_escape(commodity.id) << "\":"
                   << economy_.get_target_stock(station, commodity.id);
            first_target = false;
        }
        output << "},\"demand_units\":";
        write_goods(station_state.demand_units);
        output << ",\"unmet_units\":";
        write_goods(station_state.unmet_units);
        output << ",\"imports_per_day\":";
        write_goods(station_state.import_units_per_day);
        output << ",\"exports_per_day\":";
        write_goods(station_state.export_units_per_day);
        output << ",\"ship_fuel_per_day\":" << station_state.ship_fuel_units_per_day
               << ",\"fuel_factory_per_day\":" << economy_.fuel_factory_output(station)
               << ",\"export_market\":[";
        bool first_export = true;
        for (const auto& commodity : universe_.commodities) {
            if (economy_.is_export_market(station, commodity.id)) {
                output << (first_export ? "" : ",") << "\"" << json_escape(commodity.id) << "\"";
                first_export = false;
            }
        }
        // Step 14-17 state: reference prices of what it consumes, what limits its outputs, its
        // upkeep, its import costs, what it can pay and its open emergencies.
        output << "],\"reference_prices\":{";
        bool first_reference = true;
        for (const auto& commodity : universe_.commodities) {
            const auto rate_it = net_rates.find(commodity.id);
            if (rate_it == net_rates.end() || rate_it->second >= 0.0) {
                continue;
            }
            output << (first_reference ? "" : ",") << "\"" << json_escape(commodity.id) << "\":"
                   << economy_.reference_price(station, commodity.id);
            first_reference = false;
        }
        output << "},\"output_factor\":";
        write_goods(station_state.output_factor);
        output << ",\"output_limited_by\":{";
        bool first_limit = true;
        for (const auto& [output_id, input_id] : station_state.output_limited_by) {
            output << (first_limit ? "" : ",") << "\"" << json_escape(output_id) << "\":\"" << json_escape(input_id) << "\"";
            first_limit = false;
        }
        output << "},\"upkeep_multiplier\":" << station_state.upkeep_multiplier
               << ",\"upkeep_availability\":{";
        bool first_upkeep = true;  // zeros included: those are the shortages
        for (const auto& commodity : universe_.commodities) {
            if (const auto it = station_state.upkeep_availability.find(commodity.id); it != station_state.upkeep_availability.end()) {
                output << (first_upkeep ? "" : ",") << "\"" << json_escape(commodity.id) << "\":" << it->second;
                first_upkeep = false;
            }
        }
        output << "},\"import_unit_cost\":";
        write_goods(station_state.import_unit_cost);
        output << ",\"affordability\":" << station_state.affordability << ",\"emergencies\":[";
        bool first_emergency = true;
        for (const auto& emergency : emergencies_) {
            if (emergency.station_id != station.id) {
                continue;
            }
            output << (first_emergency ? "" : ",") << "{\"commodity_id\":\"" << json_escape(emergency.commodity_id)
                   << "\",\"premium\":" << emergency.premium << ",\"units_open\":" << emergency.units_open
                   << ",\"days\":" << (game_time_s_ - emergency.opened_s) / 86400.0
                   << ",\"faction_paid\":" << emergency.faction_paid << "}";
            first_emergency = false;
        }
        output << "],\"ledger\":{"
               << "\"household_sales\":" << station_state.ledger.household_sales << ","
               << "\"producer_purchases\":" << station_state.ledger.producer_purchases << ","
               << "\"dividends\":" << station_state.ledger.dividends << ","
               << "\"subsidies\":" << station_state.ledger.subsidies << ","
               << "\"taxes\":" << station_state.ledger.taxes
               << "}"
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
               << "\"commodity_id\":\"" << json_escape(largest_lot(ship.active_mission.cargo)) << "\","
               << "\"cargo_units\":" << domain::total_units(ship.active_mission.cargo) << ","
               << "\"cargo\":" << cargo_json(ship.active_mission.cargo) << ","
               << "\"departure_time_s\":" << ship.active_mission.departure_time_s << ","
               << "\"arrival_time_s\":" << ship.active_mission.arrival_time_s << ","
               << "\"wait_time_s\":" << ship.active_mission.wait_time_s << ","
               << "\"coast_time_s\":" << ship.active_mission.coast_time_s << ","
               << "\"total_travel_time_s\":" << ship.active_mission.total_travel_time_s << ","
               << "\"remaining_travel_time_s\":" << ship.active_mission.remaining_travel_time_s << ","
               << "\"x\":" << position.x << ","
               << "\"y\":" << position.y << ","
               << "\"z\":" << position.z << ","
               << "\"class_name\":\"" << json_escape(ship_class.name) << "\","
               << "\"hull_id\":\"" << json_escape(ship_class.hull_id.empty() ? ship_class.id : ship_class.hull_id) << "\","
               << "\"crew_size\":" << ship_class.crew_size << ","
               << "\"provision_days\":" << [&] {
                      double days = std::numeric_limits<double>::infinity();
                      for (const auto& [commodity_id, per_crew_day] : universe_.ship_operations.life_support_units_per_crew_day) {
                          const double need = ship_class.crew_size * per_crew_day;
                          if (need > 0.0) {
                              const auto it = ship.provisions.find(commodity_id);
                              days = std::min(days, (it == ship.provisions.end() ? 0.0 : it->second) / need);
                          }
                      }
                      return std::isfinite(days) ? days : 0.0;
                  }() << ","
               << "\"home_station_id\":\"" << json_escape(ship.home_station_id) << "\","
               << "\"pickup_commodity_id\":\"" << json_escape(ship.active_mission.pickup_commodity_id) << "\","
               << "\"pickup_units\":" << ship.active_mission.pickup_units << ","
               << "\"expected_revenue\":" << ship.active_mission.expected_revenue << ","
               << "\"purchase_cost\":" << ship.active_mission.purchase_cost << ","
               << "\"liner_stops\":[" << [&] {
                      std::string text;
                      if (const auto* stops = liner_stops(ship)) {
                          for (const auto& stop : *stops) {
                              text += (text.empty() ? "\"" : ",\"") + json_escape(stop) + "\"";
                          }
                      }
                      return text;
                  }() << "],"
               << "\"route_destination_id\":\"" << json_escape(ship.route_destination_id) << "\","
               << "\"route_commodity_id\":\"" << json_escape(ship.route_commodity_id) << "\","
               << "\"route_until_s\":" << ship.route_until_s << ","
               << "\"refit_class_id\":\"" << json_escape(ship.refit_class_id) << "\","
               << "\"refit_done_s\":" << ship.refit_done_s << ","
               << "\"commissioned_s\":" << ship.commissioned_s << ","
               << "\"ledger\":{"
               << "\"cargo_revenue\":" << ship.ledger.cargo_revenue << ","
               << "\"cargo_purchases\":" << ship.ledger.cargo_purchases << ","
               << "\"fuel\":" << ship.ledger.fuel << ","
               << "\"wages\":" << ship.ledger.wages << ","
               << "\"capital\":" << ship.ledger.capital << ","
               << "\"provisions\":" << ship.ledger.provisions << ","
               << "\"refits\":" << ship.ledger.refits << ","
               << "\"dividends\":" << ship.ledger.dividends
               << "}";
        if (ship.phase == domain::ShipMissionPhase::InTransit || ship.phase == domain::ShipMissionPhase::AwaitingDeparture) {
            output << ",\"path_id\":\"" << json_escape(bridge_path_id(ship)) << "\"";
            if (with_paths) {
                output << ",";
                write_ship_path_json(output, ship);
            }
        }
        output << "}";
    }
    output << "],";

    {
        // Economy overview: consumption that found no stock (cumulative, at base prices),
        // exports to the outside economy, the fleet's trade in ships.
        double demand_value = 0.0;
        double unmet_value = 0.0;
        double exports_value = 0.0;
        for (const auto& station : stations_) {
            for (const auto& [commodity_id, units] : station.demand_units) {
                demand_value += units * get_commodity(commodity_id).base_price;
            }
            for (const auto& [commodity_id, units] : station.unmet_units) {
                unmet_value += units * get_commodity(commodity_id).base_price;
            }
            for (const auto& [commodity_id, units] : station.market_sold_units) {
                exports_value += units * get_commodity(commodity_id).base_price;
            }
        }
        output << "\"economy\":{"
               << "\"demand_value\":" << demand_value << ","
               << "\"unmet_value\":" << unmet_value << ","
               << "\"exports_value\":" << exports_value << ","
               << "\"money_supply_target\":" << seeded_money_supply_ << ","
               << "\"ships_commissioned\":" << investment_ledger_.ships_commissioned << ","
               << "\"ships_sold\":" << investment_ledger_.ships_sold << ","
               << "\"emergencies_opened\":" << emergencies_opened_ << ","
               << "\"emergency_paid\":" << emergency_paid_ << ","
               << "\"faction_interest_paid\":" << faction_interest_paid_
               << "},";
    }
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
