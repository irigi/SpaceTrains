#include "simulation/Simulation.hpp"
#include "domain/ShipDesign.hpp"
#include "simulation/CrewEnvironment.hpp"

#include "util/Profiling.hpp"

#include <algorithm>
#include <chrono>
#include <array>
#include <tuple>
#include <cmath>
#include <numbers>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "simulation/SimulationInternal.hpp"

namespace spacetrains::simulation {

using namespace detail;


Simulation::Simulation(domain::UniverseDefinition universe)
    : universe_(std::move(universe)),
      mechanics_(universe_),
      economy_(universe_) {
    kepler_planner_ = std::make_unique<trajectory::KeplerTrajectoryPlanner>(universe_, mechanics_);
    for (const auto& station : universe_.stations) {
        station_defs_by_id_[station.id] = &station;
        stations_.push_back({.station_id = station.id, .inventory = starting_inventory(station), .credits = station.initial_credits, .ledger = {}});
        starting_inventories_[station.id] = stations_.back().inventory;
        stations_.back().population = static_cast<double>(station.population);
        stations_.back().population_announced = stations_.back().population;
    }
    for (const auto& ship_class : universe_.ship_classes) {
        ship_classes_by_id_[ship_class.id] = &ship_class;
    }
    for (const auto& module : universe_.ship_modules) {
        modules_by_key_[module.hull_id + "|" + module.id] = &module;
    }
    event_rng_.seed(universe_.event_seed);
    environment_rng_.seed(universe_.event_seed + 1);
    if (universe_.ship_environment.radiation) {
        active_effects_.insert("spe_shield_g_cm2");
    }
    if (universe_.ship_environment.boiloff) {
        active_effects_.insert("zbo_lift_w");
        active_effects_.insert("boiloff_intercept");
    }
    if (universe_.ship_environment.cold_storage) {
        active_effects_.insert("perishable_decay_factor");
    }
    if (universe_.passengers.enabled != 0.0) {
        active_effects_.insert("berths");
        if (universe_.ship_environment.radiation) {
            active_effects_.insert("cabin_shield_g_cm2");
            active_effects_.insert("tank_wrap_m2");
        }
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
            .fit = {},
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
    // The colonies' people (labour market).
    for (const auto& economy : universe_.background_economies) {
        if (economy.population > 0.0) {
            background_population_[economy.id] = economy.population;
        }
        if (economy.id != "earth") {
            colony_state_[economy.id] = {.wage_median = economy.wage_median_cr_per_day};
        }
    }
    setup_money_journal();
    // Residents start with their savings buffer, brought from Earth (economy overhaul).
    if (households_on()) {
        for (auto& station : stations_) {
            if (station.population <= 0.0) {
                station.population = static_cast<double>(get_station_definition(station.station_id).population);
            }
            if (in_labour_market(station)) {
                station.jobs = station_workers(station);
                station.wage_cr_per_day = 0.0;
                station.wage_cr_per_day = station_wage_cr_per_day(station);
            }
            const double savings = universe_.households.savings_days * resident_wages_per_day(station);
            station.household_credits += savings;
            outside_economy_credits_ -= savings;
            journal_.record(account_earth(), account_households(station), MoneyCategory::SavingsEndowment, savings);
        }
    }
    seeded_money_supply_ = internal_money_supply();
    compute_rotation_tours();
    next_investment_review_s_ = universe_.fleet_investment.review_days * 86400.0;
    // Ships start with their full life-support endurance aboard, as if fresh from the yard.
    constexpr double INITIAL_PROVISION_DAYS = PROVISION_ENDURANCE_DAYS;
    for (auto& ship : ships_) {
        const auto& ship_class = ship_design(ship);
        for (const auto& [commodity_id, units_per_crew_day] : universe_.ship_operations.life_support_units_per_crew_day) {
            ship.provisions[commodity_id] = ship_class.crew_size * units_per_crew_day * INITIAL_PROVISION_DAYS;
        }
        // The starting fleet is of mixed ages (tier 2 wear): spread by a hash of the ship id
        // (FNV-1a, the same on every platform), so overhauls do not all fall due at once.
        if (universe_.ship_environment.wear) {
            std::uint64_t hash = 1469598103934665603ULL;
            for (const char ch : ship.id) {
                hash = (hash ^ static_cast<unsigned char>(ch)) * 1099511628211ULL;
            }
            ship.engine_wear = universe_.ship_environment.initial_wear_max * static_cast<double>(hash % 10000) / 10000.0;
        }
    }
    seed_order_stats();
    init_failure_clocks();
}


Simulation Simulation::from_data_root(const std::string& data_root) {
    return from_data_root(data_root, [](domain::UniverseDefinition&) {});
}

Simulation Simulation::from_data_root(const std::string& data_root,
    const std::function<void(domain::UniverseDefinition&)>& adjust) {
    data_loader::DataLoader loader;
    auto universe = loader.load_universe(data_root);
    adjust(universe);
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
    environment_rng_.seed((universe_.event_seed + 1) ^ static_cast<std::uint64_t>(time_s));
    next_investment_review_s_ = time_s;
    journal_.set_next_close_day(time_s / 86400.0 + MoneyJournal::kPeriodDays);
    for (auto& ship : ships_) {
        ship.next_review_s = time_s;
        ship.idle_since_s = time_s;
        ship.next_refit_review_s = time_s;
        ship.fails_at_s = 0.0;  // drawn again below, relative to the new start
    }
    init_failure_clocks();
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

double Simulation::radiator_bucket(const domain::ShipState& ship) const {
    // Only a plasma drive's radiators limit its power (register_design makes damaged designs
    // for variable-Isp hulls only), so other hulls are always in the undamaged bucket.
    if (!universe_.ship_environment.wear || get_ship_class(ship.class_id).propulsion_type != "variable_isp") {
        return 1.0;
    }
    return std::clamp(std::round(ship.radiator_condition * 10.0) / 10.0, 0.1, 1.0);
}

namespace {
std::string damaged_design_id(const std::string& class_id, const std::vector<std::string>& fit, double radiator_bucket) {
    auto key = domain::design_id(class_id, fit);
    if (radiator_bucket < 1.0) {
        key += std::format("#r{}", static_cast<int>(std::lround(radiator_bucket * 10.0)));
    }
    return key;
}
}  // namespace

const domain::ShipClassDefinition& Simulation::ship_design(const domain::ShipState& ship) const {
    const double bucket = radiator_bucket(ship);
    if (ship.fit.empty() && bucket >= 1.0) {
        return get_ship_class(ship.class_id);
    }
    const auto key = damaged_design_id(ship.class_id, ship.fit, bucket);
    const auto it = designs_.find(key);
    if (it == designs_.end()) {
        throw std::runtime_error("Unregistered ship design: " + key);
    }
    return it->second;
}

const domain::ShipModuleDefinition* Simulation::find_module(const std::string& hull_id, const std::string& module_id) const {
    const auto it = modules_by_key_.find(hull_id + "|" + module_id);
    return it == modules_by_key_.end() ? nullptr : it->second;
}

bool Simulation::module_has_active_effect(const domain::ShipModuleDefinition& module) const {
    for (const auto& [effect, value] : module.effects) {
        if (value != 0.0 && active_effects_.contains(effect)) {
            return true;
        }
    }
    return false;
}

std::vector<Simulation::FitOption> Simulation::neighbouring_fits(
    const std::string& class_id, const std::vector<std::string>& fit, double radiator_bucket) {
    const auto& current = get_ship_class(class_id);
    std::vector<FitOption> options;
    const auto add = [&](const std::string& option_class, std::vector<std::string> option_fit) {
        try {
            const auto& design = register_design(option_class, option_fit, radiator_bucket);
            options.push_back({.design = &design, .class_id = option_class, .fit = std::move(option_fit)});
        } catch (const std::runtime_error&) {
            // The fit does not suit this hull (slots full): not an option.
        }
    };
    // Only the neighbouring tank sizes: larger steps take several refits.
    const domain::ShipClassDefinition* next_smaller = nullptr;
    const domain::ShipClassDefinition* next_larger = nullptr;
    for (const auto& other : universe_.ship_classes) {
        if (other.hull_id != current.hull_id) {
            continue;
        }
        const double tank_kg = other.propellant_capacity_kg;
        if (tank_kg < current.propellant_capacity_kg
            && (next_smaller == nullptr || tank_kg > next_smaller->propellant_capacity_kg)) {
            next_smaller = &other;
        }
        if (tank_kg > current.propellant_capacity_kg
            && (next_larger == nullptr || tank_kg < next_larger->propellant_capacity_kg)) {
            next_larger = &other;
        }
    }
    for (const auto* tank : {next_smaller, next_larger}) {
        if (tank != nullptr) {
            add(tank->id, fit);
        }
    }
    for (const auto& module : universe_.ship_modules) {
        if (module.hull_id != current.hull_id) {
            continue;
        }
        const bool fitted = std::find(fit.begin(), fit.end(), module.id) != fit.end();
        if (fitted) {
            std::vector<std::string> without;
            std::copy_if(fit.begin(), fit.end(), std::back_inserter(without),
                [&](const auto& id) { return id != module.id; });
            add(class_id, std::move(without));
        } else if (module_has_active_effect(module)) {
            auto with = fit;
            with.push_back(module.id);
            std::sort(with.begin(), with.end());
            add(class_id, std::move(with));
        }
    }
    return options;
}

const domain::ShipClassDefinition& Simulation::register_design(
    const std::string& class_id, const std::vector<std::string>& fit, double radiator_bucket) {
    const auto& base = get_ship_class(class_id);
    const bool damaged = radiator_bucket < 1.0 && base.propulsion_type == "variable_isp";
    if (fit.empty() && !damaged) {
        return base;
    }
    const auto key = damaged_design_id(class_id, fit, damaged ? radiator_bucket : 1.0);
    if (const auto it = designs_.find(key); it != designs_.end()) {
        return it->second;
    }
    std::vector<std::string> sorted = fit;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
        throw std::runtime_error("Ship design fits a module twice: " + key);
    }
    std::vector<domain::ShipComponentDefinition> parts;
    std::unordered_map<std::string, int> used;
    std::unordered_map<std::string, double> effects;
    std::string names;
    for (const auto& module_id : sorted) {
        const auto* module = find_module(base.hull_id, module_id);
        if (module == nullptr) {
            throw std::runtime_error("Hull " + base.hull_id + " has no module " + module_id);
        }
        const auto slots = universe_.hull_slots.find(base.hull_id);
        const int capacity = slots == universe_.hull_slots.end() || !slots->second.contains(module->slot)
            ? 0 : slots->second.at(module->slot);
        if (++used[module->slot] > capacity) {
            throw std::runtime_error("Ship design overfills the " + module->slot + " slots: " + key);
        }
        parts.push_back(module->part);
        for (const auto& [effect, value] : module->effects) {
            effects[effect] += value;
        }
        names += (names.empty() ? "" : ", ") + module->part.name;
    }
    auto design = domain::derive_design(base, parts, key);
    design.name = names.empty() ? base.name : base.name + " [" + names + "]";
    design.base_class_id = base.id;
    if (damaged) {
        // Holed radiators reject less heat, and the drive may only make as much waste heat as
        // they reject: jet power, and with it alpha, falls in proportion.
        design.specific_engine_power_w_per_kg *= radiator_bucket;
        for (auto& part : design.components) {
            if (part.kind == "engine") {
                part.power_w *= radiator_bucket;
                part.heat_w *= radiator_bucket;
            } else if (part.kind == "radiator") {
                part.heat_w *= radiator_bucket;
            }
        }
        design.name += std::format(" (radiators {:.0f}%)", 100.0 * radiator_bucket);
    }
    if (const auto it = effects.find("berths"); it != effects.end() && active_effects_.contains("berths")) {
        design.berths += it->second;
    }
    design.fit = std::move(sorted);
    design.effects = std::move(effects);
    return designs_.emplace(key, std::move(design)).first->second;
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
    // Debug aid: SPACETRAINS_TRACE_CONTRACTS=1 logs rotation-contract events (stderr).
    if (static const bool trace_contracts = std::getenv("SPACETRAINS_TRACE_CONTRACTS") != nullptr;
        trace_contracts && text.find("rotation contract") != std::string::npos) {
        std::cerr << std::format("[CONTRACT] day {:.1f} {}\n", game_time_s_ / 86400.0, text);
    }
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
    update_market_responses();
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
        for (auto& [commodity_id, stats] : station.order_stats) {
            stats.ship_sales_per_day *= decay;
        }
    }
    settle_local_economy(stocks_before, dt_s);
    step_households(dt_s);
    step_surface_markets(dt_s);
    step_labour(dt_s);
    step_colony_labour(dt_s);
    step_population(dt_s);
    if (passengers_on()) {
        step_passengers(dt_s);
    }
    step_events(dt_s);
    step_particle_events(dt_s);
    step_treasuries(dt_s);
    step_emergencies();
    update_market_prices();
    step_orders();
    step_fleet_investment();
    step_failures();

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

    // Debug aid: SPACETRAINS_TRACE_FLEET=1 logs the fleet's hold use every 5 days (stderr):
    // ships and hold units by phase, and the units aboard in flight and while waiting to leave.
    if (static const bool trace_fleet = std::getenv("SPACETRAINS_TRACE_FLEET") != nullptr;
        trace_fleet && std::fmod(game_time_s_ / 86400.0 + 1e-6, 5.0) < dt_s / 86400.0) {
        int loaded = 0, empty = 0, waiting = 0, docked = 0, contracted = 0;
        double hold = 0.0, hold_loaded = 0.0, hold_empty = 0.0, aboard_flying = 0.0, aboard_waiting = 0.0;
        for (const auto& ship : ships_) {
            contracted += under_contract(ship) ? 1 : 0;
            const double capacity = ship_design(ship).cargo_capacity_units;
            const double aboard = domain::total_units(ship.active_mission.cargo);
            hold += capacity;
            if (ship.phase == domain::ShipMissionPhase::InTransit) {
                ++(aboard > 0.0 ? loaded : empty);
                (aboard > 0.0 ? hold_loaded : hold_empty) += capacity;
                aboard_flying += aboard;
            } else if (ship.phase == domain::ShipMissionPhase::AwaitingDeparture) {
                ++waiting;
                aboard_waiting += aboard;
            } else {
                ++docked;
            }
        }
        std::cerr << std::format("[FLEET] day {:.0f} ships {} hold {:.0f} loaded {} {:.0f} empty {} {:.0f} waiting {} docked {} "
            "aboard_flying {:.0f} aboard_waiting {:.0f} contracts {}\n", game_time_s_ / 86400.0, ships_.size(), hold, loaded,
            hold_loaded, empty, hold_empty, waiting, docked, aboard_flying, aboard_waiting, contracted);
    }
    // The money journal's monthly sample (statistics).
    if (game_time_s_ / 86400.0 + 1e-6 >= journal_.next_close_day()) {
        close_journal_period();
    }
}

void Simulation::settle_local_economy(const std::vector<domain::Inventory>& stocks_before, double dt_s) {
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
        double spent = 0.0;
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
            (households_on() ? station.household_credits : outside_economy_credits_) -= value;
            journal_.record(households_on() ? account_households(station) : account_earth(), account_station(station),
                MoneyCategory::HouseholdGoods, value);
            spent += value;
        }
        // Passages the station paid (passengers) come back through its prices: a markup on what
        // the residents spend, at most passage_max_markup of it.
        if (station.passage_debt_cr > 0.0 && universe_.passengers.passage_recovery_days > 0.0) {
            const double due = station.passage_debt_cr
                * (1.0 - std::exp(-dt_s / 86400.0 / universe_.passengers.passage_recovery_days));
            const double recovered = std::min(due, universe_.passengers.passage_max_markup * spent);
            station.credits += recovered;
            station.ledger.passages_recovered += recovered;
            station.passage_debt_cr -= recovered;
            (households_on() ? station.household_credits : outside_economy_credits_) -= recovered;
            journal_.record(households_on() ? account_households(station) : account_earth(), account_station(station),
                MoneyCategory::PassageRecovery, recovered);
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
        if (in_labour_market(station) && migration_by_ship()) {
            // Labour market: settlers come for the station's vacancies, the unemployed go home;
            // what each would pay is their own gain (passenger_bands).
            if (const double in = labour_migrants_per_day(station) * dt_days; in > 0.0) {
                post_travellers(get_station_state(gateway_for(station.station_id, true)), domain::PassengerKind::Migrant,
                    station.station_id, station.station_id, in, true);
            }
            if (const double out = labour_emigrants_per_day(station) * dt_days; out > 0.0) {
                post_travellers(station, domain::PassengerKind::Emigrant, gateway_for(station.station_id, false), "", out, true);
            }
        } else if (migration_by_ship() && !is_earth_gateway(station.station_id)) {
            // Passengers: people come and go by ship. A thriving station draws settlers from Earth
            // at the growth rate (at the reference cost of the trip; the fare elasticity spreads
            // them over cheaper and dearer trips), a failing one sees its people leave for Earth.
            const double ceiling = growth.max_population_factor * seeded;
            if (rate > 0.0
                && station.population + travellers_pending(station.station_id, domain::PassengerKind::Migrant) < ceiling) {
                post_travellers(get_station_state(gateway_for(station.station_id, true)), domain::PassengerKind::Migrant,
                    station.station_id, employer_pays(domain::PassengerKind::Migrant) ? station.station_id : "",
                    rate * station.population * dt_days / 365.25);
            } else if (rate < 0.0
                && station.population - travellers_pending(station.station_id, domain::PassengerKind::Emigrant) > floor) {
                post_travellers(station, domain::PassengerKind::Emigrant, gateway_for(station.station_id, false), "",
                    -rate * station.population * dt_days / 365.25);
            }
        } else {
            station.population = std::clamp(station.population * std::exp(rate * dt_days / 365.25),
                std::min(floor, seeded), growth.max_population_factor * seeded);
        }
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
        if (event.kind == domain::EventKind::Migration && migration_by_ship() && !is_earth_gateway(station.station_id)) {
            // With passengers the settlers wait at Earth for ships.
            post_travellers(get_station_state(gateway_for(station.station_id, true)), domain::PassengerKind::Migrant,
                station.station_id, employer_pays(domain::PassengerKind::Migrant) ? station.station_id : "",
                event.magnitude * station.population, true);
            continue;
        }
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

}  // namespace spacetrains::simulation
