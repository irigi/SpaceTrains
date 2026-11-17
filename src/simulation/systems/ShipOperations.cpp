#include "simulation/systems/ShipOperations.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <random>
#include <string>

#include "simulation/CrewEnvironment.hpp"
#include "simulation/SimulationInternal.hpp"
#include "simulation/query/Accounts.hpp"
#include "simulation/query/Events.hpp"
#include "simulation/query/Labour.hpp"
#include "simulation/query/Lookups.hpp"
#include "simulation/query/ShipRates.hpp"
#include "simulation/systems/Market.hpp"

namespace spacetrains::simulation::systems {

using namespace detail;

namespace {

void pay_home_station(World& w, domain::ShipState& ship, double amount, MoneyCategory category) {
    ship.credits -= amount;
    ship.lifetime_profit -= amount;
    auto& home = query::get_station_state(w, ship.home_station_id);
    home.credits += amount;
    w.state.journal_.record(query::account_fleet(w, ship.faction_id), query::account_station(w, home), category, amount);
}

void pay_crew(World& w, domain::ShipState& ship, double amount, MoneyCategory category) {
    // The crew are residents of the home station: with households on (economy overhaul) their pay
    // goes to its households, who spend it there; before, to the station itself.
    if (!query::households_on(w)) {
        pay_home_station(w, ship, amount, category);
        return;
    }
    ship.credits -= amount;
    ship.lifetime_profit -= amount;
    auto& home = query::get_station_state(w, ship.home_station_id);
    home.household_credits += amount;
    home.ledger.crew_wages += amount;
    w.state.journal_.record(query::account_fleet(w, ship.faction_id), query::account_households(w, home), category, amount);
}

void accrue_radiation(World& w, domain::ShipState& ship, const domain::ShipClassDefinition& design, double days) {
    const auto& env = w.data.universe_.ship_environment;
    if (!env.radiation || days <= 0.0) {
        return;
    }
    double rate = 0.0;
    if (ship.phase == domain::ShipMissionPhase::InTransit) {
        // Cosmic rays where the ship is now (the distance from the Sun interpolated over the
        // leg); particle events strike separately (step_particle_events).
        rate = crew_environment::gcr_msv_per_day(env, query::ship_r_au(w, ship), w.state.game_time_s_ / 86400.0)
            * crew_environment::transit_gcr_factor(env, design, query::cabin_shield_g_cm2(w, design, ship.propellant_kg));
    } else {
        // In port the crew lives in the station's habitat.
        const auto it = w.data.universe_.station_radiation.find(ship.current_station_id);
        const double factor = it == w.data.universe_.station_radiation.end() ? 1.0 : it->second.location_factor;
        const double shield = it == w.data.universe_.station_radiation.end() ? 0.0 : it->second.shield_g_cm2;
        rate = factor * std::exp(-shield / env.gcr_attenuation_g_cm2)
            * crew_environment::gcr_msv_per_day(env, query::station_r_au(w, query::get_station_definition(w, ship.current_station_id), w.state.game_time_s_),
                w.state.game_time_s_ / 86400.0);
    }
    const double dose = rate * days;
    ship.leg_dose_msv += dose;
    const double pay = design.crew_size * dose * env.hazard_cr_per_msv;
    if (pay > 0.0) {
        pay_crew(w, ship, pay, MoneyCategory::HazardPay);
        ship.ledger.wages += pay;
    }
    w.audits.radiation_audit_.crew_dose_msv += design.crew_size * dose;
    w.audits.radiation_audit_.hazard_pay_cr += pay;
    w.audits.radiation_audit_.highest_leg_dose_msv = std::max(w.audits.radiation_audit_.highest_leg_dose_msv, ship.leg_dose_msv);
}

void accrue_wear(World& w, domain::ShipState& ship, const domain::ShipClassDefinition& design, double days) {
    const auto& env = w.data.universe_.ship_environment;
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
    const double r = std::max(0.1, query::ship_r_au(w, ship));
    const double hits_per_m2 = env.puncture_flux_m2_yr * std::pow(r, -env.puncture_flux_exponent) * days / 365.25;
    const double before = query::radiator_bucket(w, ship);
    ship.radiator_condition = std::max(0.0,
        ship.radiator_condition - hits_per_m2 * env.radiator_vulnerable_fraction * env.radiator_segment_m2);
    if (const double after = query::radiator_bucket(w, ship); after != before) {
        (void)query::register_design(w, ship.class_id, ship.fit, after);
    }
}

}  // namespace

double daily_crew_cost(const World& w, const domain::ShipClassDefinition& ship_class,
    const domain::StationState& market) {
    const auto& operations = w.data.universe_.ship_operations;
    double cost = ship_class.crew_size * operations.wage_cr_per_crew_day;
    for (const auto& [commodity_id, units_per_crew_day] : operations.life_support_units_per_crew_day) {
        cost += ship_class.crew_size * units_per_crew_day * systems::station_price(w, market, commodity_id);
    }
    return cost;
}

void buy_provisions(World& w, domain::ShipState& ship, domain::StationState& market, double days, bool departing) {
    const auto& market_def = query::get_station_definition(w, market.station_id);
    for (const auto& [commodity_id, units_per_crew_day] : w.data.universe_.ship_operations.life_support_units_per_crew_day) {
        const double missing = query::persons_aboard(w, ship) * units_per_crew_day * days - ship.provisions[commodity_id];
        const double units = std::min(missing, sellable_units(w.engines.economy_, market_def, market, commodity_id, departing));
        if (units <= 0.0) {
            continue;
        }
        // Price on the stock before the transfer, like every other trade.
        const double unit_price = systems::station_price(w, market, commodity_id);
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
        w.state.journal_.record(query::account_fleet(w, ship.faction_id), query::account_station(w, market), MoneyCategory::Provisions, bill);
        query::record_trade(w, {
            .time_s = w.state.game_time_s_,
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

bool consider_repair(World& w, domain::ShipState& ship) {
    const auto& env = w.data.universe_.ship_environment;
    if (!env.wear || ship.phase != domain::ShipMissionPhase::Idle) {
        return false;
    }
    const auto& design = query::ship_design(w, ship);
    const bool drive_due = ship.engine_wear >= env.overhaul_wear;
    const bool radiators_due = ship.radiator_condition < env.radiator_repair_condition;
    if (!drive_due && !radiators_due) {
        return false;
    }
    const auto bill = query::overhaul_bill(w, design, drive_due ? ship.engine_wear : 0.0,
        radiators_due ? 1.0 - ship.radiator_condition : 0.0);
    auto& yard = query::get_station_state(w, ship.current_station_id);
    double cost = bill.labour_cr;
    for (const auto& [commodity_id, units] : bill.spares_units) {
        const auto stock = yard.inventory.find(commodity_id);
        if (stock == yard.inventory.end() || stock->second < units) {
            ++w.audits.radiation_audit_.repairs_waiting_for_spares;
            return false;  // no spares here: fly on within the drive's remaining life
        }
        cost += units * systems::station_price(w, yard, commodity_id);
    }
    if (ship.credits - cost < -query::credit_line(design)) {
        return false;
    }
    double spares_cr = 0.0;
    for (const auto& [commodity_id, units] : bill.spares_units) {
        const double value = units * systems::station_price(w, yard, commodity_id);
        spares_cr += value;
        yard.inventory[commodity_id] -= units;
        query::record_trade(w, {
            .time_s = w.state.game_time_s_,
            .ship_id = ship.id,
            .station_id = yard.station_id,
            .commodity_id = commodity_id,
            .kind = "spares",
            .units = units,
            .unit_price = systems::station_price(w, yard, commodity_id),
            .total = value,
        });
        w.audits.radiation_audit_.spares_units[commodity_id] += units;
    }
    ship.credits -= cost;
    ship.lifetime_profit -= cost;
    ship.ledger.refits += cost;
    yard.credits += cost;
    yard.ledger.ship_sales += cost;
    w.state.journal_.record(query::account_fleet(w, ship.faction_id), query::account_station(w, yard), MoneyCategory::RefitsRepairs, cost);
    w.audits.radiation_audit_.repair_bills_cr += cost;
    w.audits.radiation_audit_.repair_spares_cr += spares_cr;
    w.audits.radiation_audit_.repair_labour_cr += bill.labour_cr;
    w.audits.radiation_audit_.repair_days += bill.days;
    ++w.audits.radiation_audit_.overhauls;
    ++(design.propulsion_type == "variable_isp" ? w.audits.radiation_audit_.plasma_overhauls : w.audits.radiation_audit_.thermal_overhauls);
    ship.overhaul = true;
    ship.refit_class_id = ship.class_id;
    ship.refit_fit = ship.fit;
    ship.refit_done_s = w.state.game_time_s_ + bill.days * 86400.0;
    ship.phase = domain::ShipMissionPhase::Refitting;
    query::add_event(w, std::format("{} overhauling at {}: {}{}{} ({:.0f} cr, {:.1f} days)", ship.name,
        query::get_station_definition(w, ship.current_station_id).name,
        drive_due ? (design.propulsion_type == "variable_isp" ? "plasma drive" : "reactor core") : "",
        drive_due && radiators_due ? " and " : "", radiators_due ? "radiators" : "", cost, bill.days), "mission");
    return true;
}

void step_particle_events(World& w, double dt_s) {
    const auto& env = w.data.universe_.ship_environment;
    if (!env.radiation || !w.data.events_enabled_) {
        return;
    }
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    const double roll = uniform(w.state.environment_rng_);
    const double size = uniform(w.state.environment_rng_);
    const double day = w.state.game_time_s_ / 86400.0;
    const double rate_per_day = env.spe_rate_per_year / 365.25 * 2.0 * crew_environment::solar_activity(env, day);
    if (roll >= rate_per_day * dt_s / 86400.0) {
        return;
    }
    const double magnitude = crew_environment::draw_spe_magnitude(env, size);
    ++w.audits.radiation_audit_.particle_events;
    w.audits.radiation_audit_.largest_event_magnitude = std::max(w.audits.radiation_audit_.largest_event_magnitude, magnitude);
    int hit = 0;
    double worst = 0.0;
    std::string worst_ship;
    for (auto& ship : w.state.ships_) {
        if (ship.phase == domain::ShipMissionPhase::LaidUp || ship.phase == domain::ShipMissionPhase::Refitting) {
            continue;  // no crew aboard
        }
        const auto& design = query::ship_design(w, ship);
        double dose = 0.0;
        if (ship.phase == domain::ShipMissionPhase::InTransit) {
            const double r = std::max(0.1, query::ship_r_au(w, ship));
            dose = magnitude * crew_environment::spe_reference_msv(env,
                crew_environment::shelter_g_cm2(design) + query::cabin_shield_g_cm2(w, design, ship.propellant_kg)) / (r * r);
            ++hit;
        } else {
            // Stations have shelters.
            const auto it = w.data.universe_.station_radiation.find(ship.current_station_id);
            const double factor = it == w.data.universe_.station_radiation.end() ? 1.0 : it->second.location_factor;
            const double r = std::max(0.1, query::station_r_au(w, query::get_station_definition(w, ship.current_station_id), w.state.game_time_s_));
            dose = factor * magnitude * env.spe_shelter_msv / (r * r);
        }
        ship.leg_dose_msv += dose;
        const double pay = design.crew_size * dose * env.hazard_cr_per_msv;
        if (pay > 0.0) {
            pay_crew(w, ship, pay, MoneyCategory::HazardPay);
            ship.ledger.wages += pay;
        }
        w.audits.radiation_audit_.crew_dose_msv += design.crew_size * dose;
        w.audits.radiation_audit_.hazard_pay_cr += pay;
        w.audits.radiation_audit_.highest_leg_dose_msv = std::max(w.audits.radiation_audit_.highest_leg_dose_msv, ship.leg_dose_msv);
        if (dose > worst) {
            worst = dose;
            worst_ship = ship.name;
        }
        if (ship.leg_dose_msv > env.career_limit_msv && ship.leg_dose_msv - dose <= env.career_limit_msv) {
            query::add_event(w, std::format("{}'s crew passed the {:.0f} mSv career limit ({:.0f} mSv this leg)",
                ship.name, env.career_limit_msv, ship.leg_dose_msv), "alert");
        }
    }
    query::add_event(w, std::format("Solar particle event ({:.2f}x August 1972): {} ships in flight exposed{}", magnitude, hit,
        worst > 0.0 ? std::format(", worst {} with {:.0f} mSv", worst_ship, worst) : std::string()), "news");
}

void accrue_operating_costs(World& w, domain::ShipState& ship, double dt_s) {
    const auto& ship_class = query::ship_design(w, ship);
    const double days = dt_s / 86400.0;

    // Liquid hydrogen boils off in flight (tier 2, step F); in port the depot keeps it cold.
    if (const double boil = ship.phase == domain::ShipMissionPhase::InTransit
                ? query::boiloff_kg_per_day(w, ship_class, query::ship_r_au(w, ship)) * days : 0.0;
        boil > 0.0 && ship.propellant_kg > 0.0) {
        const double lost = std::min(ship.propellant_kg, boil);
        ship.propellant_kg -= lost;
        w.audits.radiation_audit_.hydrogen_boiled_kg += lost;
        // Under way the tank level follows the plan's mass samples less what has boiled.
        ship.active_mission.boiled_propellant_kg += lost;
    }
    accrue_wear(w, ship, ship_class, days);
    // Capital (interest + amortisation) is owed whatever the ship does.
    const double capital = query::daily_capital_cost(w, ship_class) * days;
    pay_home_station(w, ship, capital, MoneyCategory::CapitalCharge);
    ship.ledger.capital += capital;
    if (ship.phase == domain::ShipMissionPhase::LaidUp || ship.phase == domain::ShipMissionPhase::Refitting) {
        return;  // crew discharged: no wages, no life support
    }

    const double wages = ship_class.crew_size * w.data.universe_.ship_operations.wage_cr_per_crew_day * days;
    pay_crew(w, ship, wages, MoneyCategory::CrewWages);
    ship.ledger.wages += wages;
    accrue_radiation(w, ship, ship_class, days);

    const bool docked = ship.phase == domain::ShipMissionPhase::Idle
        || ship.phase == domain::ShipMissionPhase::Stranded
        || ship.phase == domain::ShipMissionPhase::Refueling;
    const auto& life_support = w.data.universe_.ship_operations.life_support_units_per_crew_day;
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
            buy_provisions(w, ship, query::get_station_state(w, ship.current_station_id),
                std::max(PROVISION_ENDURANCE_DAYS, PROVISION_MIN_RETURN_DAYS + DOCKED_PROVISION_TARGET_DAYS), false);
        }
    }
    bool reserve_breached = false;
    const double persons = query::persons_aboard(w, ship);
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
        ship.laid_up_since_s = w.state.game_time_s_;
        ship.next_review_s = w.state.game_time_s_ + LAYUP_REVIEW_DAYS * 86400.0;
        query::add_event(w, std::format("{} laid up at {} (port cannot provision the crew)",
            ship.name, query::get_station_definition(w, ship.current_station_id).name), "alert");
    }
}

void buy_propellant(World& w, domain::ShipState& ship, double kg) {
    const double transferable_kg = std::min(std::max(0.0, kg), query::purchasable_propellant_kg(w, ship));
    if (transferable_kg <= 0.0) {
        return;
    }
    auto& station = query::get_station_state(w, ship.current_station_id);
    const double transferred_units = transferable_kg / FUEL_UNITS_TO_KG;
    // Price the fuel on the stock before the transfer. Ships may go into debt for fuel
    // (but not for cargo) so an empty wallet never permanently strands a ship.
    const double fuel_bill = systems::trade_value(w, station, "fuel", -transferred_units);
    const double fuel_unit_price = fuel_bill / transferred_units;
    station.inventory["fuel"] -= transferred_units;
    station.ship_fuel_units_per_day += transferred_units / TRADE_FLOW_DAYS;
    ship.propellant_kg += transferable_kg;
    ship.credits -= fuel_bill;
    ship.lifetime_profit -= fuel_bill;
    ship.ledger.fuel += fuel_bill;
    station.credits += fuel_bill;
    station.ledger.ship_sales += fuel_bill;
    w.state.journal_.record(query::account_fleet(w, ship.faction_id), query::account_station(w, station), MoneyCategory::Fuel, fuel_bill);
    query::record_trade(w, {
        .time_s = w.state.game_time_s_,
        .ship_id = ship.id,
        .station_id = station.station_id,
        .commodity_id = "fuel",
        .kind = "fuel",
        .units = transferred_units,
        .unit_price = fuel_unit_price,
        .total = fuel_bill,
    });
    query::add_event(w, std::format(
        "{} refueled {:.0f} kg at {} for {:.0f} cr",
        ship.name, transferable_kg, query::get_station_definition(w, ship.current_station_id).name, fuel_bill),
        "fuel");
}

}  // namespace spacetrains::simulation::systems
