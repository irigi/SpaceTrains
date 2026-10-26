// Running a ship: capital and crew costs, provisions, radiation, wear and repair, boil-off, propellant.

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
        journal_.record(account_fleet(ship.faction_id), account_station(market), MoneyCategory::Provisions, bill);
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
    // last answer per station. A pure cache for the simulation thread; parallel work (the
    // reference prices and fares) computes it directly.
    if (util::ThreadPool::in_parallel_for()) {
        return mechanics_.get_heliocentric_radius(station.parent_body_id, time_s) / 1.495978707e11;
    }
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
        pay_crew(ship, pay, MoneyCategory::HazardPay);
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
    journal_.record(account_fleet(ship.faction_id), account_station(yard), MoneyCategory::RefitsRepairs, cost);
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
            pay_crew(ship, pay, MoneyCategory::HazardPay);
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

void Simulation::pay_crew(domain::ShipState& ship, double amount, MoneyCategory category) {
    // The crew are residents of the home station: with households on (economy overhaul) their pay
    // goes to its households, who spend it there; before, to the station itself.
    if (!households_on()) {
        pay_home_station(ship, amount, category);
        return;
    }
    ship.credits -= amount;
    ship.lifetime_profit -= amount;
    auto& home = get_station_state(ship.home_station_id);
    home.household_credits += amount;
    home.ledger.crew_wages += amount;
    journal_.record(account_fleet(ship.faction_id), account_households(home), category, amount);
}

void Simulation::pay_home_station(domain::ShipState& ship, double amount, MoneyCategory category) {
    ship.credits -= amount;
    ship.lifetime_profit -= amount;
    auto& home = get_station_state(ship.home_station_id);
    home.credits += amount;
    journal_.record(account_fleet(ship.faction_id), account_station(home), category, amount);
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
    pay_home_station(ship, capital, MoneyCategory::CapitalCharge);
    ship.ledger.capital += capital;
    if (ship.phase == domain::ShipMissionPhase::LaidUp || ship.phase == domain::ShipMissionPhase::Refitting) {
        return;  // crew discharged: no wages, no life support
    }

    const double wages = ship_class.crew_size * universe_.ship_operations.wage_cr_per_crew_day * days;
    pay_crew(ship, wages, MoneyCategory::CrewWages);
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
    journal_.record(account_fleet(ship.faction_id), account_station(station), MoneyCategory::Fuel, fuel_bill);
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

}  // namespace spacetrains::simulation
