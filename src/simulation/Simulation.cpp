#include "simulation/Simulation.hpp"
#include "persistence/SimulationPersistence.hpp"
#include "domain/ShipDesign.hpp"

#include "util/Profiling.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <iostream>
#include <stdexcept>

#include "simulation/SimulationInternal.hpp"
#include "simulation/WorldSetup.hpp"
#include "simulation/query/Accounts.hpp"
#include "simulation/query/Events.hpp"
#include "simulation/query/Lookups.hpp"
#include "simulation/query/Positions.hpp"
#include "simulation/systems/StationSystems.hpp"

namespace spacetrains::simulation {

using namespace detail;


Simulation::Simulation(domain::UniverseDefinition universe)
    : world_(std::make_unique<World>(std::move(universe))) {
    init_world(*world_);
}

Simulation::Simulation(std::unique_ptr<World> world)
    : world_(std::move(world)) {}

Simulation Simulation::from_data_root(const std::string& data_root) {
    return from_data_root(data_root, [](domain::UniverseDefinition&) {});
}

Simulation Simulation::from_data_root(const std::string& data_root,
    const std::function<void(domain::UniverseDefinition&)>& adjust) {
    return from_data_root(data_root, std::string {}, adjust);
}

std::string Simulation::default_atlas_path(const std::string& data_root) {
    return simulation::default_atlas_path(data_root);
}

Simulation Simulation::from_data_root(const std::string& data_root, const std::string& atlas_file,
    const std::function<void(domain::UniverseDefinition&)>& adjust) {
    return Simulation(make_world(data_root, atlas_file, adjust));
}

namespace {

// Defined below, file-local.
void settle_local_economy(World& w, const std::vector<domain::Inventory>& stocks_before, double dt_s);
void step_population(World& w, double dt_s);
void step_events(World& w, double dt_s);

}  // namespace

void Simulation::start_at(double time_s) {
    simulation::start_at(*world_, time_s);
}

void Simulation::step(double real_dt_s) {
    simulation::step(*world_, real_dt_s);
}

void Simulation::set_timewarp(double timewarp_factor) {
    world_->timewarp_factor_ = std::max(1.0, timewarp_factor);
}

double Simulation::timewarp_factor() const {
    return world_->timewarp_factor_;
}

const domain::UniverseDefinition& Simulation::universe() const {
    return world_->universe_;
}

const economy::EconomySystem& Simulation::economy_system() const {
    return world_->economy_;
}

std::vector<Simulation::ReferencePrice> Simulation::compute_reference_prices() const {
    return systems::compute_reference_prices(*world_);
}

void Simulation::set_station_stock(const std::string& station_id, const std::string& commodity_id, double units) {
    systems::set_station_stock(*world_, station_id, commodity_id, units);
}

domain::OrderView Simulation::order_view(const domain::StationState& state, const std::string& commodity_id) const {
    return systems::order_view(*world_, state, commodity_id);
}

Simulation::OverhaulBill Simulation::overhaul_bill(const domain::ShipClassDefinition& design, double drive_share,
    double radiator_share) const {
    return query::overhaul_bill(*world_, design, drive_share, radiator_share);
}

double Simulation::wear_cr_per_drive_hour(const domain::ShipClassDefinition& design) const {
    return query::wear_cr_per_drive_hour(*world_, design);
}

double Simulation::wear_cr_per_flight_day(const domain::ShipClassDefinition& design) const {
    return query::wear_cr_per_flight_day(*world_, design);
}

double Simulation::wear_cr_per_propellant_kg(const domain::ShipClassDefinition& design) const {
    return query::wear_cr_per_propellant_kg(*world_, design);
}

double Simulation::station_profit_last_year(const domain::StationState& station) const {
    return query::station_profit_last_year(*world_, station);
}

double Simulation::account_balance(int account) const {
    return systems::account_balance(*world_, account);
}

std::string Simulation::build_stats_json() const {
    return systems::build_stats_json(*world_);
}

double Simulation::resident_wages_per_day(const domain::StationState& station) const {
    return query::resident_wages_per_day(*world_, station);
}

double Simulation::colony_wage_median(const domain::BackgroundEconomyDefinition& economy) const {
    return query::colony_wage_median(*world_, economy);
}

bool Simulation::in_labour_market(const domain::StationState& station) const {
    return query::in_labour_market(*world_, station);
}

double Simulation::station_wage_cr_per_day(const domain::StationState& station) const {
    return query::station_wage_cr_per_day(*world_, station);
}

double Simulation::station_workers(const domain::StationState& station) const {
    return query::station_workers(*world_, station);
}

double Simulation::station_lifetime_profit(const domain::StationState& station) const {
    return query::station_lifetime_profit(*world_, station);
}

double Simulation::colony_people_factor(const std::string& economy_id) const {
    return systems::colony_people_factor(*world_, economy_id);
}

double Simulation::colony_affordability(double account, double credit_line, double buffer) {
    return systems::colony_affordability(account, credit_line, buffer);
}

double Simulation::colony_import_share(double affordability, bool essential) {
    return systems::colony_import_share(affordability, essential);
}

double Simulation::labour_migrants_per_day(const domain::StationState& station) const {
    return systems::labour_migrants_per_day(*world_, station);
}

double Simulation::labour_emigrants_per_day(const domain::StationState& station) const {
    return systems::labour_emigrants_per_day(*world_, station);
}

double Simulation::settler_gain_tail(const domain::StationState& destination, double gain_cr) const {
    return systems::settler_gain_tail(*world_, destination, gain_cr);
}

double Simulation::traveller_budget_tail(const std::string& home_id, double cost_cr) const {
    return systems::traveller_budget_tail(*world_, home_id, cost_cr);
}

const domain::StationDefinition* Simulation::yard_station_for(const domain::StationDefinition& home) const {
    return systems::yard_station_for(*world_, home);
}

bool Simulation::employer_pays(domain::PassengerKind kind) const {
    return query::employer_pays(*world_, kind);
}

double Simulation::port_fuel_price(const std::string& station_id) const {
    return systems::port_fuel_price(*world_, station_id);
}

double Simulation::live_fare_cr(const std::string& origin_id, const std::string& destination_id,
    const domain::PassengerDefinition::ReferenceFare& reference) const {
    return systems::live_fare_cr(*world_, origin_id, destination_id, reference);
}

const char* Simulation::passenger_kind_name(domain::PassengerKind kind) {
    return systems::passenger_kind_name(kind);
}

double Simulation::settler_share(double payback_years) const {
    return systems::settler_share(*world_, payback_years);
}

double Simulation::passenger_flow_per_day(const std::string& origin_id, const std::string& destination_id) const {
    return systems::passenger_flow_per_day(*world_, origin_id, destination_id);
}

double Simulation::carried_passengers_per_day(const std::string& origin_id, const std::string& destination_id) const {
    return systems::carried_passengers_per_day(*world_, origin_id, destination_id);
}

Simulation::PassengerRevenue Simulation::expected_passenger_revenue(const std::string& origin_id,
    const std::string& destination_id, double days, double window_days, double berths, double open_share) const {
    return systems::expected_passenger_revenue(*world_, origin_id, destination_id, days, window_days, berths, open_share);
}

double Simulation::expected_fare_cr(const std::string& origin_id, const std::string& destination_id, double days,
    double demand_per_trip, double berths) const {
    return systems::expected_fare_cr(*world_, origin_id, destination_id, days, demand_per_trip, berths);
}

std::vector<Simulation::ReferenceFareRow> Simulation::compute_reference_fares() const {
    return systems::compute_reference_fares(*world_);
}

double Simulation::station_tax_ceiling(const domain::OpenEconomyDefinition& open, double treasury) {
    return systems::station_tax_ceiling(open, treasury);
}

domain::SimulationSnapshot Simulation::snapshot() const {
    return systems::snapshot(*world_);
}

math::Vec3d Simulation::get_ship_render_position(const domain::ShipState& ship) const {
    return query::get_ship_render_position(*world_, ship);
}

std::string Simulation::bridge_paths_signature() const {
    return systems::bridge_paths_signature(*world_);
}

std::string Simulation::build_bridge_paths_json() const {
    return systems::build_bridge_paths_json(*world_);
}

std::string Simulation::build_bridge_designs_json() const {
    return systems::build_bridge_designs_json(*world_);
}

std::string Simulation::build_bridge_snapshot_json(bool paused, std::uint64_t snapshot_seq, double snapshot_real_time_s,
    bool with_paths) const {
    return systems::build_bridge_snapshot_json(*world_, paused, snapshot_seq, snapshot_real_time_s, with_paths);
}

std::string Simulation::build_report() const {
    return systems::build_report(*world_);
}

std::string Simulation::data_fingerprint() const {
    return persistence::data_fingerprint(*world_);
}

std::string Simulation::save_state_json() const {
    return persistence::save_state_json(*world_);
}

void Simulation::load_state_json(const std::string& text) {
    persistence::load_state_json(*world_, text);
}

void start_at(World& w, double time_s) {
    w.game_time_s_ = time_s;
    w.event_rng_.seed(w.universe_.event_seed ^ static_cast<std::uint64_t>(time_s));
    w.environment_rng_.seed((w.universe_.event_seed + 1) ^ static_cast<std::uint64_t>(time_s));
    w.next_investment_review_s_ = time_s;
    w.journal_.set_next_close_day(time_s / 86400.0 + MoneyJournal::kPeriodDays);
    for (auto& ship : w.ships_) {
        ship.next_review_s = time_s;
        ship.idle_since_s = time_s;
        ship.next_refit_review_s = time_s;
        ship.fails_at_s = 0.0;  // drawn again below, relative to the new start
    }
    systems::init_failure_clocks(w);
    for (std::size_t i = 0; i < w.patrols_.size(); ++i) {
        w.patrols_[i].next_review_s = time_s + 0.25 * 86400.0 * static_cast<double>(i + 1);
    }
}

void step(World& w, double real_dt_s) {
    w.untimed_s_ += real_dt_s * w.timewarp_factor_;
    // A millisecond of slack absorbs rounding (0.1 s x 86400 is not exactly 8640 s).
    while (w.untimed_s_ >= TICK_S - 1e-3) {
        w.untimed_s_ = std::max(0.0, w.untimed_s_ - TICK_S);
        tick(w);
    }
}

void tick(World& w) {
    const profiling::Scope profile_scope(profiling::Phase::Step);
    const double dt_s = TICK_S;
    w.game_time_s_ += dt_s;
    std::vector<domain::Inventory> stocks_before;
    stocks_before.reserve(w.stations_.size());
    for (const auto& station : w.stations_) {
        stocks_before.push_back(station.inventory);
    }
    systems::update_market_responses(w);
    {
        const profiling::Scope economy_scope(profiling::Phase::EconomyStep);
        w.economy_.step(w.stations_, dt_s);
    }
    for (auto& station : w.stations_) {
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
    settle_local_economy(w, stocks_before, dt_s);
    systems::step_households(w, dt_s);
    systems::step_surface_markets(w, dt_s);
    systems::step_labour(w, dt_s);
    systems::step_colony_labour(w, dt_s);
    step_population(w, dt_s);
    if (query::passengers_on(w)) {
        systems::step_passengers(w, dt_s);
    }
    step_events(w, dt_s);
    systems::step_particle_events(w, dt_s);
    systems::step_treasuries(w, dt_s);
    systems::step_emergencies(w);
    systems::update_market_prices(w);
    systems::step_orders(w);
    systems::step_fleet_investment(w);
    systems::step_failures(w);
    systems::step_patrols(w, dt_s);

    for (auto& ship : w.ships_) {
        systems::accrue_operating_costs(w, ship, dt_s);
        switch (ship.phase) {
            case domain::ShipMissionPhase::Idle:
            case domain::ShipMissionPhase::LaidUp:
                systems::step_idle_ship(w, ship);
                break;
            case domain::ShipMissionPhase::AwaitingDeparture:
                systems::step_awaiting_departure_ship(w, ship);
                break;
            case domain::ShipMissionPhase::InTransit:
                systems::step_in_transit_ship(w, ship, dt_s);
                break;
            case domain::ShipMissionPhase::Refueling:
                ship.phase = domain::ShipMissionPhase::Idle;
                break;
            case domain::ShipMissionPhase::Refitting:
                systems::step_refitting_ship(w, ship);
                break;
            case domain::ShipMissionPhase::Stranded:
                // Fuel for sale here is enough: missions buy their own load.
                if (query::purchasable_propellant_kg(w, ship) > 0.0) {
                    ship.phase = domain::ShipMissionPhase::Idle;
                    query::add_event(w, std::format("{} recovered from stranded state at {}", ship.name, query::get_station_definition(w, ship.current_station_id).name), "alert");
                }
                break;
        }
    }

    // Debug aid: SPACETRAINS_TRACE_FLEET=1 logs the fleet's hold use every 5 days (stderr):
    // ships and hold units by phase, and the units aboard in flight and while waiting to leave.
    if (static const bool trace_fleet = std::getenv("SPACETRAINS_TRACE_FLEET") != nullptr;
        trace_fleet && std::fmod(w.game_time_s_ / 86400.0 + 1e-6, 5.0) < dt_s / 86400.0) {
        int loaded = 0, empty = 0, waiting = 0, docked = 0, contracted = 0;
        double hold = 0.0, hold_loaded = 0.0, hold_empty = 0.0, aboard_flying = 0.0, aboard_waiting = 0.0;
        for (const auto& ship : w.ships_) {
            contracted += systems::under_contract(w, ship) ? 1 : 0;
            const double capacity = query::ship_design(w, ship).cargo_capacity_units;
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
            "aboard_flying {:.0f} aboard_waiting {:.0f} contracts {}\n", w.game_time_s_ / 86400.0, w.ships_.size(), hold, loaded,
            hold_loaded, empty, hold_empty, waiting, docked, aboard_flying, aboard_waiting, contracted);
    }
    // The money journal's monthly sample (statistics).
    if (w.game_time_s_ / 86400.0 + 1e-6 >= w.journal_.next_close_day()) {
        systems::close_journal_period(w);
    }
}

namespace {

void settle_local_economy(World& w, const std::vector<domain::Inventory>& stocks_before, double dt_s) {
    // A station and its residents are one account (step 17): local production costs the station
    // nothing (before, it paid local producers up to the base price for new output, and a
    // producer such as Venus that sold its glut to ships for less went broke). Residents pay for
    // what they use up, with income from outside like wages, at what their station paid ships
    // for it (the reference price until a delivery): before step 17 they paid the scarcity
    // curve, up to 16x for goods nobody had delivered, the main source of new money; paying
    // only the reference price left every chronically short station losing money on each import.
    for (std::size_t i = 0; i < w.stations_.size(); ++i) {
        auto& station = w.stations_[i];
        const auto& definition = query::get_station_definition(w, station.station_id);
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
                ? w.economy_.reference_price(definition, commodity_id) : cost->second;
            const double value = retail * used;
            station.credits += value;
            station.ledger.household_sales += value;
            (query::households_on(w) ? station.household_credits : w.outside_economy_credits_) -= value;
            w.journal_.record(query::households_on(w) ? query::account_households(w, station) : query::account_earth(), query::account_station(w, station),
                MoneyCategory::HouseholdGoods, value);
            spent += value;
        }
        // Passages the station paid (passengers) come back through its prices: a markup on what
        // the residents spend, at most passage_max_markup of it.
        if (station.passage_debt_cr > 0.0 && w.universe_.passengers.passage_recovery_days > 0.0) {
            const double due = station.passage_debt_cr
                * (1.0 - std::exp(-dt_s / 86400.0 / w.universe_.passengers.passage_recovery_days));
            const double recovered = std::min(due, w.universe_.passengers.passage_max_markup * spent);
            station.credits += recovered;
            station.ledger.passages_recovered += recovered;
            station.passage_debt_cr -= recovered;
            (query::households_on(w) ? station.household_credits : w.outside_economy_credits_) -= recovered;
            w.journal_.record(query::households_on(w) ? query::account_households(w, station) : query::account_earth(), query::account_station(w, station),
                MoneyCategory::PassageRecovery, recovered);
        }
    }
}

// Step 27: residents come while their station keeps them supplied and pays its way, and leave
// while it does not (see GrowthDefinition). Rates follow the population, so a grown station
// needs more and makes more; growth stops by itself where supply cannot keep up.
void step_population(World& w, double dt_s) {
    const auto& growth = w.universe_.growth;
    const double dt_days = dt_s / 86400.0;
    for (auto& station : w.stations_) {
        const auto& definition = query::get_station_definition(w, station.station_id);
        double worst = 1.0;
        for (const auto& [commodity_id, available] : station.upkeep_availability) {
            worst = std::min(worst, available);
        }
        station.supply_index += (worst - station.supply_index) * std::min(1.0, dt_days / growth.window_days);
        double rate = 0.0;
        if (station.supply_index >= growth.grow_above && station.credits >= w.universe_.open_economy.station_credit_floor) {
            rate = growth.growth_per_year;
        } else if (station.supply_index < growth.decline_below) {
            rate = -growth.decline_per_year;
        }
        const double seeded = static_cast<double>(definition.population);
        const double floor = std::max(w.universe_.open_economy.core_crew_fraction, 0.0) * seeded;
        const double before = station.population;
        if (query::in_labour_market(w, station) && query::migration_by_ship(w)) {
            // Labour market: settlers come for the station's vacancies, the unemployed go home;
            // what each would pay is their own gain (passenger_bands).
            if (const double in = systems::labour_migrants_per_day(w, station) * dt_days; in > 0.0) {
                systems::post_travellers(w, query::get_station_state(w, systems::gateway_for(w, station.station_id, true)), domain::PassengerKind::Migrant,
                    station.station_id, station.station_id, in, true);
            }
            if (const double out = systems::labour_emigrants_per_day(w, station) * dt_days; out > 0.0) {
                systems::post_travellers(w, station, domain::PassengerKind::Emigrant, systems::gateway_for(w, station.station_id, false), "", out, true);
            }
        } else if (query::migration_by_ship(w) && !query::is_earth_gateway(w, station.station_id)) {
            // Passengers: people come and go by ship. A thriving station draws settlers from Earth
            // at the growth rate (at the reference cost of the trip; the fare elasticity spreads
            // them over cheaper and dearer trips), a failing one sees its people leave for Earth.
            const double ceiling = growth.max_population_factor * seeded;
            if (rate > 0.0
                && station.population + systems::travellers_pending(w, station.station_id, domain::PassengerKind::Migrant) < ceiling) {
                systems::post_travellers(w, query::get_station_state(w, systems::gateway_for(w, station.station_id, true)), domain::PassengerKind::Migrant,
                    station.station_id, query::employer_pays(w, domain::PassengerKind::Migrant) ? station.station_id : "",
                    rate * station.population * dt_days / 365.25);
            } else if (rate < 0.0
                && station.population - systems::travellers_pending(w, station.station_id, domain::PassengerKind::Emigrant) > floor) {
                systems::post_travellers(w, station, domain::PassengerKind::Emigrant, systems::gateway_for(w, station.station_id, false), "",
                    -rate * station.population * dt_days / 365.25);
            }
        } else {
            station.population = std::clamp(station.population * std::exp(rate * dt_days / 365.25),
                std::min(floor, seeded), growth.max_population_factor * seeded);
        }
        if (station.population != before) {
            w.economy_.set_population(station.station_id, station.population);
        }
        const double announced = station.population_announced > 0.0 ? station.population_announced : seeded;
        if (std::abs(station.population / announced - 1.0) >= 0.05) {
            query::add_event(w, station.population > announced
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
void step_events(World& w, double dt_s) {
    for (auto& station : w.stations_) {
        std::erase_if(station.events, [&](const domain::ActiveEvent& active) {
            if (active.end_s > w.game_time_s_) {
                return false;
            }
            const auto it = std::find_if(w.universe_.events.begin(), w.universe_.events.end(),
                [&](const auto& event) { return event.id == active.event_id; });
            if (it != w.universe_.events.end() && !it->end_headline.empty()) {
                query::add_event(w, it->end_headline, "news");
            }
            return true;
        });
    }
    if (!w.events_enabled_) {
        return;
    }
    const double dt_years = dt_s / 86400.0 / 365.25;
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    for (const auto& event : w.universe_.events) {
        const double roll = uniform(w.event_rng_);
        const double length = uniform(w.event_rng_);
        if (roll >= event.rate_per_year * dt_years) {
            continue;
        }
        auto& station = query::get_station_state(w, event.station_id);
        if (std::any_of(station.events.begin(), station.events.end(),
                [&](const auto& active) { return active.event_id == event.id; })) {
            continue;
        }
        ++w.events_started_[event.id];
        query::add_event(w, event.headline, "news");
        if (event.kind == domain::EventKind::Migration && query::migration_by_ship(w) && !query::is_earth_gateway(w, station.station_id)) {
            // With passengers the settlers wait at Earth for ships.
            systems::post_travellers(w, query::get_station_state(w, systems::gateway_for(w, station.station_id, true)), domain::PassengerKind::Migrant,
                station.station_id, query::employer_pays(w, domain::PassengerKind::Migrant) ? station.station_id : "",
                event.magnitude * station.population, true);
            continue;
        }
        if (event.kind == domain::EventKind::Migration) {
            const auto& definition = query::get_station_definition(w, station.station_id);
            station.population = std::min(station.population * (1.0 + event.magnitude),
                w.universe_.growth.max_population_factor * static_cast<double>(definition.population));
            station.population_announced = station.population;
            w.economy_.set_population(station.station_id, station.population);
            continue;
        }
        const double days = event.min_days + (event.max_days - event.min_days) * length;
        station.events.push_back({.event_id = event.id, .start_s = w.game_time_s_, .end_s = w.game_time_s_ + days * 86400.0});
    }
}

}  // namespace

}  // namespace spacetrains::simulation
