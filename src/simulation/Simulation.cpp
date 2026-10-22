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

namespace spacetrains::simulation {

namespace {
constexpr double FUEL_UNITS_TO_KG = 100.0;
// Propellant a plan uses up: burned, plus hydrogen boiled off in flight (bought, never burned).
double consumed_propellant_kg(const domain::TrajectoryPlan& plan) {
    return plan.propellant_required_kg + plan.propellant_boiled_kg;
}
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
// within the horizon, closed when it covers EMERGENCY_CLOSE_DAYS again. The premium price covers
// one delivery of EMERGENCY_COVER_DAYS of use (shared by the ships that take it), paid by the
// faction above the order's price. It starts at the station's own cap (step 29) and grows by
// EMERGENCY_RAISE_FACTOR every EMERGENCY_RAISE_DAYS that no ship takes a delivery, up to
// orders.life_value: a small station's month of water is worth little at 3x, less than a trip to
// it costs, so the offer grows until a ship comes (Mercury's water stayed short for two years
// at 8x; doubling to 96x paid 2M cr in two years).
// A station's import cost (what its residents pay) averages deliveries over this many days of its use.
constexpr double IMPORT_COST_DAYS = 30.0;
constexpr double EMERGENCY_HORIZON_DAYS = 21.0;
constexpr double EMERGENCY_CLOSE_DAYS = 14.0;
constexpr double EMERGENCY_COVER_DAYS = 30.0;
constexpr double EMERGENCY_RAISE_FACTOR = 1.5;
constexpr double EMERGENCY_RAISE_DAYS = 15.0;

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

double Simulation::radiator_bucket(const domain::ShipState& ship) const {
    if (!universe_.ship_environment.wear) {
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

domain::Inventory Simulation::starting_inventory(const domain::StationDefinition& station) const {
    // A station starts with what it would hold if it had been supplied all along: each good it
    // consumes at least at its seeded order-up-to level (step 29; until then its resupply cover).
    // With the few weeks of the data files, Ceres and Ganymede ran out of oxygen on day 20 and
    // the outposts' first emergencies stayed open for 450-660 days while the first freighters
    // were on their way. The raise is scaled down to fit 85% of the station's storage. Depot
    // fuel starts as the data has it: its use is what ships buy there, unknown until they do.
    domain::Inventory inventory = station.initial_inventory;
    domain::Inventory raises;
    const domain::StationState seeded {.station_id = station.id};
    for (const auto& [commodity_id, rate] : economy_.get_station_net_rates(station)) {
        if (rate >= 0.0 || !has_order(station, commodity_id)) {
            continue;
        }
        const double stock = inventory.contains(commodity_id) ? inventory.at(commodity_id) : 0.0;
        const double raise = order_levels(seeded, commodity_id, seed_order_stats(station, commodity_id)).order_up_to_units - stock;
        if (raise > 0.0) {
            raises[commodity_id] = raise;
        }
    }
    double raised_storage = 0.0;
    for (const auto& [commodity_id, raise] : raises) {
        raised_storage += economy_.storage_used_units({{commodity_id, raise}});
    }
    const double room = 0.85 * station.storage_capacity_units - economy_.storage_used_units(station, inventory);
    const double scale = station.storage_capacity_units > 0.0 && raised_storage > room
        ? std::max(0.0, room) / raised_storage : 1.0;
    for (const auto& [commodity_id, raise] : raises) {
        inventory[commodity_id] += raise * scale;
    }
    // A background economy's gateway starts stocked with what it sells in any amount.
    for (const auto* market : economy_.surface_markets_at(station.id)) {
        if (economy_.surface_supplied(station, market->commodity_id)) {
            inventory[market->commodity_id] = std::max(inventory[market->commodity_id], economy_.surface_depth_units());
        }
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
        const bool fuel_depot = economy_.fuel_factory_output(consumer) <= 0.0;
        if (fuel_depot) {
            goods[economy::FUEL_ID] = std::min(goods[economy::FUEL_ID], -1.0);
        } else {
            goods.erase(economy::FUEL_ID);
        }
        for (const auto& [commodity_id, rate] : goods) {
            // A background economy's gateway is priced at the surface's ask (no route).
            if (rate >= 0.0 || economy_.is_export_market(consumer, commodity_id) || economy_.surface_supplied(consumer, commodity_id)) {
                continue;
            }
            Route route {.consumer = &consumer, .commodity = &get_commodity(commodity_id)};
            for (const auto& producer : universe_.stations) {
                const auto& rates = economy_.get_station_net_rates(producer);
                const auto it = rates.find(commodity_id);
                if (producer.id != consumer.id
                    && ((it != rates.end() && it->second > 0.0) || economy_.surface_supplied(producer, commodity_id))) {
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
            .boiloff_kg_per_day = leg_boiloff_kg_per_day(*leg.hull, *leg.from, *leg.to, leg.departure_s),
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
        double best_total = std::numeric_limits<double>::infinity();
        for (const auto* producer : route.producers) {
            // What the good costs where it is loaded: a background economy's ask at its gateway
            // (a small colony sells below Earth's price), the base price elsewhere.
            double producer_price = route.commodity->base_price;
            if (const auto* market = economy_.surface_market(*producer, route.commodity->id); market != nullptr && market->ask_cr > 0.0) {
                producer_price = market->ask_cr;
            }
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
                        + (consumed_propellant_kg(out) + consumed_propellant_kg(back)) * fuel_cr_per_kg;
                    trips.emplace_back(cost / units, days);
                }
                // A hull that cannot fly the route at most dates does not set its price.
                if (trips.size() * 2 <= static_cast<std::size_t>(departures)) {
                    continue;
                }
                std::sort(trips.begin(), trips.end());
                const auto& median = trips[trips.size() / 2];
                if (producer_price + pricing.carrier_margin * median.first < best_total) {
                    best_total = producer_price + pricing.carrier_margin * median.first;
                    best.base_price = producer_price;
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
    // A good the station orders it sells to ships (provisions, depot fuel) at its own bid on its
    // stock now plus a margin (step 29). At its import cost plus the margin a hub resold dear
    // whatever it had once paid in a shortage, and onward routes from it looked poor (4.0% unmet
    // against 5.2%, 4 starts x 1460 days).
    if (const auto order = state.order_stats.find(commodity_id); priced_order(state, commodity_id)) {
        const auto stock_it = state.inventory.find(commodity_id);
        return order_bid(state, commodity_id, order_levels(state, commodity_id, order->second),
            stock_it == state.inventory.end() ? 0.0 : stock_it->second).price * (1.0 + universe_.orders.resale_margin);
    }
    const auto& definition = get_station_definition(state.station_id);
    const auto stock_it = state.inventory.find(commodity_id);
    const double stock = stock_it == state.inventory.end() ? 0.0 : stock_it->second;
    return economy_.get_price(definition, commodity_id, stock, get_commodity(commodity_id).base_price);
}

double Simulation::trade_value(
    const domain::StationState& state, const std::string& commodity_id, double units_into_station) const {
    // An ordered good: sold to ships at a flat price, bought under the order as if delivered now.
    if (priced_order(state, commodity_id)) {
        return units_into_station <= 0.0
            ? -units_into_station * station_price(state, commodity_id)
            : quote_split(order_quote(state, commodity_id, game_time_s_, {}), 0.0, units_into_station).total;
    }
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
    if (priced_order(state, commodity_id)) {
        return quote_split(order_quote(state, commodity_id, game_time_s_ + std::max(0.0, days_ahead) * 86400.0, seller_ship_id),
            0.0, units);
    }
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
    // A good the station posts no order for (producers, non-traders): along its curve.
    return {.total = economy_.get_trade_value(station, commodity_id, stock, units, get_commodity(commodity_id).base_price)};
}

void Simulation::step_emergencies() {
    for (const auto& state : stations_) {
        const auto& definition = get_station_definition(state.station_id);
        const auto& rates = economy_.get_station_net_rates(definition);
        for (const auto& commodity_id : universe_.emergency_goods) {
            const auto rate = rates.find(commodity_id);
            if (rate == rates.end() || rate->second >= 0.0 || !economy_.is_upkeep(definition, commodity_id)
                || economy_.surface_supplied(definition, commodity_id)) {
                continue;
            }
            const double use_per_day = -rate->second;
            const double forecast = forecast_stock_on_arrival(state, commodity_id, EMERGENCY_HORIZON_DAYS, {});
            const auto open = std::find_if(emergencies_.begin(), emergencies_.end(), [&](const domain::Emergency& e) {
                return e.station_id == state.station_id && e.commodity_id == commodity_id;
            });
            if (open == emergencies_.end()) {
                if (forecast <= 0.0) {
                    // The faction pays above the station's own cap (its order's bid on an empty
                    // stock), so the premium starts there (step 29). A faction offer that rose with
                    // the projected stock instead (24x at once on an empty stock) opened emergencies
                    // three to four times as often and paid 2M cr in four years.
                    const auto& stats = state.order_stats.at(commodity_id);
                    const double premium = order_bid(state, commodity_id, order_levels(state, commodity_id, stats), 0.0).cap
                        / economy_.reference_price(definition, commodity_id);
                    emergencies_.push_back({
                        .station_id = state.station_id,
                        .commodity_id = commodity_id,
                        .premium = premium,
                        .units_open = use_per_day * EMERGENCY_COVER_DAYS,
                        .opened_s = game_time_s_,
                        .last_raise_s = game_time_s_,
                    });
                    ++emergencies_opened_;
                    add_event(std::format("EMERGENCY: {} will run out of {} — {} pays {:.0f}x for deliveries",
                        definition.name, get_commodity(commodity_id).name, faction_name(definition.faction_id),
                        premium), "alert");
                }
                continue;
            }
            if (forecast >= use_per_day * EMERGENCY_CLOSE_DAYS) {
                add_event(std::format("Emergency over: {} has enough {} coming ({:.0f} days after it began)",
                    definition.name, get_commodity(commodity_id).name, (game_time_s_ - open->opened_s) / 86400.0), "alert");
                emergencies_.erase(open);
            } else if (game_time_s_ - open->last_raise_s >= EMERGENCY_RAISE_DAYS * 86400.0
                && open->premium < universe_.orders.life_value && open->units_open > 0.0) {
                open->premium = std::min(universe_.orders.life_value, open->premium * EMERGENCY_RAISE_FACTOR);
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
    const auto& rates = economy_.get_station_net_rates(definition);
    const double rate = rates.contains(commodity_id) ? rates.at(commodity_id) : 0.0;
    double time_s = game_time_s_;
    for (const auto& [delivery_s, delivered] : deliveries) {
        stock = std::max(0.0, stock + rate * std::max(0.0, delivery_s - time_s) / 86400.0) + delivered;
        time_s = std::max(time_s, delivery_s);
    }
    return std::max(0.0, stock + rate * std::max(0.0, arrival_s - time_s) / 86400.0);
}

bool Simulation::priced_order(const domain::StationState& state, const std::string& commodity_id) const {
    return state.order_stats.contains(commodity_id);
}

bool Simulation::has_order(const domain::StationDefinition& station, const std::string& commodity_id) const {
    if (economy_.is_export_market(station, commodity_id) || economy_.surface_supplied(station, commodity_id)) {
        return false;
    }
    // Depot fuel: every station without a fuel factory orders the fuel ships buy there.
    if (commodity_id == economy::FUEL_ID) {
        return economy_.fuel_factory_output(station) <= 0.0;
    }
    const auto& rates = economy_.get_station_net_rates(station);
    const auto rate = rates.find(commodity_id);
    return rate != rates.end() && rate->second < 0.0;
}

domain::OrderStats Simulation::seed_order_stats(const domain::StationDefinition& station, const std::string& commodity_id) const {
    // The nearest producer, by the map-level transfer estimate (fuel: the nearest factory).
    double nearest = std::numeric_limits<double>::infinity();
    for (const auto& producer : universe_.stations) {
        bool makes = (commodity_id == economy::FUEL_ID && economy_.fuel_factory_output(producer) > 0.0)
            || economy_.surface_supplied(producer, commodity_id);
        for (const auto* recipe : economy_.recipes_of(producer)) {
            makes = makes || (recipe->commodity_id == commodity_id && recipe->units_per_day > 0.0);
        }
        if (makes && producer.id != station.id) {
            nearest = std::min(nearest, economy_.transfer_days(producer, station));
        }
    }
    double round_trip = std::isfinite(nearest) ? 2.0 * nearest : 42.0;
    const auto& trips = universe_.pricing.round_trip_days;
    if (const auto station_it = trips.find(station.id); station_it != trips.end()) {
        if (const auto it = station_it->second.find(commodity_id); it != station_it->second.end()) {
            round_trip = it->second;
        }
    }
    // Within one planet's system the reference plasma ship's spirals overstate a local
    // shuttle's trip (as in step 23's cover rule): plan for three weeks between deliveries.
    const bool local = std::isfinite(nearest) && nearest <= 5.0;
    domain::OrderStats stats;
    stats.lead_days = local ? std::max(1.0, nearest) : std::max(1.0, 0.5 * round_trip);
    stats.interval_days = local ? 21.0 : std::max(21.0, round_trip);
    stats.spread_days = 0.5 * stats.lead_days;
    stats.going_rate = economy_.reference_price(station, commodity_id);
    stats.order_price = stats.going_rate;
    return stats;
}

void Simulation::seed_order_stats() {
    for (auto& state : stations_) {
        const auto& definition = get_station_definition(state.station_id);
        for (const auto& commodity : universe_.commodities) {
            if (has_order(definition, commodity.id) && !state.order_stats.contains(commodity.id)) {
                state.order_stats[commodity.id] = seed_order_stats(definition, commodity.id);
            }
        }
    }
}

void Simulation::observe_contract(domain::StationState& state, const std::string& commodity_id, double units, double unit_price) {
    const auto it = state.order_stats.find(commodity_id);
    if (it == state.order_stats.end() || units <= 0.0) {
        return;
    }
    // Weighted by units: a lot of one interval's use moves the going rate by 1/ema_deliveries.
    auto& stats = it->second;
    const double use = order_levels(state, commodity_id, stats).use_per_day;
    const double history = use * stats.interval_days * (universe_.orders.ema_deliveries - 1.0);
    stats.going_rate += (unit_price - stats.going_rate) * units / (units + history);
}

void Simulation::observe_delivery(domain::StationState& state, const std::string& commodity_id, double contracted_s) {
    const auto it = state.order_stats.find(commodity_id);
    if (it == state.order_stats.end()) {
        return;
    }
    auto& stats = it->second;
    const double alpha = 1.0 / universe_.orders.ema_deliveries;
    const auto fold = [&](double& mean, double sample) {
        stats.spread_days += alpha * (std::abs(sample - mean) - stats.spread_days);
        mean += alpha * (sample - mean);
    };
    fold(stats.lead_days, std::max(0.0, game_time_s_ - contracted_s) / 86400.0);
    // Ships landing within a day of each other make one delivery.
    if (stats.last_delivery_s >= 0.0 && game_time_s_ - stats.last_delivery_s >= 86400.0) {
        fold(stats.interval_days, (game_time_s_ - stats.last_delivery_s) / 86400.0);
    }
    stats.last_delivery_s = game_time_s_;
    ++stats.deliveries;
}

double Simulation::order_ceiling(const domain::StationState& state, const std::string& commodity_id,
    std::string& kind, double& station_ceiling) const {
    const auto& definition = get_station_definition(state.station_id);
    const auto& orders = universe_.orders;
    const double reference = economy_.reference_price(definition, commodity_id);
    if (commodity_id == economy::FUEL_ID && economy_.fuel_factory_output(definition) <= 0.0) {
        kind = "depot_fuel";
        station_ceiling = orders.depot_fuel_value * reference;
        return station_ceiling;
    }
    const auto& recipes = economy_.recipes_of(definition);
    const auto going_rate = [&](const std::string& id) {
        const auto it = state.order_stats.find(id);
        return it == state.order_stats.end() ? economy_.reference_price(definition, id) : it->second.going_rate;
    };
    // Output value per day at reference prices (population scaling cancels out below).
    const auto output_value = [&](const std::string& output_id) {
        double value = 0.0;
        for (const auto* recipe : recipes) {
            if (recipe->role == domain::RecipeRole::Output && recipe->units_per_day > 0.0
                && (output_id.empty() || recipe->commodity_id == output_id)) {
                value += recipe->units_per_day * economy_.reference_price(definition, recipe->commodity_id);
            }
        }
        return value;
    };
    // The most a unit is worth over the recipes that use it.
    double best = 0.0;
    bool found = false;
    kind = "consume";
    for (const auto* recipe : recipes) {
        if (recipe->commodity_id != commodity_id || recipe->units_per_day >= 0.0) {
            continue;
        }
        const double use = -recipe->units_per_day;
        double value = 0.0;
        std::string role;
        switch (recipe->role) {
            case domain::RecipeRole::Input: {
                // The outputs it feeds, less the going cost of the other inputs they need.
                double margin = 0.0;
                for (const auto& output_id : recipe->feeds) {
                    double others = 0.0;
                    for (const auto* other : recipes) {
                        if (other->role == domain::RecipeRole::Input && other->commodity_id != commodity_id
                            && other->units_per_day < 0.0 && !other->feeds.empty()
                            && std::find(other->feeds.begin(), other->feeds.end(), output_id) != other->feeds.end()) {
                            others += -other->units_per_day / static_cast<double>(other->feeds.size())
                                * going_rate(other->commodity_id);
                        }
                    }
                    margin += std::max(0.0, output_value(output_id) - others);
                }
                value = (1.0 - orders.keep_share) * margin / use;
                role = "input";
                break;
            }
            case domain::RecipeRole::Upkeep: {
                // The output a full shortage would cost, per unit of use; at least what a good
                // used up without output is worth, since residents need it whatever the station
                // makes (Mars food came to 20 cr against a landed cost of 64; user, 2026-10-16).
                const auto penalty = universe_.upkeep_penalties.find(commodity_id);
                const double lost = penalty == universe_.upkeep_penalties.end() ? 0.0 : 1.0 - penalty->second;
                value = std::max(lost * output_value({}) / use, orders.consume_value * reference);
                role = "upkeep";
                break;
            }
            default:
                value = orders.consume_value * reference;
                role = "consume";
                break;
        }
        if (!found || value > best) {
            best = value;
            kind = role;
            found = true;
        }
    }
    station_ceiling = best;
    if (universe_.emergency_goods.contains(commodity_id) && economy_.is_upkeep(definition, commodity_id)) {
        kind = "life";
        return std::max(best, orders.life_value * reference);
    }
    return best;
}

Simulation::OrderLevels Simulation::order_levels(
    const domain::StationState& state, const std::string& commodity_id, const domain::OrderStats& stats) const {
    const auto& definition = get_station_definition(state.station_id);
    const auto& orders = universe_.orders;
    const bool depot_fuel = commodity_id == economy::FUEL_ID && economy_.fuel_factory_output(definition) <= 0.0;
    const auto& rates = economy_.get_station_net_rates(definition);
    const auto rate = rates.find(commodity_id);
    const double recipe_use = rate == rates.end() ? 0.0 : std::max(0.0, -rate->second);
    OrderLevels levels;
    levels.use_per_day = recipe_use + (depot_fuel ? state.ship_fuel_units_per_day : stats.ship_sales_per_day);
    const double u = levels.use_per_day;
    levels.safety_units = u * orders.safety_factor * stats.spread_days;
    // Storage is shared by every good (depot fuel stands outside it).
    double cap = u * universe_.pricing.max_cover_days;
    if (definition.storage_capacity_units > 0.0 && !depot_fuel) {
        cap = std::min(cap, definition.storage_capacity_units);
    }
    levels.order_up_to_units = std::min(cap, levels.safety_units + u * (stats.lead_days + stats.interval_days));
    // Extra days the discount pays for: holding a unit costs interest plus its spoilage. For a
    // good that does not spoil that is two years, so it is capped at one interval and at
    // max_extra_days (user, 2026-10-16): a little extra, not a stockpile.
    const double holding_per_day = universe_.ship_operations.interest_rate_per_year / 365.0
        + get_commodity(commodity_id).decay_fraction_per_day;
    const double paid_days = holding_per_day > 0.0 ? orders.extra_discount / holding_per_day : orders.max_extra_days;
    levels.extra_days = std::min({paid_days, stats.interval_days, orders.max_extra_days});
    levels.max_units = std::min(cap, levels.order_up_to_units + u * levels.extra_days);
    return levels;
}

std::vector<std::pair<double, double>> Simulation::contracted_arrivals(
    const domain::StationState& state, const std::string& commodity_id, const std::string& except_ship_id) const {
    const double decay = get_commodity(commodity_id).decay_fraction_per_day;
    std::vector<std::pair<double, double>> arrivals;
    for (const auto& ship : ships_) {
        if (ship.id != except_ship_id
            && (ship.phase == domain::ShipMissionPhase::InTransit || ship.phase == domain::ShipMissionPhase::AwaitingDeparture)
            && ship.active_mission.destination_station_id == state.station_id) {
            if (const double units = domain::units_of(ship.active_mission.cargo, commodity_id); units > 0.0) {
                arrivals.emplace_back(ship.active_mission.arrival_time_s,
                    units * std::pow(1.0 - decay, ship.active_mission.total_travel_time_s / 86400.0));
            }
        }
    }
    // Rotation contracts: the deliveries still to come on each loop, at their estimated dates
    // (the leg under way is in the cargo above).
    for (const auto& ship : ships_) {
        if (!under_contract(ship)) {
            continue;
        }
        const auto& contract = ship.contract;
        const auto count = contract.stops.size();
        std::size_t at = count;
        double t = game_time_s_;
        const bool moving = ship.phase == domain::ShipMissionPhase::InTransit
            || ship.phase == domain::ShipMissionPhase::AwaitingDeparture;
        const auto& here = moving ? ship.active_mission.destination_station_id : ship.current_station_id;
        for (std::size_t k = 0; k < count; ++k) {
            if (contract.stops[k] == here) {
                at = k;
            }
        }
        if (at == count) {
            continue;
        }
        if (moving) {
            t = std::max(game_time_s_, ship.active_mission.arrival_time_s);
        }
        const double dwell_s = contract_dwell_days(contract) * 86400.0;
        for (std::size_t step = 0; step < 2 * count; ++step) {
            const auto next = (at + 1) % count;
            if (at == 0) {
                // The wait in port at the first stop: a docked ship has served part of it.
                t = step == 0 && !moving ? std::max(t, ship.idle_since_s + dwell_s) : t + dwell_s;
            }
            t += contract.leg_days[at] * 86400.0;
            if (t > contract.until_s) {
                break;
            }
            if (contract.stops[next] == state.station_id) {
                for (const auto& delivery : contract.deliveries) {
                    if (delivery.to_station_id == state.station_id && delivery.commodity_id == commodity_id) {
                        arrivals.emplace_back(t, delivery.units);
                    }
                }
            }
            at = next;
        }
    }
    std::sort(arrivals.begin(), arrivals.end());
    return arrivals;
}

namespace {

// The projected stock of a good (today's, falling at `use` per day, rising at each contracted
// arrival) seen by a delivery landing at `t`: the open quantity N(t) = max over [t, end] of
// (safety - p), and the highest the stock gets over that window. Before the delivery the stock
// can go no lower than empty; after it, the delivery's own units fill the dip, so it runs on
// below zero.
struct ProfileGap {
    double open_units {0.0};
    double peak {0.0};
    double at_arrival {0.0};
};

ProfileGap profile_gap(const std::vector<std::pair<double, double>>& arrivals, double stock, double use,
    double safety, double now, double t, double end) {
    constexpr double day_s = 86400.0;
    double p = stock;
    double time = now;
    std::size_t next = 0;
    for (; next < arrivals.size() && arrivals[next].first <= t; ++next) {
        p = std::max(0.0, p - use * std::max(0.0, arrivals[next].first - time) / day_s) + arrivals[next].second;
        time = std::max(time, arrivals[next].first);
    }
    p = std::max(0.0, p - use * std::max(0.0, t - time) / day_s);
    time = std::max(time, t);
    const double at_arrival = p;
    double low = p;
    double peak = p;
    for (; next < arrivals.size() && arrivals[next].first <= end; ++next) {
        p -= use * (arrivals[next].first - time) / day_s;
        low = std::min(low, p);
        p += arrivals[next].second;
        peak = std::max(peak, p);
        time = arrivals[next].first;
    }
    low = std::min(low, p - use * std::max(0.0, end - time) / day_s);
    return {.open_units = std::max(0.0, safety - low), .peak = peak, .at_arrival = at_arrival};
}

// Days from now until the projected stock (every contract counted) first falls below `safety`,
// within `horizon_s`; -1 if it does not.
double days_to_safety(const std::vector<std::pair<double, double>>& arrivals, double stock, double use,
    double safety, double now, double horizon_s) {
    constexpr double day_s = 86400.0;
    if (use <= 0.0) {
        return -1.0;
    }
    double p = stock;
    double time = now;
    const auto crossing = [&](double until) {
        if (p < safety) {
            return time;
        }
        const double below_s = time + (p - safety) / use * day_s;
        return below_s < until ? below_s : -1.0;
    };
    for (const auto& [arrival_s, units] : arrivals) {
        if (arrival_s > horizon_s) {
            break;
        }
        if (const double when = crossing(arrival_s); when >= 0.0) {
            return (when - now) / day_s;
        }
        p = std::max(0.0, p - use * std::max(0.0, arrival_s - time) / day_s) + units;
        time = std::max(time, arrival_s);
    }
    const double when = crossing(horizon_s);
    return when < 0.0 ? -1.0 : (when - now) / day_s;
}

}  // namespace

domain::OrderView Simulation::order_view(const domain::StationState& state, const std::string& commodity_id) const {
    const auto& definition = get_station_definition(state.station_id);
    const auto stats_it = state.order_stats.find(commodity_id);
    const auto stats = stats_it == state.order_stats.end() ? seed_order_stats(definition, commodity_id) : stats_it->second;
    const auto levels = order_levels(state, commodity_id, stats);

    domain::OrderView view;
    view.use_per_day = levels.use_per_day;
    view.lead_days = stats.lead_days;
    view.interval_days = stats.interval_days;
    view.spread_days = stats.spread_days;
    view.going_rate = stats.going_rate;
    view.order_price = stats.order_price;
    view.reference_price = economy_.reference_price(definition, commodity_id);
    view.safety_units = levels.safety_units;
    view.reorder_units = levels.use_per_day * (stats.lead_days + universe_.orders.safety_factor * stats.spread_days);
    view.order_up_to_units = levels.order_up_to_units;
    view.extra_days = levels.extra_days;
    view.max_units = levels.max_units;
    const auto stock_it = state.inventory.find(commodity_id);
    view.stock = stock_it == state.inventory.end() ? 0.0 : std::max(0.0, stock_it->second);
    const auto arrivals = contracted_arrivals(state, commodity_id, {});
    for (const auto& [arrival_s, units] : arrivals) {
        view.contracted_units += units;
    }
    // N for a delivery one lead time from now.
    const double day_s = 86400.0;
    const double t = game_time_s_ + stats.lead_days * day_s;
    view.open_units = profile_gap(arrivals, view.stock, levels.use_per_day, levels.safety_units, game_time_s_, t,
        t + (stats.interval_days + stats.lead_days) * day_s).open_units;
    // While the projected stock falls below B within the lead time (a delivery ordered now
    // comes too late) and nobody takes the order, its price rises.
    view.days_to_safety = days_to_safety(arrivals, view.stock, levels.use_per_day, levels.safety_units, game_time_s_,
        game_time_s_ + stats.lead_days * day_s);
    view.ceiling = order_ceiling(state, commodity_id, view.ceiling_kind, view.station_ceiling);
    return view;
}

Simulation::OrderQuote Simulation::order_quote(const domain::StationState& state, const std::string& commodity_id,
    double arrival_s, const std::string& except_ship_id) const {
    const auto& stats = state.order_stats.at(commodity_id);
    const auto levels = order_levels(state, commodity_id, stats);
    const auto stock_it = state.inventory.find(commodity_id);
    const double stock = stock_it == state.inventory.end() ? 0.0 : std::max(0.0, stock_it->second);
    const double t = std::max(game_time_s_, arrival_s);
    const auto gap = profile_gap(contracted_arrivals(state, commodity_id, except_ship_id), stock, levels.use_per_day,
        levels.safety_units, game_time_s_, t, t + (stats.interval_days + stats.lead_days) * 86400.0);
    OrderQuote quote;
    quote.open_units = gap.open_units;
    quote.extra_units = std::max(0.0, levels.max_units - std::max(gap.peak + gap.open_units, levels.order_up_to_units));
    const auto bid = order_bid(state, commodity_id, levels, gap.at_arrival);
    quote.price = bid.price;
    quote.station_cap = bid.cap;
    // Competitive pricing: the order pays no more than the ships near a source ask.
    if (const auto& market = universe_.market; market.competitive > 0.0 && stats.market_price > 0.0) {
        double urgency = 1.0;
        if (market.urgency_elasticity > 0.0 && levels.order_up_to_units > 0.0) {
            urgency = std::clamp(std::pow(levels.order_up_to_units / std::max(gap.at_arrival, 0.5), market.urgency_elasticity),
                1.0, market.urgency_max);
        }
        quote.price = std::min(quote.price, stats.market_price * urgency);
    }
    // The extra only when it is cheap: below both the going rate and what the order bids now. The
    // floor follows the reference, not the going rate: a going rate raised by a shortage made
    // dumping at a floor of a quarter of it pay (47,000 u of food at Low Earth Logistics).
    const double reference = economy_.reference_price(get_station_definition(state.station_id), commodity_id);
    quote.extra_price = std::min(quote.price, stats.going_rate) * (1.0 - universe_.orders.extra_discount);
    quote.floor_price = std::min(quote.extra_price, economy::PRICE_FLOOR_MULTIPLIER * reference);
    if (const auto* emergency = find_emergency(state.station_id, commodity_id); emergency != nullptr) {
        quote.emergency_price = emergency->premium * reference;
        quote.emergency_units = emergency->units_open;
    }
    return quote;
}

Simulation::SaleSplit Simulation::quote_split(const OrderQuote& quote, double from, double units) const {
    SaleSplit split;
    if (units <= 0.0) {
        return split;
    }
    const auto overlap = [&](double lo, double hi) {
        return std::max(0.0, std::min(hi, from + units) - std::max(lo, from));
    };
    const double ordered = overlap(0.0, quote.open_units);
    const double extra = overlap(quote.open_units, quote.open_units + quote.extra_units);
    const double rest = units - ordered - extra;
    split.order_units = ordered;
    split.total = ordered * quote.price + extra * quote.extra_price + rest * quote.floor_price;
    split.order_value = ordered * quote.price;
    // An emergency's price for the first of the ordered units, paid above the order price by the faction.
    if (quote.emergency_price > quote.price) {
        const double emergency = overlap(0.0, std::min(quote.open_units, quote.emergency_units));
        split.emergency_units = emergency;
        split.faction = emergency * (quote.emergency_price - quote.price);
        split.total += split.faction;
    }
    return split;
}

Simulation::OrderBid Simulation::order_bid(const domain::StationState& state, const std::string& commodity_id,
    const OrderLevels& levels, double stock_at_arrival) const {
    // State, not time: a bid that rose while nobody came saturated at its cap almost everywhere
    // with the fleet short of ships, so ships chose by cap, not by need (8.5% unmet against 4.3%).
    // The projected stock at arrival ranks stations by how short and how soon; the price is flat
    // over the slice, so a big delivery does not cut its own price.
    // The cap is the lower of price_cap (4x; inputs input_cap, 12x) and what the good is worth to the
    // station, V (step 3), so an input that does not pay is not bought dear. V alone as the cap
    // let any dip in stock pay up to 75x for an input, and residents pay what their station paid:
    // money grew 2-5x in four years and unmet rose to 7.8%. Oxygen and water above the station's
    // cap are its faction's (emergencies, step_emergencies).
    const auto& definition = get_station_definition(state.station_id);
    const double reference = economy_.reference_price(definition, commodity_id);
    std::string kind;
    double station_ceiling = 0.0;
    static_cast<void>(order_ceiling(state, commodity_id, kind, station_ceiling));
    OrderBid bid;
    // Depot fuel: its own ceiling (depot_fuel_value x the reference). An input: its V within
    // [price_cap, input_cap], which with keep_share 0.75 is step 24's cap (a quarter of the value
    // of what a unit makes, 4-12x). V at 80% of the margin up to 12x: 3.9% unmet against 3.3%.
    const double limit = kind == "depot_fuel" ? station_ceiling / reference
        : kind == "input" ? universe_.orders.input_cap : universe_.pricing.price_cap;
    const double lowest = kind == "input" ? universe_.pricing.price_cap : economy::PRICE_FLOOR_MULTIPLIER;
    bid.cap = reference * std::clamp(station_ceiling / reference, std::min(lowest, limit), std::max(lowest, limit));
    if (levels.order_up_to_units <= 0.0) {
        bid.price = std::min(reference, bid.cap);
        return bid;
    }
    const double ratio = levels.order_up_to_units / std::max(stock_at_arrival, 0.5);
    bid.price = std::min(bid.cap, reference * std::max(economy::PRICE_FLOOR_MULTIPLIER, std::pow(ratio, economy::PRICE_ELASTICITY)));
    return bid;
}

void Simulation::update_market_prices() {
    const auto& market = universe_.market;
    if (market.competitive <= 0.0 || std::fmod(game_time_s_, 86400.0) >= TICK_S * 0.5) {
        return;
    }
    const profiling::Scope profile_scope(profiling::Phase::EstimateLeg);
    constexpr double day_s = 86400.0;
    // Where each ship is free next, and from when: docked ships now, ships under way on arrival.
    struct Free {
        const domain::ShipState* ship;
        const domain::StationDefinition* at;
        double from_s;
    };
    std::vector<Free> free;
    for (const auto& ship : ships_) {
        switch (ship.phase) {
            case domain::ShipMissionPhase::Idle:
            case domain::ShipMissionPhase::LaidUp:
            case domain::ShipMissionPhase::Refueling:
                free.push_back({&ship, &get_station_definition(ship.current_station_id), game_time_s_});
                break;
            case domain::ShipMissionPhase::InTransit:
            case domain::ShipMissionPhase::AwaitingDeparture:
                if (!ship.active_mission.destination_station_id.empty()
                    && ship.active_mission.arrival_time_s <= game_time_s_ + market.available_days * day_s) {
                    free.push_back({&ship, &get_station_definition(ship.active_mission.destination_station_id),
                        std::max(game_time_s_, ship.active_mission.arrival_time_s)});
                }
                break;
            default:
                break;
        }
    }
    const double fuel_base = get_commodity(economy::FUEL_ID).base_price;
    // A source: a station that makes the good (fuel: a factory) and has some to spare.
    const auto spare = [&](const domain::StationDefinition& station, const std::string& commodity_id) {
        const auto& state = get_station_state(station.id);
        const auto& rates = economy_.get_station_net_rates(station);
        const auto rate = rates.find(commodity_id);
        const double r = rate == rates.end() ? 0.0 : rate->second;
        const bool makes = commodity_id == economy::FUEL_ID ? economy_.fuel_factory_output(station) > 0.0 : r > 0.0;
        if (!makes) {
            return 0.0;
        }
        const auto stock = state.inventory.find(commodity_id);
        return std::max(0.0, (stock == state.inventory.end() ? 0.0 : stock->second) - (8.0 + std::max(0.0, r) * 7.0));
    };
    std::unordered_map<std::string, std::vector<double>> asks;  // station|good -> asks
    // What each order still wants (open and extra units), so a big hold's trip is spread only
    // over what a leg can carry to that station from that source.
    std::unordered_map<std::string, double> wanted;  // station|good -> units
    if (market.ask_open_units > 0.0) {
        for (const auto& state : stations_) {
            for (const auto& [commodity_id, stats] : state.order_stats) {
                if (priced_order(state, commodity_id)) {
                    const auto quote = order_quote(state, commodity_id, game_time_s_ + stats.lead_days * day_s, {});
                    wanted[state.station_id + "|" + commodity_id] = quote.open_units + quote.extra_units;
                }
            }
        }
    }
    const auto joint_load = [&](const domain::StationDefinition& source, const domain::StationState& destination) {
        double units = 0.0;
        for (const auto& [commodity_id, stats] : destination.order_stats) {
            const auto it = wanted.find(destination.station_id + "|" + commodity_id);
            if (it != wanted.end()) {
                const double available = spare(source, commodity_id);
                if (available >= market.min_source_surplus) {
                    units += std::min(it->second, available);
                }
            }
        }
        return units;
    };
    // Plans are estimated in parallel passes, like dispatch: a pass that misses plans queues them.
    with_parallel_plans("market#", [&] {
        asks.clear();
        for (const auto& state : stations_) {
            const auto& destination = get_station_definition(state.station_id);
            for (const auto& [commodity_id, stats] : state.order_stats) {
                if (!priced_order(state, commodity_id)) {
                    continue;
                }
                const auto& commodity = get_commodity(commodity_id);
                auto& list = asks[state.station_id + "|" + commodity_id];
                for (const auto& f : free) {
                    if (f.at->id == destination.id || spare(*f.at, commodity_id) < market.min_source_surplus) {
                        continue;
                    }
                    const auto& ship_class = ship_design(*f.ship);
                    if (ship_class.cargo_capacity_units <= 0.0) {
                        continue;
                    }
                    const double hold_kg = ship_class.cargo_capacity_units * commodity.mass_per_unit_kg;
                    const auto leg = estimate_leg(ship_class, *f.at, destination, f.from_s, ship_class.propellant_capacity_kg,
                        market.fill_ref * hold_kg);
                    if (!leg.feasible) {
                        continue;
                    }
                    double crew = ship_class.crew_size * universe_.ship_operations.wage_cr_per_crew_day;
                    for (const auto& [good, units_per_crew_day] : universe_.ship_operations.life_support_units_per_crew_day) {
                        crew += ship_class.crew_size * units_per_crew_day * get_commodity(good).base_price;
                    }
                    const double fuel_price = std::max(fuel_base, station_price(get_station_state(f.at->id), economy::FUEL_ID));
                    const double trip = leg.travel_days * (daily_capital_cost(ship_class) + crew
                            + hazard_cr_per_day(ship_class, *f.at, destination, f.from_s) + wear_cr_per_flight_day(ship_class))
                        + leg.propellant_kg / FUEL_UNITS_TO_KG * fuel_price
                        + leg.propellant_kg * wear_cr_per_propellant_kg(ship_class);
                    double units = std::max(1.0, market.fill_ref * ship_class.cargo_capacity_units);
                    if (market.ask_open_units > 0.0) {
                        units = std::clamp(joint_load(*f.at, state), 1.0, units);
                    }
                    const double buy = station_price(get_station_state(f.at->id), commodity_id);
                    list.push_back(buy + trip * (1.0 + market.return_share) / units * (1.0 + market.ask_markup));
                }
            }
        }
    });
    for (auto& state : stations_) {
        for (auto& [commodity_id, stats] : state.order_stats) {
            auto it = asks.find(state.station_id + "|" + commodity_id);
            if (it == asks.end() || it->second.empty()) {
                stats.market_price = 0.0;
                stats.market_bidders = 0;
                continue;
            }
            auto& list = it->second;
            std::sort(list.begin(), list.end());
            stats.market_bidders = static_cast<int>(list.size());
            stats.market_price = list.size() >= 2 ? list[1] : list[0] * (1.0 + market.solo_premium);
        }
    }
}

void Simulation::step_orders() {
    for (auto& state : stations_) {
        for (auto& [commodity_id, stats] : state.order_stats) {
            const auto quote = order_quote(state, commodity_id, game_time_s_ + stats.lead_days * 86400.0, {});
            stats.order_price = quote.price;
            // Debug aid: SPACETRAINS_TRACE_ORDER="<station id>:<good>" logs that order once a day (stderr).
            if (static const char* const trace_order = std::getenv("SPACETRAINS_TRACE_ORDER");
                trace_order != nullptr && state.station_id + ":" + commodity_id == trace_order
                && std::fmod(game_time_s_, 86400.0) < Simulation::TICK_S * 0.5) {
                const auto view = order_view(state, commodity_id);
                std::cerr << std::format("[ORDER] day {:.0f} {} stock {:.0f} coming {:.0f} S {:.0f} N {:.0f} short {:.0f} "
                    "P {:.1f} G {:.1f} L {:.0f} I {:.0f} sigma {:.0f}\n", game_time_s_ / 86400.0, trace_order, view.stock,
                    view.contracted_units, view.order_up_to_units, view.open_units, view.days_to_safety, stats.order_price,
                    stats.going_rate, stats.lead_days, stats.interval_days, stats.spread_days);
            }
        }
    }
}

double Simulation::consumer_bid(const domain::StationState& state, const std::string& commodity_id) const {
    const auto it = state.order_stats.find(commodity_id);
    // The going rate, not today's bid: a calm order probes down toward the floor, and a route
    // is bought for years (valued at the bid, local routes looked worthless).
    return priced_order(state, commodity_id) ? std::max(it->second.order_price, it->second.going_rate) : station_price(state, commodity_id);
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
    const auto& rates = economy.get_station_net_rates(station_def);
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

double Simulation::persons_aboard(const domain::ShipState& ship) const {
    double persons = ship_design(ship).crew_size;
    for (const auto& lot : ship.passengers) {
        persons += lot.count;
    }
    return persons;
}

double Simulation::provisionable_days(const domain::ShipState& ship, const domain::StationState& market) const {
    const auto& market_def = get_station_definition(market.station_id);
    double days = std::numeric_limits<double>::infinity();
    for (const auto& [commodity_id, units_per_crew_day] : universe_.ship_operations.life_support_units_per_crew_day) {
        const double need_per_day = persons_aboard(ship) * units_per_crew_day;
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
    const auto& market_def = get_station_definition(market.station_id);
    for (const auto& [commodity_id, units_per_crew_day] : universe_.ship_operations.life_support_units_per_crew_day) {
        const double missing = persons_aboard(ship) * units_per_crew_day * days - ship.provisions[commodity_id];
        const double units = std::min(missing, sellable_units(economy_, market_def, market, commodity_id, departing));
        if (units <= 0.0) {
            continue;
        }
        // Price on the stock before the transfer, like every other trade.
        const double unit_price = station_price(market, commodity_id);
        const double bill = units * unit_price;
        market.inventory[commodity_id] -= units;
        market.credits += bill;
        market.ledger.ship_sales += bill;
        if (const auto stats = market.order_stats.find(commodity_id); stats != market.order_stats.end()) {
            stats->second.ship_sales_per_day += units / TRADE_FLOW_DAYS;
        }
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

double Simulation::station_r_au(const domain::StationDefinition& station, double time_s) const {
    // Asked many times per tick at the same few times (now, a bucket start): remember the
    // last answer per station. Simulation thread only; a pure cache.
    auto& slot = station_radius_cache_[station.id];
    if (slot.first != time_s || slot.second <= 0.0) {
        slot = {time_s, mechanics_.get_heliocentric_radius(station.parent_body_id, time_s) / 1.495978707e11};
    }
    return slot.second;
}

double Simulation::docked_dose_msv_per_day(const std::string& station_id, double time_s) const {
    const auto& env = universe_.ship_environment;
    const auto it = universe_.station_radiation.find(station_id);
    const domain::StationRadiationDefinition open_space;
    return crew_environment::expected_docked_msv_per_day(env, it == universe_.station_radiation.end() ? open_space : it->second,
        station_r_au(get_station_definition(station_id), time_s), time_s / 86400.0);
}

double Simulation::cabin_shield_g_cm2(const domain::ShipClassDefinition& design, double propellant_kg) const {
    if (!active_effects_.contains("cabin_shield_g_cm2")) {
        return 0.0;
    }
    return crew_environment::cabin_shield_g_cm2(design, propellant_kg);
}

double Simulation::leg_dose_msv_per_day(const domain::ShipClassDefinition& design,
    const domain::StationDefinition& from, const domain::StationDefinition& to, double time_s) const {
    // Before the leg is flown: cabins inside the tanks count their mean fill over a leg.
    const double cabin = cabin_shield_g_cm2(design, universe_.passengers.tank_wrap_mean_fill * design.propellant_capacity_kg);
    return crew_environment::expected_transit_msv_per_day(universe_.ship_environment, design,
        station_r_au(from, time_s), station_r_au(to, time_s), time_s / 86400.0, cabin);
}

double Simulation::hazard_cr_per_day(const domain::ShipClassDefinition& design,
    const domain::StationDefinition& from, const domain::StationDefinition& to, double time_s) const {
    const auto& env = universe_.ship_environment;
    if (!env.radiation || env.hazard_cr_per_msv <= 0.0) {
        return 0.0;
    }
    return design.crew_size * leg_dose_msv_per_day(design, from, to, time_s) * env.hazard_cr_per_msv;
}

bool Simulation::leg_dose_allowed(const domain::ShipClassDefinition& design,
    const domain::StationDefinition& from, const domain::StationDefinition& to, double time_s, double days) const {
    const auto& env = universe_.ship_environment;
    return !env.radiation || leg_dose_msv_per_day(design, from, to, time_s) * days <= env.career_limit_msv;
}

void Simulation::accrue_radiation(domain::ShipState& ship, const domain::ShipClassDefinition& design, double days) {
    const auto& env = universe_.ship_environment;
    if (!env.radiation || days <= 0.0) {
        return;
    }
    double rate = 0.0;
    if (ship.phase == domain::ShipMissionPhase::InTransit) {
        // Cosmic rays where the ship is now (the distance from the Sun interpolated over the
        // leg); particle events strike separately (step_particle_events).
        rate = crew_environment::gcr_msv_per_day(env, ship_r_au(ship), game_time_s_ / 86400.0)
            * crew_environment::transit_gcr_factor(env, design, cabin_shield_g_cm2(design, ship.propellant_kg));
    } else {
        // In port the crew lives in the station's habitat.
        const auto it = universe_.station_radiation.find(ship.current_station_id);
        const double factor = it == universe_.station_radiation.end() ? 1.0 : it->second.location_factor;
        const double shield = it == universe_.station_radiation.end() ? 0.0 : it->second.shield_g_cm2;
        rate = factor * std::exp(-shield / env.gcr_attenuation_g_cm2)
            * crew_environment::gcr_msv_per_day(env, station_r_au(get_station_definition(ship.current_station_id), game_time_s_),
                game_time_s_ / 86400.0);
    }
    const double dose = rate * days;
    ship.leg_dose_msv += dose;
    const double pay = design.crew_size * dose * env.hazard_cr_per_msv;
    if (pay > 0.0) {
        pay_crew(ship, pay);
        ship.ledger.wages += pay;
    }
    radiation_audit_.crew_dose_msv += design.crew_size * dose;
    radiation_audit_.hazard_pay_cr += pay;
    radiation_audit_.highest_leg_dose_msv = std::max(radiation_audit_.highest_leg_dose_msv, ship.leg_dose_msv);
}

double Simulation::cargo_decay_per_day(const domain::ShipClassDefinition& design, const std::string& commodity_id) const {
    const double ambient = get_commodity(commodity_id).decay_fraction_per_day;
    if (ambient <= 0.0 || commodity_id == economy::FUEL_ID || !universe_.ship_environment.cold_storage) {
        return ambient;
    }
    const auto it = design.effects.find("perishable_decay_factor");
    return it == design.effects.end() ? ambient : ambient * it->second;
}

double Simulation::drive_life_h(const domain::ShipClassDefinition& design) const {
    const auto& env = universe_.ship_environment;
    return design.propulsion_type == "variable_isp" ? env.plasma_drive_life_h : env.ntr_core_life_h;
}

double Simulation::drive_hours(const domain::ShipClassDefinition& design, const domain::TrajectoryPlan& plan) const {
    if (design.propulsion_type == "variable_isp") {
        return plan.coast_time_s / 3600.0;  // plasma drives thrust the whole transfer
    }
    for (const auto& part : design.components) {
        if (part.kind == "engine" && part.thrust_n > 0.0 && part.isp_s > 0.0) {
            const double mass_flow_kg_s = part.thrust_n / (part.isp_s * 9.80665);
            return plan.propellant_required_kg / mass_flow_kg_s / 3600.0;
        }
    }
    return 0.0;
}

bool Simulation::drive_life_allows(const domain::ShipState& ship, const domain::ShipClassDefinition& design,
    const domain::TrajectoryPlan& plan) const {
    if (!universe_.ship_environment.wear) {
        return true;
    }
    return drive_hours(design, plan) <= (1.0 - ship.engine_wear) * drive_life_h(design);
}

void Simulation::accrue_wear(domain::ShipState& ship, const domain::ShipClassDefinition& design, double days) {
    const auto& env = universe_.ship_environment;
    if (!env.wear || days <= 0.0) {
        return;
    }
    // Nuclear-thermal burns are booked when the mission starts (they are impulsive).
    if (design.propulsion_type == "variable_isp" && ship.phase == domain::ShipMissionPhase::InTransit) {
        ship.engine_wear = std::min(1.0, ship.engine_wear + days * 24.0 / env.plasma_drive_life_h);
    }
    double radiator_area = 0.0;
    for (const auto& part : design.components) {
        if (part.kind == "radiator") {
            radiator_area += part.area_m2;
        }
    }
    if (radiator_area <= 0.0) {
        return;
    }
    // Expected heat-pipe segments lost: hits per m2 of pipe x segment area / panel area.
    const double r = std::max(0.1, ship_r_au(ship));
    const double hits_per_m2 = env.puncture_flux_m2_yr * std::pow(r, -env.puncture_flux_exponent) * days / 365.25;
    const double before = radiator_bucket(ship);
    ship.radiator_condition = std::max(0.0,
        ship.radiator_condition - hits_per_m2 * env.radiator_vulnerable_fraction * env.radiator_segment_m2);
    if (const double after = radiator_bucket(ship); after != before) {
        (void)register_design(ship.class_id, ship.fit, after);
    }
}

Simulation::OverhaulBill Simulation::overhaul_bill(const domain::ShipClassDefinition& design, double drive_share,
    double radiator_share) const {
    // The wearing parts (spares.csv) in the share that is worn: their goods, and the hours to
    // make the spares from them and fit them, at the labour scale.
    OverhaulBill bill;
    for (const auto& part : design.components) {
        if (part.parent_id.empty()) {
            continue;
        }
        const auto spec = universe_.spare_parts.find(part.kind);
        if (spec == universe_.spare_parts.end()) {
            continue;
        }
        const double share = spec->second.wears_with == "drive" ? drive_share : radiator_share;
        const double kg = part.mass_kg * share;
        if (kg <= 0.0) {
            continue;
        }
        for (const auto& [commodity_id, mass_share] : spec->second.materials) {
            bill.spares_units[commodity_id] += kg * mass_share / get_commodity(commodity_id).mass_per_unit_kg;
        }
        const double hours = kg * (spec->second.fabrication_h_per_kg + spec->second.install_h_per_kg);
        bill.hours += hours;
        bill.labour_cr += hours * universe_.labour.wage_cr_per_hour(spec->second.grade);
    }
    const auto& env = universe_.ship_environment;
    bill.days = std::max(env.min_repair_days, bill.hours / (env.repair_team_size * env.repair_team_hours_per_day));
    return bill;
}

double Simulation::wear_cr_per_drive_hour(const domain::ShipClassDefinition& design) const {
    if (!universe_.ship_environment.wear) {
        return 0.0;
    }
    // A whole drive life's overhaul (goods at base price), spread over its hours.
    const auto bill = overhaul_bill(design, 1.0, 0.0);
    double cost = bill.labour_cr;
    for (const auto& [commodity_id, units] : bill.spares_units) {
        cost += units * get_commodity(commodity_id).base_price;
    }
    return cost / drive_life_h(design);
}

double Simulation::wear_cr_per_flight_day(const domain::ShipClassDefinition& design) const {
    // Plasma drives thrust the whole transfer.
    return design.propulsion_type == "variable_isp" ? 24.0 * wear_cr_per_drive_hour(design) : 0.0;
}

double Simulation::wear_cr_per_propellant_kg(const domain::ShipClassDefinition& design) const {
    // Nuclear-thermal cores wear by the hour of burn, that is by the kilogram burned.
    if (design.propulsion_type == "variable_isp" || !universe_.ship_environment.wear) {
        return 0.0;
    }
    for (const auto& part : design.components) {
        if (part.kind == "engine" && part.thrust_n > 0.0 && part.isp_s > 0.0) {
            const double mass_flow_kg_s = part.thrust_n / (part.isp_s * 9.80665);
            return wear_cr_per_drive_hour(design) / (mass_flow_kg_s * 3600.0);
        }
    }
    return 0.0;
}

bool Simulation::consider_repair(domain::ShipState& ship) {
    const auto& env = universe_.ship_environment;
    if (!env.wear || ship.phase != domain::ShipMissionPhase::Idle) {
        return false;
    }
    const auto& design = ship_design(ship);
    const bool drive_due = ship.engine_wear >= env.overhaul_wear;
    const bool radiators_due = ship.radiator_condition < env.radiator_repair_condition;
    if (!drive_due && !radiators_due) {
        return false;
    }
    const auto bill = overhaul_bill(design, drive_due ? ship.engine_wear : 0.0,
        radiators_due ? 1.0 - ship.radiator_condition : 0.0);
    auto& yard = get_station_state(ship.current_station_id);
    double cost = bill.labour_cr;
    for (const auto& [commodity_id, units] : bill.spares_units) {
        const auto stock = yard.inventory.find(commodity_id);
        if (stock == yard.inventory.end() || stock->second < units) {
            ++radiation_audit_.repairs_waiting_for_spares;
            return false;  // no spares here: fly on within the drive's remaining life
        }
        cost += units * station_price(yard, commodity_id);
    }
    if (ship.credits - cost < -credit_line(design)) {
        return false;
    }
    double spares_cr = 0.0;
    for (const auto& [commodity_id, units] : bill.spares_units) {
        const double value = units * station_price(yard, commodity_id);
        spares_cr += value;
        yard.inventory[commodity_id] -= units;
        record_trade({
            .time_s = game_time_s_,
            .ship_id = ship.id,
            .station_id = yard.station_id,
            .commodity_id = commodity_id,
            .kind = "spares",
            .units = units,
            .unit_price = station_price(yard, commodity_id),
            .total = value,
        });
        radiation_audit_.spares_units[commodity_id] += units;
    }
    ship.credits -= cost;
    ship.lifetime_profit -= cost;
    ship.ledger.refits += cost;
    yard.credits += cost;
    yard.ledger.ship_sales += cost;
    radiation_audit_.repair_bills_cr += cost;
    radiation_audit_.repair_spares_cr += spares_cr;
    radiation_audit_.repair_labour_cr += bill.labour_cr;
    radiation_audit_.repair_days += bill.days;
    ++radiation_audit_.overhauls;
    ++(design.propulsion_type == "variable_isp" ? radiation_audit_.plasma_overhauls : radiation_audit_.thermal_overhauls);
    ship.overhaul = true;
    ship.refit_class_id = ship.class_id;
    ship.refit_fit = ship.fit;
    ship.refit_done_s = game_time_s_ + bill.days * 86400.0;
    ship.phase = domain::ShipMissionPhase::Refitting;
    add_event(std::format("{} overhauling at {}: {}{}{} ({:.0f} cr, {:.1f} days)", ship.name,
        get_station_definition(ship.current_station_id).name,
        drive_due ? (design.propulsion_type == "variable_isp" ? "plasma drive" : "reactor core") : "",
        drive_due && radiators_due ? " and " : "", radiators_due ? "radiators" : "", cost, bill.days), "mission");
    return true;
}

double Simulation::ship_r_au(const domain::ShipState& ship) const {
    if (ship.phase != domain::ShipMissionPhase::InTransit) {
        return station_r_au(get_station_definition(ship.current_station_id), game_time_s_);
    }
    const auto& mission = ship.active_mission;
    const double r1 = station_r_au(get_station_definition(mission.origin_station_id), mission.departure_time_s);
    const double r2 = station_r_au(get_station_definition(mission.destination_station_id), mission.arrival_time_s);
    const double span = std::max(1.0, mission.arrival_time_s - mission.departure_time_s);
    const double f = std::clamp((game_time_s_ - mission.departure_time_s) / span, 0.0, 1.0);
    return r1 + f * (r2 - r1);
}

double Simulation::boiloff_kg_per_day(const domain::ShipClassDefinition& design, double r_au) const {
    return leg_boiloff_kg_per_day(design, r_au, r_au);
}

double Simulation::leg_boiloff_kg_per_day(const domain::ShipClassDefinition& design, double r1_au, double r2_au) const {
    const auto& env = universe_.ship_environment;
    if (!env.boiloff || design.propulsion_type == "variable_isp") {
        return 0.0;
    }
    double volume_m3 = 0.0;
    for (const auto& part : design.components) {
        if (part.kind == "tank") {
            volume_m3 += part.volume_m3;
        }
    }
    const double area_m2 = env.tank_area_factor * 4.0 * std::numbers::pi
        * std::pow(3.0 * volume_m3 / (4.0 * std::numbers::pi), 2.0 / 3.0);
    // A 90 K broad-area-cooled shield intercepts a share of the leak; a 20 K cryocooler lifts
    // the rest up to its rating.
    const auto shield = design.effects.find("boiloff_intercept");
    const auto lift = design.effects.find("zbo_lift_w");
    const double intercepted = shield == design.effects.end() ? 0.0 : std::clamp(shield->second, 0.0, 1.0);
    const double lift_w = lift == design.effects.end() ? 0.0 : lift->second;
    const auto net_w = [&](double mean_r_power) {
        return std::max(0.0, env.mli_flux_w_m2 * mean_r_power * area_m2 * (1.0 - intercepted) - lift_w);
    };
    double watts = 0.0;
    if (lift_w <= 0.0 || r1_au == r2_au) {
        watts = net_w(crew_environment::mean_inverse_power(r1_au, r2_au, env.mli_flux_exponent));
    } else {
        // A cooler that keeps up near one end but not the other: average the net leak along the leg.
        constexpr int kSteps = 16;
        for (int i = 0; i < kSteps; ++i) {
            const double r = r1_au + (i + 0.5) / kSteps * (r2_au - r1_au);
            watts += net_w(crew_environment::mean_inverse_power(r, r, env.mli_flux_exponent)) / kSteps;
        }
    }
    return watts / env.lh2_latent_j_kg * 86400.0;
}

double Simulation::leg_boiloff_kg_per_day(const domain::ShipClassDefinition& design,
    const domain::StationDefinition& from, const domain::StationDefinition& to, double time_s) const {
    if (!universe_.ship_environment.boiloff || design.propulsion_type == "variable_isp") {
        return 0.0;
    }
    return leg_boiloff_kg_per_day(design, station_r_au(from, time_s), station_r_au(to, time_s));
}

void Simulation::step_particle_events(double dt_s) {
    const auto& env = universe_.ship_environment;
    if (!env.radiation || !events_enabled_) {
        return;
    }
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    const double roll = uniform(environment_rng_);
    const double size = uniform(environment_rng_);
    const double day = game_time_s_ / 86400.0;
    const double rate_per_day = env.spe_rate_per_year / 365.25 * 2.0 * crew_environment::solar_activity(env, day);
    if (roll >= rate_per_day * dt_s / 86400.0) {
        return;
    }
    const double magnitude = crew_environment::draw_spe_magnitude(env, size);
    ++radiation_audit_.particle_events;
    radiation_audit_.largest_event_magnitude = std::max(radiation_audit_.largest_event_magnitude, magnitude);
    int hit = 0;
    double worst = 0.0;
    std::string worst_ship;
    for (auto& ship : ships_) {
        if (ship.phase == domain::ShipMissionPhase::LaidUp || ship.phase == domain::ShipMissionPhase::Refitting) {
            continue;  // no crew aboard
        }
        const auto& design = ship_design(ship);
        double dose = 0.0;
        if (ship.phase == domain::ShipMissionPhase::InTransit) {
            const double r = std::max(0.1, ship_r_au(ship));
            dose = magnitude * crew_environment::spe_reference_msv(env,
                crew_environment::shelter_g_cm2(design) + cabin_shield_g_cm2(design, ship.propellant_kg)) / (r * r);
            ++hit;
        } else {
            // Stations have shelters.
            const auto it = universe_.station_radiation.find(ship.current_station_id);
            const double factor = it == universe_.station_radiation.end() ? 1.0 : it->second.location_factor;
            const double r = std::max(0.1, station_r_au(get_station_definition(ship.current_station_id), game_time_s_));
            dose = factor * magnitude * env.spe_shelter_msv / (r * r);
        }
        ship.leg_dose_msv += dose;
        const double pay = design.crew_size * dose * env.hazard_cr_per_msv;
        if (pay > 0.0) {
            pay_crew(ship, pay);
            ship.ledger.wages += pay;
        }
        radiation_audit_.crew_dose_msv += design.crew_size * dose;
        radiation_audit_.hazard_pay_cr += pay;
        radiation_audit_.highest_leg_dose_msv = std::max(radiation_audit_.highest_leg_dose_msv, ship.leg_dose_msv);
        if (dose > worst) {
            worst = dose;
            worst_ship = ship.name;
        }
        if (ship.leg_dose_msv > env.career_limit_msv && ship.leg_dose_msv - dose <= env.career_limit_msv) {
            add_event(std::format("{}'s crew passed the {:.0f} mSv career limit ({:.0f} mSv this leg)",
                ship.name, env.career_limit_msv, ship.leg_dose_msv), "alert");
        }
    }
    add_event(std::format("Solar particle event ({:.2f}x August 1972): {} ships in flight exposed{}", magnitude, hit,
        worst > 0.0 ? std::format(", worst {} with {:.0f} mSv", worst_ship, worst) : std::string()), "news");
}

void Simulation::pay_crew(domain::ShipState& ship, double amount) {
    // The crew are residents of the home station: with households on (economy overhaul) their pay
    // goes to its households, who spend it there; before, to the station itself.
    if (!households_on()) {
        pay_home_station(ship, amount);
        return;
    }
    ship.credits -= amount;
    ship.lifetime_profit -= amount;
    auto& home = get_station_state(ship.home_station_id);
    home.household_credits += amount;
    home.ledger.crew_wages += amount;
}

void Simulation::pay_home_station(domain::ShipState& ship, double amount) {
    ship.credits -= amount;
    ship.lifetime_profit -= amount;
    get_station_state(ship.home_station_id).credits += amount;
}

void Simulation::accrue_operating_costs(domain::ShipState& ship, double dt_s) {
    const auto& ship_class = ship_design(ship);
    const double days = dt_s / 86400.0;

    // Liquid hydrogen boils off in flight (tier 2, step F); in port the depot keeps it cold.
    if (const double boil = ship.phase == domain::ShipMissionPhase::InTransit
                ? boiloff_kg_per_day(ship_class, ship_r_au(ship)) * days : 0.0;
        boil > 0.0 && ship.propellant_kg > 0.0) {
        const double lost = std::min(ship.propellant_kg, boil);
        ship.propellant_kg -= lost;
        radiation_audit_.hydrogen_boiled_kg += lost;
        // Under way the tank level follows the plan's mass samples less what has boiled.
        ship.active_mission.boiled_propellant_kg += lost;
    }
    accrue_wear(ship, ship_class, days);
    // Capital (interest + amortisation) is owed whatever the ship does.
    const double capital = daily_capital_cost(ship_class) * days;
    pay_home_station(ship, capital);
    ship.ledger.capital += capital;
    if (ship.phase == domain::ShipMissionPhase::LaidUp || ship.phase == domain::ShipMissionPhase::Refitting) {
        return;  // crew discharged: no wages, no life support
    }

    const double wages = ship_class.crew_size * universe_.ship_operations.wage_cr_per_crew_day * days;
    pay_crew(ship, wages);
    ship.ledger.wages += wages;
    accrue_radiation(ship, ship_class, days);

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
    const double persons = persons_aboard(ship);
    for (const auto& [commodity_id, units_per_crew_day] : life_support) {
        // Passengers eat too while aboard; the crew's return reserve stays the crew's.
        const double use = ship_class.crew_size * units_per_crew_day;
        auto& carried = ship.provisions[commodity_id];
        carried = std::max(0.0, carried - persons * units_per_crew_day * days);
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
    const auto& ship_class = ship_design(ship);
    // Leave the station its own working reserve (it may consume fuel too) — except for a
    // nearly-dry ship, which may tap the reserve to get unstuck.
    const bool emergency = ship.propellant_kg <= ship_class.propellant_capacity_kg * 0.05;
    const auto& station_rates = economy_.get_station_net_rates(get_station_definition(ship.current_station_id));
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
    station.ledger.ship_sales += fuel_bill;
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
        .propellant_cr_per_kg = get_commodity("fuel").base_price / FUEL_UNITS_TO_KG + wear_cr_per_propellant_kg(ship_class),
        .time_cr_per_day = daily_capital_cost(ship_class) + typical_crew_cost
            + hazard_cr_per_day(ship_class, origin, destination, static_cast<double>(bucket) * BUCKET_S)
            + wear_cr_per_flight_day(ship_class),
        .payload_kg = static_cast<double>(payload_bucket) * PAYLOAD_STEP_KG,
        .purchasable_propellant_kg = static_cast<double>(fuel_bucket) * fuel_step_kg,
        .reserve_fraction = PROPELLANT_RESERVE_FRACTION,
        // Boil-off averaged over the leg (the leak falls about as 1/r).
        .boiloff_kg_per_day = leg_boiloff_kg_per_day(ship_class, origin, destination,
            static_cast<double>(bucket) * BUCKET_S),
        .include_path = false,
    };
    leg_estimates_.erase(leg_estimates_.begin(),
        leg_estimates_.lower_bound(static_cast<std::int64_t>(std::floor(game_time_s_ / BUCKET_S))));
    auto& slot = leg_estimates_[bucket];
    const auto key = std::format("{}|{}|{}|{}|{}", ship_class.id, origin.id, destination.id, fuel_bucket, payload_bucket);
    // Measured from this caller's departure, not the bucket edge.
    // A leg that would take its crew past the career limit is not flown (tier 2 radiation).
    const auto for_caller = [&, departure_time_s](LegEstimate estimate) {
        estimate.travel_days = std::max(0.0, estimate.arrival_time_s - departure_time_s) / 86400.0;
        if (!leg_dose_allowed(ship_class, origin, destination, departure_time_s, estimate.travel_days)) {
            estimate.feasible = false;
        }
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
            .propellant_kg = plan.propellant_required_kg + plan.propellant_boiled_kg,
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
    std::vector<MissionChoice::PassengerPick> best_passengers;
    std::vector<MissionChoice::CargoOption> cargo_options;

    // A newly commissioned ship works the route it was bought for: from home only to its route
    // destination, from anywhere else only home. Otherwise it left once the gap it was bought
    // for closed, the gap reopened and the treasuries ordered another ship for it.
    // A liner (step 22) flies only to the stop after the one it is at, forever.
    // A ship under a rotation contract flies its loop the same way.
    const bool liner = !cargo_only && scheduled_stops(ship) != nullptr;
    const bool on_route = liner || (!cargo_only && !ship.route_destination_id.empty() && game_time_s_ < ship.route_until_s);
    const auto route_leg_allowed = [&](const std::string& from_id, const std::string& to_id) {
        if (liner) {
            return to_id == next_scheduled_stop(ship, from_id);
        }
        return !on_route || to_id == (from_id == ship.home_station_id ? ship.route_destination_id : ship.home_station_id);
    };

    // Every day of a mission (waiting for the window included) costs capital,
    // wages and provisions; missions are compared on profit after that cost.
    const double time_cost_per_day = daily_capital_cost(ship_class) + daily_crew_cost(ship_class, origin_state)
        + hazard_cr_per_day(ship_class, origin_def, origin_def, game_time_s_) + wear_cr_per_flight_day(ship_class);
    // The crew must be provisionable from here for the whole mission.
    // (The return-leg reserve is bought too, but only if the station has it.)
    const double max_mission_days = provisionable_days(ship, origin_state) / PROVISION_MARGIN;
    // And no leg may take the crew signed on for it past its career dose limit (tier 2).
    const auto dose_allows = [&](const domain::StationDefinition& to, double days) {
        return leg_dose_allowed(ship_class, origin_def, to, game_time_s_, days);
    };
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
                                        double carried_kg,
                                        bool tankering = true) {
        const auto& dest_state = get_station_state(destination.id);
        // The load's boil-off allowance is gone on arrival.
        const double after_arrival_kg = std::max(0.0,
            plan.propellant_load_kg - plan.propellant_required_kg - plan.propellant_boiled_kg) + carried_kg;
        // Economic tankering: where the destination sells fuel at more than twice this port's
        // price, the ship carries its return fuel rather than buying it there.
        const double dest_fuel_kg = tankering && station_price(dest_state, "fuel") > TANKERING_PRICE_RATIO * planning_fuel_price
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
        // The way back boils off too (as long a coast, the other way round).
        const double boiled_back_kg = leg_boiloff_kg_per_day(ship_class, destination, origin_def, game_time_s_)
            * plan.coast_time_s / 86400.0;
        return std::max(0.0,
            needed_kg * (1.0 + PROPELLANT_RESERVE_FRACTION) + boiled_back_kg - after_arrival_kg - dest_fuel_kg);
    };
    // Planners trade travel time against propellant at the owner's prices, and load
    // propellant for the transfer plus the reserve.
    // Hydrogen boil-off averaged over the leg (tier 2, step F).
    // Passengers aboard add their time (and dose) to the ship's: `rider_time_cr_per_day`.
    // A full-tank plan (fleet investment's passenger probes) may buy a whole tank: a route is
    // valued as it will be flown, not on the fuel this port happens to have on sale today.
    const auto planning_options = [&](const domain::StationDefinition& destination, double cargo_kg,
                                      double rider_time_cr_per_day = 0.0, bool full_tank = false) {
        return trajectory::PlanningOptions {
            .propellant_cr_per_kg = planning_fuel_price / FUEL_UNITS_TO_KG + wear_cr_per_propellant_kg(ship_class),
            .time_cr_per_day = time_cost_per_day + rider_time_cr_per_day,
            .payload_kg = provisions_kg + cargo_kg,
            .purchasable_propellant_kg = full_tank
                ? std::max(0.0, ship_class.propellant_capacity_kg - std::max(0.0, ship.propellant_kg))
                : origin_fuel_for_sale_kg,
            .reserve_fraction = PROPELLANT_RESERVE_FRACTION,
            .boiloff_kg_per_day = leg_boiloff_kg_per_day(ship_class, origin_def, destination, game_time_s_),
            .include_path = false,  // the chosen mission is planned again with its path
        };
    };
    // What each plan was planned with, so the chosen one can be planned again with its path.
    std::unordered_map<const domain::TrajectoryPlan*, std::pair<trajectory::PlanningOptions, double>> plan_inputs;
    const auto plan_to = [&](const domain::StationDefinition& destination, double cargo_kg,
                             double rider_time_cr_per_day = 0.0, bool full_tank = false) -> const domain::TrajectoryPlan& {
        // Cargo rounded up to whole tonnes keeps the cache small and the estimate safe.
        const double cargo_bucket_kg = std::ceil(std::max(0.0, cargo_kg) / 1000.0) * 1000.0;
        // Riders adding less than rider_time_min_share of the ship's own time cost fly on the plan
        // without them; above, their time is bucketed in rider_time_relative_step of it.
        const auto& pax = universe_.passengers;
        const double riders = std::max(0.0, rider_time_cr_per_day);
        const double rider_step = std::max(1.0, pax.rider_time_relative_step * time_cost_per_day);
        const double rider_bucket = riders < pax.rider_time_min_share * time_cost_per_day
            ? 0.0 : std::ceil(riders / rider_step) * rider_step;
        auto key = rider_bucket > 0.0 ? std::format("{}|{:.0f}|t{:.3f}", destination.id, cargo_bucket_kg, rider_bucket)
                                      : std::format("{}|{:.0f}", destination.id, cargo_bucket_kg);
        if (full_tank) {
            key += "|full";
        }
        if (const auto it = plans.find(key); it != plans.end()) {
            plan_inputs[&it->second] = {planning_options(destination, cargo_bucket_kg, rider_bucket, full_tank),
                std::max(game_time_s_, earliest_departure_s)};
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
                           options = planning_options(destination, cargo_bucket_kg, rider_bucket, full_tank)] {
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
    const auto plan_with_return_fuel = [&](const domain::StationDefinition& destination, double cargo_kg,
                                           double rider_time_cr_per_day = 0.0, bool full_tank = false) {
        const double limit_kg = full_tank ? ship_class.propellant_capacity_kg : fuel_limit_kg;
        const auto& plan = plan_to(destination, cargo_kg, rider_time_cr_per_day, full_tank);
        if (!plan.feasible) {
            return FuelAwarePlan {&plan, 0.0, false};
        }
        double carried_kg = leave_shortfall_kg(destination, plan, cargo_kg, 0.0);
        if (carried_kg <= 0.0) {
            return FuelAwarePlan {&plan, 0.0, true};
        }
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto& laden = plan_to(destination, cargo_kg + carried_kg, rider_time_cr_per_day, full_tank);
            if (!laden.feasible || laden.propellant_load_kg + carried_kg > limit_kg) {
                break;
            }
            const double missing_kg = leave_shortfall_kg(destination, laden, cargo_kg + carried_kg, carried_kg);
            if (missing_kg <= 0.0) {
                return FuelAwarePlan {&laden, carried_kg, true};
            }
            carried_kg += missing_kg;
        }
        // Tankering is a preference: a ship that cannot carry its return fuel buys it at the
        // destination, however dear (before, it could not fly to a dear port at all: no light
        // freighter went to Mars while Mars sold fuel at more than twice Earth's price).
        if (leave_shortfall_kg(destination, plan, cargo_kg, 0.0, false) <= 0.0) {
            return FuelAwarePlan {&plan, 0.0, true};
        }
        return FuelAwarePlan {&plan, 0.0, false};
    };

    // Fuel burned on a mission comes out of the tank, already bought and not resalable,
    // so it is valued at the neutral base price, not the origin's scarcity price: a ship
    // holding 100 t of fuel at a fuel-starved port (16x) would otherwise never leave.
    // Burning it also wears a nuclear-thermal core (0 with wear off).
    const double origin_fuel_price = get_commodity("fuel").base_price + wear_cr_per_propellant_kg(ship_class) * FUEL_UNITS_TO_KG;
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
        const auto& rates = economy_.get_station_net_rates(station_def);
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
            largest->second -= std::max(0.0, std::min(largest->second, ship_design(*other).cargo_capacity_units));
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
    // A follow-up leg counts only if the ship could leave its destination again: with what it has
    // left plus what that port can sell it, for a leg as long, flown empty. (Without this, ships
    // shuttled empty between Earth L1 and Low Earth Logistics, each time for a follow-up to Mars
    // that, once there, they could not fly: no fuel at Mars to leave again.)
    const auto can_leave_again = [&](const domain::StationDefinition& next, const LegEstimate& leg,
                                     double departure_kg, double days_ahead) {
        const double left_kg = std::max(0.0, departure_kg - leg.propellant_kg);
        const double for_sale_kg = fuel_for_sale_on_arrival_kg(economy_, next, get_station_state(next.id),
            days_ahead + leg.travel_days);
        return left_kg + for_sale_kg >= leg.propellant_kg * (1.0 + PROPELLANT_RESERVE_FRACTION);
    };
    const auto follow_ups_from = [&](const domain::StationDefinition& destination,
                                     const domain::TrajectoryPlan& plan,
                                     double carried_kg) -> const std::vector<FollowUp>& {
        if (const auto it = follow_ups_by_destination.find(destination.id); it != follow_ups_by_destination.end()) {
            return it->second;
        }
        std::vector<FollowUp> options;
        const auto& dest_state = get_station_state(destination.id);
        const double arrival_kg = std::max(0.0,
            plan.propellant_load_kg - plan.propellant_required_kg - plan.propellant_boiled_kg) + carried_kg;
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
            const double decay_per_day = cargo_decay_per_day(ship_class, commodity_id);
            for (const auto& next : universe_.stations) {
                if (next.id == destination.id || !route_leg_allowed(destination.id, next.id)) {
                    continue;
                }
                const auto& next_rates = economy_.get_station_net_rates(next);
                const auto& next_state = get_station_state(next.id);
                const bool consumes = (next_rates.contains(commodity_id) && next_rates.at(commodity_id) < 0.0)
                    || next_state.order_stats.contains(commodity_id);
                if (!consumes && station_price(next_state, commodity_id) <= get_commodity(commodity_id).base_price) {
                    continue;
                }
                // Best load: a full hold may be too heavy; where the next station posts an order,
                // its open quantity and the end of its extra are the loads that pay best.
                std::vector<double> lots {full_units, 0.5 * full_units, 0.25 * full_units};
                if (priced_order(next_state, commodity_id)) {
                    const auto light = estimate_leg(ship_class, destination, next, plan.arrival_time_s, departure_kg,
                        provisions_kg + 0.25 * full_units * get_commodity(commodity_id).mass_per_unit_kg);
                    if (light.feasible) {
                        const auto quote = order_quote(next_state, commodity_id,
                            game_time_s_ + (follow_days + light.travel_days) * 86400.0, ship.id);
                        for (const double breakpoint : {quote.open_units, quote.open_units + quote.extra_units}) {
                            if (breakpoint > 1.0 && breakpoint < full_units) {
                                lots.push_back(breakpoint);
                            }
                        }
                    }
                }
                double units = 0.0;
                double best_profit = -std::numeric_limits<double>::infinity();
                LegEstimate leg;
                for (const double lot : lots) {
                    if (lot <= 1.0) {
                        continue;
                    }
                    const auto estimate = estimate_leg(ship_class, destination, next, plan.arrival_time_s, departure_kg,
                        provisions_kg + lot * get_commodity(commodity_id).mass_per_unit_kg);
                    if (!estimate.feasible || !can_leave_again(next, estimate, departure_kg, follow_days)) {
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
        // Passengers waiting there for another station (a forecast from who waits now).
        if (!cargo_only && ship_class.berths >= 1.0 && !dest_state.waiting.empty()) {
            for (const auto& next : universe_.stations) {
                if (next.id == destination.id || !route_leg_allowed(destination.id, next.id)
                    || !passenger_leg_waiting(dest_state, next.id)) {
                    continue;
                }
                const auto leg = estimate_leg(ship_class, destination, next, plan.arrival_time_s, departure_kg,
                    provisions_kg + ship_class.berths * passenger_kg(economy_.transfer_days(destination, next)));
                if (!leg.feasible || !can_leave_again(next, leg, departure_kg, follow_days)) {
                    continue;
                }
                const auto riders = passenger_offer(ship, ship_class, dest_state, next, leg.travel_days, 0.0, ship_class.berths);
                if (riders.people < 1.0) {
                    continue;
                }
                options.push_back({
                    .profit = riders.revenue_cr - riders.marginal_cr
                        - (leg.propellant_kg / FUEL_UNITS_TO_KG) * origin_fuel_price - time_cost_per_day * leg.travel_days,
                    .days = leg.travel_days,
                    .carries_cargo = true,
                    .label = std::format("then {:.0f} passengers -> {}", riders.people, next.id),
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
            // A follow-up is a forecast; one that starts years from now is worth less (a light
            // freighter at Mars waited out 1,385 days for science samples forecast at Ceres).
            const double horizon = universe_.market.follow_up_horizon_days;
            const double weight = horizon > 0.0 && option.profit > 0.0 ? std::exp(-leg_days / horizon) : 1.0;
            const double score = (leg_profit + weight * option.profit) / std::max(1.0, leg_days + option.days);
            if (score > best) {
                best = score;
                follow = option;
            }
        }
        return best;
    };

    // The manifest (step 29, section 5b). For each destination the ship fills its hold with the
    // units that earn the most each: what the destination pays for the next unit (an open order,
    // then the extra at a discount, then the floor; along its curve where it posts no order),
    // less what the unit costs here along the producer's curve and the propellant its mass burns,
    // until the next unit would lose money or the hold, the credit or the surplus runs out. Once
    // an order slice has paid for the trip, units that just cover their price and propellant top
    // the hold up. The manifest is then planned and scored over two legs, whole and as its first
    // half and quarter in the order it was loaded: a full hold may be too heavy to fly or to
    // leave again. (Until step 29: one good at 100, 50 or 25% of the hold, and a separate mixed
    // hold.) Fleet investment probes (cargo_only) get each manifest of several goods as lots
    // sharing a run number.
    const auto& origin_rates = economy_.get_station_net_rates(origin_def);
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
    // Fleet investment values a hull fitted with a cabin module for its berths only (the bare
    // hull is the cargo candidate), so its probe plans no cargo runs.
    if (cargo_only && passengers_on() && ship_class.berths >= 1.0 && !ship_class.fit.empty()) {
        offers.clear();
    }
    // Propellant bought for the mission is paid at this port's price.
    const double marginal_fuel_price_per_kg = (origin_fuel_price + origin_fuel_premium) / FUEL_UNITS_TO_KG;
    // Passengers (docs/plans/passengers.md) ride in berths, not the hold; they board here if a
    // trip to their destination costs them less than they will pay.
    const double berths = cargo_only ? 0.0 : ship_class.berths;
    const bool passengers_waiting = berths >= 1.0 && !origin_state.waiting.empty();
    int next_run = 0;
    for (const auto& destination : universe_.stations) {
        if (offers.empty() && !passengers_waiting) {
            break;
        }
        if (destination.id == origin_def.id || !route_leg_allowed(origin_def.id, destination.id)) {
            continue;
        }
        const auto& destination_state = get_station_state(destination.id);
        const auto& destination_rates = economy_.get_station_net_rates(destination);
        struct Good {
            const Offer* offer {nullptr};
            const domain::CommodityDefinition* commodity {nullptr};
            bool ordered {false};
            OrderQuote quote {};
            double stock_on_arrival {0.0};  // where the destination posts no order
            double urgency {1.0};
            double loaded {0.0};
            double buy_cost {0.0};  // of `loaded`, along the origin's curve
            double contract_units {0.0};  // the first of `loaded`, at the contract's price
            double contract_price {0.0};
        };
        std::vector<Good> goods;
        double heaviest_kg = 0.0;
        for (const auto& offer : offers) {
            const auto& commodity = get_commodity(offer.commodity_id);
            const double rate = destination_rates.contains(offer.commodity_id) ? destination_rates.at(offer.commodity_id) : 0.0;
            const bool ordered = priced_order(destination_state, offer.commodity_id);
            // Follow the price signal, not just the recipe: a station short of something it does
            // not consume itself (water drunk by visiting crews) still bids it up.
            if (!ordered && rate >= 0.0 && station_price(destination_state, offer.commodity_id) <= commodity.base_price) {
                continue;
            }
            const auto stock_it = destination_state.inventory.find(offer.commodity_id);
            const double stock = stock_it == destination_state.inventory.end() ? 0.0 : stock_it->second;
            // An export market never starves: Earth's economy takes what arrives. The price clamp
            // saturates, so a starving station cannot bid any higher; the score is weighted by
            // how few days of stock it has left.
            const double days_left = rate < 0.0 && !economy_.is_export_market(destination, offer.commodity_id)
                ? stock / std::abs(rate) : std::numeric_limits<double>::infinity();
            goods.push_back({.offer = &offer, .commodity = &commodity, .ordered = ordered,
                .urgency = std::clamp(14.0 / std::max(days_left, 0.5), 1.0, 5.0)});
            heaviest_kg = std::max(heaviest_kg, commodity.mass_per_unit_kg);
        }
        // Riders board any leg flown there; a leg for passengers alone needs min_leg_passengers.
        const bool riders_here = passengers_waiting
            && std::any_of(origin_state.waiting.begin(), origin_state.waiting.end(), [&](const domain::PassengerGroup& group) {
                   return group.destination_id == destination.id && group.count >= 1.0;
               });
        const bool passengers_here = riders_here && passenger_leg_waiting(origin_state, destination.id);
        if (goods.empty() && !passengers_here) {
            continue;
        }
        // The empty-hold transfer dates the forecasts (a laden one differs a little); a hold of
        // the heaviest good (or half of one) gives the propellant per kg of cargo. All three are
        // asked for in one pass.
        const auto& probe = plan_to(destination, 0.0);
        // Without goods the empty plan gives the propellant per kg (berths weigh little; at the
        // same mass ratio, propellant grows with the mass it moves).
        const bool from_empty = goods.empty();
        const double laden_kg = goods.empty() ? berths * passenger_kg(economy_.transfer_days(origin_def, destination))
                                              : ship_class.cargo_capacity_units * heaviest_kg;
        const auto& laden = from_empty ? probe : plan_to(destination, laden_kg);
        const auto& half_laden = from_empty ? probe : plan_to(destination, 0.5 * laden_kg);
        if (!probe.feasible) {
            continue;
        }
        double propellant_per_kg = 0.0;
        if (from_empty) {
            propellant_per_kg = probe.propellant_required_kg / std::max(1.0, ship_class.dry_mass_kg + provisions_kg);
        } else {
            for (const auto* plan : {&laden, &half_laden}) {
                if (plan->feasible) {
                    const double kg = plan == &laden ? laden_kg : 0.5 * laden_kg;
                    propellant_per_kg = std::max(0.0, plan->propellant_required_kg - probe.propellant_required_kg) / std::max(1.0, kg);
                    break;
                }
            }
        }
        const double probe_days = probe.travel_time_s / 86400.0;
        const double arrival_s = game_time_s_ + (delay_days + probe_days) * 86400.0;
        const auto riders = riders_here
            ? passenger_offer(ship, ship_class, origin_state, destination, probe_days,
                  propellant_per_kg * marginal_fuel_price_per_kg, berths)
            : PassengerOffer {};
        for (auto& good : goods) {
            if (good.ordered) {
                good.quote = order_quote(destination_state, good.offer->commodity_id, arrival_s, ship.id);
            } else {
                good.stock_on_arrival = forecast_stock_on_arrival(destination_state, good.offer->commodity_id,
                    delay_days + probe_days, ship.id);
            }
        }
        double hold_left = ship_class.cargo_capacity_units;
        if (destination.storage_capacity_units > 0.0) {
            hold_left = std::min(hold_left, std::max(0.0,
                destination.storage_capacity_units - economy_.storage_used_units(destination, destination_state.inventory)));
        }
        double credits_left = spendable_credits;
        const double chunk = std::max(1.0, ship_class.cargo_capacity_units / 40.0);
        std::vector<std::pair<std::size_t, double>> picks;  // (good, units) in the order loaded
        // A rotation contract's deliveries on this leg go aboard first, at their fixed price.
        if (liner && under_contract(ship)) {
            for (const auto& delivery : ship.contract.deliveries) {
                if (delivery.to_station_id != destination.id) {
                    continue;
                }
                for (std::size_t g = 0; g < goods.size(); ++g) {
                    auto& good = goods[g];
                    if (good.offer->commodity_id != delivery.commodity_id || good.contract_units > 0.0) {
                        continue;
                    }
                    const double units = std::min({delivery.units, good.offer->surplus, hold_left});
                    const double buy = units >= 1.0 ? trade_value(origin_state, good.offer->commodity_id, -units) : 0.0;
                    if (units < 1.0 || buy > credits_left) {
                        continue;
                    }
                    good.loaded = units;
                    good.buy_cost = buy;
                    good.contract_units = units;
                    good.contract_price = delivery.unit_price;
                    hold_left -= units;
                    credits_left -= buy;
                    picks.emplace_back(g, units);
                }
            }
        }
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
                const double surviving = std::pow(1.0 - cargo_decay_per_day(ship_class, good.commodity->id), probe_days);
                const double spot_loaded = good.loaded - good.contract_units;
                const double sale = good.ordered
                    ? quote_split(good.quote, spot_loaded * surviving, units * surviving).total
                    : sale_split(destination, good.offer->commodity_id, good.stock_on_arrival + spot_loaded * surviving,
                          units * surviving).total;
                const double buy = trade_value(origin_state, good.offer->commodity_id, -(good.loaded + units)) - good.buy_cost;
                const double propellant = propellant_per_kg * units * good.commodity->mass_per_unit_kg * marginal_fuel_price_per_kg;
                const double gain = (sale - buy - propellant) / units;
                if (gain > best_gain && buy <= credits_left) {
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
        // The whole manifest, and its first half and quarter (passengers ride with each; with
        // passengers and no goods, the passengers alone).
        std::size_t last_count = std::numeric_limits<std::size_t>::max();
        for (const double fraction : {1.0, 0.5, 0.25}) {
            const auto count = static_cast<std::size_t>(std::ceil(static_cast<double>(picks.size()) * fraction));
            if ((count == 0 && riders.people <= 0.0) || count == last_count) {
                continue;
            }
            last_count = count;
            std::vector<double> units_by_good(goods.size(), 0.0);
            for (std::size_t i = 0; i < count; ++i) {
                units_by_good[picks[i].first] += picks[i].second;
            }
            std::vector<domain::CargoLot> lots;
            std::vector<double> lot_urgency;
            double cargo_kg = 0.0;
            for (std::size_t g = 0; g < goods.size(); ++g) {
                double units = units_by_good[g];
                if (units > 1.0) {
                    cargo_kg += units * goods[g].commodity->mass_per_unit_kg;
                }
                if (const double contracted = std::min(units, goods[g].contract_units); contracted >= 1.0) {
                    lots.push_back({.commodity_id = goods[g].offer->commodity_id, .units = contracted,
                        .fixed_unit_price = goods[g].contract_price});
                    lot_urgency.push_back(1.0);
                    units -= contracted;
                }
                if (units > 1.0) {
                    lots.push_back({.commodity_id = goods[g].offer->commodity_id, .units = units});
                    lot_urgency.push_back(goods[g].urgency);
                }
            }
            if (lots.empty() && riders.people <= 0.0) {
                continue;
            }
            cargo_kg += riders.mass_kg;
            const auto cargo_label = riders.people > 0.0
                ? std::format("cargo {} + {:.0f} passengers -> {}", describe_cargo(lots), riders.people, destination.id)
                : std::format("cargo {} -> {}", describe_cargo(lots), destination.id);
            // Tankers too: a tanker that bought its return fuel from the cargo it just delivered
            // would drain the depot it supplies.
            const auto fuel_plan = plan_with_return_fuel(destination, cargo_kg, riders.time_cr_per_day);
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
            // Passengers eat from the same stores.
            const double crew = std::max(1.0, ship_class.crew_size);
            const double provisionable = max_mission_days * crew / (crew + riders.people);
            if (travel_days > provisionable) {
                trace_line(std::format("{}: {:.0f} days exceeds provisionable {:.0f}", cargo_label, travel_days, provisionable));
                continue;
            }
            if (!dose_allows(destination, travel_days)) {
                trace_line(std::format("{}: {:.0f} days exceeds the crew's dose limit", cargo_label, travel_days));
                continue;
            }
            if (!drive_life_allows(ship, ship_class, plan)) {
                trace_line(cargo_label + ": the drive's remaining life is too short");
                continue;
            }
            const double fuel_cost = (consumed_propellant_kg(plan) / FUEL_UNITS_TO_KG) * origin_fuel_price
                + fuel_premium(plan, fuel_plan.carried_kg);
            if (cargo_only) {
                const int run = lots.size() > 1 ? next_run++ : -1;
                for (std::size_t l = 0; l < lots.size(); ++l) {
                    cargo_options.push_back({.destination = &destination, .commodity_id = lots[l].commodity_id,
                        .cargo_units = lots[l].units, .travel_days = travel_days,
                        .wait_days = plan.wait_time_s / 86400.0, .fuel_cost = l == 0 ? fuel_cost : 0.0, .run = run});
                }
            }
            const double trip_cost = fuel_cost + time_cost_per_day * travel_days;
            const double total_units = domain::total_units(lots);
            // Fares, less the passengers' provisions; with no goods aboard they carry the trip.
            double revenue = riders.revenue_cr;
            double purchases = riders.marginal_cr;
            double weighted = riders.revenue_cr - riders.marginal_cr - (lots.empty() ? trip_cost : 0.0);
            for (std::size_t l = 0; l < lots.size(); ++l) {
                const auto& lot = lots[l];
                const double surviving = lot.units
                    * std::pow(1.0 - cargo_decay_per_day(ship_class, lot.commodity_id), travel_days);
                const double lot_revenue = lot.fixed_unit_price > 0.0 ? surviving * lot.fixed_unit_price
                    : sale_value_on_arrival(destination_state, lot.commodity_id, surviving, delay_days + travel_days, ship.id);
                const double lot_cost = trade_value(origin_state, lot.commodity_id, -lot.units);
                revenue += lot_revenue;
                purchases += lot_cost;
                // Each lot's margin, less its share of the trip, weighted by its urgency.
                weighted += lot_urgency[l] * (lot_revenue - lot_cost - trip_cost * lot.units / total_units);
            }
            FollowUp follow;
            // Cargo-only probes (fleet investment) value the run on its own and skip the
            // follow-up forecast, the expensive part of a dispatch pass.
            const double score = cargo_only
                ? weighted / std::max(1.0, travel_days)
                : two_leg_score(destination, plan, fuel_plan.carried_kg, weighted, travel_days, false, follow);
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
                best_passengers = riders.picks;
            }
        }
    }

    // Investment probes plan passenger legs as the reference fares and dispatch do, with the
    // riders' time (full berths at the reference fill, a resident's time and dose per day), and
    // on a full tank (see planning_options).
    const double probe_rider_day_cr = universe_.market.fill_ref
        * passenger_nonfare_cr(value_of_time_cr_per_day(domain::PassengerKind::Migrant), 1.0,
            universe_.ship_environment.radiation ? universe_.ship_environment.gcr_msv_per_day : 0.0);
    // Fleet investment probes of a ship with berths: one passenger option per destination it can
    // carry full berths to, valued later at the sustained flow of travellers (not today's queue).
    if (cargo_only && passengers_on() && ship_class.berths >= 1.0) {
        for (const auto& destination : universe_.stations) {
            if (destination.id == origin_def.id || reference_fare(origin_def.id, destination.id) == nullptr) {
                continue;
            }
            const double load_kg = ship_class.berths * passenger_kg(economy_.transfer_days(origin_def, destination));
            const auto fuel_plan = plan_with_return_fuel(destination, load_kg, ship_class.berths * probe_rider_day_cr, true);
            const auto& plan = *fuel_plan.plan;
            if (!plan.feasible || !fuel_plan.feasible) {
                continue;
            }
            const double travel_days = plan.travel_time_s / 86400.0;
            const double crew = std::max(1.0, ship_class.crew_size);
            if (travel_days > max_mission_days * crew / (crew + ship_class.berths) || !dose_allows(destination, travel_days)
                || !drive_life_allows(ship, ship_class, plan)) {
                continue;
            }
            cargo_options.push_back({.destination = &destination, .commodity_id = PASSENGER_GOOD,
                .cargo_units = ship_class.berths, .travel_days = travel_days, .wait_days = plan.wait_time_s / 86400.0,
                .fuel_cost = (consumed_propellant_kg(plan) / FUEL_UNITS_TO_KG) * origin_fuel_price
                    + fuel_premium(plan, fuel_plan.carried_kg),
                .run = -1,
                .return_fuel_kg = fuel_plan.carried_kg > 0.0 ? -1.0 : consumed_propellant_kg(plan)});
        }
    }
    // A bare hull's probe also values the hull with each of its cabin modules (fleet investment
    // probes no cabin design of its own): the same transfers with the module's mass and full
    // berths as payload, in this probe's plans; the limits are the fitted design's.
    if (cargo_only && passengers_on() && ship_class.berths <= 0.0
        && ship_class.fit.empty()) {
        for (const auto& module : universe_.ship_modules) {
            if (module.hull_id != ship_class.id || module.slot != "cabin" || !module_has_active_effect(module)) {
                continue;
            }
            const auto& design = register_design(ship_class.id, {module.id});
            if (design.berths < 1.0) {
                continue;
            }
            const double module_kg = std::max(0.0, design.dry_mass_kg - ship_class.dry_mass_kg);
            for (const auto& destination : universe_.stations) {
                if (destination.id == origin_def.id || reference_fare(origin_def.id, destination.id) == nullptr) {
                    continue;
                }
                const double load_kg = module_kg + design.berths * passenger_kg(economy_.transfer_days(origin_def, destination));
                const auto fuel_plan = plan_with_return_fuel(destination, load_kg, design.berths * probe_rider_day_cr, true);
                const auto& plan = *fuel_plan.plan;
                if (!plan.feasible || !fuel_plan.feasible) {
                    continue;
                }
                const double travel_days = plan.travel_time_s / 86400.0;
                const double crew = std::max(1.0, design.crew_size);
                if (travel_days > max_mission_days * crew / (crew + design.berths)
                    || !leg_dose_allowed(design, origin_def, destination, game_time_s_, travel_days)
                    || !drive_life_allows(ship, design, plan)) {
                    continue;
                }
                cargo_options.push_back({.destination = &destination, .commodity_id = PASSENGER_GOOD,
                    .cargo_units = design.berths, .travel_days = travel_days, .wait_days = plan.wait_time_s / 86400.0,
                    .fuel_cost = (consumed_propellant_kg(plan) / FUEL_UNITS_TO_KG) * origin_fuel_price
                        + fuel_premium(plan, fuel_plan.carried_kg),
                    .run = -1, .module_id = module.id,
                    .return_fuel_kg = fuel_plan.carried_kg > 0.0 ? -1.0 : consumed_propellant_kg(plan)});
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
        if (leg_days > max_mission_days || !dose_allows(destination, leg_days) || !drive_life_allows(ship, ship_class, plan)) {
            continue;
        }
        const double leg_cost = (consumed_propellant_kg(plan) / FUEL_UNITS_TO_KG) * origin_fuel_price
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
            best_passengers.clear();
        }
    }

    // A liner with nothing that pays for its next leg flies it empty: its value is the schedule.
    if (best_destination == nullptr && liner) {
        const auto& next = get_station_definition(next_scheduled_stop(ship, origin_def.id));
        const auto fuel_plan = plan_with_return_fuel(next, 0.0);
        if (fuel_plan.plan->feasible && fuel_plan.feasible && fuel_plan.plan->travel_time_s / 86400.0 <= max_mission_days
            && dose_allows(next, fuel_plan.plan->travel_time_s / 86400.0)
            && drive_life_allows(ship, ship_class, *fuel_plan.plan)) {
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
        if (plan.feasible && plan.travel_time_s / 86400.0 <= max_mission_days
            && dose_allows(home, plan.travel_time_s / 86400.0) && drive_life_allows(ship, ship_class, plan)) {
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
                    const auto& other_rates = economy_.get_station_net_rates(other);
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
                    const auto& other_rates = economy_.get_station_net_rates(other);
                    const auto& other_state = get_station_state(other.id);
                    const bool consumes = (other_rates.contains(commodity_id) && other_rates.at(commodity_id) < 0.0)
                        || other_state.order_stats.contains(commodity_id);
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
            if (reposition_days > max_mission_days || !dose_allows(destination, reposition_days)
                || !drive_life_allows(ship, ship_class, plan)) {
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
            const double reposition_cost = (consumed_propellant_kg(plan) / FUEL_UNITS_TO_KG) * origin_fuel_price
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
    choice.passengers = best_passengers;
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

    if (consider_repair(ship)) {
        return;
    }
    const auto& ship_class = ship_design(ship);
    const auto review_start = std::chrono::steady_clock::now();
    // A rotation contract that ran out ends here.
    if (!ship.contract.stops.empty() && game_time_s_ >= ship.contract.until_s) {
        add_event(std::format("{}'s rotation contract ended at {}", ship.name,
            get_station_definition(ship.current_station_id).name), "mission");
        ship.contract = {};
    }
    // A loop's wait in port at its first stop, counted from arrival (cycle minus the legs).
    if (under_contract(ship) && ship.current_station_id == ship.contract.stops.front()) {
        const double ready_s = ship.idle_since_s + contract_dwell_days(ship.contract) * 86400.0;
        if (game_time_s_ < ready_s) {
            ship.next_review_s = std::min(ship.next_review_s, ready_s);
            return;
        }
    }
    auto choice = choose_mission(ship, ship_class, trace, game_time_s_);
    // Rotation contracts: a free ship looks for a loop now and then, and signs one that earns
    // about as much as the best spot mission (a repositioning earns nothing).
    const auto& market = universe_.market;
    if (market.contracts > 0.0 && ship.contract.stops.empty() && liner_stops(ship) == nullptr
        && !(!ship.route_destination_id.empty() && game_time_s_ < ship.route_until_s)
        && game_time_s_ >= ship.next_contract_review_s) {
        // Reviews are spread over the review period by ship (never the whole fleet at once).
        if (ship.next_contract_review_s <= 0.0) {
            const auto spread = static_cast<double>(std::hash<std::string> {}(ship.id) % 1000) / 1000.0;
            ship.next_contract_review_s = game_time_s_ + spread * market.contract_review_days * 86400.0;
        }
    }
    if (market.contracts > 0.0 && ship.contract.stops.empty() && liner_stops(ship) == nullptr
        && !(!ship.route_destination_id.empty() && game_time_s_ < ship.route_until_s)
        && game_time_s_ >= ship.next_contract_review_s) {
        ship.next_contract_review_s = game_time_s_ + market.contract_review_days * 86400.0;
        auto [contract, score] = find_contract(ship, ship_class);
        const double spot = choice.kind == MissionChoice::Kind::Mission ? choice.score : 0.0;
        if (!contract.stops.empty() && score > MIN_MISSION_SCORE_PER_DAY && score >= market.contract_accept * spot) {
            std::string route;
            for (const auto& stop : contract.stops) {
                route += get_station_definition(stop).name + " > ";
            }
            std::string goods;
            for (const auto& delivery : contract.deliveries) {
                goods += std::format("{}{:.0f}u {} to {} at {:.0f}", goods.empty() ? "" : ", ", delivery.units,
                    delivery.commodity_id, delivery.to_station_id, delivery.unit_price);
            }
            add_event(std::format("{} signs a rotation contract: {}({:.0f}-day loop, {:.0f} cr/day against {:.0f} spot): {}",
                ship.name, route, contract.cycle_days, score, spot, goods), "mission");
            ship.contract = std::move(contract);
            choice = choose_mission(ship, ship_class, trace, game_time_s_);
        }
    }
    // Debug aid: with SPACETRAINS_PROFILE, reviews over half a second are reported (stderr).
    if (profiling::enabled()) {
        const double review_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - review_start).count();
        if (review_s > 0.5) {
            std::cerr << std::format("[slow review day {:.1f}] {} ({}) at {}: {:.2f} s\n", game_time_s_ / 86400.0, ship.name,
                ship_class.id, ship.current_station_id, review_s);
        }
    }
    if (scheduled_stops(ship) == nullptr && consider_refit(ship, choice, trace)) {
        return;
    }

    auto& origin_state = get_station_state(ship.current_station_id);
    const auto& origin_def = get_station_definition(ship.current_station_id);
    if (choice.kind == MissionChoice::Kind::None && under_contract(ship)) {
        // The loop cannot be flown from here (no fuel, no trajectory): the contract lapses.
        add_event(std::format("{} gives up its rotation contract at {}", ship.name, origin_def.name), "alert");
        ship.contract = {};
        return;
    }
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
    const double time_cost_per_day = daily_capital_cost(ship_class) + daily_crew_cost(ship_class, origin_state)
        + hazard_cr_per_day(ship_class, origin_def, origin_def, game_time_s_) + wear_cr_per_flight_day(ship_class);
    if (trajectory_audit_enabled_) {
        record_trajectory_audit(ship, origin_def, *best_destination, plan, plan.propellant_load_kg);
    }
    // Load the mission's propellant (the plan's burn plus the reserve, and any return fuel).
    buy_propellant(ship, plan.propellant_load_kg + choice.carried_propellant_kg - ship.propellant_kg);

    if (laid_up) {
        add_event(std::format("{} rehired its crew at {}", ship.name, origin_def.name), "mission");
    }
    ship.leg_dose_msv = 0.0;  // a crew is signed on for each leg
    const double mission_days = plan.travel_time_s / 86400.0;
    {
        auto& route = passenger_audit_.routes[{origin_state.station_id, best_destination->id}];
        ++route.departures;
        if (ship_class.berths >= 1.0) {
            ++route.berth_departures;
            if (choice.passengers.empty()
                && std::any_of(origin_state.waiting.begin(), origin_state.waiting.end(), [&](const auto& group) {
                       return group.destination_id == best_destination->id && group.count >= 1.0;
                   })) {
                ++route.empty_with_waiting;
            }
        }
    }
    // Passengers board (and pay) before the stores are bought: they eat from them too.
    if (!choice.passengers.empty()) {
        board_passengers(ship, origin_state, best_destination->id, choice.passengers, mission_days);
    }
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
            * std::pow(1.0 - cargo_decay_per_day(ship_class, lot.commodity_id), plan.travel_time_s / 86400.0);
        if (lot.fixed_unit_price > 0.0) {
            lot.contract_value = surviving * lot.fixed_unit_price;
            observe_contract(get_station_state(best_destination->id), lot.commodity_id, surviving, lot.fixed_unit_price);
            continue;
        }
        const auto split = sale_split_on_arrival(destination_state, lot.commodity_id, surviving, arrival_days, ship.id);
        lot.contract_value = split.total;
        lot.emergency_premium = split.faction;
        // The going rate learns from order slices.
        if (split.order_units > 0.0) {
            observe_contract(get_station_state(best_destination->id), lot.commodity_id, split.order_units,
                split.order_value / split.order_units);
        }
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
        origin_state.ledger.ship_sales += cost;
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
        if (universe_.ship_environment.wear) {
            ship.engine_wear = std::min(1.0, ship.engine_wear + drive_hours(ship_class, plan) / drive_life_h(ship_class));
        }
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
        .propellant_cost_kg = consumed_propellant_kg(plan),
        .purchase_cost = purchase_cost,
        .fuel_cost = (consumed_propellant_kg(plan) / FUEL_UNITS_TO_KG) * station_price(origin_state, "fuel"),
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
    const auto& ship_class = ship_design(ship);
    const double refit_days = universe_.ship_operations.refit_days;
    // What the ship earns as it is: dispatch's profit rate, or nothing (a repositioning
    // score is not a profit, and such a ship has no paying work here either).
    const double current_score = current.kind == MissionChoice::Kind::Mission ? std::max(0.0, current.score) : 0.0;

    // One yard visit: the next tank size either way, or one module in or out.
    const auto options = neighbouring_fits(ship.class_id, ship.fit, radiator_bucket(ship));
    const FitOption* best_option = nullptr;
    const domain::ShipClassDefinition* best_class = nullptr;
    double best_gain = 0.0;
    for (const auto& fit_option : options) {
        const auto& candidate = *fit_option.design;
        const double bill = refit_bill(ship_class, candidate);
        if (ship.credits - bill < -credit_line(candidate)) {
            continue;
        }
        domain::ShipState probe = ship;
        probe.class_id = fit_option.class_id;
        probe.fit = fit_option.fit;
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
            best_option = &fit_option;
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
        yard.ledger.ship_purchases += value;
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
    yard.ledger.ship_sales += bill;
    ship.refit_class_id = best_option->class_id;
    ship.refit_fit = best_option->fit;
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
    if (ship.overhaul) {
        ship.engine_wear = 0.0;
        ship.radiator_condition = 1.0;
        ship.overhaul = false;
    }
    (void)register_design(ship.refit_class_id, ship.refit_fit, radiator_bucket(ship));
    ship.class_id = ship.refit_class_id;
    ship.fit = std::move(ship.refit_fit);
    ship.refit_class_id.clear();
    ship.refit_fit.clear();
    ship.phase = domain::ShipMissionPhase::Idle;
    ship.idle_since_s = game_time_s_;
    ship.next_review_s = game_time_s_;
    add_event(std::format("{} left the yard at {} as {}",
        ship.name, get_station_definition(ship.current_station_id).name, ship_design(ship).name), "mission");
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
            game_time_s_) + ship.active_mission.carried_propellant_kg - ship.active_mission.boiled_propellant_kg;
        ship.propellant_kg = std::max(0.0, ship.propellant_kg);
    }

    if (ship.active_mission.remaining_travel_time_s > 0.0) {
        return;
    }
    if (const auto& samples = ship.active_mission.sampled_propellant_kg;
        samples.size() >= 2 && ship_design(ship).propulsion_type != "variable_isp") {
        // The last sample is after the arrival burn; the planned reserve is a share of the burns.
        const double left_kg = samples.back() + ship.active_mission.carried_propellant_kg
            - ship.active_mission.boiled_propellant_kg;
        const double reserve_kg = PROPELLANT_RESERVE_FRACTION * std::max(0.0, samples.front() - samples.back());
        ++radiation_audit_.thermal_arrivals;
        if (left_kg < -1.0) {
            ++radiation_audit_.arrivals_short_of_burn;
        } else if (left_kg < 0.5 * reserve_kg) {
            ++radiation_audit_.arrivals_below_half_reserve;
        }
        if (reserve_kg > 1.0) {
            radiation_audit_.lowest_reserve_share = std::min(radiation_audit_.lowest_reserve_share, left_kg / reserve_kg);
        }
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
            ? std::max(0.0, destination_def.storage_capacity_units - economy_.storage_used_units(destination_def, destination.inventory))
            : std::numeric_limits<double>::infinity();
        double total_revenue = 0.0;
        double total_spoiled = 0.0;
        std::vector<domain::CargoLot> sold;
        for (const auto& lot : ship.active_mission.cargo) {
            // Some goods (food, medicine) spoil in transit.
            const double decay_per_day = cargo_decay_per_day(ship_design(ship), lot.commodity_id);
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
                const auto& rates = economy_.get_station_net_rates(destination_def);
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
            // Debug aid: SPACETRAINS_TRACE_DELIVERIES=1 logs every delivery (stderr).
            if (static const bool trace_deliveries = std::getenv("SPACETRAINS_TRACE_DELIVERIES") != nullptr;
                trace_deliveries && arrived > 0.0) {
                std::cerr << std::format("[DELIVERY] day {:.0f} {} {} {:.0f}u at {:.1f}/u by {} ({}) lead {:.0f}d\n",
                    game_time_s_ / 86400.0, destination.station_id, lot.commodity_id, arrived, revenue / arrived, ship.name,
                    ship.class_id, (game_time_s_ - ship.active_mission.departure_time_s + ship.active_mission.wait_time_s) / 86400.0);
            }
            if (arrived > 0.0) {
                observe_delivery(destination, lot.commodity_id,
                    ship.active_mission.departure_time_s - ship.active_mission.wait_time_s);
                // Audit only: what stations pay against the landed reference price.
                const auto& refs = universe_.pricing.reference_prices;
                if (const auto at = refs.find(destination.station_id); at != refs.end()) {
                    if (const auto ref = at->second.find(lot.commodity_id); ref != at->second.end()) {
                        delivered_paid_cr_ += revenue;
                        delivered_reference_cr_ += arrived * ref->second;
                    }
                }
            }
            destination.credits -= revenue - faction_share;
            destination.ledger.ship_purchases += revenue - faction_share;
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
    land_passengers(ship, destination);
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

double Simulation::fleet_hold_units() const {
    double units = 0.0;
    for (const auto& ship : ships_) {
        if (liner_stops(ship) == nullptr) {  // liners are a public service, outside the cap
            units += ship_design(ship).cargo_capacity_units;
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

bool Simulation::under_contract(const domain::ShipState& ship) const {
    return !ship.contract.stops.empty() && game_time_s_ < ship.contract.until_s;
}

const std::vector<std::string>* Simulation::scheduled_stops(const domain::ShipState& ship) const {
    if (const auto* stops = liner_stops(ship); stops != nullptr) {
        return stops;
    }
    return under_contract(ship) ? &ship.contract.stops : nullptr;
}

std::string Simulation::next_scheduled_stop(const domain::ShipState& ship, const std::string& from_id) const {
    if (liner_stops(ship) != nullptr) {
        return next_liner_stop(ship, from_id);
    }
    const auto& stops = ship.contract.stops;
    if (stops.empty()) {
        return {};
    }
    const auto at = std::find(stops.begin(), stops.end(), from_id);
    return at == stops.end() ? stops.front() : stops.at(static_cast<std::size_t>(at - stops.begin() + 1) % stops.size());
}

double Simulation::contract_dwell_days(const domain::ShipContract& contract) {
    double legs = 0.0;
    for (const double days : contract.leg_days) {
        legs += days;
    }
    return std::max(0.0, contract.cycle_days - legs);
}

double Simulation::contracted_flow(const std::string& station_id, const std::string& commodity_id) const {
    double flow = 0.0;
    for (const auto& ship : ships_) {
        if (!under_contract(ship) || ship.contract.cycle_days <= 0.0) {
            continue;
        }
        for (const auto& delivery : ship.contract.deliveries) {
            if (delivery.to_station_id == station_id && delivery.commodity_id == commodity_id) {
                flow += delivery.units / ship.contract.cycle_days;
            }
        }
    }
    return flow;
}

void Simulation::with_parallel_plans(const std::string& prefix, const std::function<void()>& pass) {
    for (;;) {
        std::vector<DeferredPlan> deferred;
        auto* const outer = deferred_plans_;
        deferred_plans_ = &deferred;
        deferred_keys_.clear();
        deferred_prefix_ = prefix;
        pass();
        deferred_plans_ = outer;
        deferred_prefix_.clear();
        if (deferred.empty()) {
            return;
        }
        std::vector<domain::TrajectoryPlan> results(deferred.size());
        thread_pool_->parallel_for(deferred.size(), [&](std::size_t i) { results[i] = deferred[i].compute(); });
        for (std::size_t i = 0; i < deferred.size(); ++i) {
            deferred[i].commit(std::move(results[i]));
        }
    }
}

std::pair<domain::ShipContract, double> Simulation::find_contract(
    const domain::ShipState& ship, const domain::ShipClassDefinition& ship_class) {
    const auto& market = universe_.market;
    constexpr double day_s = 86400.0;
    const auto& origin = get_station_definition(ship.current_station_id);
    const double hold = ship_class.cargo_capacity_units;
    if (hold <= 0.0) {
        return {{}, 0.0};
    }
    double crew = ship_class.crew_size * universe_.ship_operations.wage_cr_per_crew_day;
    for (const auto& [good, units_per_crew_day] : universe_.ship_operations.life_support_units_per_crew_day) {
        crew += ship_class.crew_size * units_per_crew_day * get_commodity(good).base_price;
    }
    const double time_cost = daily_capital_cost(ship_class) + crew + hazard_cr_per_day(ship_class, origin, origin, game_time_s_)
        + wear_cr_per_flight_day(ship_class);
    const double fuel_base = get_commodity(economy::FUEL_ID).base_price;
    const auto n = universe_.stations.size();
    // Every leg once, from today's bucket (later legs' windows are not dated: an estimate).
    std::vector<LegEstimate> legs(n * n);
    with_parallel_plans("contract#", [&] {
        for (std::size_t a = 0; a < n; ++a) {
            for (std::size_t b = 0; b < n; ++b) {
                if (a != b) {
                    legs[a * n + b] = estimate_leg(ship_class, universe_.stations[a], universe_.stations[b], game_time_s_,
                        ship_class.propellant_capacity_kg, market.fill_ref * hold * 100.0);
                }
            }
        }
    });
    // What each station can spare of each good, and what each still needs under no contract.
    struct Need {
        std::string commodity_id;
        double free_use {0.0};   // units per day not yet contracted
        double price {0.0};
        double cap {0.0};        // cost-plus contracts: never above what the station has been paying
    };
    std::vector<std::vector<Need>> needs(n);
    std::vector<std::unordered_map<std::string, std::pair<double, double>>> spares(n);  // good -> (units now, per day)
    for (std::size_t i = 0; i < n; ++i) {
        const auto& station = universe_.stations[i];
        const auto& state = get_station_state(station.id);
        const auto& rates = economy_.get_station_net_rates(station);
        for (const auto& [commodity_id, rate] : rates) {
            const bool makes = commodity_id == economy::FUEL_ID ? economy_.fuel_factory_output(station) > 0.0 : rate > 0.0;
            if (makes) {
                const auto stock = state.inventory.find(commodity_id);
                const double spare = std::max(0.0, (stock == state.inventory.end() ? 0.0 : stock->second) - (8.0 + std::max(0.0, rate) * 7.0));
                const double per_day = commodity_id == economy::FUEL_ID ? economy_.fuel_factory_output(station) : rate;
                spares[i][commodity_id] = {spare, std::max(0.0, per_day)};
            }
        }
        for (const auto& [commodity_id, stats] : state.order_stats) {
            if (!priced_order(state, commodity_id)) {
                continue;
            }
            const double use = order_levels(state, commodity_id, stats).use_per_day;
            const double free_use = use - contracted_flow(station.id, commodity_id);
            if (free_use <= 0.0) {
                continue;
            }
            // The price a loop is signed at: the order's market price where ships compete,
            // else the going rate, never above today's bid.
            double price = market.competitive > 0.0 && stats.market_price > 0.0 ? stats.market_price : stats.going_rate;
            price = std::min(price, std::max(stats.order_price, economy_.reference_price(station, commodity_id)));
            needs[i].push_back({commodity_id, free_use, price, std::max(stats.going_rate, price)});
        }
    }
    std::size_t origin_index = n;
    for (std::size_t i = 0; i < n; ++i) {
        if (universe_.stations[i].id == origin.id) {
            origin_index = i;
        }
    }
    if (origin_index == n) {
        return {{}, 0.0};
    }
    const auto max_stops = static_cast<std::size_t>(std::clamp(market.contract_max_stops, 2.0, 4.0));
    // A loop's value with the given legs: profit per day after time and fuel, its hold filled
    // on each leg by margin per unit with what the next stop still needs.
    // A loop may wait in port at its first stop before each lap (`dwell_days`), so each lap
    // carries more of the stations' needs on the same fuel; the wait costs the ship's time.
    // (Short hops such as Earth <-> Moon would otherwise fly half-empty loops.)
    const auto value = [&](const std::vector<std::size_t>& loop, const std::vector<LegEstimate>& loop_legs,
                           domain::ShipContract* contract, double* contract_profit_per_day = nullptr,
                           double dwell_days = 0.0) {
        std::vector<double> buys;
        std::vector<double> caps;
        double cycle_days = dwell_days;
        for (const auto& leg : loop_legs) {
            if (!leg.feasible) {
                return 0.0;
            }
            cycle_days += leg.travel_days;
        }
        if (cycle_days <= 0.5 || cycle_days > market.contract_term_days) {
            return 0.0;
        }
        double profit = -time_cost * cycle_days;
        std::vector<domain::ShipContract::Delivery> deliveries;
        for (std::size_t k = 0; k < loop.size(); ++k) {
            const auto from = loop[k];
            const auto to = loop[(k + 1) % loop.size()];
            const auto& from_state = get_station_state(universe_.stations[from].id);
            const double fuel_price = std::max(fuel_base, station_price(from_state, economy::FUEL_ID));
            profit -= loop_legs[k].propellant_kg / FUEL_UNITS_TO_KG * fuel_price
                + loop_legs[k].propellant_kg * wear_cr_per_propellant_kg(ship_class);
            struct Pick {
                const Need* need;
                double units;
                double margin;
            };
            std::vector<Pick> picks;
            for (const auto& need : needs[to]) {
                const auto spare = spares[from].find(need.commodity_id);
                if (spare == spares[from].end()) {
                    continue;
                }
                const double available = std::max(spare->second.first, spare->second.second * cycle_days);
                const double units = std::min(market.contract_cover * need.free_use * cycle_days, available);
                const double buy = station_price(from_state, need.commodity_id);
                if (units >= market.contract_min_units && need.price > buy) {
                    picks.push_back({&need, units, need.price - buy});
                }
            }
            std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.margin > b.margin; });
            double room = hold;
            for (const auto& pick : picks) {
                const double units = std::min(pick.units, room);
                if (units < market.contract_min_units) {
                    continue;
                }
                room -= units;
                profit += units * pick.margin;
                deliveries.push_back({.to_station_id = universe_.stations[to].id,
                    .commodity_id = pick.need->commodity_id, .units = units, .unit_price = pick.need->price});
                buys.push_back(pick.need->price - pick.margin);
                caps.push_back(pick.need->cap);
            }
        }
        if (deliveries.empty()) {
            return 0.0;
        }
        // Cost-plus: each unit pays its purchase plus its share of the loop's time and fuel, plus
        // the contract markup, at most what the station has been paying. The loop is still chosen
        // by the value it creates at market prices.
        if (market.contract_cost_plus > 0.0 && contract != nullptr) {
            double units_total = 0.0;
            for (const auto& delivery : deliveries) {
                units_total += delivery.units;
            }
            const double cost = -(profit - [&] {
                double margin = 0.0;
                for (std::size_t d = 0; d < deliveries.size(); ++d) {
                    margin += deliveries[d].units * (deliveries[d].unit_price - buys[d]);
                }
                return margin;
            }());
            const double share = cost / std::max(1.0, units_total);
            double contract_profit = -cost;
            for (std::size_t d = 0; d < deliveries.size(); ++d) {
                deliveries[d].unit_price = std::min(caps[d], buys[d] + share * (1.0 + market.contract_cost_plus));
                contract_profit += deliveries[d].units * (deliveries[d].unit_price - buys[d]);
            }
            *contract_profit_per_day = contract_profit / cycle_days;
        } else if (contract_profit_per_day != nullptr) {
            *contract_profit_per_day = profit / cycle_days;
        }
        if (contract != nullptr) {
            *contract = {};
            for (std::size_t k = 0; k < loop.size(); ++k) {
                contract->stops.push_back(universe_.stations[loop[k]].id);
                contract->leg_days.push_back(loop_legs[k].travel_days);
            }
            contract->deliveries = std::move(deliveries);
            contract->cycle_days = cycle_days;
        }
        return profit / cycle_days;
    };
    static constexpr std::array<double, 5> kDwellDays {0.0, 7.0, 14.0, 28.0, 56.0};
    const auto best_dwell = [&](const std::vector<std::size_t>& candidate, const std::vector<LegEstimate>& loop_legs) {
        double best_value = 0.0;
        double best_wait = 0.0;
        for (const double dwell : kDwellDays) {
            if (const double v = value(candidate, loop_legs, nullptr, nullptr, dwell); v > best_value) {
                best_value = v;
                best_wait = dwell;
            }
        }
        return std::make_pair(best_value, best_wait);
    };
    // Every loop from this port, valued with today's legs; the best few again with each leg
    // dated from when the one before it arrives (launch windows).
    std::vector<std::pair<double, std::vector<std::size_t>>> ranked;
    std::vector<std::size_t> loop {origin_index};
    const std::function<void()> extend = [&] {
        if (loop.size() >= 2) {
            std::vector<LegEstimate> loop_legs;
            for (std::size_t k = 0; k < loop.size(); ++k) {
                loop_legs.push_back(legs[loop[k] * n + loop[(k + 1) % loop.size()]]);
            }
            if (const double score = best_dwell(loop, loop_legs).first; score > 0.0) {
                ranked.emplace_back(score, loop);
            }
        }
        if (loop.size() >= max_stops) {
            return;
        }
        for (std::size_t next = 0; next < n; ++next) {
            if (std::find(loop.begin(), loop.end(), next) != loop.end() || !legs[loop.back() * n + next].feasible) {
                continue;
            }
            loop.push_back(next);
            extend();
            loop.pop_back();
        }
    };
    extend();
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    ranked.resize(std::min(ranked.size(), static_cast<std::size_t>(std::max(1.0, market.contract_refine))));
    std::vector<std::vector<LegEstimate>> dated(ranked.size());
    with_parallel_plans("contract#", [&] {
        for (std::size_t r = 0; r < ranked.size(); ++r) {
            const auto& candidate = ranked[r].second;
            dated[r].clear();
            double t = game_time_s_;
            for (std::size_t k = 0; k < candidate.size(); ++k) {
                const auto leg = estimate_leg(ship_class, universe_.stations[candidate[k]],
                    universe_.stations[candidate[(k + 1) % candidate.size()]], t, ship_class.propellant_capacity_kg,
                    market.fill_ref * hold * 100.0);
                dated[r].push_back(leg);
                t += leg.travel_days * day_s;
            }
        }
    });
    domain::ShipContract best;
    double best_score = 0.0;
    double best_profit = 0.0;
    for (std::size_t r = 0; r < ranked.size(); ++r) {
        domain::ShipContract contract;
        double profit = 0.0;
        const double dwell = best_dwell(ranked[r].second, dated[r]).second;
        if (const double score = value(ranked[r].second, dated[r], &contract, &profit, dwell); score > best_score) {
            best_score = score;
            best_profit = profit;
            best = std::move(contract);
        }
    }
    // What the ship earns on it (cost-plus: its markup on the loop's cost).
    best_score = best_profit;
    if (!best.stops.empty()) {
        best.signed_s = game_time_s_;
        best.until_s = game_time_s_ + market.contract_term_days * day_s;
    }
    return {best, best_score};
}

double Simulation::internal_money_supply() const {
    // Station and ship money (what the controller holds); residents' savings are their own.
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
                // Rotation fares (passengers) are upkeep too: the faction keeps its staff relieved.
                const auto* tour = rotation_tour(definition.id);
                const double rotation = tour == nullptr ? 0.0 : tour->fares_per_day * rotation_positions(station);
                // With households (economy overhaul), the core crew's wages are upkeep too.
                const double core_wages = households_on() ? resident_wages_per_day(station) : 0.0;
                transfer = std::min(transfer,
                    std::max((open.core_crew_fraction * (consumption_value_per_day(definition, true) + core_wages) + rotation) * dt_days,
                        savings));
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
            fleet_value += ship_design(ship).ship_value_cr;
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
    // Purchases at least purchase_spacing_days apart: each plans every variant of the winning
    // hull, and a review's purchases falling within a few days made the slowest days (0.6-1 s).
    if (investment_purchases_left_ > 0 && game_time_s_ >= next_purchase_s_) {
        switch (commission_step()) {
            case CommissionStep::Probed:
                break;
            case CommissionStep::Bought:
                --investment_purchases_left_;
                if (investment.purchase_spacing_days > 0.0) {
                    next_purchase_s_ = game_time_s_ + investment.purchase_spacing_days * 86400.0;
                }
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
    const auto& ship_class = ship_design(ship);
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
            .class_id = ship_class.base_class_id.empty() ? ship_class.id : ship_class.base_class_id,
            .fit = ship_class.fit,
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
        const auto& rates = economy_.get_station_net_rates(station);
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
    // Hulls in their standard tanks; with passengers also each hull with its cabin module (the
    // liners carry their berths built in).
    std::vector<const domain::ShipClassDefinition*> candidate_classes;
    for (const auto& ship_class : universe_.ship_classes) {
        if (!ship_class.hull_id.empty() && ship_class.hull_id != ship_class.id) {
            continue;
        }
        candidate_classes.push_back(&ship_class);
        if (passengers_on() && ship_class.berths <= 0.0) {
            for (const auto& module : universe_.ship_modules) {
                if (module.hull_id == ship_class.id && module.slot == "cabin" && module_has_active_effect(module)) {
                    candidate_classes.push_back(&register_design(ship_class.id, {module.id}));
                }
            }
        }
    }
    // Ships docked idle or laid up, by hull (looked up once per review, not per candidate).
    struct IdleHull {
        const std::string* station_id;
        const std::string* hull_id;
        double berths;
    };
    std::vector<IdleHull> idle_hulls;
    for (const auto& ship : ships_) {
        if (ship.phase == domain::ShipMissionPhase::Idle || ship.phase == domain::ShipMissionPhase::LaidUp) {
            const auto& design = ship_design(ship);
            idle_hulls.push_back({.station_id = &ship.current_station_id,
                .hull_id = design.hull_id.empty() ? &design.id : &design.hull_id, .berths = design.berths});
        }
    }
    // Passenger flows and top fares per station pair: the same for every candidate in a review.
    std::map<std::pair<std::string, std::string>, double> flow_memo;
    const auto flow_per_day = [&](const std::string& from, const std::string& to) {
        const auto key = std::make_pair(from, to);
        if (const auto it = flow_memo.find(key); it != flow_memo.end()) {
            return it->second;
        }
        return flow_memo.emplace(key, passenger_flow_per_day(from, to)).first->second;
    };
    std::map<std::tuple<std::string, std::string, double, double>, double> berth_revenue_memo;
    const auto berth_revenue = [&](const std::string& from, const std::string& to, double days, double window_days,
                                   double berths) {
        const auto key = std::make_tuple(from, to, berths, days);
        if (const auto it = berth_revenue_memo.find(key); it != berth_revenue_memo.end()) {
            return it->second;
        }
        return berth_revenue_memo.emplace(key, expected_passenger_revenue(from, to, days, window_days, berths, 1.0).fares_cr)
            .first->second;
    };
    const auto cabin_fit = [](const domain::ShipClassDefinition& ship_class) { return !ship_class.fit.empty(); };
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
        const auto& yard_rates = economy_.get_station_net_rates(yard);
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
                const double unit_margin = consumer_bid(get_station_state(destination.id), commodity_id) - buy_price;
                const double rate = consumption_rate(destination, commodity_id);
                if (unit_margin > 0.0 && rate > 0.0) {
                    outlets.push_back({unit_margin, rate});
                }
            }
        }
        for (const auto* candidate_class : candidate_classes) {
            const auto& ship_class = *candidate_class;
            if (ship_class.ship_value_cr <= 0.0
                || ship_class.ship_value_cr + investment.working_capital > richest_treasury
                || (investment.max_fleet_hold_units > 0.0
                    && fleet_hold + ship_class.cargo_capacity_units > investment.max_fleet_hold_units)) {
                continue;
            }
            // A ship of this hull docked idle or laid up here could already take the work.
            const auto& hull_id = ship_class.hull_id.empty() ? ship_class.id : ship_class.hull_id;
            const bool idle_here = std::any_of(idle_hulls.begin(), idle_hulls.end(), [&](const IdleHull& idle) {
                return *idle.station_id == yard.id && *idle.hull_id == hull_id && idle.berths >= ship_class.berths;
            });
            if (idle_here) {
                continue;
            }
            // A trip takes at least a day, so a ship moves at most a hold per day.
            double margin_bound = 0.0;
            for (const auto& outlet : outlets) {
                margin_bound = std::max(margin_bound, outlet.unit_margin * std::min(outlet.rate, ship_class.cargo_capacity_units));
            }
            // Berths: a shuttle twice as fast as the reference carrier, its berths filled each way
            // as boarding fills them (the travellers expected over its round trip, by band, at
            // their fares or the clearing fare), before provisions and fuel. A passenger run is
            // valued on its own, so the bound is the larger of the cargo and passenger bounds.
            if (passengers_on() && ship_class.berths >= 1.0) {
                double passenger_bound = 0.0;
                for (const auto& destination : universe_.stations) {
                    const auto* out = reference_fare(yard.id, destination.id);
                    if (destination.id == yard.id || out == nullptr) {
                        continue;
                    }
                    const double days = 0.5 * out->days;
                    const double cycle_days = std::max(1.0, 2.0 * days);
                    double bound = berth_revenue(yard.id, destination.id, days, cycle_days, ship_class.berths);
                    if (reference_fare(destination.id, yard.id) != nullptr) {
                        bound += berth_revenue(destination.id, yard.id, days, cycle_days, ship_class.berths);
                    }
                    passenger_bound = std::max(passenger_bound, bound / cycle_days);
                }
                // A hull fitted with a cabin module is a candidate for its berths only: for cargo
                // the bare hull is cheaper and lighter (and is a candidate itself).
                margin_bound = cabin_fit(ship_class) ? passenger_bound : std::max(margin_bound, passenger_bound);
            }
            const double running_cost = daily_capital_cost(ship_class) + daily_crew_cost(ship_class, yard_state)
                + hazard_cr_per_day(ship_class, yard, yard, game_time_s_) + wear_cr_per_flight_day(ship_class);
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
        const auto& yard_rates = economy_.get_station_net_rates(yard);
        const auto& destination_rates = economy_.get_station_net_rates(destination);
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
        // What a delivery onto that stock earns: under the destination's order (at its price up to
        // the order-up-to level, the extra at a discount, the floor beyond), else along its curve.
        // A route sustained over the commitment sells at most at the destination's landed cost
        // (its reference price): scarcity bids do not last once ships serve it (economy overhaul:
        // with Earth selling at its ask, margins are the carriage and estimates at scarcity
        // prices ran 15x the earned returns).
        // A station that keeps paying more than that (a science station far out, short for years)
        // is valued at what it has been paying: its going rate.
        double sustained_cap = economy_.reference_price(destination, commodity_id);
        if (const auto stats = destination_state.order_stats.find(commodity_id); stats != destination_state.order_stats.end()) {
            sustained_cap = std::max(sustained_cap, stats->second.going_rate);
        }
        const auto sale_value = [&](double units) {
            return std::min(units * sustained_cap, [&] {
            const auto order = destination_state.order_stats.find(commodity_id);
            if (!priced_order(destination_state, commodity_id)) {
                return economy_.get_trade_value(destination, commodity_id, destination_stock, units, commodity.base_price);
            }
            const auto levels = order_levels(destination_state, commodity_id, order->second);
            OrderQuote quote;
            quote.open_units = std::max(0.0, levels.order_up_to_units - destination_stock);
            quote.extra_units = std::max(0.0, levels.max_units - std::max(destination_stock, levels.order_up_to_units));
            // The bid on the projected stock, as a quote would make it.
            quote.price = order_bid(destination_state, commodity_id, levels, destination_stock).price;
            quote.extra_price = std::min(quote.price, order->second.going_rate) * (1.0 - universe_.orders.extra_discount);
            quote.floor_price = std::min(quote.extra_price,
                economy::PRICE_FLOOR_MULTIPLIER * economy_.reference_price(destination, commodity_id));
            return quote_split(quote, 0.0, units).total;
            }());
        };
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
            margin += sale_value(surviving) - cost;
            destination_stock += surviving;
            delivered += surviving;
            advance(cycle_days - leg_days);
        }
        units_per_day = delivered / window_days;
        return margin / window_days;
    };
    // Passengers (docs/plans/passengers.md): a shuttle between the yard and a station, full berths
    // at most each way, carrying the open part of the travellers' flow (what ships committed to
    // the route already carry is taken), at the reference fare adjusted for this ship's days.
    const auto sustained_passenger_profit_per_day = [&](const domain::ShipClassDefinition& ship_class,
                                                        const domain::StationDefinition& yard,
                                                        const MissionChoice::CargoOption& option, double& units_per_day) {
        const auto& destination = *option.destination;
        const auto* back_fare = reference_fare(destination.id, yard.id);
        const double transfer_days = std::max(0.0, option.travel_days - option.wait_days);
        const double cycle_days = std::max(1.0, 2.0 * transfer_days + option.wait_days);
        double provisions_per_day = 0.0;
        for (const auto& [commodity_id, units] : universe_.ship_operations.life_support_units_per_crew_day) {
            provisions_per_day += units * station_price(get_station_state(yard.id), commodity_id);
        }
        // Taken: what ships flying the route carry now (cabin hulls and liners alike, committed or
        // not) plus ships still in the yard, and at least what the committed ships carry.
        const double committed_in_yard = committed_flow(destination.id, PASSENGER_GOOD, true);
        const double committed = committed_flow(destination.id, PASSENGER_GOOD, false) + committed_in_yard;
        const auto open_share = [&](const std::string& from, const std::string& to) {
            const double flow = flow_per_day(from, to);
            const double taken = std::max(committed, carried_passengers_per_day(from, to) + committed_in_yard);
            return flow > 0.0 ? std::max(0.0, flow - taken) / flow : 0.0;
        };
        const auto outbound = expected_passenger_revenue(yard.id, destination.id, option.travel_days, cycle_days,
            option.cargo_units, open_share(yard.id, destination.id));
        const auto inbound = back_fare == nullptr ? PassengerRevenue {}
            : expected_passenger_revenue(destination.id, yard.id, transfer_days, cycle_days, option.cargo_units,
                open_share(destination.id, yard.id));
        // The way back is fuelled at the destination at its price (Luna makes fuel, Earth buys it
        // dear), unless the ship carries that fuel from the yard.
        const double return_fuel_cost = option.return_fuel_kg < 0.0 ? option.fuel_cost
            : option.return_fuel_kg / FUEL_UNITS_TO_KG
                * (port_fuel_price(destination.id) + wear_cr_per_propellant_kg(ship_class) * FUEL_UNITS_TO_KG);
        const double margin = outbound.fares_cr - outbound.people * provisions_per_day * option.travel_days
            + inbound.fares_cr - inbound.people * provisions_per_day * transfer_days
            - option.fuel_cost - return_fuel_cost;
        const double out = outbound.people;
        units_per_day = out / cycle_days;
        return margin / cycle_days;
    };
    const auto value_candidate = [&](const domain::ShipClassDefinition& ship_class, const domain::StationDefinition& yard,
                                     double return_bound, const MissionChoice& choice) {
        const double running_cost = daily_capital_cost(ship_class) + daily_crew_cost(ship_class, get_station_state(yard.id))
            + hazard_cr_per_day(ship_class, yard, yard, game_time_s_) + wear_cr_per_flight_day(ship_class);
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
        // A single lot's label is written only when it leads.
        const auto lot_label = [](const MissionChoice::CargoOption& option, double units_per_day) {
            return option.commodity_id == PASSENGER_GOOD
                ? std::format("{:.0f} berths -> {} in {:.1f} d, {:.2f} passengers/d", option.cargo_units,
                    option.destination->name, option.travel_days, units_per_day)
                : std::format("{:.0f}u {} -> {} in {:.1f} d, {:.2f} u/d", option.cargo_units,
                    option.commodity_id, option.destination->name, option.travel_days, units_per_day);
        };
        const auto consider = [&](const RunValue& run) {
            const double annual_return = (run.profit_per_day - running_cost) * 365.0 / ship_class.ship_value_cr;
            if (annual_return > valuation.annual_return) {
                valuation.annual_return = annual_return;
                valuation.label = run.label.empty() ? lot_label(*run.main_lot, run.main_units_per_day) : run.label;
                valuation.destination_id = run.main_lot->destination->id;
                valuation.commodity_id = run.main_lot->commodity_id;
                valuation.units_per_day = run.main_units_per_day;
            }
        };
        for (const auto& option : choice.cargo_options) {
            double units_per_day = 0.0;
            if (option.commodity_id == PASSENGER_GOOD) {
                const double profit_per_day = sustained_passenger_profit_per_day(ship_class, yard, option, units_per_day);
                consider({.profit_per_day = profit_per_day, .label = {}, .main_lot = &option, .main_units_per_day = units_per_day});
                continue;
            }
            if (cabin_fit(ship_class)) {
                continue;  // a cabin hull is valued for its berths (see the bound above)
            }
            const double profit_per_day = sustained_profit_per_day(yard, option, units_per_day);
            if (option.run < 0) {
                consider({.profit_per_day = profit_per_day, .label = {}, .main_lot = &option, .main_units_per_day = units_per_day});
                continue;
            }
            auto& run = mixed_runs[option.run];
            run.profit_per_day += profit_per_day;
            run.label += (run.label.empty() ? "mixed: " : " + ") + lot_label(option, units_per_day);
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
    // A cabin design is valued from its bare hull's probe (its runs are tagged with the cabin
    // module); every other design is probed on its own.
    const bool shared_cabin_probe = passengers_on();
    const auto probe_class = [&](const Candidate& candidate) -> const domain::ShipClassDefinition& {
        return shared_cabin_probe && cabin_fit(*candidate.ship_class) ? get_ship_class(candidate.ship_class->base_class_id)
                                                                      : *candidate.ship_class;
    };
    const auto probe_key = [&](const Candidate& candidate) {
        return probe_class(candidate).id + "|" + candidate.yard->id;
    };
    const auto run_module = [&](const Candidate& candidate) {
        return shared_cabin_probe && cabin_fit(*candidate.ship_class) ? candidate.ship_class->fit.front() : std::string();
    };
    std::vector<std::size_t> to_probe;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];
        if (candidate.rank_bound <= best_rank) {
            break;
        }
        const auto probed = review_probes_.find(probe_key(candidate));
        if (probed == review_probes_.end()) {
            std::vector<std::string> batch_keys;
            for (std::size_t j = i; j < candidates.size() && to_probe.size() < kProbeBatch; ++j) {
                const auto key = probe_key(candidates[j]);
                if (!review_probes_.contains(key) && std::find(batch_keys.begin(), batch_keys.end(), key) == batch_keys.end()) {
                    to_probe.push_back(j);
                    batch_keys.push_back(key);
                }
            }
            break;
        }
        const auto module_id = run_module(candidate);
        MissionChoice choice;
        for (const auto& run : probed->second) {
            if (run.module_id != module_id) {
                continue;
            }
            choice.cargo_options.push_back({.destination = &get_station_definition(run.destination_id),
                .commodity_id = run.commodity_id, .cargo_units = run.cargo_units, .travel_days = run.travel_days,
                .wait_days = run.wait_days, .fuel_cost = run.fuel_cost, .run = run.run,
                .return_fuel_kg = run.return_fuel_kg});
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
            const auto& probed_class = probe_class(candidates[index]);
            probe_ships.push_back(new_ship(probed_class, *candidates[index].yard));
            requests.push_back({.ship = &probe_ships.back(), .ship_class = &probed_class,
                .earliest_departure_s = game_time_s_ + investment.build_days * 86400.0, .cargo_only = true});
        }
        const auto choices = choose_missions(requests);
        for (std::size_t k = 0; k < to_probe.size(); ++k) {
            auto& runs = review_probes_[probe_key(candidates[to_probe[k]])];
            for (const auto& option : choices[k].cargo_options) {
                runs.push_back({.destination_id = option.destination->id, .commodity_id = option.commodity_id,
                    .cargo_units = option.cargo_units, .travel_days = option.travel_days,
                    .wait_days = option.wait_days, .fuel_cost = option.fuel_cost, .run = option.run,
                    .module_id = option.module_id, .return_fuel_kg = option.return_fuel_kg});
            }
        }
        if (trace) {
            std::string names;
            for (const auto index : to_probe) {
                names += std::format(" {}@{}", probe_class(candidates[index]).id, candidates[index].yard->id);
            }
            std::cerr << std::format("[invest day {:.1f}] probed {} candidates in {:.2f} s:{}\n", game_time_s_ / 86400.0,
                to_probe.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - review_start).count(), names);
        }
        return CommissionStep::Probed;
    }
    // The winning hull is built with the tanks that suit it best, judged like a refit (by
    // dispatch's full score, which carries each variant's capital charge); otherwise it would
    // go straight back into the yard.
    // (A hull bought for its cabin module keeps it.)
    if (best_class != nullptr && best_class->fit.empty()) {
        const auto* hull = best_class;
        std::vector<const domain::ShipClassDefinition*> variants;
        std::vector<domain::ShipState> variant_ships;
        for (const auto& variant : universe_.ship_classes) {
            if ((variant.hull_id.empty() ? variant.id : variant.hull_id) == hull->id
                && variant.ship_value_cr + investment.working_capital <= richest_treasury) {
                variants.push_back(&variant);
            }
        }
        // And the standard tanks with each module that does something (tier 2).
        for (const auto& module : universe_.ship_modules) {
            if (module.hull_id == hull->id && module_has_active_effect(module)) {
                const auto& fitted = register_design(hull->id, {module.id});
                if (fitted.ship_value_cr + investment.working_capital <= richest_treasury) {
                    variants.push_back(&fitted);
                }
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
    // A yard in space builds it (economy overhaul): its share of the price goes there, the
    // parts made on Earth to Earth's economy.
    double earth_part = best_class->ship_value_cr;
    if (const auto* yard = households_on() ? yard_station_for(*best_yard) : nullptr; yard != nullptr) {
        double yard_part = universe_.households.yard_share * best_class->ship_value_cr;
        auto& yard_state = get_station_state(yard->id);
        // The yard builds the hull from goods worth yard_materials_share of the price (metals half,
        // machinery 30%, electronics 20%, at its reference prices), out of its stock; what it lacks
        // comes from Earth, and that part of the price goes there.
        const double materials = universe_.households.yard_materials_share * best_class->ship_value_cr;
        for (const auto& [good, share] : std::initializer_list<std::pair<const char*, double>> {
                 {"metals", 0.5}, {"machinery", 0.3}, {"electronics", 0.2}}) {
            const double price = economy_.reference_price(*yard, good);
            if (price <= 0.0) {
                continue;
            }
            const double wanted = share * materials / price;
            double& stock = yard_state.inventory[good];
            const double used = std::clamp(stock, 0.0, wanted);
            stock -= used;
            yard_state.demand_units[good] += used;
            yard_part -= (wanted - used) * price;
        }
        yard_part = std::max(0.0, yard_part);
        yard_state.credits += yard_part;
        yard_state.ledger.yard_sales += yard_part;
        earth_part -= yard_part;
    }
    outside_economy_credits_ += earth_part;
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
    ship.refit_fit = ship.fit;
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
    domain::SimulationSnapshot snapshot {
        .game_time_s = game_time_s_,
        .outside_economy_credits = outside_economy_credits_,
        .background_credits = background_credits_,
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
    for (auto& station : snapshot.stations) {
        for (const auto& [commodity_id, stats] : station.order_stats) {
            station.orders[commodity_id] = order_view(station, commodity_id);
        }
    }
    return snapshot;
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

// Live state of a ship's systems for the inspector (tier 1 of docs/plans/ship_internals.md):
// whether the engine runs, and how hot the drive radiators are. Nuclear-thermal burns are
// impulsive in the planners; for display each burn lasts its share of the mission's
// propellant (half each) at the engine's mass flow. Variable-Isp ships thrust the whole way.
// Idle radiators sit near the sunlit equilibrium of a grey body, 278.6 K / sqrt(r in AU).
std::string Simulation::ship_systems_json(
    const domain::ShipState& ship, const domain::ShipClassDefinition& ship_class, const math::Vec3d& position) const {
    std::string engine_state = "off";
    double burn_s = 0.0;
    const domain::ShipComponentDefinition* engine = nullptr;
    const domain::ShipComponentDefinition* radiator = nullptr;
    for (const auto& c : ship_class.components) {
        engine = c.kind == "engine" ? &c : engine;
        radiator = c.kind == "radiator" ? &c : radiator;
    }
    if (ship.phase == domain::ShipMissionPhase::InTransit) {
        const auto& mission = ship.active_mission;
        if (ship_class.propulsion_type == "variable_isp") {
            engine_state = "thrusting";
        } else if (engine != nullptr && engine->thrust_n > 0.0 && engine->isp_s > 0.0) {
            const double mass_flow_kg_s = engine->thrust_n / (engine->isp_s * 9.80665);
            burn_s = 0.5 * std::max(0.0, mission.propellant_cost_kg) / mass_flow_kg_s;
            const bool burning = game_time_s_ - mission.departure_time_s < burn_s || mission.arrival_time_s - game_time_s_ < burn_s;
            engine_state = burning ? "burning" : "coasting";
        }
    }
    double radiator_temperature_k = 0.0;
    if (radiator != nullptr) {
        const double r_au = std::max(0.05, std::hypot(position.x, position.z) / 1.495978707e11);
        radiator_temperature_k = engine_state == "thrusting" ? radiator->temperature_k : 278.6 / std::sqrt(r_au);
    }
    // Tier 2: the crew's dose this leg against the limit, today's dose rate, boil-off and wear.
    const auto& env = universe_.ship_environment;
    std::string tier2;
    if (env.radiation) {
        const double rate = ship.phase == domain::ShipMissionPhase::InTransit
            ? crew_environment::gcr_msv_per_day(env, ship_r_au(ship), game_time_s_ / 86400.0)
                * crew_environment::transit_gcr_factor(env, ship_class, cabin_shield_g_cm2(ship_class, ship.propellant_kg))
            : docked_dose_msv_per_day(ship.current_station_id, game_time_s_);
        tier2 += std::format("\"leg_dose_msv\":{:.1f},\"dose_limit_msv\":{:.0f},\"dose_rate_msv_day\":{:.3f},",
            ship.leg_dose_msv, env.career_limit_msv, rate);
    }
    if (const double boil = boiloff_kg_per_day(ship_class, ship_r_au(ship)); boil > 0.0) {
        tier2 += std::format("\"boiloff_kg_day\":{:.1f},", ship.phase == domain::ShipMissionPhase::InTransit ? boil : 0.0);
    }
    if (env.wear) {
        tier2 += std::format("\"engine_wear\":{:.4f},\"drive_life_h\":{:.0f},\"radiator_condition\":{:.4f},",
            ship.engine_wear, drive_life_h(ship_class), ship.radiator_condition);
    }
    return std::format("\"engine_state\":\"{}\",\"engine_burn_s\":{:.1f},\"radiator_temperature_k\":{:.1f},{}",
        engine_state, burn_s, radiator_temperature_k, tier2);
}

std::string Simulation::build_bridge_designs_json() const {
    std::ostringstream output;
    output << std::setprecision(10);
    output << "{\"designs\":{";
    bool first_class = true;
    std::vector<const domain::ShipClassDefinition*> all;
    for (const auto& ship_class : universe_.ship_classes) {
        all.push_back(&ship_class);
    }
    for (const auto& [id, design] : designs_) {
        all.push_back(&design);
    }
    for (const auto* class_ptr : all) {
        const auto& ship_class = *class_ptr;
        output << (first_class ? "" : ",") << "\"" << json_escape(ship_class.id) << "\":{"
               << "\"name\":\"" << json_escape(ship_class.name) << "\","
               << "\"hull_id\":\"" << json_escape(ship_class.hull_id) << "\","
               << "\"propulsion_type\":\"" << json_escape(ship_class.propulsion_type) << "\","
               << "\"dry_mass_kg\":" << ship_class.dry_mass_kg << ","
               << "\"propellant_capacity_kg\":" << ship_class.propellant_capacity_kg << ","
               << "\"cargo_capacity_units\":" << ship_class.cargo_capacity_units << ","
               << "\"crew_size\":" << ship_class.crew_size << ","
               << (ship_class.berths > 0.0 ? std::format("\"berths\":{},", ship_class.berths) : std::string())
               << "\"ship_value_cr\":" << ship_class.ship_value_cr << ","
               << "\"max_delta_v_mps\":" << ship_class.max_delta_v_mps << ","
               << "\"cruise_accel_mps2\":" << ship_class.cruise_accel_mps2 << ","
               << "\"specific_engine_power_w_per_kg\":" << ship_class.specific_engine_power_w_per_kg << ","
               << "\"components\":[";
        bool first = true;
        for (const auto& c : ship_class.components) {
            output << (first ? "" : ",") << "{"
                   << "\"id\":\"" << json_escape(c.id) << "\","
                   << "\"parent_id\":\"" << json_escape(c.parent_id) << "\","
                   << "\"kind\":\"" << json_escape(c.kind) << "\","
                   << "\"name\":\"" << json_escape(c.name) << "\","
                   << "\"mass_kg\":" << c.mass_kg << ","
                   << "\"price_cr\":" << c.price_cr << ","
                   << "\"power_w\":" << c.power_w << ","
                   << "\"heat_w\":" << c.heat_w << ","
                   << "\"temperature_k\":" << c.temperature_k << ","
                   << "\"volume_m3\":" << c.volume_m3 << ","
                   << "\"area_m2\":" << c.area_m2 << ","
                   << "\"capacity_kg\":" << c.capacity_kg << ","
                   << "\"crew_capacity\":" << c.crew_capacity << ","
                   << "\"isp_s\":" << c.isp_s << ","
                   << "\"thrust_n\":" << c.thrust_n << "}";
            first = false;
        }
        output << "]}";
        first_class = false;
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
               << station_economy_json(get_station_state(station.id))
               << station_passengers_json(get_station_state(station.id))
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
        const auto& net_rates = economy_.get_station_net_rates(station);
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
            // Ordered goods: the order-up-to level S (step 29).
            const auto order = station_state.order_stats.find(commodity.id);
            output << (first_target ? "" : ",") << "\"" << json_escape(commodity.id) << "\":"
                   << (order == station_state.order_stats.end() ? economy_.get_target_stock(station, commodity.id)
                       : order_levels(station_state, commodity.id, order->second).order_up_to_units);
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
        // Step 29, observed only: the purchase order the station would post for each good.
        output << ",\"orders\":{";
        bool first_order = true;
        for (const auto& [commodity_id, stats] : station_state.order_stats) {
            const auto view = order_view(station_state, commodity_id);
            output << (first_order ? "" : ",") << "\"" << json_escape(commodity_id) << "\":{"
                   << "\"kind\":\"" << view.ceiling_kind << "\""
                   << ",\"use_per_day\":" << view.use_per_day
                   << ",\"lead_days\":" << view.lead_days
                   << ",\"interval_days\":" << view.interval_days
                   << ",\"spread_days\":" << view.spread_days
                   << ",\"going_rate\":" << view.going_rate
                   << ",\"order_price\":" << view.order_price
                   << ",\"reference_price\":" << view.reference_price
                   << ",\"safety_units\":" << view.safety_units
                   << ",\"reorder_units\":" << view.reorder_units
                   << ",\"order_up_to_units\":" << view.order_up_to_units
                   << ",\"max_units\":" << view.max_units
                   << ",\"contracted_units\":" << view.contracted_units
                   << ",\"open_units\":" << view.open_units
                   << ",\"days_to_safety\":" << view.days_to_safety
                   << ",\"ceiling\":" << view.ceiling
                   << ",\"station_ceiling\":" << view.station_ceiling
                   << ",\"market_price\":" << stats.market_price
                   << ",\"market_bidders\":" << stats.market_bidders
                   << ",\"deliveries\":" << stats.deliveries << "}";
            first_order = false;
        }
        output << "}";
        output << ",\"emergencies\":[";
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
        const auto& ship_class = ship_design(ship);
        const auto position = get_ship_render_position(ship);
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"id\":\"" << json_escape(ship.id) << "\","
               << "\"name\":\"" << json_escape(ship.name) << "\","
               << "\"faction_id\":\"" << json_escape(ship.faction_id) << "\","
               << "\"class_id\":\"" << json_escape(ship.class_id) << "\","
               << "\"design_id\":\"" << json_escape(ship_class.id) << "\","
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
               << "\"cargo_mass_kg\":" << [&] {
                      double kg = 0.0;
                      for (const auto& lot : ship.active_mission.cargo) {
                          kg += lot.units * get_commodity(lot.commodity_id).mass_per_unit_kg;
                      }
                      return kg;
                  }() << ","
               << "\"provisions_mass_kg\":" << provisions_mass_kg(ship) << ","
               << ship_passengers_json(ship, ship_class)
               << ship_systems_json(ship, ship_class, position)
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
               << "\"contract_stops\":[" << [&] {
                      std::string text;
                      if (under_contract(ship)) {
                          for (const auto& stop : ship.contract.stops) {
                              text += (text.empty() ? "\"" : ",\"") + json_escape(stop) + "\"";
                          }
                      }
                      return text;
                  }() << "],"
               << "\"contract_until_s\":" << (under_contract(ship) ? ship.contract.until_s : 0.0) << ","
               << "\"contract_deliveries\":[" << [&] {
                      std::string text;
                      if (under_contract(ship)) {
                          for (const auto& delivery : ship.contract.deliveries) {
                              text += std::format("{}{{\"to\":\"{}\",\"commodity_id\":\"{}\",\"units\":{:.1f},\"unit_price\":{:.2f}}}",
                                  text.empty() ? "" : ",", json_escape(delivery.to_station_id), json_escape(delivery.commodity_id),
                                  delivery.units, delivery.unit_price);
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
