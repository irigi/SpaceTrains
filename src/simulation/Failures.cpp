// Ship failures, derelicts and salvage (docs/modules/ships_and_fleet.md, "Failures and salvage").
//
// Every hull has a Weibull-distributed life drawn from its own random stream. When it runs out
// in flight the ship becomes a derelict coasting on the orbit it had; idle ships may race to it
// for its cargo and scrap. The owner loses the hull, the cargo and the delivery, and has the
// first right to the lost ship's place under the fleet cap for a while.

#include "simulation/Simulation.hpp"

#include "trajectory/ChaseSolver.hpp"
#include "trajectory/KeplerPropagation.hpp"
#include "trajectory/Lambert.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <iostream>
#include <numbers>

#include "simulation/SimulationInternal.hpp"

namespace spacetrains::simulation {

using namespace detail;

namespace {

constexpr double kYearS = 365.25 * 86400.0;
constexpr double kDerelictSampleS = 2.0 * 86400.0;

std::uint64_t fnv1a(std::uint64_t hash, const std::string& text) {
    for (const char ch : text) {
        hash = (hash ^ static_cast<unsigned char>(ch)) * 1099511628211ULL;
    }
    return (hash ^ 0xffULL) * 1099511628211ULL;  // a separator, so "ab"+"c" differs from "a"+"bc"
}

}  // namespace

double Simulation::ship_uniform(const std::string& ship_id, const std::string& salt) const {
    std::uint64_t hash = 1469598103934665603ULL ^ universe_.event_seed;
    hash = fnv1a(hash, ship_id);
    hash = fnv1a(hash, salt);
    std::mt19937_64 rng(hash);
    rng.discard(4);
    // 53 random bits, shifted off zero: (0, 1].
    return (static_cast<double>(rng() >> 11) + 1.0) / 9007199254740992.0;
}

double Simulation::weibull_scale_s() const {
    const auto& f = universe_.failures;
    const double mean_s = universe_.ship_operations.lifetime_years * f.mean_life_factor * kYearS;
    return mean_s / std::tgamma(1.0 + 1.0 / f.weibull_shape);
}

void Simulation::draw_failure_clock(domain::ShipState& ship, bool aged) {
    const double k = universe_.failures.weibull_shape;
    const double lambda = weibull_scale_s();
    double age_s = 0.0;
    if (aged) {
        // The steady-state age of a fleet that replaces what fails has density ∝ survival S(a):
        // draw by rejection under S on [0, 4λ] (S(4λ) is negligible for k >= 1).
        for (int attempt = 0; attempt < 1000; ++attempt) {
            const double a = 4.0 * lambda * ship_uniform(ship.id, std::format("age{}", attempt));
            if (ship_uniform(ship.id, std::format("accept{}", attempt)) <= std::exp(-std::pow(a / lambda, k))) {
                age_s = a;
                break;
            }
        }
    }
    // The life, conditional on having reached age_s: S(T)/S(a) = U.
    const double u = ship_uniform(ship.id, aged ? "life_aged" : "life");
    const double life_s = lambda * std::pow(std::pow(age_s / lambda, k) - std::log(u), 1.0 / k);
    ship.built_s = game_time_s_ - age_s;
    ship.fails_at_s = ship.built_s + std::max(life_s, age_s + 86400.0);
    ship.fault_pending = false;
}

void Simulation::init_failure_clocks() {
    if (!failures_on()) {
        return;
    }
    static const bool trace = std::getenv("SPACETRAINS_TRACE_FAILURES") != nullptr;
    for (auto& ship : ships_) {
        if (ship.fails_at_s <= 0.0) {
            draw_failure_clock(ship, ship.commissioned_s <= 0.0);
            if (trace) {
                std::cerr << std::format("[FAILURE] clock {}: built day {:.0f}, fails day {:.0f}\n", ship.name,
                    ship.built_s / 86400.0, ship.fails_at_s / 86400.0);
            }
        }
    }
}

double Simulation::sun_mu() const {
    return mechanics_.get_body(mechanics_.get_root_body_id()).mu_m3_s2;
}

bool Simulation::local_leg(const domain::MissionAssignment& mission) const {
    return primary_body_id(mission.origin_station_id) == primary_body_id(mission.destination_station_id);
}

double Simulation::cargo_value_cr(const std::vector<domain::CargoLot>& cargo, const std::string& station_id) const {
    const auto& station = get_station_definition(station_id);
    double value = 0.0;
    for (const auto& lot : cargo) {
        value += lot.contract_value > 0.0 ? lot.contract_value : lot.units * economy_.reference_price(station, lot.commodity_id);
    }
    return value;
}

int Simulation::fleet_places_held_for_others(const std::string& faction_id) const {
    int held = 0;
    for (const auto& [owner, expiries] : replacement_claims_) {
        if (owner != faction_id) {
            held += static_cast<int>(expiries.size());
        }
    }
    return held;
}

void Simulation::step_failures() {
    if (!failures_on()) {
        return;
    }
    // Exposure for the mean-life factor: once a day, the cargo value in flight and the hull value.
    if (game_time_s_ >= next_exposure_sample_s_) {
        next_exposure_sample_s_ = game_time_s_ + 86400.0;
        for (const auto& ship : ships_) {
            failure_audit_.hull_value_cr_days += ship_design(ship).ship_value_cr;
            // What the owner paid for the cargo is what a failure takes (the margin is a profit
            // not made, which the charge need not cover).
            if (ship.phase == domain::ShipMissionPhase::InTransit) {
                failure_audit_.cargo_aboard_cr_days += ship.active_mission.purchase_cost;
            }
        }
    }
    for (std::size_t i = ships_.size(); i-- > 0;) {
        auto& ship = ships_[i];
        if (ship.fails_at_s <= 0.0) {
            draw_failure_clock(ship, false);  // a new hull (commissioned since the last tick)
        }
        if (!ship.fault_pending && game_time_s_ >= ship.fails_at_s) {
            if (ship.phase == domain::ShipMissionPhase::InTransit) {
                fail_ship(i);
                continue;
            }
            // Faults show under load: the next leg, at a point drawn from the ship's stream.
            ship.fault_pending = true;
            ship.fault_leg_fraction = ship_uniform(ship.id, "leg");
            ++failure_audit_.faults_armed_in_port;
        }
        if (ship.fault_pending && ship.phase == domain::ShipMissionPhase::InTransit) {
            const auto& mission = ship.active_mission;
            const double strikes_s = mission.departure_time_s
                + ship.fault_leg_fraction * std::max(0.0, mission.arrival_time_s - mission.departure_time_s);
            if (game_time_s_ >= strikes_s) {
                fail_ship(i);
            }
        }
    }
    step_derelicts();
}

void Simulation::fail_ship(std::size_t index) {
    auto& ship = ships_[index];
    const auto& design = ship_design(ship);
    auto& mission = ship.active_mission;
    const bool local = local_leg(mission);
    const double age_years = (game_time_s_ - ship.built_s) / kYearS;
    const double cargo_value = mission.purchase_cost;
    ++failure_audit_.failures;
    ++failure_audit_.by_class[design.hull_id.empty() ? design.id : design.hull_id];
    failure_audit_.age_years_sum += age_years;
    failure_audit_.hulls_lost_cr += design.ship_value_cr;
    failure_audit_.cargo_lost_cr += cargo_value;

    // A racer that fails drops out of its race.
    for (auto& derelict : derelicts_) {
        if (derelict.claimant_ship_id == ship.id) {
            derelict.claimant_ship_id.clear();
            derelict.claim_eta_s = 0.0;
        }
    }

    auto& home = get_station_state(ship.home_station_id);
    auto& destination = get_station_state(mission.destination_station_id);
    const auto& destination_def = get_station_definition(mission.destination_station_id);
    // People aboard leave in the lifeboat and are picked up and taken on to the destination by
    // rescue craft we do not simulate (assumption; a timed crew rescue is a later extension).
    land_passengers(ship, destination);

    std::string where;
    if (local) {
        // Near a planet the station's tugs bring the hulk in at once: its cargo goes into the
        // destination's stock and the outside economy pays the station the scrap.
        const double scrap = universe_.failures.scrap_fraction * design.ship_value_cr;
        for (const auto& lot : mission.cargo) {
            destination.inventory[lot.commodity_id] += lot.units;
        }
        outside_economy_credits_ -= scrap;
        destination.credits += scrap;
        journal_.record(account_earth(), account_station(destination), MoneyCategory::Salvage, scrap);
        failure_audit_.scrap_paid_cr += scrap;
        ++failure_audit_.local_recoveries;
        add_event(std::format("{} broke down near {} after {:.1f} years in service; towed in, its cargo ({}) landed there",
            ship.name, destination_def.name, age_years, mission.cargo.empty() ? "none" : describe_cargo(mission.cargo)), "alert");
    } else {
        domain::DerelictState derelict;
        derelict.id = "derelict_" + ship.id;
        derelict.name = ship.name;
        derelict.faction_id = ship.faction_id;
        derelict.class_id = ship.class_id;
        derelict.fit = ship.fit;
        derelict.home_station_id = ship.home_station_id;
        derelict.origin_station_id = mission.origin_station_id;
        derelict.destination_station_id = mission.destination_station_id;
        derelict.cargo = mission.cargo;
        derelict.ship_value_cr = design.ship_value_cr;
        derelict.failed_at_s = game_time_s_;
        derelict.expires_s = game_time_s_ + universe_.failures.claim_days * 86400.0;
        // Its state now: position on the planned path, velocity across an hour either side.
        const auto at = [&](double t) {
            return interpolate_timed_sampled_path(mission.sampled_path, mission.sampled_times_s, t);
        };
        const double t0 = std::max(mission.sampled_times_s.empty() ? game_time_s_ : mission.sampled_times_s.front(), game_time_s_ - 3600.0);
        const double t1 = std::min(mission.sampled_times_s.empty() ? game_time_s_ : mission.sampled_times_s.back(), game_time_s_ + 3600.0);
        derelict.epoch_position_m = get_ship_render_position(ship);
        derelict.epoch_velocity_mps = t1 > t0 ? (at(t1) - at(t0)) / (t1 - t0) : math::Vec3d {};
        trajectory::sample_kepler(derelict.epoch_position_m, derelict.epoch_velocity_mps, game_time_s_, derelict.expires_s,
            kDerelictSampleS, sun_mu(), derelict.sampled_path, derelict.sampled_times_s);
        const double r_au = derelict.epoch_position_m.length() / 1.495978707e11;
        // Debug aid: SPACETRAINS_TRACE_FAILURES=1 logs each derelict's state at failure (stderr).
        if (static const bool trace = std::getenv("SPACETRAINS_TRACE_FAILURES") != nullptr; trace) {
            const double v = derelict.epoch_velocity_mps.length();
            std::cerr << std::format("[FAILURE] day {:.2f} {} ({}) {} -> {} type {} r {:.3f} AU v {:.0f} m/s circ {:.0f} samples {} "
                                     "t0 {:.2f} t1 {:.2f}\n",
                game_time_s_ / 86400.0, ship.name, ship.class_id, mission.origin_station_id, mission.destination_station_id,
                mission.trajectory_type, r_au, v, std::sqrt(sun_mu() / derelict.epoch_position_m.length()),
                mission.sampled_path.size(), t0 / 86400.0, t1 / 86400.0);
        }
        add_event(std::format("{} disabled {:.2f} AU from the Sun after {:.1f} years in service, drifting with {}",
            ship.name, r_au, age_years, mission.cargo.empty() ? "an empty hold" : describe_cargo(mission.cargo)), "alert");
        ++failure_audit_.derelicts;
        derelicts_.push_back(std::move(derelict));
    }

    // The owner's first right to the lost ship's place under the cap.
    if (universe_.failures.replacement_days > 0.0) {
        replacement_claims_[ship.faction_id].push_back(game_time_s_ + universe_.failures.replacement_days * 86400.0);
    }
    // The ship's cash, or its debt, goes to the home station, as when a ship is sold.
    home.credits += ship.credits;
    home.ledger.dividends += ship.credits;
    journal_.record(account_fleet(ship.faction_id), account_station(home), MoneyCategory::ShipSaleCash, ship.credits);
    ship.ledger.dividends += ship.credits;
    ship.credits = 0.0;
    ship.phase = domain::ShipMissionPhase::Idle;
    ship.active_mission = {};
    sold_ships_.push_back(std::move(ship));
    ships_.erase(ships_.begin() + static_cast<std::ptrdiff_t>(index));
}

void Simulation::step_derelicts() {
    for (std::size_t i = derelicts_.size(); i-- > 0;) {
        if (game_time_s_ >= derelicts_[i].expires_s) {
            add_event(std::format("The wreck of {} is written off: nobody reached it in {:.0f} days", derelicts_[i].name,
                universe_.failures.claim_days), "alert");
            ++failure_audit_.expired;
            derelicts_.erase(derelicts_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    // Replacement claims run out.
    for (auto& [faction_id, expiries] : replacement_claims_) {
        const auto before = expiries.size();
        std::erase_if(expiries, [&](double until_s) { return game_time_s_ >= until_s; });
        failure_audit_.replacement_claims_lapsed += static_cast<int>(before - expiries.size());
    }
    if (universe_.failures.salvage && !derelicts_.empty() && game_time_s_ >= next_salvage_review_s_) {
        next_salvage_review_s_ = game_time_s_ + universe_.failures.salvage_review_days * 86400.0;
        review_salvage();
    }
}

std::string Simulation::primary_body_id(const std::string& station_id) const {
    const std::string root = mechanics_.get_root_body_id();
    const auto* body = &mechanics_.get_body(get_station_definition(station_id).parent_body_id);
    while (!body->orbit.parent_id.empty() && body->orbit.parent_id != root) {
        body = &mechanics_.get_body(body->orbit.parent_id);
    }
    return body->id;
}

bool Simulation::can_race(const domain::ShipState& ship) const {
    // Docked and free (a laid-up ship rehires its crew for it): not bound by a contract, a liner's loop or a new ship's route,
    // and not carrying a fault that is about to show.
    return (ship.phase == domain::ShipMissionPhase::Idle || ship.phase == domain::ShipMissionPhase::LaidUp) && !under_contract(ship) && liner_stops(ship) == nullptr
        && !(!ship.route_destination_id.empty() && game_time_s_ < ship.route_until_s) && !ship.fault_pending
        && ship.current_station_id.size() > 0;
}

namespace {

math::Vec3d circular_velocity(const math::Vec3d& r, double mu) {
    const double rm = std::max(1.0, r.length());
    const double vc = std::sqrt(mu / rm);
    return {-r.z / rm * vc, 0.0, r.x / rm * vc};
}

// The salvageable cargo: lots in order until the salvager's hold is full.
std::vector<domain::CargoLot> lots_that_fit(const std::vector<domain::CargoLot>& cargo, double hold_units) {
    std::vector<domain::CargoLot> taken;
    for (const auto& lot : cargo) {
        if (hold_units <= 0.0) {
            break;
        }
        const double units = std::min(lot.units, hold_units);
        taken.push_back({.commodity_id = lot.commodity_id, .units = units});
        hold_units -= units;
    }
    return taken;
}

}  // namespace

Simulation::SalvagePlan Simulation::plan_salvage(const domain::ShipState& ship, const domain::DerelictState& derelict,
    bool both_legs, double latest_meet_s, double fuel_cr_per_kg, double cr_per_day) const {
    // The cheapest salvage run, as the trade planners pick a trip: propellant at the origin's fuel
    // price plus the ship's daily costs, less the prize where it is sold (ports differ); the way out
    // and the way home are chosen together. A race only adds the deadline `latest_meet_s` (meet the
    // wreck before the current claimant, by the margin).
    SalvagePlan plan;
    const auto& design = ship_design(ship);
    const double mu = sun_mu();
    const double now = game_time_s_;
    const double meet_limit_s = std::min(derelict.expires_s, latest_meet_s);
    if (meet_limit_s - now < 2.0 * 86400.0) {
        return plan;
    }
    const auto derelict_at = [&](double t) {
        return trajectory::propagate_kepler(derelict.epoch_position_m, derelict.epoch_velocity_mps, t - derelict.failed_at_s, mu);
    };
    const double load = std::min(design.propellant_capacity_kg, ship.propellant_kg + purchasable_propellant_kg(ship));
    if (load <= 0.0) {
        return plan;
    }
    const auto taken = lots_that_fit(derelict.cargo, design.cargo_capacity_units);
    double cargo_kg = 0.0;
    for (const auto& lot : taken) {
        cargo_kg += lot.units * get_commodity(lot.commodity_id).mass_per_unit_kg;
    }
    const double dry = design.dry_mass_kg;
    const double reserve = PROPELLANT_RESERVE_FRACTION * load;
    const auto& origin = get_station_definition(ship.current_station_id);
    std::vector<std::string> ports;
    for (const auto& id : {ship.home_station_id, derelict.destination_station_id, derelict.origin_station_id}) {
        if (std::find(ports.begin(), ports.end(), id) == ports.end()) {
            ports.push_back(id);
        }
    }
    // Everything in kilograms of propellant at the origin's price.
    const double fuel_price = std::max(1.0e-6, fuel_cr_per_kg);
    const double kg_per_s = cr_per_day / fuel_price / 86400.0;
    const auto prize_kg = [&](const std::string& port) {
        double value = 0.0;
        for (const auto& lot : taken) {
            value += lot.units * economy_.reference_price(get_station_definition(port), lot.commodity_id);
        }
        return value / fuel_price;
    };
    plan.propellant_load_kg = load;

    if (design.propulsion_type == "variable_isp") {
        const double power_w = design.specific_engine_power_w_per_kg * dry;
        const auto d_now = derelict_at(now);
        trajectory::ChaseUnits units;
        units.mu_m3_s2 = mu;
        units.length_m = std::max(0.2 * 1.495978707e11, d_now.position_m.length());
        const auto r0 = mechanics_.get_station_position(origin, now);
        const trajectory::ChaseRequest leg1 {
            .position_m = r0,
            .velocity_mps = circular_velocity(r0, mu),
            .depart_time_s = now,
            .power_w = power_w,
            .initial_mass_kg = dry + load,
            .dry_mass_kg = dry + reserve,
            .end_condition = variable_isp::general::EndCondition::Rendezvous,
        };
        const auto wreck = trajectory::kepler_target(d_now.position_m, d_now.velocity_mps, units);
        const auto out = trajectory::meeting_candidates(leg1, wreck, units, 86400.0, std::min(365.0 * 86400.0, meet_limit_s - now),
            kg_per_s, 10);
        if (out.empty()) {
            return plan;
        }
        // The way home from a meeting: the cheapest port (cost less prize) and its candidate.
        struct Home {
            std::string port;
            trajectory::ChasePlan leg;
            double objective {std::numeric_limits<double>::infinity()};
        };
        const auto home_from = [&](double rendezvous_s, double used_kg, bool refine) {
            Home best;
            const auto d_rv = derelict_at(rendezvous_s);
            trajectory::ChaseUnits home_units = units;
            home_units.length_m = std::max(0.2 * 1.495978707e11, d_rv.position_m.length());
            const trajectory::ChaseRequest leg2 {
                .position_m = d_rv.position_m,
                .velocity_mps = d_rv.velocity_mps,
                .depart_time_s = rendezvous_s,
                .power_w = power_w,
                .initial_mass_kg = dry + load - used_kg + cargo_kg,
                .dry_mass_kg = dry + cargo_kg + reserve,
                .end_condition = variable_isp::general::EndCondition::Rendezvous,
            };
            if (leg2.initial_mass_kg <= leg2.dry_mass_kg) {
                return best;
            }
            for (const auto& port : ports) {
                // The port's planet, sampled every two days for 500 days.
                domain::TrajectoryPlan planet;
                const auto planet_id = primary_body_id(port);
                for (double t = rendezvous_s; t <= rendezvous_s + 500.0 * 86400.0; t += 2.0 * 86400.0) {
                    planet.sampled_path.push_back(mechanics_.get_body_position(planet_id, t));
                    planet.sampled_times_s.push_back(t);
                }
                const auto target = trajectory::path_target(planet, rendezvous_s, home_units);
                const auto cands = trajectory::meeting_candidates(leg2, target, home_units, 86400.0, 495.0 * 86400.0, kg_per_s, 8);
                if (cands.empty()) {
                    continue;
                }
                const double objective = cands.front().objective - prize_kg(port);
                if (objective < best.objective) {
                    best.objective = objective;
                    best.port = port;
                    best.leg = refine ? trajectory::refine_meeting(leg2, target, home_units, kg_per_s, cands.front(), 6, 60)
                                      : trajectory::ChasePlan {.feasible = true, .transfer_time_s = cands.front().transfer_time_s,
                                            .propellant_kg = cands.front().propellant_kg};
                }
            }
            return best;
        };
        // The way out: the cheapest few meeting times, each with its way home.
        const trajectory::MeetingCandidate* chosen = &out.front();
        if (both_legs) {
            chosen = nullptr;
            double best_total = std::numeric_limits<double>::infinity();
            for (std::size_t k = 0; k < std::min<std::size_t>(3, out.size()); ++k) {
                const auto home = home_from(now + out[k].transfer_time_s, out[k].propellant_kg, false);
                if (!home.port.empty() && out[k].objective + home.objective < best_total) {
                    best_total = out[k].objective + home.objective;
                    chosen = &out[k];
                }
            }
            if (chosen == nullptr) {
                return plan;
            }
        }
        auto chase = trajectory::refine_meeting(leg1, wreck, units, kg_per_s, *chosen, 6, 60);
        Home home;
        if (both_legs) {
            home = home_from(now + chase.transfer_time_s, chase.propellant_kg, true);
            if (home.port.empty()) {
                // The refined meeting left no way home: the grid point's had one.
                chase = trajectory::refine_meeting(leg1, wreck, units, kg_per_s, *chosen, 0, 60);
                home = home_from(now + chase.transfer_time_s, chase.propellant_kg, true);
                if (home.port.empty()) {
                    return plan;
                }
            }
        }
        plan.depart_s = now;
        plan.rendezvous_s = now + chase.transfer_time_s;
        plan.path = chase.path_m;
        plan.times = chase.times_s;
        for (std::size_t i = 0; i < plan.times.size(); ++i) {
            const double share = plan.times.size() > 1 ? static_cast<double>(i) / (plan.times.size() - 1) : 1.0;
            plan.propellant.push_back(load - share * chase.propellant_kg);
        }
        plan.propellant_used_kg = chase.propellant_kg;
        plan.trajectory_type = "variable_isp";
        plan.feasible = true;
        if (!both_legs) {
            return plan;
        }
        plan.destination_id = home.port;
        plan.arrival_s = plan.rendezvous_s + home.leg.transfer_time_s;
        const double left = load - chase.propellant_kg;
        for (std::size_t i = 1; i < home.leg.times_s.size(); ++i) {
            const double share = static_cast<double>(i) / (home.leg.times_s.size() - 1);
            plan.path.push_back(home.leg.path_m[i]);
            plan.times.push_back(home.leg.times_s[i]);
            plan.propellant.push_back(left - share * home.leg.propellant_kg);
        }
        plan.propellant_used_kg += home.leg.propellant_kg;
        return plan;
    }

    // Nuclear-thermal: impulsive Lambert arcs, the burns at the ends of each arc.
    const double ve = trajectory::thermal_exhaust_velocity_mps(design);
    if (ve <= 0.0) {
        return plan;
    }
    const double min_r = trajectory::kMinPerihelionM;
    // The cheapest arc for the time: direct, or with one or two whole revolutions more (a long wait
    // for the wreck or the port can be cheaper spent on a longer orbit), clear of the Sun.
    const auto cheapest_arc = [&](const math::Vec3d& r1, const math::Vec3d& v_from, const math::Vec3d& r2,
                                  const math::Vec3d& v_to, double transit) {
        trajectory::LambertResult best_arc;
        double best_dv = std::numeric_limits<double>::infinity();
        for (const auto& arc : trajectory::solve_lambert_revolutions(r1, r2, transit, mu, 2)) {
            const double dv = (arc.v1 - v_from).length() + (arc.v2 - v_to).length();
            if (dv < best_dv && trajectory::conic_arc_min_radius(r1, arc.v1, r2, mu, arc.revolutions) >= min_r) {
                best_dv = dv;
                best_arc = arc;
            }
        }
        return best_arc;
    };
    struct Leg1 {
        double depart_s, rendezvous_s, used_kg, objective;
        math::Vec3d r1, v1;
        int revolutions;
    };
    std::vector<Leg1> first;
    plan.best_dv_mps = std::numeric_limits<double>::infinity();
    plan.dv_available_mps = ve * std::log((dry + load) / (dry + reserve));
    for (const double wait_days : {0.0, 5.0, 10.0, 20.0, 40.0}) {
        const double depart = now + wait_days * 86400.0;
        const auto r0 = mechanics_.get_station_position(origin, depart);
        const auto v0 = circular_velocity(r0, mu);
        for (int i = 0; i < 30; ++i) {
            const double transit = 10.0 * 86400.0 * std::pow(40.0, i / 29.0);  // 10 to 400 days
            const double meet = depart + transit;
            if (meet > meet_limit_s) {
                break;
            }
            const auto target = derelict_at(meet);
            const auto lam = cheapest_arc(r0, v0, target.position_m, target.velocity_mps, transit);
            if (!lam.found) {
                continue;
            }
            const double dv = (lam.v1 - v0).length() + (lam.v2 - target.velocity_mps).length();
            plan.best_dv_mps = std::min(plan.best_dv_mps, dv);
            const double m0 = dry + load;
            const double m1 = m0 * std::exp(-dv / ve);
            if (m1 < dry + reserve) {
                continue;  // the way home is checked below
            }
            first.push_back({depart, meet, m0 - m1, m0 - m1 + kg_per_s * (meet - now), r0, lam.v1, lam.revolutions});
        }
    }
    std::sort(first.begin(), first.end(), [](const Leg1& a, const Leg1& b) {
        return a.objective < b.objective || (a.objective == b.objective && a.rendezvous_s < b.rendezvous_s);
    });
    if (first.empty()) {
        return plan;
    }
    if (!both_legs) {
        const auto& leg = first.front();
        plan.feasible = true;
        plan.depart_s = leg.depart_s;
        plan.rendezvous_s = leg.rendezvous_s;
        plan.propellant_used_kg = leg.used_kg;
        plan.trajectory_type = "keplerian_lambert";
        return plan;
    }
    // The cheapest few ways out, each with its cheapest way home (port and time).
    struct Best {
        const Leg1* leg {nullptr};
        double total {std::numeric_limits<double>::infinity()};
        double arrival_s {0.0};
        double used_kg {0.0};
        std::string port;
        math::Vec3d v1;
        int revolutions {0};
    } best;
    for (std::size_t k = 0; k < std::min<std::size_t>(6, first.size()); ++k) {
        const auto& leg = first[k];
        const auto d_rv = derelict_at(leg.rendezvous_s);
        const double m1 = dry + load - leg.used_kg + cargo_kg;
        for (const auto& port : ports) {
            const auto planet_id = primary_body_id(port);
            const double prize = prize_kg(port);
            for (int i = 0; i < 30; ++i) {
                const double transit = 10.0 * 86400.0 * std::pow(50.0, i / 29.0);  // 10 to 500 days
                const double arrive = leg.rendezvous_s + transit;
                const auto rp = mechanics_.get_body_position(planet_id, arrive);
                const auto lam = cheapest_arc(d_rv.position_m, d_rv.velocity_mps, rp, circular_velocity(rp, mu), transit);
                if (!lam.found) {
                    continue;
                }
                const double dv = (lam.v1 - d_rv.velocity_mps).length() + (lam.v2 - circular_velocity(rp, mu)).length();
                const double m2 = m1 * std::exp(-dv / ve);
                if (m2 < dry + cargo_kg + reserve) {
                    continue;
                }
                const double total = leg.objective + (m1 - m2) + kg_per_s * transit - prize;
                if (total < best.total) {
                    best = {.leg = &leg, .total = total, .arrival_s = arrive, .used_kg = m1 - m2, .port = port, .v1 = lam.v1,
                        .revolutions = lam.revolutions};
                }
            }
        }
    }
    if (best.leg == nullptr) {
        return plan;
    }
    const auto& leg = *best.leg;
    const auto sample_arc = [&](const math::Vec3d& r, const math::Vec3d& v, double t0, double t1, double left_kg,
                                bool skip_first, int revolutions) {
        std::vector<math::Vec3d> path;
        std::vector<double> times;
        trajectory::sample_kepler(r, v, t0, t1, (t1 - t0) / (40.0 * (1 + revolutions)), mu, path, times);
        for (std::size_t i = skip_first ? 1 : 0; i < path.size(); ++i) {
            plan.path.push_back(path[i]);
            plan.times.push_back(times[i]);
            plan.propellant.push_back(left_kg);
        }
    };
    plan.feasible = true;
    plan.destination_id = best.port;
    plan.depart_s = leg.depart_s;
    plan.rendezvous_s = leg.rendezvous_s;
    plan.arrival_s = best.arrival_s;
    plan.propellant_used_kg = leg.used_kg + best.used_kg;
    plan.trajectory_type = "keplerian_lambert";
    sample_arc(leg.r1, leg.v1, leg.depart_s, leg.rendezvous_s, load - leg.used_kg, false, leg.revolutions);
    sample_arc(derelict_at(leg.rendezvous_s).position_m, best.v1, leg.rendezvous_s, best.arrival_s, load - plan.propellant_used_kg, true,
        best.revolutions);
    return plan;
}

void Simulation::start_salvage_run(domain::ShipState& ship, domain::DerelictState& derelict, const SalvagePlan& plan,
    double value_cr, double cost_cr) {
    const auto& design = ship_design(ship);
    auto& origin_state = get_station_state(ship.current_station_id);
    const auto& origin_def = get_station_definition(ship.current_station_id);
    const auto& destination_def = get_station_definition(plan.destination_id);
    buy_propellant(ship, plan.propellant_load_kg - ship.propellant_kg);
    const double mission_days = (plan.arrival_s - game_time_s_) / 86400.0;
    ship.leg_dose_msv = 0.0;
    buy_provisions(ship, origin_state, mission_days * PROVISION_MARGIN + std::max(PROVISION_MIN_RETURN_DAYS, mission_days), true);
    buy_provisions(ship, origin_state, PROVISION_ENDURANCE_DAYS, false);
    const bool plasma = design.propulsion_type == "variable_isp";
    if (!plasma) {
        // Impulsive burns: booked at the start, as for other nuclear-thermal missions.
        ship.propellant_kg -= plan.propellant_used_kg;
        if (universe_.ship_environment.wear) {
            domain::TrajectoryPlan burn;
            burn.propellant_required_kg = plan.propellant_used_kg;
            ship.engine_wear = std::min(1.0, ship.engine_wear + drive_hours(design, burn) / drive_life_h(design));
        }
    }
    const double time_cost_per_day = daily_capital_cost(design) + daily_crew_cost(design, origin_state)
        + wear_cr_per_flight_day(design);
    const bool waits = plan.depart_s > game_time_s_;
    ship.phase = waits ? domain::ShipMissionPhase::AwaitingDeparture : domain::ShipMissionPhase::InTransit;
    domain::MissionAssignment mission;
    mission.origin_station_id = origin_def.id;
    mission.destination_station_id = plan.destination_id;
    mission.departure_time_s = plan.depart_s;
    mission.arrival_time_s = plan.arrival_s;
    mission.wait_time_s = plan.depart_s - game_time_s_;
    mission.coast_time_s = plan.arrival_s - plan.depart_s;
    mission.total_travel_time_s = plan.arrival_s - game_time_s_;
    mission.remaining_travel_time_s = mission.total_travel_time_s;
    mission.propellant_cost_kg = plan.propellant_used_kg;
    mission.fuel_cost = plan.propellant_used_kg / FUEL_UNITS_TO_KG * station_price(origin_state, "fuel");
    mission.expected_revenue = value_cr;
    mission.operating_cost = time_cost_per_day * mission_days;
    mission.sampled_path = plan.path;
    mission.sampled_times_s = plan.times;
    if (plasma) {
        mission.sampled_propellant_kg = plan.propellant;
    }
    mission.trajectory_type = plan.trajectory_type;
    mission.salvage_derelict_id = derelict.id;
    mission.rendezvous_time_s = plan.rendezvous_s;
    ship.active_mission = std::move(mission);

    const bool overtakes = !derelict.claimant_ship_id.empty();
    std::string overtaken;
    if (overtakes) {
        for (const auto& other : ships_) {
            if (other.id == derelict.claimant_ship_id) {
                overtaken = other.name;
            }
        }
    }
    derelict.claimant_ship_id = ship.id;
    derelict.claim_eta_s = plan.rendezvous_s;
    ++derelict.racers;
    ++failure_audit_.racers_sent;
    add_event(std::format("{} {} {} to salvage the wreck of {}: rendezvous day {:.0f}{}, then to {} (worth ~{:.0f} cr, costs ~{:.0f})",
        ship.name, waits ? "will leave" : "sets out from", origin_def.name, derelict.name, plan.rendezvous_s / 86400.0,
        overtakes ? std::format(", ahead of {}", overtaken.empty() ? std::string("an earlier racer") : overtaken) : std::string(),
        destination_def.name, value_cr, cost_cr), "mission");
}

std::optional<Simulation::SalvageOption> Simulation::best_salvage_for(const domain::ShipState& ship, double spot_per_day) const {
    const auto& f = universe_.failures;
    if (!f.salvage || derelicts_.empty() || !can_race(ship)) {
        return std::nullopt;
    }
    std::optional<SalvageOption> best;
    const auto& design = ship_design(ship);
    const auto& origin_state = get_station_state(ship.current_station_id);
    const double per_day_cost = daily_capital_cost(design) + daily_crew_cost(design, origin_state) + wear_cr_per_flight_day(design);
    const double fuel_cr_per_kg = station_price(origin_state, "fuel") / FUEL_UNITS_TO_KG;
    for (std::size_t i = 0; i < derelicts_.size(); ++i) {
        const auto& derelict = derelicts_[i];
        if (derelict.racers >= static_cast<int>(f.max_racers)) {
            continue;
        }
        // A new racer must meet the wreck sooner than the current claimant, by the margin.
        const double best_s = derelict.claimant_ship_id.empty() ? std::numeric_limits<double>::infinity()
                                                                : derelict.claim_eta_s - game_time_s_;
        const double latest_meet_s = game_time_s_ + best_s * (1.0 - f.racer_margin);
        const auto out = plan_salvage(ship, derelict, false, latest_meet_s, fuel_cr_per_kg, per_day_cost);
        if (!out.feasible || out.rendezvous_s - game_time_s_ >= best_s * (1.0 - f.racer_margin)) {
            continue;
        }
        // Even the whole prize over the way out alone must beat the best mission: the bound
        // spares the way home's planning most of the time.
        double prize = f.scrap_fraction * derelict.ship_value_cr;
        for (const auto& lot : lots_that_fit(derelict.cargo, design.cargo_capacity_units)) {
            prize += lot.units * economy_.reference_price(get_station_definition(derelict.destination_station_id), lot.commodity_id);
        }
        if (1.5 * prize / std::max(1.0, (out.rendezvous_s - game_time_s_) / 86400.0) <= std::max(0.0, spot_per_day)) {
            continue;
        }
        auto full = plan_salvage(ship, derelict, true, latest_meet_s, fuel_cr_per_kg, per_day_cost);
        if (!full.feasible || full.rendezvous_s - game_time_s_ >= best_s * (1.0 - f.racer_margin)) {
            continue;
        }
        const auto& port = get_station_definition(full.destination_id);
        double value = f.scrap_fraction * derelict.ship_value_cr;
        for (const auto& lot : lots_that_fit(derelict.cargo, design.cargo_capacity_units)) {
            value += lot.units * economy_.reference_price(port, lot.commodity_id);
        }
        const double days = std::max(1.0, (full.arrival_s - game_time_s_) / 86400.0);
        const double cost = full.propellant_used_kg / FUEL_UNITS_TO_KG * station_price(origin_state, "fuel") + days * per_day_cost;
        const double per_day = (value - cost) / days;
        if (value > cost && (!best || per_day > best->per_day)) {
            best = SalvageOption {.derelict_index = i, .plan = std::move(full), .value_cr = value, .cost_cr = cost, .per_day = per_day};
        }
    }
    return best;
}

bool Simulation::consider_salvage(domain::ShipState& ship, double spot_per_day) {
    // Planning a chase takes milliseconds: a ship that passed looks again after a few days.
    constexpr double kLookAgainDays = 5.0;
    if (game_time_s_ < ship.next_salvage_look_s) {
        return false;
    }
    ship.next_salvage_look_s = game_time_s_ + kLookAgainDays * 86400.0;
    const auto option = best_salvage_for(ship, spot_per_day);
    static const bool trace = std::getenv("SPACETRAINS_TRACE_SALVAGE") != nullptr;
    if (trace && option) {
        std::cerr << std::format("[SALVAGE] day {:.1f} {} at {}: wreck of {} pays {:.0f} cr/day against {:.0f} (worth {:.0f}, costs {:.0f})\n",
            game_time_s_ / 86400.0, ship.name, ship.current_station_id, derelicts_[option->derelict_index].name, option->per_day,
            spot_per_day, option->value_cr, option->cost_cr);
    }
    if (!option || option->per_day <= spot_per_day) {
        return false;
    }
    if (ship.phase == domain::ShipMissionPhase::LaidUp) {
        add_event(std::format("{} rehired its crew at {}", ship.name, get_station_definition(ship.current_station_id).name), "mission");
    }
    start_salvage_run(ship, derelicts_[option->derelict_index], option->plan, option->value_cr, option->cost_cr);
    return true;
}

void Simulation::review_salvage() {
    // Ships that found nothing to do (idle or laid up) look at the open derelicts now and then;
    // busy ones weigh salvage against their missions at dispatch (consider_salvage).
    for (std::size_t i = 0; i < ships_.size(); ++i) {
        if (can_race(ships_[i])) {
            (void)consider_salvage(ships_[i], 0.0);
        }
    }
}

void Simulation::reach_derelict(domain::ShipState& ship) {
    auto& mission = ship.active_mission;
    mission.salvage_done = true;
    const auto it = std::find_if(derelicts_.begin(), derelicts_.end(), [&](const auto& d) { return d.id == mission.salvage_derelict_id; });
    const auto& destination = get_station_definition(mission.destination_station_id);
    if (it == derelicts_.end()) {
        ++failure_audit_.races_lost;
        add_event(std::format("{} reaches the drift of a wreck already taken; heads on to {} empty", ship.name, destination.name),
            "mission");
        return;
    }
    const auto& design = ship_design(ship);
    const auto taken = lots_that_fit(it->cargo, design.cargo_capacity_units);
    for (const auto& lot : taken) {
        mission.cargo.push_back(lot);
    }
    const double scrap = universe_.failures.scrap_fraction * it->ship_value_cr;
    outside_economy_credits_ -= scrap;
    ship.credits += scrap;
    ship.lifetime_profit += scrap;
    ship.ledger.cargo_revenue += scrap;
    journal_.record(account_earth(), account_fleet(ship.faction_id), MoneyCategory::Salvage, scrap);
    failure_audit_.scrap_paid_cr += scrap;
    failure_audit_.cargo_recovered_cr += cargo_value_cr(taken, destination.id);
    ++failure_audit_.salvaged;
    add_event(std::format("{} salvages the wreck of {} ({:.0f} cr scrap{}); on to {}", ship.name, it->name, scrap,
        taken.empty() ? std::string() : ", cargo " + describe_cargo(taken), destination.name), "trade");
    derelicts_.erase(it);
}

}  // namespace spacetrains::simulation
