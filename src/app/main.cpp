#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "celestial/CelestialMechanics.hpp"
#include "economy/EconomySystem.hpp"
#include "simulation/Simulation.hpp"
#include "trajectory/TrajectoryAudit.hpp"
#include "trajectory/TrajectoryPlanner.hpp"
#include "trajectory/VariableIspTrajectoryPlanner.hpp"
#include "variable_isp/VariableIsp.hpp"

namespace {

constexpr double kAU = 1.495978707e11;
constexpr double kDayS = 86400.0;

void print_celestial_summary(const spacetrains::domain::UniverseDefinition& universe) {
    std::cout << "\n=== Celestial Mechanics ===\n";
    for (const auto& body : universe.bodies) {
        if (body.orbit.parent_id.empty()) {
            std::cout << std::format("  {:12s}  (root body)\n", body.name);
        } else {
            const double r_au = body.orbit.semi_major_axis_m / kAU;
            const double period_days = body.orbit.orbital_period_s / kDayS;
            std::cout << std::format(
                "  {:12s}  r={:.3f} AU  T={:.1f} days  parent={}\n",
                body.name, r_au, period_days, body.orbit.parent_id);
        }
    }
}

void print_ship_class_summary(const spacetrains::domain::UniverseDefinition& universe) {
    constexpr double kG0 = 9.80665;
    constexpr double kMuSun = 1.32712440018e20;
    const double kappa_scale = std::pow(kAU, 2.5) / std::pow(kMuSun, 1.5);

    std::cout << "\n=== Ship Classes ===\n";
    for (const auto& sc : universe.ship_classes) {
        if (sc.propulsion_type == "variable_isp") {
            const double m_dry = sc.dry_mass_kg;
            const double m0 = m_dry + sc.propellant_capacity_kg;
            const double P = sc.specific_engine_power_w_per_kg * m_dry;
            const double kappa = (m0 > m_dry && P > 0.0)
                ? 2.0 * P * (1.0 / m_dry - 1.0 / m0) * kappa_scale : 0.0;
            const double fuel_frac = sc.propellant_capacity_kg / m0;
            std::cout << std::format(
                "  {:20s}  [variable_isp]     m_dry={:.0f}kg  propellant={:.0f}kg  "
                "alpha={:.0f}W/kg  eps={:.2f}  kappa={:.3f}\n",
                sc.name, m_dry, sc.propellant_capacity_kg,
                sc.specific_engine_power_w_per_kg, fuel_frac, kappa);
        } else {
            const double m_wet = sc.dry_mass_kg + sc.propellant_capacity_kg;
            const double mass_ratio = (sc.dry_mass_kg > 0.0) ? m_wet / sc.dry_mass_kg : 0.0;
            const double ve = (mass_ratio > 1.0 && sc.max_delta_v_mps > 0.0)
                ? sc.max_delta_v_mps / std::log(mass_ratio) : 0.0;
            const double isp_s = ve / kG0;
            const double fuel_frac = (m_wet > 0.0) ? sc.propellant_capacity_kg / m_wet : 0.0;
            std::cout << std::format(
                "  {:20s}  [nuclear_thermal]  m_dry={:.0f}kg  propellant={:.0f}kg  "
                "dv={:.0f}m/s  ISP={:.0f}s  MR={:.2f}  eps={:.2f}\n",
                sc.name, sc.dry_mass_kg, sc.propellant_capacity_kg,
                sc.max_delta_v_mps, isp_s, mass_ratio, fuel_frac);
        }
    }
}

void print_route_diagnosis(
    const spacetrains::domain::UniverseDefinition& universe,
    const spacetrains::celestial::CelestialMechanics& mechanics) {
    constexpr double kG0 = 9.80665;
    constexpr double PI = 3.14159265358979323846;

    // Find sun
    double mu_sun = 0.0;
    for (const auto& body : universe.bodies) {
        if (body.orbit.parent_id.empty()) { mu_sun = body.mu_m3_s2; break; }
    }

    std::cout << "\n=== Interplanetary Route Diagnosis (t=0) ===\n";
    std::cout << std::format("  {:8s}  {:8s}  {:>10s}  {:>10s}  {:>10s}  {:>12s}  {:>12s}\n",
        "From", "To", "Hohm.dv", "Hohm.trn", "Synodic", "Wait(t=0)", "Arr.(t=0)");

    const std::vector<std::pair<std::string,std::string>> routes = {
        {"earth","mars"}, {"earth","venus"}, {"earth","mercury"}, {"earth","ceres"},
        {"mars","earth"}, {"venus","earth"}, {"mars","ceres"}
    };

    for (const auto& [from, to] : routes) {
        const double r1 = mechanics.get_heliocentric_radius(from, 0.0);
        const double r2 = mechanics.get_heliocentric_radius(to, 0.0);
        if (r1 <= 0.0 || r2 <= 0.0 || mu_sun <= 0.0) continue;
        const double a = (r1 + r2) * 0.5;
        const double hohm_time_s = PI * std::sqrt(a * a * a / mu_sun);
        const double v1 = std::sqrt(mu_sun / r1);
        const double v2 = std::sqrt(mu_sun / r2);
        const double vt1 = std::sqrt(mu_sun * (2.0 / r1 - 1.0 / a));
        const double vt2 = std::sqrt(mu_sun * (2.0 / r2 - 1.0 / a));
        const double dv = std::abs(vt1 - v1) + std::abs(v2 - vt2);

        // Find orbital periods for synodic
        double T1 = 0.0, T2 = 0.0;
        for (const auto& body : universe.bodies) {
            if (body.id == from) T1 = body.orbit.orbital_period_s;
            if (body.id == to)   T2 = body.orbit.orbital_period_s;
        }
        double synodic_days = 0.0;
        if (T1 > 0.0 && T2 > 0.0 && std::abs(1.0/T1 - 1.0/T2) > 1e-20) {
            synodic_days = 1.0 / std::abs(1.0/T1 - 1.0/T2) / kDayS;
        }

        // Compute wait for a light_freighter (any nuclear-thermal ship will use same Hohmann)
        double wait_days = 0.0;
        {
            const double TAU = 2.0 * PI;
            const double om1 = (T1 > 0.0) ? TAU / T1 : 0.0;
            const double om2 = (T2 > 0.0) ? TAU / T2 : 0.0;
            const double rel = om2 - om1;
            const double ang1 = std::atan2(mechanics.get_body_position(from, 0.0).z,
                                           mechanics.get_body_position(from, 0.0).x);
            const double ang2 = std::atan2(mechanics.get_body_position(to, 0.0).z,
                                           mechanics.get_body_position(to, 0.0).x);
            const double cur_phase = std::fmod(ang2 - ang1 + TAU * 10, TAU);
            const double req_phase = std::fmod(PI - om2 * hohm_time_s + TAU * 10, TAU);
            if (std::abs(rel) > 1e-12) {
                const double synodic_s = TAU / std::abs(rel);
                double w = std::fmod((req_phase - cur_phase) / rel, synodic_s);
                if (w < 0.0) w += synodic_s;
                wait_days = w / kDayS;
            }
        }

        std::cout << std::format("  {:8s}  {:8s}  {:>10.0f}  {:>10.1f}d  {:>10.1f}d  {:>12.1f}d  {:>12.1f}d\n",
            from, to, dv,
            hohm_time_s / kDayS,
            synodic_days,
            wait_days,
            wait_days + hohm_time_s / kDayS);
    }

    // Per ship class: can it do Earth-Mars? Show propellant fraction needed.
    std::cout << "\n=== NTR Ship Class Feasibility: Earth→Mars Hohmann (dv≈5591 m/s) ===\n";
    const double earth_mars_dv = 5591.0;  // m/s (Hohmann, no 250 fudge)
    for (const auto& sc : universe.ship_classes) {
        if (sc.propulsion_type == "variable_isp") continue;
        const double m_wet = sc.dry_mass_kg + sc.propellant_capacity_kg;
        const double mass_ratio = (sc.dry_mass_kg > 0.0) ? m_wet / sc.dry_mass_kg : 0.0;
        const double ve = (mass_ratio > 1.0 && sc.max_delta_v_mps > 0.0)
            ? sc.max_delta_v_mps / std::log(mass_ratio) : 0.0;
        const double isp_s = ve / kG0;
        if (ve <= 0.0) continue;
        const double prop_needed = m_wet * (1.0 - std::exp(-earth_mars_dv / ve));
        const double frac = prop_needed / sc.propellant_capacity_kg;
        std::cout << std::format(
            "  {:20s}  ISP={:.0f}s  MR={:.2f}  prop_needed={:.0f}kg/{:.0f}kg ({:.0f}%)  {}\n",
            sc.name, isp_s, mass_ratio,
            prop_needed, sc.propellant_capacity_kg, frac * 100.0,
            (frac <= 1.0) ? "FEASIBLE" : "INFEASIBLE");
    }
}

void print_economy_summary(
    const spacetrains::domain::UniverseDefinition& universe,
    const spacetrains::economy::EconomySystem& economy) {
    std::cout << "\n=== Station Economy (net rates, units/day) ===\n";
    for (const auto& station : universe.stations) {
        const auto rates = economy.get_station_net_rates(station);
        std::cout << std::format("  {:30s}  profile={}\n", station.name, station.economy_profile_id);
        for (const auto& [commodity, rate] : rates) {
            if (std::abs(rate) > 0.001) {
                std::cout << std::format("    {:15s}  {:+.2f}/day\n", commodity, rate);
            }
        }
    }
}

void print_station_inventories(
    const spacetrains::domain::SimulationSnapshot& snap,
    const spacetrains::domain::UniverseDefinition& universe) {
    std::cout << "  Stations:\n";
    for (const auto& ss : snap.stations) {
        std::string name = ss.station_id;
        for (const auto& sd : universe.stations) {
            if (sd.id == ss.station_id) { name = sd.name; break; }
        }
        std::cout << std::format("    {:30s}", name);
        for (const auto& [commodity, amount] : ss.inventory) {
            if (amount > 0.1) {
                std::cout << std::format("  {}={:.1f}", commodity, amount);
            }
        }
        std::cout << "\n";
    }
}

void print_economy_audit(
    const spacetrains::domain::SimulationSnapshot& snap,
    const spacetrains::domain::UniverseDefinition& universe,
    const spacetrains::economy::EconomySystem& economy) {

    // Collect all commodity IDs
    std::vector<std::string> commodity_ids;
    for (const auto& c : universe.commodities) {
        commodity_ids.push_back(c.id);
    }

    // Per-commodity system totals
    std::unordered_map<std::string, double> sys_stock;
    std::unordered_map<std::string, double> sys_prod_rate;
    std::unordered_map<std::string, double> sys_cons_rate;
    std::unordered_map<std::string, double> in_transit;

    for (const auto& c : commodity_ids) {
        sys_stock[c] = 0.0;
        sys_prod_rate[c] = 0.0;
        sys_cons_rate[c] = 0.0;
        in_transit[c] = 0.0;
    }

    for (const auto& ss : snap.stations) {
        for (const auto& sd : universe.stations) {
            if (sd.id != ss.station_id) continue;
            const auto rates = economy.get_station_net_rates(sd);
            for (const auto& [c, amount] : ss.inventory) {
                sys_stock[c] += amount;
            }
            for (const auto& [c, rate] : rates) {
                if (rate > 0.0) sys_prod_rate[c] += rate;
                else sys_cons_rate[c] += std::abs(rate);
            }
            break;
        }
    }
    for (const auto& ship : snap.ships) {
        if (!ship.active_mission.commodity_id.empty() && ship.active_mission.cargo_units > 0.0) {
            in_transit[ship.active_mission.commodity_id] += ship.active_mission.cargo_units;
        }
    }

    std::cout << "\n  [ECON AUDIT] Commodity System Balance:\n";
    std::cout << std::format("  {:15s}  {:>10s}  {:>10s}  {:>10s}  {:>10s}  {:>10s}\n",
        "Commodity", "Stock", "In-Transit", "Prod/day", "Cons/day", "Balance/day");
    for (const auto& c : commodity_ids) {
        const double balance = sys_prod_rate[c] - sys_cons_rate[c];
        const char sign = balance >= 0 ? '+' : ' ';
        std::cout << std::format("  {:15s}  {:10.1f}  {:10.1f}  {:10.2f}  {:10.2f}  {}{:.2f}\n",
            c, sys_stock[c], in_transit[c], sys_prod_rate[c], sys_cons_rate[c], sign, balance);
    }

    // Station stress: highlight stations short on a consumed commodity
    std::cout << "\n  [ECON AUDIT] Station Stress (low stock on consumed goods):\n";
    for (const auto& ss : snap.stations) {
        for (const auto& sd : universe.stations) {
            if (sd.id != ss.station_id) continue;
            const auto rates = economy.get_station_net_rates(sd);
            for (const auto& [c, rate] : rates) {
                if (rate >= 0.0) continue;
                const double stock = ss.inventory.count(c) ? ss.inventory.at(c) : 0.0;
                const double days_remaining = (std::abs(rate) > 0.0) ? stock / std::abs(rate) : 999.0;
                if (days_remaining < 30.0) {
                    std::cout << std::format("    {:30s}  {:15s}  stock={:.1f}  rate={:.2f}/day  {:5.1f} days left  {}\n",
                        sd.name, c, stock, rate, days_remaining,
                        days_remaining < 7.0 ? "*** CRITICAL ***" : (days_remaining < 14.0 ? "** LOW **" : "* marginal *"));
                }
            }
            break;
        }
    }

    // Unmet demand over the whole run: the share of what consumers asked for that they went
    // without, weighted by base value so a unit of medicine counts more than a unit of water.
    {
        std::unordered_map<std::string, double> base_prices;
        for (const auto& commodity : universe.commodities) {
            base_prices[commodity.id] = commodity.base_price;
        }
        double demand_value = 0.0;
        double unmet_value = 0.0;
        std::cout << "\n  [ECON AUDIT] Unmet Demand (since start, consumers that went without):\n";
        for (const auto& ss : snap.stations) {
            const auto& name = std::find_if(universe.stations.begin(), universe.stations.end(),
                [&](const auto& sd) { return sd.id == ss.station_id; })->name;
            for (const auto& [c, demand] : ss.demand_units) {
                const double unmet = ss.unmet_units.contains(c) ? ss.unmet_units.at(c) : 0.0;
                demand_value += demand * base_prices[c];
                unmet_value += unmet * base_prices[c];
                if (demand > 0.0 && unmet / demand >= 0.05) {
                    std::cout << std::format("    {:30s}  {:15s}  {:8.0f} of {:8.0f}u unmet  {:5.1f}%\n",
                        name, c, unmet, demand, 100.0 * unmet / demand);
                }
            }
        }
        std::cout << std::format("    Unmet demand: {:.1f}% of {:.0f} cr consumed at base prices ({:.0f} cr unmet)\n",
            demand_value > 0.0 ? 100.0 * unmet_value / demand_value : 0.0, demand_value, unmet_value);
    }

    // Ship utilization
    int ships_with_cargo = 0, ships_repositioning = 0, ships_idle = 0, ships_waiting = 0, ships_stranded = 0, ships_laid_up = 0,
        ships_refitting = 0;
    std::unordered_map<std::string, double> cargo_by_commodity;
    for (const auto& ship : snap.ships) {
        switch (ship.phase) {
            case spacetrains::domain::ShipMissionPhase::InTransit:
                if (ship.active_mission.cargo_units > 0.0) {
                    ++ships_with_cargo;
                    cargo_by_commodity[ship.active_mission.commodity_id] += ship.active_mission.cargo_units;
                } else {
                    ++ships_repositioning;
                }
                break;
            case spacetrains::domain::ShipMissionPhase::AwaitingDeparture:
                ++ships_waiting;
                break;
            case spacetrains::domain::ShipMissionPhase::Idle:
                ++ships_idle;
                break;
            case spacetrains::domain::ShipMissionPhase::Stranded:
                ++ships_stranded;
                break;
            case spacetrains::domain::ShipMissionPhase::Refueling:
                ++ships_idle;
                break;
            case spacetrains::domain::ShipMissionPhase::LaidUp:
                ++ships_laid_up;
                break;
            case spacetrains::domain::ShipMissionPhase::Refitting:
                ++ships_refitting;
                break;
        }
    }
    const int total = static_cast<int>(snap.ships.size());
    std::cout << std::format("\n  [ECON AUDIT] Fleet: {}/{} hauling cargo  {}/{} repositioning  {}/{} waiting  {}/{} idle  {}/{} stranded  {}/{} laid up  {}/{} refitting\n",
        ships_with_cargo, total, ships_repositioning, total, ships_waiting, total, ships_idle, total, ships_stranded, total,
        ships_laid_up, total, ships_refitting, total);
    if (!cargo_by_commodity.empty()) {
        std::cout << "             Active cargo: ";
        for (const auto& [c, units] : cargo_by_commodity) {
            std::cout << std::format("{}={:.0f}u  ", c, units);
        }
        std::cout << "\n";
    }

    // Money: per-entity balances and total supply. Stations, ships and the external
    // account must add up to the seeded amount exactly: every payment is a transfer.
    double initial_supply = 0.0;
    for (const auto& sd : universe.stations) {
        initial_supply += sd.initial_credits;
    }
    for (const auto& seed : universe.ship_seeds) {
        initial_supply += seed.initial_credits;
    }
    double station_credits = 0.0;
    double ship_credits = 0.0;
    std::cout << "\n  [ECON AUDIT] Money:\n";
    // A station's stock is worth the price curve integrated from zero up to it; trades along
    // the curve move money and stock value in step, so this shows where money went into goods.
    const auto stock_value = [&](const spacetrains::domain::StationDefinition& sd,
                                 const spacetrains::domain::Inventory& inventory) {
        double value = 0.0;
        for (const auto& commodity : universe.commodities) {
            const auto it = inventory.find(commodity.id);
            const double stock = it == inventory.end() ? 0.0 : std::max(0.0, it->second);
            value += economy.get_trade_value(sd, commodity.id, 0.0, stock, commodity.base_price);
        }
        return value;
    };
    std::cout << std::format("    {:30s}  {:>12s}     {:>9s} {:>9s} {:>8s} {:>8s} {:>8s} {:>9s}\n",
        "", "", "household", "producer", "dividend", "subsidy", "tax", "Δstock");
    spacetrains::domain::StationLedger stations_total;
    for (const auto& ss : snap.stations) {
        station_credits += ss.credits;
        const auto& l = ss.ledger;
        stations_total.household_sales += l.household_sales;
        stations_total.producer_purchases += l.producer_purchases;
        stations_total.dividends += l.dividends;
        stations_total.subsidies += l.subsidies;
        stations_total.taxes += l.taxes;
        for (const auto& sd : universe.stations) {
            if (sd.id != ss.station_id) continue;
            std::cout << std::format("    {:30s}  {:>12.0f} cr  {:9.0f} {:9.0f} {:8.0f} {:8.0f} {:8.0f} {:+9.0f}\n",
                sd.name, ss.credits, l.household_sales, l.producer_purchases, l.dividends, l.subsidies, l.taxes,
                stock_value(sd, ss.inventory) - stock_value(sd, sd.initial_inventory));
            break;
        }
    }
    std::cout << std::format(
        "    Stations: residents paid {:.0f}, producers were paid {:.0f}, dividends {:.0f}, subsidies {:.0f}, taxes {:.0f}\n",
        stations_total.household_sales, stations_total.producer_purchases, stations_total.dividends,
        stations_total.subsidies, stations_total.taxes);
    spacetrains::domain::ShipLedger fleet;
    int profitable = 0;
    std::cout << std::format("    {:30s}  {:>12s}     {:>8s} {:>8s} {:>7s} {:>7s} {:>7s} {:>6s} {:>6s} {:>7s}  {}\n",
        "", "", "profit", "margin", "fuel", "wages", "capital", "prov", "refit", "divid", "class");
    // Sold ships keep their lifetime ledger; their cash went to the home station.
    std::vector<std::pair<const spacetrains::domain::ShipState*, bool>> all_ships;
    for (const auto& ship : snap.ships) {
        all_ships.emplace_back(&ship, false);
    }
    for (const auto& ship : snap.sold_ships) {
        all_ships.emplace_back(&ship, true);
    }
    for (const auto& [ship_ptr, sold] : all_ships) {
        const auto& ship = *ship_ptr;
        ship_credits += ship.credits;
        const auto& l = ship.ledger;
        std::cout << std::format("    {:30s}  {:>12.0f} cr  {:+8.0f} {:8.0f} {:7.0f} {:7.0f} {:7.0f} {:6.0f} {:6.0f} {:7.0f}  {}{}{}\n",
            ship.name, ship.credits, ship.lifetime_profit, l.cargo_revenue - l.cargo_purchases,
            l.fuel, l.wages, l.capital, l.provisions, l.refits, l.dividends, ship.class_id,
            ship.commissioned_s > 0.0
                ? std::format("  (new day {:.0f}, {} -> {} until day {:.0f})", ship.commissioned_s / 86400.0,
                      ship.route_commodity_id, ship.route_destination_id, ship.route_until_s / 86400.0)
                : "",
            sold ? "  (sold)" : "");
        fleet.cargo_revenue += l.cargo_revenue;
        fleet.cargo_purchases += l.cargo_purchases;
        fleet.fuel += l.fuel;
        fleet.wages += l.wages;
        fleet.capital += l.capital;
        fleet.provisions += l.provisions;
        fleet.refits += l.refits;
        fleet.dividends += l.dividends;
        profitable += ship.lifetime_profit > 0.0 ? 1 : 0;
    }
    std::cout << std::format(
        "    Fleet: cargo margin {:.0f} (sold {:.0f}, bought {:.0f})  fuel {:.0f}  wages {:.0f}  capital {:.0f}"
        "  provisions {:.0f}  refits {:.0f}  dividends {:.0f}  -> {}/{} ships profitable\n",
        fleet.cargo_revenue - fleet.cargo_purchases, fleet.cargo_revenue, fleet.cargo_purchases, fleet.fuel,
        fleet.wages, fleet.capital, fleet.provisions, fleet.refits, fleet.dividends, profitable, all_ships.size());
    const double internal_supply = station_credits + ship_credits;
    double treasuries = 0.0;
    std::cout << "    Faction treasuries:";
    for (const auto& faction : universe.factions) {
        const auto it = snap.faction_treasuries.find(faction.id);
        const double balance = it == snap.faction_treasuries.end() ? 0.0 : it->second;
        treasuries += balance;
        std::cout << std::format("  {} {:.0f}", faction.name, balance);
    }
    std::cout << " cr\n";
    const auto& investment = snap.fleet_investment;
    std::cout << std::format("    Fleet investment: {} ships commissioned (hulls {:.0f}, working capital {:.0f}), {} sold (salvage {:.0f})\n",
        investment.ships_commissioned, investment.hulls_bought, investment.working_capital,
        investment.ships_sold, investment.salvage);
    const double external = snap.outside_economy_credits + treasuries;
    const double total_supply = internal_supply + external;
    // The target is the seeded money plus the working capital the treasuries gave new ships.
    const double target = snap.money_supply_target;
    std::cout << std::format("    Money supply: stations {:.0f} + ships {:.0f} = {:.0f} cr  (target {:.0f}, {:+.1f}%)\n",
        station_credits, ship_credits, internal_supply, target,
        target != 0.0 ? 100.0 * (internal_supply / target - 1.0) : 0.0);
    std::cout << std::format("    External: residents and producers {:.0f} + treasuries {:.0f} cr.  Total {:.2f} cr  (initial {:.2f}, drift {:+.4f})\n",
        snap.outside_economy_credits, treasuries, total_supply, initial_supply, total_supply - initial_supply);

    // Per-commodity price spread across stations.
    std::cout << "\n  [ECON AUDIT] Prices (min/avg/max across stations):\n";
    for (const auto& commodity : universe.commodities) {
        double min_price = 1.0e18;
        double max_price = 0.0;
        double sum_price = 0.0;
        int count = 0;
        for (const auto& ss : snap.stations) {
            for (const auto& sd : universe.stations) {
                if (sd.id != ss.station_id) continue;
                const double stock = ss.inventory.count(commodity.id) ? ss.inventory.at(commodity.id) : 0.0;
                const double price = economy.get_price(sd, commodity.id, stock, commodity.base_price);
                min_price = std::min(min_price, price);
                max_price = std::max(max_price, price);
                sum_price += price;
                ++count;
                break;
            }
        }
        if (count > 0) {
            std::cout << std::format("    {:15s}  base={:>6.1f}  min={:>7.1f}  avg={:>7.1f}  max={:>7.1f}\n",
                commodity.id, commodity.base_price, min_price, sum_price / count, max_price);
        }
    }
}

void print_ship_phases(
    const spacetrains::domain::SimulationSnapshot& snap,
    const spacetrains::domain::UniverseDefinition& universe) {
    int idle = 0, awaiting = 0, transit = 0, stranded = 0, laid_up = 0;
    std::cout << "  Ships:\n";
    for (const auto& ship : snap.ships) {
        std::string class_name = ship.class_id;
        std::string propulsion;
        for (const auto& sc : universe.ship_classes) {
            if (sc.id == ship.class_id) { class_name = sc.name; propulsion = sc.propulsion_type; break; }
        }
        const char* phase_str = "?";
        switch (ship.phase) {
            case spacetrains::domain::ShipMissionPhase::Idle:            phase_str = "idle"; ++idle; break;
            case spacetrains::domain::ShipMissionPhase::AwaitingDeparture: phase_str = "awaiting"; ++awaiting; break;
            case spacetrains::domain::ShipMissionPhase::InTransit:       phase_str = "in_transit"; ++transit; break;
            case spacetrains::domain::ShipMissionPhase::Stranded:        phase_str = "stranded"; ++stranded; break;
            case spacetrains::domain::ShipMissionPhase::LaidUp:          phase_str = "laid_up"; ++laid_up; break;
            case spacetrains::domain::ShipMissionPhase::Refueling:       phase_str = "refueling"; break;
            case spacetrains::domain::ShipMissionPhase::Refitting:       phase_str = "refitting"; break;
        }
        std::cout << std::format(
            "    {:20s}  [{:12s}]  {:10s}  fuel={:.0f}kg",
            ship.name, propulsion, phase_str, ship.propellant_kg);
        if (ship.phase == spacetrains::domain::ShipMissionPhase::InTransit
            || ship.phase == spacetrains::domain::ShipMissionPhase::AwaitingDeparture) {
            std::cout << std::format("  -> {}  arr=day{:.1f}",
                ship.active_mission.destination_station_id,
                ship.active_mission.arrival_time_s / kDayS);
        }
        std::cout << "\n";
    }
    std::cout << std::format(
        "  Phase summary: idle={} awaiting={} in_transit={} stranded={} laid_up={}\n",
        idle, awaiting, transit, stranded, laid_up);
}

constexpr double kAuM = 1.495978707e11;

std::string join_flags(const std::vector<std::string>& flags) {
    std::string out;
    for (const auto& flag : flags) {
        if (!out.empty()) out += ",";
        out += flag;
    }
    return out.empty() ? "ok" : out;
}

void print_trajectory_record(const spacetrains::trajectory::TrajectoryAuditRecord& r) {
    const auto& d = r.diagnostics;
    const auto& m = r.metrics;
    std::cout << std::format(
        "[traj day {:7.1f}] {} ({}) {} -> {}  {}",
        r.planned_at_s / kDayS, r.ship_name, r.class_id,
        r.origin_station_id, r.destination_station_id, r.trajectory_type);
    if (r.trajectory_type == "variable_isp") {
        std::cout << std::format(
            "  seed={} iters={} windows={} rho={:.3f} kappa={:.3f} theta={:.3f}->{:.3f} rEnd/rho={:.3f}",
            d.seed_source, d.refine_iterations, d.windows_tried, d.rho, d.kappa, d.theta_target_rad,
            d.theta_actual_rad, d.r_end_canonical_ratio);
    }
    std::cout << std::format(
        "\n      miss={:.4f}AU start_miss={:.4f}AU rev={:.2f} max_step={:.1f}deg end_turn={:.1f}deg"
        " max_turn={:.1f}deg last_seg={:.1f}x wait_rev={:.2f} wait_step={:.1f}deg r=[{:.3f},{:.3f}]AU fuel={:.0f}kg wait={:.1f}d coast={:.1f}d samples={} plan={:.0f}ms\n"
        "      flags={}\n",
        d.endpoint_miss_m / kAuM, d.start_miss_m / kAuM, m.revolutions, m.max_step_deg, m.end_turn_deg,
        m.max_interior_turn_deg, m.last_segment_ratio, m.wait_revolutions, m.wait_max_step_deg, m.min_radius_m / kAuM, m.max_radius_m / kAuM,
        r.planning_propellant_kg, r.wait_time_s / kDayS, r.coast_time_s / kDayS, m.transfer_sample_count, r.plan_ms,
        join_flags(m.flags));
}

void print_trajectory_audit_summary(const std::vector<spacetrains::trajectory::TrajectoryAuditRecord>& records) {
    std::cout << "\n=== Trajectory Audit Summary ===\n";
    std::map<std::string, std::vector<const spacetrains::trajectory::TrajectoryAuditRecord*>> by_type;
    for (const auto& r : records) {
        std::string key = r.trajectory_type;
        if (key == "variable_isp") key += "/" + r.diagnostics.seed_source;
        by_type[key].push_back(&r);
    }
    for (const auto& [type, group] : by_type) {
        std::vector<double> misses;
        double max_rev = 0.0;
        std::size_t flagged = 0;
        std::map<std::string, int> flag_counts;
        for (const auto* r : group) {
            misses.push_back(r->diagnostics.endpoint_miss_m / kAuM);
            max_rev = std::max(max_rev, r->metrics.revolutions);
            if (!r->metrics.flags.empty()) ++flagged;
            for (const auto& f : r->metrics.flags) ++flag_counts[f];
        }
        std::vector<double> plan_ms;
        for (const auto* r : group) plan_ms.push_back(r->plan_ms);
        std::sort(plan_ms.begin(), plan_ms.end());
        std::sort(misses.begin(), misses.end());
        const auto pct = [&](double q) { return misses[static_cast<std::size_t>(q * (misses.size() - 1))]; };
        std::cout << std::format(
            "  {:28s} plans={:5d} flagged={:5d}  miss AU p50={:.4f} p95={:.4f} max={:.4f}  max_rev={:.2f}"
            "  plan ms p50={:.1f} p99={:.1f} max={:.0f}\n",
            type, group.size(), flagged, pct(0.5), pct(0.95), misses.back(), max_rev,
            plan_ms[plan_ms.size() / 2], plan_ms[static_cast<std::size_t>(0.99 * (plan_ms.size() - 1))], plan_ms.back());
        for (const auto& [flag, count] : flag_counts) {
            std::cout << std::format("      {:20s} {}\n", flag, count);
        }
    }

    // Worst cases per planner family, so one family's outliers don't hide another's.
    const auto print_worst = [&](const char* title, auto key) {
        for (const char* family : {"keplerian", "variable_isp"}) {
            std::vector<const spacetrains::trajectory::TrajectoryAuditRecord*> sorted;
            for (const auto& r : records) {
                if (r.trajectory_type.starts_with(family)) sorted.push_back(&r);
            }
            std::sort(sorted.begin(), sorted.end(), [&](auto* a, auto* b) { return key(*a) > key(*b); });
            std::cout << std::format("\n  Worst {} by {}:\n", family, title);
            for (std::size_t i = 0; i < std::min<std::size_t>(5, sorted.size()); ++i) {
                print_trajectory_record(*sorted[i]);
            }
        }
    };
    print_worst("endpoint miss", [](const auto& r) { return r.diagnostics.endpoint_miss_m; });
    print_worst("revolutions", [](const auto& r) { return r.metrics.revolutions; });
    print_worst("end turn", [](const auto& r) { return r.metrics.end_turn_deg; });
    print_worst("wait revolutions", [](const auto& r) { return r.metrics.wait_revolutions; });
    print_worst("planning time", [](const auto& r) { return r.plan_ms; });
}

void dump_flagged_trajectories(
    const std::vector<spacetrains::trajectory::TrajectoryAuditRecord>& records,
    const std::string& path) {
    std::ofstream out(path);
    out << "record,planned_day,ship,type,seed,flags,sample,t_day,x_au,z_au\n";
    std::size_t dumped = 0;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        if (r.metrics.flags.empty()) continue;
        ++dumped;
        auto flags_field = join_flags(r.metrics.flags);
        std::replace(flags_field.begin(), flags_field.end(), ',', '|');
        for (std::size_t k = 0; k < r.sampled_path.size(); ++k) {
            const double t = k < r.sampled_times_s.size() ? r.sampled_times_s[k] : 0.0;
            out << std::format("{},{:.2f},{},{},{},{},{},{:.3f},{:.6f},{:.6f}\n",
                i, r.planned_at_s / kDayS, r.ship_name, r.trajectory_type, r.diagnostics.seed_source,
                flags_field, k, t / kDayS, r.sampled_path[k].x / kAuM, r.sampled_path[k].z / kAuM);
        }
    }
    std::cout << std::format("Dumped {} flagged trajectories to {}\n", dumped, path);
}

// Plan every interplanetary transfer over a grid of station pairs, fuel levels and
// departure days, bypassing the economy, so rare atlas regions get exercised.
std::vector<spacetrains::trajectory::TrajectoryAuditRecord> run_trajectory_sweep(
    const std::filesystem::path& data_root,
    const spacetrains::domain::UniverseDefinition& universe,
    const spacetrains::celestial::CelestialMechanics& mechanics,
    int sweep_days,
    int sweep_step_days) {
    spacetrains::variable_isp::VariableIspAtlas atlas;
    atlas.load_binary((data_root.parent_path() / "tests" / "data" / "variable_isp" / "variable_isp_atlas.bin").string());
    spacetrains::trajectory::VariableIspTrajectoryPlanner ion_planner(universe, mechanics, atlas);
    spacetrains::trajectory::KeplerTrajectoryPlanner kepler_planner(universe, mechanics);

    const std::vector<double> fuel_fractions {0.03, 0.06, 0.1, 0.2, 0.35, 0.5, 0.75, 1.0};
    struct SweepJob {
        const spacetrains::domain::ShipClassDefinition* ship_class;
        const spacetrains::domain::StationDefinition* origin;
        const spacetrains::domain::StationDefinition* destination;
        double fuel_fraction;
    };
    std::vector<SweepJob> jobs;
    for (const auto& ship_class : universe.ship_classes) {
        for (const auto& origin : universe.stations) {
            for (const auto& destination : universe.stations) {
                if (origin.parent_body_id == destination.parent_body_id) continue;
                for (const double fraction : fuel_fractions) {
                    jobs.push_back({&ship_class, &origin, &destination, fraction});
                }
            }
        }
    }

    // Planners are const and stateless, so jobs run on a simple worker pool;
    // results are merged in job order to keep the output deterministic.
    std::vector<std::vector<spacetrains::trajectory::TrajectoryAuditRecord>> job_records(jobs.size());
    std::atomic<std::size_t> next_job {0};
    std::atomic<std::size_t> attempted {0};
    std::atomic<std::size_t> integration_failures {0};
    // Watchdog: each worker publishes its current job and day; plans that run
    // for more than 10 s are reported (planner hangs are otherwise invisible).
    const unsigned thread_count = std::max(1u, std::thread::hardware_concurrency());
    struct InFlight {
        std::atomic<std::size_t> job {SIZE_MAX};
        std::atomic<int> day {0};
        std::atomic<std::int64_t> started_ms {0};
    };
    std::vector<InFlight> in_flight(thread_count);
    std::atomic<unsigned> next_slot {0};
    const auto now_ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    const auto worker = [&] {
        auto& slot = in_flight[next_slot++];
        for (std::size_t j = next_job++; j < jobs.size(); j = next_job++) {
            const auto& [ship_class, origin, destination, fraction] = jobs[j];
            const auto& planner = ship_class->propulsion_type == "variable_isp"
                ? static_cast<const spacetrains::trajectory::ITrajectoryPlanner&>(ion_planner)
                : static_cast<const spacetrains::trajectory::ITrajectoryPlanner&>(kepler_planner);
            spacetrains::domain::ShipState ship;
            ship.name = ship_class->id;
            ship.class_id = ship_class->id;
            ship.propellant_kg = fraction * ship_class->propellant_capacity_kg;
            for (int day = 0; day < sweep_days; day += sweep_step_days) {
                const double t = day * kDayS;
                ++attempted;
                slot.job = j;
                slot.day = day;
                slot.started_ms = now_ms();
                const auto started = std::chrono::steady_clock::now();
                const auto plan = planner.plan_transfer(*origin, *destination, ship, *ship_class, t);
                const double plan_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                if (plan_ms > 1000.0) {
                    std::cout << std::format("[slow plan {:.0f} ms] {} {} -> {} fuel={:.0f}% day={} feasible={} {}\n",
                        plan_ms, ship_class->id, origin->id, destination->id, fraction * 100.0, day, plan.feasible, plan.summary);
                }
                slot.job = SIZE_MAX;
                if (plan.summary.find("integration failed") != std::string::npos) {
                    if (integration_failures++ < 20) {
                        std::cout << std::format("[integration failure] {} fuel={:.0f}% day={} ({:.0f} ms): {}\n",
                            ship_class->id, fraction * 100.0, day, plan_ms, plan.summary);
                    }
                }
                if (!plan.feasible) continue;
                const double r_origin = mechanics.get_heliocentric_radius(origin->parent_body_id, plan.departure_time_s);
                const double r_dest = mechanics.get_heliocentric_radius(destination->parent_body_id, plan.arrival_time_s);
                auto metrics = spacetrains::trajectory::audit_trajectory(plan);
                const bool flagged = !metrics.flags.empty();
                job_records[j].push_back({
                    .planned_at_s = t,
                    .ship_name = std::format("{}@{:.0f}%", ship_class->id, fraction * 100.0),
                    .class_id = ship_class->id,
                    .origin_station_id = origin->id,
                    .destination_station_id = destination->id,
                    .trajectory_type = plan.trajectory_type,
                    .planning_propellant_kg = ship.propellant_kg,
                    .wait_time_s = plan.wait_time_s,
                    .coast_time_s = plan.coast_time_s,
                    .r_origin_m = r_origin,
                    .r_dest_m = r_dest,
                    .plan_ms = plan_ms,
                    .diagnostics = plan.diagnostics,
                    .metrics = std::move(metrics),
                    .sampled_path = flagged ? plan.sampled_path : std::vector<spacetrains::math::Vec3d>{},
                    .sampled_times_s = flagged ? plan.sampled_times_s : std::vector<double>{},
                });
            }
        }
    };
    {
        std::vector<std::jthread> threads;
        for (unsigned i = 0; i < thread_count; ++i) {
            threads.emplace_back(worker);
        }
        std::jthread watchdog([&](std::stop_token stop) {
            std::vector<std::pair<std::size_t, int>> reported;
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                for (auto& f : in_flight) {
                    const std::size_t j = f.job;
                    const int day = f.day;
                    if (j == SIZE_MAX || now_ms() - f.started_ms < 10000) continue;
                    if (std::find(reported.begin(), reported.end(), std::pair{j, day}) != reported.end()) continue;
                    reported.emplace_back(j, day);
                    const auto& job = jobs[j];
                    std::cout << std::format("[hung plan >10s] {} {} -> {} fuel={:.0f}% day={}\n",
                        job.ship_class->id, job.origin->id, job.destination->id, job.fuel_fraction * 100.0, day)
                              << std::flush;
                }
            }
        });
        for (auto& t : threads) t.join();
    }
    std::vector<spacetrains::trajectory::TrajectoryAuditRecord> records;
    for (auto& group : job_records) {
        std::move(group.begin(), group.end(), std::back_inserter(records));
    }
    std::cout << std::format("Sweep: {} plans attempted, {} feasible, {} integration failures\n",
        attempted.load(), records.size(), integration_failures.load());
    return records;
}

}  // namespace

int main(int argc, char** argv) {
    std::string data_root_str;
    int sim_days = 365;
    bool verbose = false;
    bool econ_audit = false;
    int report_interval_days = 30;
    bool trajectory_audit = false;
    std::string trajectory_dump_path;
    int sweep_step_days = 0;

    // Parse arguments
    std::vector<std::string> args(argv + 1, argv + argc);
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--days" && i + 1 < args.size()) {
            sim_days = std::stoi(args[++i]);
        } else if (args[i] == "--report-interval" && i + 1 < args.size()) {
            report_interval_days = std::stoi(args[++i]);
        } else if (args[i] == "--verbose" || args[i] == "-v") {
            verbose = true;
        } else if (args[i] == "--econ-audit") {
            econ_audit = true;
        } else if (args[i] == "--trajectory-audit") {
            trajectory_audit = true;
        } else if (args[i] == "--trajectory-sweep" && i + 1 < args.size()) {
            sweep_step_days = std::stoi(args[++i]);
        } else if (args[i] == "--trajectory-dump" && i + 1 < args.size()) {
            trajectory_audit = true;
            trajectory_dump_path = args[++i];
        } else if (args[i][0] != '-') {
            data_root_str = args[i];
        }
    }
    if (data_root_str.empty()) {
        data_root_str = std::filesystem::current_path().string();
    }
    const std::filesystem::path data_root = std::filesystem::path(data_root_str) / "data";

    std::cout << "SpaceTrains Headless Simulation\n";
    std::cout << std::format("Data root: {}\n", data_root.string());
    std::cout << std::format("Simulating {} days, report every {} days\n", sim_days, report_interval_days);

    auto sim = spacetrains::simulation::Simulation::from_data_root(data_root.string());
    sim.set_trajectory_audit_enabled(trajectory_audit);
    if (sweep_step_days > 0) {
        spacetrains::celestial::CelestialMechanics sweep_mechanics(sim.universe());
        const auto records = run_trajectory_sweep(data_root, sim.universe(), sweep_mechanics, sim_days, sweep_step_days);
        print_trajectory_audit_summary(records);
        if (!trajectory_dump_path.empty()) {
            dump_flagged_trajectories(records, trajectory_dump_path);
        }
        return 0;
    }
    std::size_t audit_records_printed = 0;
    spacetrains::celestial::CelestialMechanics mechanics(sim.universe());

    // Startup summaries
    print_celestial_summary(sim.universe());
    print_ship_class_summary(sim.universe());
    print_economy_summary(sim.universe(), sim.economy_system());
    print_route_diagnosis(sim.universe(), mechanics);

    std::cout << "\n=== Running Simulation ===\n";

    // Timewarp: each step() call advances one real second × timewarp.
    // Use 1-day steps for legible reports.
    sim.set_timewarp(kDayS);  // 1 real second = 1 simulated day per step

    double last_report_day = 0.0;
    int total_events = 0;

    for (int step = 0; step < sim_days; ++step) {
        sim.step(1.0);  // advance 1 simulated day

        const double game_day = sim.snapshot().game_time_s / kDayS;

        if (trajectory_audit) {
            const auto& records = sim.trajectory_audit_records();
            for (; audit_records_printed < records.size(); ++audit_records_printed) {
                if (!records[audit_records_printed].metrics.flags.empty()) {
                    print_trajectory_record(records[audit_records_printed]);
                }
            }
        }

        if (verbose) {
            for (const auto& event : sim.snapshot().recent_events) {
                // Only print events newer than the last step
                if (event.time_s > (step * kDayS) && event.time_s <= ((step + 1) * kDayS)) {
                    std::cout << std::format("[day {:7.1f}] {}\n", event.time_s / kDayS, event.text);
                    ++total_events;
                }
            }
        }

        if (game_day - last_report_day >= report_interval_days) {
            last_report_day = game_day;
            const auto snap = sim.snapshot();
            std::cout << std::format("\n--- Day {:.1f} ---\n", game_day);
            print_station_inventories(snap, sim.universe());
            print_ship_phases(snap, sim.universe());
            if (econ_audit) {
                print_economy_audit(snap, sim.universe(), sim.economy_system());
            }
            if (!verbose) {
                std::cout << "  Recent events:\n";
                for (const auto& event : snap.recent_events) {
                    std::cout << std::format("    [day {:7.1f}] {}\n", event.time_s / kDayS, event.text);
                }
            }
        }
    }

    std::cout << "\n=== Final Report ===\n";
    std::cout << sim.build_report();
    if (trajectory_audit) {
        print_trajectory_audit_summary(sim.trajectory_audit_records());
        if (!trajectory_dump_path.empty()) {
            dump_flagged_trajectories(sim.trajectory_audit_records(), trajectory_dump_path);
        }
    }
    return 0;
}
