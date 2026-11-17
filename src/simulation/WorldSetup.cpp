// Building a World: the set-up a new game runs once (init_world), then the engines that need
// files or threads (attach_engines). Simulation's constructor and from_data_root, and the
// tests through make_world, share this code.

#include "simulation/WorldSetup.hpp"

#include "data_loader/DataLoader.hpp"
#include "simulation/SimulationInternal.hpp"
#include "simulation/query/Accounts.hpp"
#include "simulation/query/Finance.hpp"
#include "simulation/query/Labour.hpp"
#include "simulation/query/Lookups.hpp"
#include "simulation/systems/Failures.hpp"
#include "simulation/systems/FleetInvestment.hpp"
#include "simulation/systems/Market.hpp"
#include "simulation/systems/Passengers.hpp"
#include "simulation/systems/Patrols.hpp"
#include "simulation/systems/Statistics.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>

namespace spacetrains::simulation {

using namespace detail;

void init_world(World& w) {
    w.engines.kepler_planner_ = std::make_unique<trajectory::KeplerTrajectoryPlanner>(w.data.universe_, w.engines.mechanics_);
    for (const auto& station : w.data.universe_.stations) {
        w.data.station_defs_by_id_[station.id] = &station;
        w.state.stations_.push_back({.station_id = station.id, .inventory = systems::starting_inventory(w, station), .credits = station.initial_credits, .ledger = {}});
        w.data.starting_inventories_[station.id] = w.state.stations_.back().inventory;
        w.state.stations_.back().population = static_cast<double>(station.population);
        w.state.stations_.back().population_announced = w.state.stations_.back().population;
    }
    for (const auto& ship_class : w.data.universe_.ship_classes) {
        w.data.ship_classes_by_id_[ship_class.id] = &ship_class;
    }
    for (const auto& module : w.data.universe_.ship_modules) {
        w.data.modules_by_key_[module.hull_id + "|" + module.id] = &module;
    }
    w.state.event_rng_.seed(w.data.universe_.event_seed);
    w.state.environment_rng_.seed(w.data.universe_.event_seed + 1);
    if (w.data.universe_.ship_environment.radiation) {
        w.data.active_effects_.insert("spe_shield_g_cm2");
    }
    if (w.data.universe_.ship_environment.boiloff) {
        w.data.active_effects_.insert("zbo_lift_w");
        w.data.active_effects_.insert("boiloff_intercept");
    }
    if (w.data.universe_.ship_environment.cold_storage) {
        w.data.active_effects_.insert("perishable_decay_factor");
    }
    if (w.data.universe_.passengers.enabled != 0.0) {
        w.data.active_effects_.insert("berths");
        if (w.data.universe_.ship_environment.radiation) {
            w.data.active_effects_.insert("cabin_shield_g_cm2");
            w.data.active_effects_.insert("tank_wrap_m2");
        }
    }
    for (const auto& faction : w.data.universe_.factions) {
        w.state.faction_treasuries_[faction.id] = 0.0;
    }
    for (const auto& seed : w.data.universe_.ship_seeds) {
        w.state.ships_.push_back({
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
    for (const auto& economy : w.data.universe_.background_economies) {
        if (economy.population > 0.0) {
            w.state.background_population_[economy.id] = economy.population;
        }
        if (economy.id != "earth") {
            w.state.colony_state_[economy.id] = {.wage_median = economy.wage_median_cr_per_day};
        }
    }
    systems::setup_money_journal(w);
    // Residents start with their savings buffer, brought from Earth (economy overhaul).
    if (query::households_on(w)) {
        for (auto& station : w.state.stations_) {
            if (station.population <= 0.0) {
                station.population = static_cast<double>(query::get_station_definition(w, station.station_id).population);
            }
            if (query::in_labour_market(w, station)) {
                station.jobs = query::station_workers(w, station);
                station.wage_cr_per_day = 0.0;
                station.wage_cr_per_day = query::station_wage_cr_per_day(w, station);
            }
            const double savings = w.data.universe_.households.savings_days * query::resident_wages_per_day(w, station);
            station.household_credits += savings;
            w.state.outside_economy_credits_ -= savings;
            w.state.journal_.record(query::account_earth(), query::account_households(w, station), MoneyCategory::SavingsEndowment, savings);
        }
    }
    w.state.seeded_money_supply_ = systems::internal_money_supply(w);
    systems::compute_rotation_tours(w);
    w.state.next_investment_review_s_ = w.data.universe_.fleet_investment.review_days * 86400.0;
    // Ships start with their full life-support endurance aboard, as if fresh from the yard.
    constexpr double INITIAL_PROVISION_DAYS = PROVISION_ENDURANCE_DAYS;
    for (auto& ship : w.state.ships_) {
        const auto& ship_class = query::ship_design(w, ship);
        for (const auto& [commodity_id, units_per_crew_day] : w.data.universe_.ship_operations.life_support_units_per_crew_day) {
            ship.provisions[commodity_id] = ship_class.crew_size * units_per_crew_day * INITIAL_PROVISION_DAYS;
        }
        // The starting fleet is of mixed ages (tier 2 wear): spread by a hash of the ship id
        // (FNV-1a, the same on every platform), so overhauls do not all fall due at once.
        if (w.data.universe_.ship_environment.wear) {
            std::uint64_t hash = 1469598103934665603ULL;
            for (const char ch : ship.id) {
                hash = (hash ^ static_cast<unsigned char>(ch)) * 1099511628211ULL;
            }
            ship.engine_wear = w.data.universe_.ship_environment.initial_wear_max * static_cast<double>(hash % 10000) / 10000.0;
        }
    }
    systems::seed_order_stats(w);
    systems::init_failure_clocks(w);
    systems::init_patrols(w);
}

std::string default_atlas_path(const std::string& data_root) {
    return (std::filesystem::path(data_root).parent_path() / "tests" / "data" / "variable_isp"
        / "variable_isp_atlas.bin").string();
}

domain::UniverseDefinition load_universe(const std::string& data_root,
    const std::function<void(domain::UniverseDefinition&)>& adjust) {
    data_loader::DataLoader loader;
    auto universe = loader.load_universe(data_root);
    adjust(universe);
    return universe;
}

void attach_engines(World& w, const std::string& data_root, const std::string& atlas_file) {
    w.engines.thread_pool_ = std::make_unique<util::ThreadPool>();

    // Try to load the VariableISP atlas.
    const std::filesystem::path atlas_path = atlas_file.empty() ? default_atlas_path(data_root) : atlas_file;
    if (std::filesystem::exists(atlas_path)) {
        try {
            w.engines.atlas_.load_binary(atlas_path.string());
            w.engines.atlas_loaded_ = true;
            w.engines.variable_isp_planner_ = std::make_unique<trajectory::VariableIspTrajectoryPlanner>(
                w.data.universe_, w.engines.mechanics_, w.engines.atlas_);
        } catch (const std::exception& ex) {
            std::cerr << "[VariableISP] Atlas load failed: " << ex.what() << " — electric ships will be stranded.\n";
        }
    } else {
        std::cerr << "[VariableISP] Atlas not found at " << atlas_path.string()
                  << " — electric ships will be stranded.\n";
    }
}

std::unique_ptr<World> make_world(const std::string& data_root, const std::string& atlas_file,
    const std::function<void(domain::UniverseDefinition&)>& adjust) {
    auto world = std::make_unique<World>(load_universe(data_root, adjust));
    init_world(*world);
    attach_engines(*world, data_root, atlas_file);
    return world;
}

}  // namespace spacetrains::simulation
