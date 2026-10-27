// Summaries and audits printed by the headless run.

#include "app/Reports.hpp"

namespace spacetrains::app {

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
        const auto& rates = economy.get_station_net_rates(station);
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

// Passengers (docs/modules/ships_and_fleet.md): who travelled, what they paid, who is still waiting.
void print_passenger_audit(const spacetrains::domain::SimulationSnapshot& snap,
    const spacetrains::domain::UniverseDefinition& universe,
    const spacetrains::simulation::Simulation::PassengerAudit& audit,
    const std::map<std::string, spacetrains::simulation::Simulation::RotationTour>& tours) {
    if (universe.passengers.enabled == 0.0) {
        return;
    }
    if (!tours.empty()) {
        std::string line;
        for (const auto& [station_id, tour] : tours) {
            line += std::format("{}{} {:.0f} d ({:.2f} cr/day)", line.empty() ? "" : ", ", station_id, tour.tour_days,
                tour.fares_per_day);
        }
        std::cout << "    Rotation tours (fares per position-day): " << line << "\n";
    }
    using spacetrains::simulation::Simulation;
    std::array<double, 6> waiting {};
    for (const auto& station : snap.stations) {
        for (const auto& group : station.waiting) {
            waiting[static_cast<std::size_t>(group.kind)] += group.count;
        }
    }
    std::array<double, 6> aboard {};
    double fares = 0.0;
    for (const auto& ship : snap.ships) {
        fares += ship.ledger.fares;
        for (const auto& lot : ship.passengers) {
            aboard[static_cast<std::size_t>(lot.kind)] += lot.count;
        }
    }
    double flown = 0.0;
    for (const double days : audit.flight_days) {
        flown += days;
    }
    std::cout << std::format("    Passengers: {:.0f} cr in fares, {} departures with passengers, load factor {:.0f}%\n",
        fares, audit.departures_with_passengers, audit.berth_days > 0.0 ? 100.0 * flown / audit.berth_days : 0.0);
    for (std::size_t k = 0; k < waiting.size(); ++k) {
        if (audit.posted[k] <= 0.0 && waiting[k] <= 0.0 && aboard[k] <= 0.0) {
            continue;
        }
        const double boarded = audit.boarded[k];
        std::cout << std::format("      {:14s} posted {:.0f}, boarded {:.0f} (mean wait {:.0f} d, fare {:.0f} cr), landed {:.0f}, "
                                 "gave up {:.0f}, waiting {:.0f}, aboard {:.0f}\n",
            Simulation::passenger_kind_name(static_cast<spacetrains::domain::PassengerKind>(k)), audit.posted[k], boarded,
            boarded > 0.0 ? audit.wait_days[k] / boarded : 0.0, boarded > 0.0 ? audit.fares_cr[k] / boarded : 0.0,
            audit.landed[k], audit.gave_up[k], waiting[k], aboard[k]);
    }
    // Passages stations paid (rotation, sponsored settlers) and recovered through their prices.
    double paid = 0.0;
    double recovered = 0.0;
    double owed = 0.0;
    double household = 0.0;
    for (const auto& station : snap.stations) {
        paid += station.ledger.passages_paid;
        recovered += station.ledger.passages_recovered;
        owed += station.passage_debt_cr;
        household += station.ledger.household_sales;
    }
    if (paid > 0.0) {
        std::cout << std::format("    Passages paid by stations: {:.0f} cr, recovered {:.0f} cr through prices "
                                 "(+{:.1f}% on residents' goods), {:.0f} cr still to recover\n",
            paid, recovered, household > 0.0 ? 100.0 * recovered / household : 0.0, owed);
    }
    if (audit.colony_recruits_landed > 0.0) {
        std::cout << std::format("    Of the settlers landed, {:.0f} were recruited by a colony and went down to its surface\n",
            audit.colony_recruits_landed);
    }
    // Ships with berths, by hull and fit.
    std::map<std::string, int> carriers;
    for (const auto& ship : snap.ships) {
        const spacetrains::domain::ShipClassDefinition* ship_class = nullptr;
        for (const auto& candidate : universe.ship_classes) {
            if (candidate.id == ship.class_id) {
                ship_class = &candidate;
            }
        }
        const bool liner = ship_class != nullptr && ship_class->berths > 0.0;
        bool cabin = false;
        std::string fit;
        for (const auto& module : ship.fit) {
            cabin = cabin || module == "cabin" || module == "shielded_cabin";
            if (module == "cabin" || module == "shielded_cabin" || module == "cabin_water_wall" || module == "tank_wrap") {
                fit += "+" + module;
            }
        }
        if (liner || cabin) {
            ++carriers[(ship_class != nullptr ? ship_class->hull_id : ship.class_id) + fit];
        }
    }
    if (!carriers.empty()) {
        std::string line;
        for (const auto& [name, count] : carriers) {
            line += std::format("{}{} {}", line.empty() ? "" : ", ", name, count);
        }
        std::cout << "    Passenger carriers: " << line << "\n";
    }
    // By destination station (all kinds and origins), then every route to or from Titan.
    struct Totals {
        double posted {0.0};
        double boarded {0.0};
        double landed {0.0};
        double gave_up {0.0};
    };
    std::map<std::string, Totals> to;
    std::map<std::string, Totals> from;
    for (const auto& [key, route] : audit.routes) {
        for (auto* totals : {&to[key.second], &from[key.first]}) {
            totals->posted += route.posted;
            totals->boarded += route.boarded;
            totals->landed += route.landed;
            totals->gave_up += route.gave_up;
        }
    }
    std::cout << "    Passengers by station (to it: posted/boarded/landed/gave up | from it: posted/boarded/landed):\n";
    for (const auto& [station_id, t] : to) {
        const auto& f = from[station_id];
        std::cout << std::format("      {:18s} to {:6.0f} {:6.0f} {:6.0f} {:6.0f}   from {:6.0f} {:6.0f} {:6.0f}\n", station_id,
            t.posted, t.boarded, t.landed, t.gave_up, f.posted, f.boarded, f.landed);
    }
    // The routes where most people gave up, with what flew them.
    std::vector<std::pair<std::pair<std::string, std::string>, const spacetrains::simulation::Simulation::PassengerAudit::Route*>> worst;
    for (const auto& [key, route] : audit.routes) {
        worst.emplace_back(key, &route);
    }
    std::ranges::sort(worst, [](const auto& a, const auto& b) { return a.second->gave_up > b.second->gave_up; });
    std::cout << "    Routes by people who gave up (posted/boarded/gave up | legs flown, by ships with berths, "
                 "of those left empty while people waited):\n";
    {
        int legs = 0, berth_legs = 0, berth_legs_queue = 0;
        double berth_boarded = 0.0;
        for (const auto& [key, route] : audit.routes) {
            legs += route.departures;
            berth_legs += route.berth_departures;
            if (route.posted > 0.0) {
                berth_legs_queue += route.berth_departures;
            }
            berth_boarded += route.boarded;
        }
        std::cout << std::format("    Legs flown: {}, by ships with berths {} ({} on routes where people post trips), {:.0f} boarded\n",
            legs, berth_legs, berth_legs_queue, berth_boarded);
    }
    for (std::size_t i = 0; i < std::min<std::size_t>(10, worst.size()); ++i) {
        const auto& [key, route] = worst[i];
        std::cout << std::format("      {:>15s} -> {:15s} {:6.0f} {:6.0f} {:6.0f} | {:5d} {:5d} {:5d}\n", key.first, key.second,
            route->posted, route->boarded, route->gave_up, route->departures, route->berth_departures, route->empty_with_waiting);
    }
    for (const auto& [key, route] : audit.routes) {
        if (key.first != "titan_works" && key.second != "titan_works") {
            continue;
        }
        std::string carriers;
        for (const auto& [design, count] : route.carriers) {
            carriers += std::format("{}{} x{}", carriers.empty() ? "" : ", ", design, count);
        }
        std::cout << std::format("    Titan route {} -> {}: posted {:.0f}, boarded {:.0f}, landed {:.0f}, gave up {:.0f}, "
                                 "mean trip {:.0f} d, max leg dose {:.0f} mSv{}{}\n",
            key.first, key.second, route.posted, route.boarded, route.landed, route.gave_up,
            route.landed > 0.0 ? route.flight_days / route.landed : 0.0, route.max_leg_dose_msv,
            carriers.empty() ? "" : "; boardings: ", carriers);
    }
}

void print_economy_audit(
    const spacetrains::domain::SimulationSnapshot& snap,
    const spacetrains::domain::UniverseDefinition& universe,
    const spacetrains::economy::EconomySystem& economy,
    double delivered_price_ratio,
    const spacetrains::simulation::Simulation::RadiationAudit& radiation,
    const std::function<double(const spacetrains::domain::StationState&)>& lifetime_profit) {

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
            const auto& rates = economy.get_station_net_rates(sd);
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
        for (const auto& lot : ship.active_mission.cargo) {
            in_transit[lot.commodity_id] += lot.units;
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
            const auto& rates = economy.get_station_net_rates(sd);
            for (const auto& [c, rate] : rates) {
                if (rate >= 0.0 || economy.is_export_market(sd, c)) continue;
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

        // Export markets: what Earth's economy bought from the outer system.
        double market_value = 0.0;
        std::cout << "\n  [ECON AUDIT] Export Markets (sold to the outside economy since start):\n";
        for (const auto& ss : snap.stations) {
            const auto& name = std::find_if(universe.stations.begin(), universe.stations.end(),
                [&](const auto& sd) { return sd.id == ss.station_id; })->name;
            for (const auto& [c, units] : ss.market_sold_units) {
                market_value += units * base_prices[c];
                std::cout << std::format("    {:30s}  {:15s}  {:8.0f}u  {:10.0f} cr\n", name, c, units, units * base_prices[c]);
            }
        }
        std::cout << std::format("    Exports: {:.0f} cr\n", market_value);
        std::cout << std::format("    Delivered price: {:.2f}x the reference\n", delivered_price_ratio);
        if (radiation.crew_dose_msv > 0.0) {
            std::cout << std::format("    Radiation: {:.0f} person-mSv, hazard pay {:.0f} cr, highest leg {:.0f} mSv,"
                " {} particle events (largest {:.2f}x 1972)\n", radiation.crew_dose_msv, radiation.hazard_pay_cr,
                radiation.highest_leg_dose_msv, radiation.particle_events, radiation.largest_event_magnitude);
        }
        if (radiation.hydrogen_boiled_kg > 0.0) {
            std::cout << std::format("    Boil-off: {:.0f} t of hydrogen; {} nuclear-thermal arrivals, {} short of the arrival burn, "
                "{} under half the reserve, lowest {:.2f}x the reserve\n", radiation.hydrogen_boiled_kg / 1000.0,
                radiation.thermal_arrivals, radiation.arrivals_short_of_burn, radiation.arrivals_below_half_reserve,
                radiation.thermal_arrivals > 0 ? radiation.lowest_reserve_share : 0.0);
        }
        if (radiation.overhauls > 0 || radiation.repairs_waiting_for_spares > 0) {
            std::string spares;
            for (const auto& [commodity_id, units] : radiation.spares_units) {
                spares += std::format("{}{} {:.0f}u", spares.empty() ? "" : ", ", commodity_id, units);
            }
            std::cout << std::format("    Overhauls: {} ({} nuclear-thermal, {} plasma; {:.0f} cr = spares {:.0f} + labour {:.0f}; "
                "{:.1f} yard days each; spares {}), {} reviews found no spares\n",
                radiation.overhauls, radiation.thermal_overhauls, radiation.plasma_overhauls, radiation.repair_bills_cr,
                radiation.repair_spares_cr, radiation.repair_labour_cr,
                radiation.overhauls > 0 ? radiation.repair_days / radiation.overhauls : 0.0,
                spares.empty() ? "none" : spares, radiation.repairs_waiting_for_spares);
        }
    }

    // Purchase orders (step 29, step 1: observed only, nothing uses them yet).
    std::cout << "\n  [ECON AUDIT] Station Orders (observed: u use/day, L lead, I interval, sigma spread in days;"
                 " S order-up-to, Smax with the discounted extra, N open now, P order price, G going rate, V ceiling):\n";
    std::cout << std::format("    {:24s} {:13s} {:10s} {:>7s} {:>5s} {:>5s} {:>5s} {:>8s} {:>8s} {:>8s} {:>8s} {:>8s} {:>6s} {:>8s} {:>8s} {:>8s} {:>8s} {:>3s}\n",
        "Station", "Good", "Kind", "u", "L", "I", "sig", "Stock", "Coming", "S", "Smax", "N", "short", "P", "G", "ref", "V", "n");
    for (const auto& ss : snap.stations) {
        const auto& name = std::find_if(universe.stations.begin(), universe.stations.end(),
            [&](const auto& sd) { return sd.id == ss.station_id; })->name;
        for (const auto& [c, o] : ss.orders) {
            const int deliveries = ss.order_stats.contains(c) ? ss.order_stats.at(c).deliveries : 0;
            std::cout << std::format("    {:24.24s} {:13.13s} {:10s} {:7.2f} {:5.0f} {:5.0f} {:5.0f} {:8.0f} {:8.0f} {:8.0f} {:8.0f} {:8.0f} {:>6s} {:8.1f} {:8.1f} {:8.1f} {:8.1f} {:3d}\n",
                name, c, o.ceiling_kind, o.use_per_day, o.lead_days, o.interval_days, o.spread_days, o.stock,
                o.contracted_units, o.order_up_to_units, o.max_units, o.open_units,
                o.days_to_safety < 0.0 ? std::string("-") : std::format("{:.0f}d", o.days_to_safety),
                o.order_price, o.going_rate, o.reference_price, o.ceiling, deliveries);
        }
    }

    // Production (step 14): stations whose outputs run below full rate now, and why.
    std::cout << "\n  [ECON AUDIT] Production (outputs below 90% now; upkeep = product of the upkeep penalties):\n";
    for (const auto& ss : snap.stations) {
        const auto& name = std::find_if(universe.stations.begin(), universe.stations.end(),
            [&](const auto& sd) { return sd.id == ss.station_id; })->name;
        std::string slow;
        for (const auto& [c, run] : ss.output_factor) {
            if (run < 0.9) {
                slow += std::format(" {}={:.0f}%", c, 100.0 * run);
            }
        }
        if (!slow.empty() || ss.upkeep_multiplier < 0.99) {
            std::cout << std::format("    {:30s}  upkeep {:5.1f}% {}\n", name, 100.0 * ss.upkeep_multiplier, slow);
        }
    }
    std::cout << std::format("    Emergencies: {} opened, {} open now, {:.0f} cr paid by the factions\n",
        snap.emergencies_opened, snap.emergencies.size(), snap.emergency_paid);
    {
        int total = 0;
        std::string listed;
        for (const auto& [event_id, count] : snap.events_started) {
            total += count;
            listed += std::format(" {}={}", event_id, count);
        }
        std::cout << std::format("    Events: {} struck{}\n", total, listed);
        for (const auto& station : snap.stations) {
            for (const auto& active : station.events) {
                std::cout << std::format("      running: {} at {} for {:.0f} more days\n", active.event_id,
                    station.station_id, (active.end_s - snap.game_time_s) / 86400.0);
            }
        }
    }
    for (const auto& emergency : snap.emergencies) {
        std::cout << std::format("      open: {} {} for {:.0f} days at {:.0f}x\n", emergency.station_id,
            emergency.commodity_id, (snap.game_time_s - emergency.opened_s) / 86400.0, emergency.premium);
    }

    // Ship utilization
    int ships_with_cargo = 0, ships_repositioning = 0, ships_idle = 0, ships_waiting = 0, ships_stranded = 0, ships_laid_up = 0,
        ships_refitting = 0;
    std::unordered_map<std::string, double> cargo_by_commodity;
    for (const auto& ship : snap.ships) {
        switch (ship.phase) {
            case spacetrains::domain::ShipMissionPhase::InTransit:
                if (!ship.active_mission.cargo.empty()) {
                    ++ships_with_cargo;
                    for (const auto& lot : ship.active_mission.cargo) {
                        cargo_by_commodity[lot.commodity_id] += lot.units;
                    }
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
    std::cout << std::format("    {:30s}  {:>12s}     {:>9s} {:>9s} {:>8s} {:>8s} {:>8s} {:>9s} {:>9s}\n",
        "", "", "household", "producer", "dividend", "subsidy", "tax", "Δstock", "profit");
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
            // Stock gained since the start (which is the data files' stock raised to the order levels).
            const double profit = lifetime_profit(ss);
            const double stock_gain = profit - (ss.credits - sd.initial_credits + l.taxes - l.subsidies);
            std::cout << std::format("    {:30s}  {:>12.0f} cr  {:9.0f} {:9.0f} {:8.0f} {:8.0f} {:8.0f} {:+9.0f} {:+9.0f}\n",
                sd.name, ss.credits, l.household_sales, l.producer_purchases, l.dividends, l.subsidies, l.taxes,
                stock_gain, profit);
            break;
        }
    }
    std::cout << std::format(
        "    Stations: residents paid {:.0f}, producers were paid {:.0f}, dividends {:.0f}, subsidies {:.0f}, taxes {:.0f}\n",
        stations_total.household_sales, stations_total.producer_purchases, stations_total.dividends,
        stations_total.subsidies, stations_total.taxes);
    // Economy overhaul: residents' accounts, trade with the background economies, incomes.
    double household_credits = 0.0;
    if (universe.households.enabled != 0.0 || !universe.surface_markets.empty()) {
        std::cout << std::format("    {:30s}  {:>10s} {:>9s} {:>9s} {:>9s} {:>9s} {:>8s} {:>8s} {:>8s} {:>9s} {:>9s} {:>7s} {:>7s} {:>6s}  {}\n", "",
            "residents", "wages", "spending", "surf.buy", "surf.sell", "incomes", "visitors", "yard", "to ships", "from ship",
            "people", "jobs", "wage", "red days, below floor");
        for (const auto& ss : snap.stations) {
            household_credits += ss.household_credits;
            const auto& l = ss.ledger;
            for (const auto& sd : universe.stations) {
                if (sd.id != ss.station_id) continue;
                std::cout << std::format("    {:30s}  {:>10.0f} {:9.0f} {:9.0f} {:9.0f} {:9.0f} {:8.0f} {:8.0f} {:8.0f} {:9.0f} {:9.0f} {:7.0f} {:7.0f} {:6.2f}  {}\n",
                    sd.name, ss.household_credits, l.wages_paid, l.household_spending, l.surface_bought, l.surface_sold, l.incomes,
                    l.visitor_spending, l.yard_sales, l.ship_sales, l.ship_purchases, ss.population,
                    ss.jobs / std::max(1e-9, universe.households.participation),
                    ss.wage_cr_per_day > 0.0 ? ss.wage_cr_per_day : universe.labour.reference_wage_cr_per_day,
                    std::format("{:5.0f}{}", l.days_in_red, ss.credits < 0.0 ? std::format("  yes, in the red {:.0f}", ss.credits)
                        : ss.credits < universe.open_economy.station_credit_floor ? std::string("  yes") : std::string()));
                break;
            }
        }
        std::cout << "    Background economies:";
        std::cout << std::format("  Earth {:.0f}", snap.outside_economy_credits);
        for (const auto& [economy_id, balance] : snap.background_credits) {
            std::cout << std::format("  {} {:.0f}", economy_id, balance);
        }
        std::cout << " cr (negative: paid into the simulated economy)\n";
        if (!snap.background_population.empty()) {
            std::cout << "    Colonies' people:";
            for (const auto& [economy_id, people] : snap.background_population) {
                std::cout << std::format("  {} {:.0f}", economy_id, people);
            }
            double up = 0.0;
            double down = 0.0;
            for (const auto& ss : snap.stations) {
                up += ss.ledger.hired_from_surface;
                down += ss.ledger.moved_to_surface;
            }
            std::cout << std::format("  (hired up to their gateways {:.0f}, gone down {:.0f})\n", up, down);
        }
    }
    spacetrains::domain::ShipLedger fleet;
    int profitable = 0;
    struct DriveTally {
        int ships {0};
        int profitable {0};
        double profit {0.0};
        double refits {0.0};
    };
    std::map<std::string, DriveTally> by_drive;
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
            l.fuel, l.wages, l.capital, l.provisions, l.refits, l.dividends,
            ship.fit.empty() ? ship.class_id : spacetrains::domain::design_id(ship.class_id, ship.fit),
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
        if (!sold) {
            std::string propulsion = "?";
            for (const auto& sc : universe.ship_classes) {
                if (sc.id == ship.class_id) {
                    propulsion = sc.propulsion_type == "variable_isp" ? "plasma" : "nuclear-thermal";
                    break;
                }
            }
            auto& tally = by_drive[propulsion];
            tally.ships += 1;
            tally.profitable += ship.lifetime_profit > 0.0 ? 1 : 0;
            tally.profit += ship.lifetime_profit;
            tally.refits += l.refits;
        }
    }
    for (const auto& [propulsion, tally] : by_drive) {
        std::cout << std::format("    In service, {}: {}/{} profitable, profit {:.0f} cr, refits and overhauls {:.0f} cr\n",
            propulsion, tally.profitable, tally.ships, tally.profit, tally.refits);
    }
    std::cout << std::format(
        "    Fleet: cargo margin {:.0f} (sold {:.0f}, bought {:.0f})  fuel {:.0f}  wages {:.0f}  capital {:.0f}"
        "  provisions {:.0f}  refits {:.0f}  dividends {:.0f}  -> {}/{} ships profitable\n",
        fleet.cargo_revenue - fleet.cargo_purchases, fleet.cargo_revenue, fleet.cargo_purchases, fleet.fuel,
        fleet.wages, fleet.capital, fleet.provisions, fleet.refits, fleet.dividends, profitable, all_ships.size());
    // Tier 2: how many ships in service fly each module (printed once any ship is fitted).
    std::map<std::string, int> module_census;
    for (const auto& ship : snap.ships) {
        for (const auto& module : ship.fit) {
            ++module_census[module];
        }
    }
    if (!module_census.empty()) {
        std::string line;
        for (const auto& [module, count] : module_census) {
            line += std::format("{}{} {}", line.empty() ? "" : ", ", module, count);
        }
        std::cout << std::format("    Fits: {}\n", line);
    }
    // Scheduled liners (step 22): their loop, where they are, what they earned and the top-ups.
    for (const auto& ship : snap.ships) {
        const auto loop = universe.liners.find(ship.id);
        if (loop == universe.liners.end()) {
            continue;
        }
        std::string stops;
        for (const auto& stop : loop->second) {
            stops += (stops.empty() ? "" : " > ") + stop;
        }
        std::cout << std::format("    Liner {:22s} {}  now {} ({})  profit {:.0f}  faction top-ups {:.0f}\n", ship.name, stops,
            ship.current_station_id, ship.active_mission.destination_station_id.empty() ? "docked"
                : "to " + ship.active_mission.destination_station_id, ship.lifetime_profit, ship.ledger.subsidies);
    }
    // The money-supply controller holds stations + ships; residents' savings are their own.
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
    int over_limit = 0;
    std::cout << "    Faction credit (may borrow):";
    for (const auto& faction : universe.factions) {
        const auto it = snap.faction_credit_limits.find(faction.id);
        const double limit = it == snap.faction_credit_limits.end() ? 0.0 : it->second;
        const auto balance = snap.faction_treasuries.find(faction.id);
        if (balance != snap.faction_treasuries.end() && -balance->second > limit) {
            ++over_limit;
        }
        std::cout << std::format("  {} {:.0f}", faction.name, limit);
    }
    std::cout << std::format(" cr; interest paid {:.0f} cr; factions over their limit: {}\n", snap.faction_interest_paid, over_limit);
    const auto& investment = snap.fleet_investment;
    std::cout << std::format("    Fleet investment: {} ships commissioned (hulls {:.0f}, working capital {:.0f}), {} sold (salvage {:.0f})\n",
        investment.ships_commissioned, investment.hulls_bought, investment.working_capital,
        investment.ships_sold, investment.salvage);
    double background = 0.0;
    for (const auto& [economy_id, balance] : snap.background_credits) {
        background += balance;
    }
    const double external = snap.outside_economy_credits + treasuries + background;
    const double total_supply = internal_supply + household_credits + external;
    // The target is the seeded money plus the working capital the treasuries gave new ships.
    const double target = snap.money_supply_target;
    std::cout << std::format("    Money supply: stations {:.0f} + ships {:.0f} = {:.0f} cr  (target {:.0f}, {:+.1f}%)\n",
        station_credits, ship_credits, internal_supply, target,
        target != 0.0 ? 100.0 * (internal_supply / target - 1.0) : 0.0);
    std::cout << std::format("    External: Earth {:.0f} + other economies {:.0f} + treasuries {:.0f} cr; residents {:.0f} cr.  Total {:.2f} cr  (initial {:.2f}, drift {:+.4f})\n",
        snap.outside_economy_credits, background, treasuries, household_credits, total_supply, initial_supply,
        total_supply - initial_supply);

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

// supply needs rate x round trip units in holds (loaded out, empty back).
// The money journal's accounts (MoneyJournal.hpp): balances, lifetime flows and the main sources
// of income and expense, by sector; factions split into operating result and investment.
void print_failure_audit(const spacetrains::simulation::Simulation& sim) {
    if (!sim.universe().failures.failures) {
        return;
    }
    const auto& a = sim.failure_audit();
    std::cout << "\n  Failures and salvage:\n";
    std::cout << std::format("    Failures {} (towed in near a planet {}, derelicts {}), faults found in port {}, mean age {:.1f} y\n",
        a.failures, a.local_recoveries, a.derelicts, a.faults_armed_in_port, a.failures > 0 ? a.age_years_sum / a.failures : 0.0);
    std::cout << std::format("    Lost: hulls {:.0f} cr, cargo {:.0f} cr; cargo at risk / hull value (time average) {:.4f}\n",
        a.hulls_lost_cr, a.cargo_lost_cr, a.hull_value_cr_days > 0.0 ? a.cargo_aboard_cr_days / a.hull_value_cr_days : 0.0);
    std::string classes;
    for (const auto& [hull, count] : a.by_class) {
        classes += std::format("{}{} {}", classes.empty() ? "" : ", ", hull, count);
    }
    std::cout << "    By hull: " << (classes.empty() ? "none" : classes) << "\n";
    std::cout << std::format("    Derelicts: salvaged {}, written off {}, drifting {}; racers sent {}, lost races {}\n",
        a.salvaged, a.expired, sim.derelicts().size(), a.racers_sent, a.races_lost);
    std::cout << std::format("    Salvage: cargo recovered {:.0f} cr, scrap paid {:.0f} cr; replacements bought {}, claims lapsed {}\n",
        a.cargo_recovered_cr, a.scrap_paid_cr, a.replacements_bought, a.replacement_claims_lapsed);
}

void print_accounts_audit(const spacetrains::simulation::Simulation& sim) {
    using spacetrains::simulation::AccountKind;
    using spacetrains::simulation::MoneyCategory;
    using spacetrains::simulation::kMoneyCategories;
    using spacetrains::simulation::money_category_info;
    const auto& journal = sim.money_journal();
    const auto& accounts = journal.accounts();
    const auto short_cr = [](double v) {
        const double a = std::abs(v);
        return a >= 1e6 ? std::format("{:.2f}M", v / 1e6) : a >= 1e3 ? std::format("{:.0f}k", v / 1e3) : std::format("{:.0f}", v);
    };
    const auto top = [&](const spacetrains::simulation::CategoryFlows& flows, bool income) {
        std::vector<std::pair<double, std::size_t>> list;
        for (std::size_t c = 0; c < kMoneyCategories; ++c) {
            const double v = income ? flows[c].in : flows[c].out;
            if (v > 0.0) {
                list.push_back({v, c});
            }
        }
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string out;
        for (std::size_t k = 0; k < std::min<std::size_t>(3, list.size()); ++k) {
            out += std::format("{}{} {}", k > 0 ? ", " : "", money_category_info(static_cast<MoneyCategory>(list[k].second)).id,
                short_cr(list[k].first));
        }
        return out;
    };
    const auto group_net = [](const spacetrains::simulation::CategoryFlows& flows, std::string_view group, bool inside) {
        double net = 0.0;
        for (std::size_t c = 0; c < kMoneyCategories; ++c) {
            if ((money_category_info(static_cast<MoneyCategory>(c)).group == group) == inside) {
                net += flows[c].in - flows[c].out;
            }
        }
        return net;
    };
    // Colonies' welfare (docs/modules/economy.md, "Colonies").
    std::cout << "\n  [ECON AUDIT] Colonies:\n";
    for (const auto& economy : sim.universe().background_economies) {
        const auto* state = sim.colony_state(economy.id);
        if (state == nullptr) {
            continue;
        }
        std::cout << std::format("    {:14s} satisfaction {:5.1f}%  throttle {:4.2f}  income {:>7.0f}/yr  base imports {:>7.0f}/yr  "
            "wage {:.3f} (data {:.3f})  people factor {:.2f}\n", economy.id, 100.0 * state->satisfaction, state->throttle,
            state->income_per_day * 365.25, state->base_imports_per_day * 365.25, sim.colony_wage_median(economy),
            economy.wage_median_cr_per_day, sim.colony_people_factor(economy.id));
    }
    std::cout << "\n  [ECON AUDIT] Accounts (money journal, since the start):\n";
    const std::pair<AccountKind, const char*> sectors[] = {{AccountKind::Earth, "Earth"}, {AccountKind::Colony, "Colonies"},
        {AccountKind::Faction, "Factions"}, {AccountKind::Station, "Stations"}, {AccountKind::Households, "Residents"},
        {AccountKind::Fleet, "Fleets"}};
    for (const auto& [kind, title] : sectors) {
        std::cout << std::format("    {}:\n", title);
        for (int i = 0; i < static_cast<int>(accounts.size()); ++i) {
            const auto& info = accounts[static_cast<std::size_t>(i)];
            if (info.kind != kind) {
                continue;
            }
            const auto& flows = journal.lifetime(i);
            double in = 0.0;
            double out = 0.0;
            for (const auto& f : flows) {
                in += f.in;
                out += f.out;
            }
            std::string extra;
            if (kind == AccountKind::Faction) {
                extra = std::format("  operating {} investment {}", short_cr(group_net(flows, "investment", false)),
                    short_cr(group_net(flows, "investment", true)));
            } else if (kind == AccountKind::Station) {
                extra = std::format("  before the faction {}", short_cr(group_net(flows, "factions", false)));
            }
            std::cout << std::format("      {:34s} {:>9s}  in {:>8s} out {:>8s}{}\n", info.name, short_cr(sim.account_balance(i)),
                short_cr(in), short_cr(out), extra);
            std::cout << std::format("        in: {}\n        out: {}\n", top(flows, true), top(flows, false));
        }
    }
}


// Demand audit: the transport capacity the stations' consumption needs, against the
// fleet's. For each consumed good, the nearest producer by a rough transfer time (Hohmann
// between the parent planets' orbits, a few days within one planet's system); a steady
void print_capacity_balance(const spacetrains::domain::UniverseDefinition& universe,
    const spacetrains::economy::EconomySystem& economy, const spacetrains::domain::SimulationSnapshot& snap) {
    using namespace spacetrains;
    const auto& bodies = universe.bodies;
    const auto body_of = [&](const std::string& id) -> const domain::CelestialBodyDefinition& {
        for (const auto& b : bodies) {
            if (b.id == id) return b;
        }
        throw std::runtime_error("unknown body " + id);
    };
    // The planet (child of the Sun) a station's body belongs to.
    const auto planet_of = [&](const std::string& body_id) {
        const auto* body = &body_of(body_id);
        while (!body->orbit.parent_id.empty() && !body_of(body->orbit.parent_id).orbit.parent_id.empty()) {
            body = &body_of(body->orbit.parent_id);
        }
        return body;
    };
    const double mu_sun = body_of("sun").mu_m3_s2;
    const auto one_way_days = [&](const domain::StationDefinition& a, const domain::StationDefinition& b) {
        const auto* pa = planet_of(a.parent_body_id);
        const auto* pb = planet_of(b.parent_body_id);
        if (pa == pb) {
            return a.parent_body_id == b.parent_body_id ? 0.5 : 5.0;
        }
        const double axis = 0.5 * (pa->orbit.semi_major_axis_m + pb->orbit.semi_major_axis_m);
        return 3.14159265358979 * std::sqrt(axis * axis * axis / mu_sun) / kDayS;
    };
    double needed_units = 0.0;
    double needed_inner = 0.0;
    std::cout << "\n  [DEMAND AUDIT] Transport capacity for steady supply (nearest producer, Hohmann-like times):\n";
    std::cout << std::format("    {:<26}{:<14}{:>8}{:>26}{:>9}{:>11}\n", "consumer", "good", "u/day", "nearest producer", "days", "hold u");
    for (const auto& consumer : universe.stations) {
        const auto& rates = economy.get_station_net_rates(consumer);
        for (const auto& commodity : universe.commodities) {
            const auto it = rates.find(commodity.id);
            if (it == rates.end() || it->second >= 0.0) continue;
            const double rate = -it->second;
            const domain::StationDefinition* best = nullptr;
            double best_days = 1e18;
            for (const auto& producer : universe.stations) {
                const auto& prates = economy.get_station_net_rates(producer);
                const auto pit = prates.find(commodity.id);
                if (pit == prates.end() || pit->second <= 0.0) continue;
                const double days = one_way_days(producer, consumer);
                if (days < best_days) {
                    best_days = days;
                    best = &producer;
                }
            }
            if (best == nullptr) continue;
            const double hold = rate * 2.0 * best_days;
            needed_units += hold;
            if (best_days < 200.0) needed_inner += hold;
            std::cout << std::format("    {:<26}{:<14}{:>8.1f}{:>26}{:>9.0f}{:>11.0f}\n", consumer.name.substr(0, 25), commodity.id, rate,
                best->name.substr(0, 25), best_days, hold);
        }
    }
    double fleet_units = 0.0;
    for (const auto& ship : snap.ships) {
        for (const auto& ship_class : universe.ship_classes) {
            if (ship_class.id == ship.class_id) fleet_units += ship_class.cargo_capacity_units;
        }
    }
    std::cout << std::format("    Holds needed for steady supply: {:.0f} u (routes under 200 days: {:.0f} u); fleet holds now: {:.0f} u in {} ships\n",
        needed_units, needed_inner, fleet_units, snap.ships.size());
}

}  // namespace spacetrains::app
