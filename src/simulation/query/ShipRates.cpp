#include "simulation/query/ShipRates.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "economy/EconomySystem.hpp"
#include "simulation/CrewEnvironment.hpp"
#include "simulation/SimulationInternal.hpp"
#include "simulation/query/Lookups.hpp"
#include "util/ThreadPool.hpp"

namespace spacetrains::simulation::query {

using namespace detail;

double daily_capital_cost(const World& w, const domain::ShipClassDefinition& ship_class) {
    const auto& operations = w.universe_.ship_operations;
    return ship_class.ship_value_cr
        * (operations.interest_rate_per_year + 1.0 / operations.lifetime_years) / 365.0;
}

double station_r_au(const World& w, const domain::StationDefinition& station, double time_s) {
    // Asked many times per tick at the same few times (now, a bucket start): remember the
    // last answer per station. A pure cache for the simulation thread; parallel work (the
    // reference prices and fares) computes it directly.
    if (util::ThreadPool::in_parallel_for()) {
        return w.mechanics_.get_heliocentric_radius(station.parent_body_id, time_s) / 1.495978707e11;
    }
    auto& slot = w.station_radius_cache_[station.id];
    if (slot.first != time_s || slot.second <= 0.0) {
        slot = {time_s, w.mechanics_.get_heliocentric_radius(station.parent_body_id, time_s) / 1.495978707e11};
    }
    return slot.second;
}

double docked_dose_msv_per_day(const World& w, const std::string& station_id, double time_s) {
    const auto& env = w.universe_.ship_environment;
    const auto it = w.universe_.station_radiation.find(station_id);
    const domain::StationRadiationDefinition open_space;
    return crew_environment::expected_docked_msv_per_day(env, it == w.universe_.station_radiation.end() ? open_space : it->second,
        query::station_r_au(w, query::get_station_definition(w, station_id), time_s), time_s / 86400.0);
}

double cabin_shield_g_cm2(const World& w, const domain::ShipClassDefinition& design, double propellant_kg) {
    if (!w.active_effects_.contains("cabin_shield_g_cm2")) {
        return 0.0;
    }
    return crew_environment::cabin_shield_g_cm2(design, propellant_kg);
}

double leg_dose_msv_per_day(const World& w, const domain::ShipClassDefinition& design,
    const domain::StationDefinition& from, const domain::StationDefinition& to, double time_s) {
    // Before the leg is flown: cabins inside the tanks count their mean fill over a leg.
    const double cabin = query::cabin_shield_g_cm2(w, design, w.universe_.passengers.tank_wrap_mean_fill * design.propellant_capacity_kg);
    return crew_environment::expected_transit_msv_per_day(w.universe_.ship_environment, design,
        query::station_r_au(w, from, time_s), query::station_r_au(w, to, time_s), time_s / 86400.0, cabin);
}

double hazard_cr_per_day(const World& w, const domain::ShipClassDefinition& design,
    const domain::StationDefinition& from, const domain::StationDefinition& to, double time_s) {
    const auto& env = w.universe_.ship_environment;
    if (!env.radiation || env.hazard_cr_per_msv <= 0.0) {
        return 0.0;
    }
    return design.crew_size * query::leg_dose_msv_per_day(w, design, from, to, time_s) * env.hazard_cr_per_msv;
}

bool leg_dose_allowed(const World& w, const domain::ShipClassDefinition& design,
    const domain::StationDefinition& from, const domain::StationDefinition& to, double time_s, double days) {
    const auto& env = w.universe_.ship_environment;
    return !env.radiation || query::leg_dose_msv_per_day(w, design, from, to, time_s) * days <= env.career_limit_msv;
}

double cargo_decay_per_day(const World& w, const domain::ShipClassDefinition& design, const std::string& commodity_id) {
    const double ambient = query::get_commodity(w, commodity_id).decay_fraction_per_day;
    if (ambient <= 0.0 || commodity_id == economy::FUEL_ID || !w.universe_.ship_environment.cold_storage) {
        return ambient;
    }
    const auto it = design.effects.find("perishable_decay_factor");
    return it == design.effects.end() ? ambient : ambient * it->second;
}

double drive_life_h(const World& w, const domain::ShipClassDefinition& design) {
    const auto& env = w.universe_.ship_environment;
    return design.propulsion_type == "variable_isp" ? env.plasma_drive_life_h : env.ntr_core_life_h;
}

double drive_hours(const domain::ShipClassDefinition& design, const domain::TrajectoryPlan& plan) {
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

OverhaulBill overhaul_bill(const World& w, const domain::ShipClassDefinition& design, double drive_share,
    double radiator_share) {
    // The wearing parts (spares.csv) in the share that is worn: their goods, and the hours to
    // make the spares from them and fit them, at the labour scale.
    OverhaulBill bill;
    for (const auto& part : design.components) {
        if (part.parent_id.empty()) {
            continue;
        }
        const auto spec = w.universe_.spare_parts.find(part.kind);
        if (spec == w.universe_.spare_parts.end()) {
            continue;
        }
        const double share = spec->second.wears_with == "drive" ? drive_share : radiator_share;
        const double kg = part.mass_kg * share;
        if (kg <= 0.0) {
            continue;
        }
        for (const auto& [commodity_id, mass_share] : spec->second.materials) {
            bill.spares_units[commodity_id] += kg * mass_share / query::get_commodity(w, commodity_id).mass_per_unit_kg;
        }
        const double hours = kg * (spec->second.fabrication_h_per_kg + spec->second.install_h_per_kg);
        bill.hours += hours;
        bill.labour_cr += hours * w.universe_.labour.wage_cr_per_hour(spec->second.grade);
    }
    const auto& env = w.universe_.ship_environment;
    bill.days = std::max(env.min_repair_days, bill.hours / (env.repair_team_size * env.repair_team_hours_per_day));
    return bill;
}

double wear_cr_per_drive_hour(const World& w, const domain::ShipClassDefinition& design) {
    if (!w.universe_.ship_environment.wear) {
        return 0.0;
    }
    // A whole drive life's overhaul (goods at base price), spread over its hours.
    const auto bill = query::overhaul_bill(w, design, 1.0, 0.0);
    double cost = bill.labour_cr;
    for (const auto& [commodity_id, units] : bill.spares_units) {
        cost += units * query::get_commodity(w, commodity_id).base_price;
    }
    return cost / query::drive_life_h(w, design);
}

double wear_cr_per_flight_day(const World& w, const domain::ShipClassDefinition& design) {
    // Plasma drives thrust the whole transfer.
    return design.propulsion_type == "variable_isp" ? 24.0 * query::wear_cr_per_drive_hour(w, design) : 0.0;
}

double wear_cr_per_propellant_kg(const World& w, const domain::ShipClassDefinition& design) {
    // Nuclear-thermal cores wear by the hour of burn, that is by the kilogram burned.
    if (design.propulsion_type == "variable_isp" || !w.universe_.ship_environment.wear) {
        return 0.0;
    }
    for (const auto& part : design.components) {
        if (part.kind == "engine" && part.thrust_n > 0.0 && part.isp_s > 0.0) {
            const double mass_flow_kg_s = part.thrust_n / (part.isp_s * 9.80665);
            return query::wear_cr_per_drive_hour(w, design) / (mass_flow_kg_s * 3600.0);
        }
    }
    return 0.0;
}

double ship_r_au(const World& w, const domain::ShipState& ship) {
    if (ship.phase != domain::ShipMissionPhase::InTransit) {
        return query::station_r_au(w, query::get_station_definition(w, ship.current_station_id), w.game_time_s_);
    }
    const auto& mission = ship.active_mission;
    const double r1 = query::station_r_au(w, query::get_station_definition(w, mission.origin_station_id), mission.departure_time_s);
    const double r2 = query::station_r_au(w, query::get_station_definition(w, mission.destination_station_id), mission.arrival_time_s);
    const double span = std::max(1.0, mission.arrival_time_s - mission.departure_time_s);
    const double f = std::clamp((w.game_time_s_ - mission.departure_time_s) / span, 0.0, 1.0);
    return r1 + f * (r2 - r1);
}

double boiloff_kg_per_day(const World& w, const domain::ShipClassDefinition& design, double r_au) {
    return query::leg_boiloff_kg_per_day(w, design, r_au, r_au);
}

double leg_boiloff_kg_per_day(const World& w, const domain::ShipClassDefinition& design, double r1_au, double r2_au) {
    const auto& env = w.universe_.ship_environment;
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

double leg_boiloff_kg_per_day(const World& w, const domain::ShipClassDefinition& design,
    const domain::StationDefinition& from, const domain::StationDefinition& to, double time_s) {
    if (!w.universe_.ship_environment.boiloff || design.propulsion_type == "variable_isp") {
        return 0.0;
    }
    return query::leg_boiloff_kg_per_day(w, design, query::station_r_au(w, from, time_s), query::station_r_au(w, to, time_s));
}

double purchasable_propellant_kg(const World& w, const domain::ShipState& ship) {
    const auto& station = query::get_station_state(w, ship.current_station_id);
    const auto& ship_class = query::ship_design(w, ship);
    // Leave the station its own working reserve (it may consume fuel too) — except for a
    // nearly-dry ship, which may tap the reserve to get unstuck.
    const bool emergency = ship.propellant_kg <= ship_class.propellant_capacity_kg * 0.05;
    const auto& station_rates = w.economy_.get_station_net_rates(query::get_station_definition(w, ship.current_station_id));
    const double station_fuel_rate = station_rates.contains("fuel") ? station_rates.at("fuel") : 0.0;
    const double station_reserve = (station_fuel_rate < 0.0 && !emergency)
        ? std::abs(station_fuel_rate) * 14.0 : 0.0;
    const auto it = station.inventory.find("fuel");
    const double stock = it == station.inventory.end() ? 0.0 : it->second;
    const double for_sale_kg = std::max(0.0, stock - station_reserve) * FUEL_UNITS_TO_KG;
    return std::min(for_sale_kg, std::max(0.0, ship_class.propellant_capacity_kg - ship.propellant_kg));
}

double credit_line(const domain::ShipClassDefinition& ship_class) {
    // Owners lend against the hull: a ship may finance cargo down to -50% of its value.
    return 0.5 * ship_class.ship_value_cr;
}

double persons_aboard(const World& w, const domain::ShipState& ship) {
    double persons = query::ship_design(w, ship).crew_size;
    for (const auto& lot : ship.passengers) {
        persons += lot.count;
    }
    return persons;
}

double provisionable_days(const World& w, const domain::ShipState& ship, const domain::StationState& market) {
    const auto& market_def = query::get_station_definition(w, market.station_id);
    double days = std::numeric_limits<double>::infinity();
    for (const auto& [commodity_id, units_per_crew_day] : w.universe_.ship_operations.life_support_units_per_crew_day) {
        const double need_per_day = query::persons_aboard(w, ship) * units_per_crew_day;
        if (need_per_day <= 0.0) {
            continue;
        }
        const auto carried_it = ship.provisions.find(commodity_id);
        const double carried = carried_it == ship.provisions.end() ? 0.0 : carried_it->second;
        days = std::min(days, (carried + sellable_units(w.economy_, market_def, market, commodity_id, true)) / need_per_day);
    }
    return days;
}

bool drive_life_allows(const World& w, const domain::ShipState& ship, const domain::ShipClassDefinition& design,
    const domain::TrajectoryPlan& plan) {
    if (!w.universe_.ship_environment.wear) {
        return true;
    }
    return query::drive_hours(design, plan) <= (1.0 - ship.engine_wear) * query::drive_life_h(w, design);
}

double provisions_mass_kg(const World& w, const domain::ShipState& ship) {
    double mass_kg = 0.0;
    for (const auto& [commodity_id, units] : ship.provisions) {
        mass_kg += std::max(0.0, units) * query::get_commodity(w, commodity_id).mass_per_unit_kg;
    }
    return mass_kg;
}

}  // namespace spacetrains::simulation::query
