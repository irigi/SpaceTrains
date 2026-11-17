#include "simulation/Simulation.hpp"
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
    : World(std::move(universe)) {
    init_world(*this);
}


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
    Simulation sim(load_universe(data_root, adjust));
    attach_engines(sim, data_root, atlas_file);
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
    for (std::size_t i = 0; i < patrols_.size(); ++i) {
        patrols_[i].next_review_s = time_s + 0.25 * 86400.0 * static_cast<double>(i + 1);
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
    return query::radiator_bucket(*this, ship);
}

std::string Simulation::station_systems_json(const domain::StationDefinition& station, const domain::StationState& state) const {
    return systems::station_systems_json(*this, station, state);
}

const domain::ShipClassDefinition& Simulation::ship_design(const domain::ShipState& ship) const {
    return query::ship_design(*this, ship);
}

const domain::ShipModuleDefinition* Simulation::find_module(const std::string& hull_id, const std::string& module_id) const {
    return query::find_module(*this, hull_id, module_id);
}

bool Simulation::module_has_active_effect(const domain::ShipModuleDefinition& module) const {
    return query::module_has_active_effect(*this, module);
}

const domain::ShipClassDefinition& Simulation::register_design(
    const std::string& class_id, const std::vector<std::string>& fit, double radiator_bucket) {
    return query::register_design(*this, class_id, fit, radiator_bucket);
}

const domain::ShipClassDefinition& Simulation::get_ship_class(const std::string& class_id) const {
    return query::get_ship_class(*this, class_id);
}

const domain::CelestialBodyDefinition& Simulation::get_body_definition(const std::string& body_id) const {
    return query::get_body_definition(*this, body_id);
}

const domain::StationDefinition& Simulation::get_station_definition(const std::string& station_id) const {
    return query::get_station_definition(*this, station_id);
}

domain::StationState& Simulation::get_station_state(const std::string& station_id) {
    return query::get_station_state(static_cast<World&>(*this), station_id);
}

const domain::StationState& Simulation::get_station_state(const std::string& station_id) const {
    return query::get_station_state(static_cast<const World&>(*this), station_id);
}

void Simulation::add_event(std::string text, std::string category) {
    query::add_event(*this, std::move(text), std::move(category));
}

void Simulation::record_trade(domain::TradeEntry trade) {
    query::record_trade(*this, std::move(trade));
}

const domain::CommodityDefinition& Simulation::get_commodity(const std::string& commodity_id) const {
    return query::get_commodity(*this, commodity_id);
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
    step_patrols(dt_s);

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
