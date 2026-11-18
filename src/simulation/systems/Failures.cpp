// Ship failures, derelicts and salvage (docs/modules/ships_and_fleet.md, "Failures and salvage").
//
// Every hull has a Weibull-distributed life drawn from its own random stream. When it runs out
// in flight the ship becomes a derelict coasting on the orbit it had; idle ships may race to it
// for its cargo and scrap. The owner loses the hull, the cargo and the delivery, and has the
// first right to the lost ship's place under the fleet cap for a while.

#include "simulation/systems/Failures.hpp"

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
#include "simulation/query/Accounts.hpp"
#include "simulation/query/Events.hpp"
#include "simulation/query/Lookups.hpp"
#include "simulation/query/Positions.hpp"
#include "simulation/query/ShipRates.hpp"
#include "simulation/systems/Contracts.hpp"
#include "simulation/systems/Market.hpp"
#include "simulation/systems/Passengers.hpp"
#include "simulation/systems/ShipOperations.hpp"

namespace spacetrains::simulation::systems {

using namespace detail;

namespace {

[[nodiscard]] bool failures_on(const World& w) { return w.data.universe.failures.failures; }

// A salvage run: from the ship's port to the derelict (rendezvous) and on to a station.
struct SalvagePlan {
    bool feasible {false};
    std::string destination_id;
    double depart_s {0.0};
    double rendezvous_s {0.0};
    double arrival_s {0.0};
    double propellant_load_kg {0.0};
    double propellant_used_kg {0.0};
    std::vector<math::Vec3d> path;
    std::vector<double> times;
    std::vector<double> propellant;   // aboard (plasma ships burn as they go)
    std::string trajectory_type;
    double best_dv_mps {0.0};        // diagnostics: cheapest way out found, and what the ship has
    double dv_available_mps {0.0};
};

// The ship's best salvage run now (dispatch compares it with its best mission): profit per
// day over the run, or nothing.
struct SalvageOption {
    std::size_t derelict_index {0};
    SalvagePlan plan;
    double value_cr {0.0};
    double cost_cr {0.0};
    double per_day {0.0};
};

// Defined below, file-local.
// A uniform draw in (0, 1] from the ship's own stream (data seed, ship id, salt).
[[nodiscard]] double ship_uniform(const World& w, const std::string& ship_id, const std::string& salt);
[[nodiscard]] double weibull_scale_s(const World& w);
// Draws the ship's failure age: as a new hull, or (starting fleet) at an age from the
// fleet's steady state, conditional on having lived that long.
void draw_failure_clock(World& w, domain::ShipState& ship, bool aged);
[[nodiscard]] double cargo_value_cr(const World& w, const std::vector<domain::CargoLot>& cargo,
    const std::string& station_id);
void fail_ship(World& w, std::size_t index);
void step_derelicts(World& w);
[[nodiscard]] bool can_race(const World& w, const domain::ShipState& ship);
// Pure (runs in parallel): leg one only, or both legs (home to the best of the ship's home,
// the derelict's destination and its origin). The cheapest run (propellant at the fuel price,
// days at cr_per_day, less the prize at the port), meeting the wreck by latest_meet_s.
[[nodiscard]] SalvagePlan plan_salvage(const World& w, const domain::ShipState& ship,
    const domain::DerelictState& derelict, bool both_legs, double latest_meet_s, double fuel_cr_per_kg,
    double cr_per_day);
void start_salvage_run(World& w, domain::ShipState& ship, domain::DerelictState& derelict, const SalvagePlan& plan,
    double value_cr, double cost_cr);
[[nodiscard]] std::optional<SalvageOption> best_salvage_for(const World& w, const domain::ShipState& ship,
    double spot_per_day);
void review_salvage(World& w);

}  // namespace

namespace {

constexpr double kYearS = 365.25 * 86400.0;
constexpr double kDerelictSampleS = 2.0 * 86400.0;

std::uint64_t fnv1a(std::uint64_t hash, const std::string& text) {
    for (const char ch : text) {
        hash = (hash ^ static_cast<unsigned char>(ch)) * 1099511628211ULL;
    }
    return (hash ^ 0xffULL) * 1099511628211ULL;  // a separator, so "ab"+"c" differs from "a"+"bc"
}

double ship_uniform(const World& w, const std::string& ship_id, const std::string& salt) {
    std::uint64_t hash = 1469598103934665603ULL ^ w.data.universe.event_seed;
    hash = fnv1a(hash, ship_id);
    hash = fnv1a(hash, salt);
    std::mt19937_64 rng(hash);
    rng.discard(4);
    // 53 random bits, shifted off zero: (0, 1].
    return (static_cast<double>(rng() >> 11) + 1.0) / 9007199254740992.0;
}

double weibull_scale_s(const World& w) {
    const auto& f = w.data.universe.failures;
    const double mean_s = w.data.universe.ship_operations.lifetime_years * f.mean_life_factor * kYearS;
    return mean_s / std::tgamma(1.0 + 1.0 / f.weibull_shape);
}

void draw_failure_clock(World& w, domain::ShipState& ship, bool aged) {
    const double k = w.data.universe.failures.weibull_shape;
    const double lambda = weibull_scale_s(w);
    double age_s = 0.0;
    if (aged) {
        // The steady-state age of a fleet that replaces what fails has density ∝ survival S(a):
        // draw by rejection under S on [0, 4λ] (S(4λ) is negligible for k >= 1).
        for (int attempt = 0; attempt < 1000; ++attempt) {
            const double a = 4.0 * lambda * ship_uniform(w, ship.id, std::format("age{}", attempt));
            if (ship_uniform(w, ship.id, std::format("accept{}", attempt)) <= std::exp(-std::pow(a / lambda, k))) {
                age_s = a;
                break;
            }
        }
    }
    // The life, conditional on having reached age_s: S(T)/S(a) = U.
    const double u = ship_uniform(w, ship.id, aged ? "life_aged" : "life");
    const double life_s = lambda * std::pow(std::pow(age_s / lambda, k) - std::log(u), 1.0 / k);
    ship.built_s = w.state.game_time_s - age_s;
    ship.fails_at_s = ship.built_s + std::max(life_s, age_s + 86400.0);
    ship.fault_pending = false;
}

}  // namespace

void init_failure_clocks(World& w) {
    if (!failures_on(w)) {
        return;
    }
    static const bool trace = std::getenv("SPACETRAINS_TRACE_FAILURES") != nullptr;
    for (auto& ship : w.state.ships) {
        if (ship.fails_at_s <= 0.0) {
            draw_failure_clock(w, ship, ship.commissioned_s <= 0.0);
            if (trace) {
                std::cerr << std::format("[FAILURE] clock {}: built day {:.0f}, fails day {:.0f}\n", ship.name,
                    ship.built_s / 86400.0, ship.fails_at_s / 86400.0);
            }
        }
    }
}

namespace {

double cargo_value_cr(const World& w, const std::vector<domain::CargoLot>& cargo, const std::string& station_id) {
    const auto& station = query::get_station_definition(w, station_id);
    double value = 0.0;
    for (const auto& lot : cargo) {
        value += lot.contract_value > 0.0 ? lot.contract_value : lot.units * w.engines.economy.reference_price(station, lot.commodity_id);
    }
    return value;
}

}  // namespace


void step_failures(World& w) {
    if (!failures_on(w)) {
        return;
    }
    // Exposure for the mean-life factor: once a day, the cargo value in flight and the hull value.
    if (w.state.game_time_s >= w.audits.next_exposure_sample_s) {
        w.audits.next_exposure_sample_s = w.state.game_time_s + 86400.0;
        for (const auto& ship : w.state.ships) {
            w.audits.failure_audit.hull_value_cr_days += query::ship_design(w, ship).ship_value_cr;
            // What the owner paid for the cargo is what a failure takes (the margin is a profit
            // not made, which the charge need not cover).
            if (ship.phase == domain::ShipMissionPhase::InTransit) {
                w.audits.failure_audit.cargo_aboard_cr_days += ship.active_mission.purchase_cost;
            }
        }
    }
    for (std::size_t i = w.state.ships.size(); i-- > 0;) {
        auto& ship = w.state.ships[i];
        if (ship.fails_at_s <= 0.0) {
            draw_failure_clock(w, ship, false);  // a new hull (commissioned since the last tick)
        }
        if (!ship.fault_pending && w.state.game_time_s >= ship.fails_at_s) {
            if (ship.phase == domain::ShipMissionPhase::InTransit) {
                fail_ship(w, i);
                continue;
            }
            // Faults show under load: the next leg, at a point drawn from the ship's stream.
            ship.fault_pending = true;
            ship.fault_leg_fraction = ship_uniform(w, ship.id, "leg");
            ++w.audits.failure_audit.faults_armed_in_port;
        }
        if (ship.fault_pending && ship.phase == domain::ShipMissionPhase::InTransit) {
            const auto& mission = ship.active_mission;
            const double strikes_s = mission.departure_time_s
                + ship.fault_leg_fraction * std::max(0.0, mission.arrival_time_s - mission.departure_time_s);
            if (w.state.game_time_s >= strikes_s) {
                fail_ship(w, i);
            }
        }
    }
    step_derelicts(w);
}

namespace {

void fail_ship(World& w, std::size_t index) {
    auto& ship = w.state.ships[index];
    const auto& design = query::ship_design(w, ship);
    auto& mission = ship.active_mission;
    const bool local = query::local_leg(w, mission);
    const double age_years = (w.state.game_time_s - ship.built_s) / kYearS;
    const double cargo_value = mission.purchase_cost;
    ++w.audits.failure_audit.failures;
    ++w.audits.failure_audit.by_class[design.hull_id.empty() ? design.id : design.hull_id];
    w.audits.failure_audit.age_years_sum += age_years;
    w.audits.failure_audit.hulls_lost_cr += design.ship_value_cr;
    w.audits.failure_audit.cargo_lost_cr += cargo_value;

    // A racer that fails drops out of its race.
    for (auto& derelict : w.state.derelicts) {
        if (derelict.claimant_ship_id == ship.id) {
            derelict.claimant_ship_id.clear();
            derelict.claim_eta_s = 0.0;
        }
    }

    auto& home = query::get_station_state(w, ship.home_station_id);
    auto& destination = query::get_station_state(w, mission.destination_station_id);
    const auto& destination_def = query::get_station_definition(w, mission.destination_station_id);
    // People aboard leave in the lifeboat and are picked up and taken on to the destination by
    // rescue craft we do not simulate (assumption; a timed crew rescue is a later extension).
    systems::land_passengers(w, ship, destination);

    std::string where;
    if (local) {
        // Near a planet the station's tugs bring the hulk in at once: its cargo goes into the
        // destination's stock and the outside economy pays the station the scrap.
        const double scrap = w.data.universe.failures.scrap_fraction * design.ship_value_cr;
        for (const auto& lot : mission.cargo) {
            destination.inventory[lot.commodity_id] += lot.units;
        }
        w.state.outside_economy_credits -= scrap;
        destination.credits += scrap;
        w.state.journal.record(query::account_earth(), query::account_station(w, destination), MoneyCategory::Salvage, scrap);
        w.audits.failure_audit.scrap_paid_cr += scrap;
        ++w.audits.failure_audit.local_recoveries;
        query::add_event(w, std::format("{} broke down near {} after {:.1f} years in service; towed in, its cargo ({}) landed there",
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
        derelict.failed_at_s = w.state.game_time_s;
        derelict.expires_s = w.state.game_time_s + w.data.universe.failures.claim_days * 86400.0;
        // Its state now: position on the planned path, velocity across an hour either side.
        const auto at = [&](double t) {
            return interpolate_timed_sampled_path(mission.sampled_path, mission.sampled_times_s, t);
        };
        const double t0 = std::max(mission.sampled_times_s.empty() ? w.state.game_time_s : mission.sampled_times_s.front(), w.state.game_time_s - 3600.0);
        const double t1 = std::min(mission.sampled_times_s.empty() ? w.state.game_time_s : mission.sampled_times_s.back(), w.state.game_time_s + 3600.0);
        derelict.epoch_position_m = query::get_ship_render_position(w, ship);
        derelict.epoch_velocity_mps = t1 > t0 ? (at(t1) - at(t0)) / (t1 - t0) : math::Vec3d {};
        trajectory::sample_kepler(derelict.epoch_position_m, derelict.epoch_velocity_mps, w.state.game_time_s, derelict.expires_s,
            kDerelictSampleS, query::sun_mu(w), derelict.sampled_path, derelict.sampled_times_s);
        const double r_au = derelict.epoch_position_m.length() / 1.495978707e11;
        // Debug aid: SPACETRAINS_TRACE_FAILURES=1 logs each derelict's state at failure (stderr).
        if (static const bool trace = std::getenv("SPACETRAINS_TRACE_FAILURES") != nullptr; trace) {
            const double v = derelict.epoch_velocity_mps.length();
            std::cerr << std::format("[FAILURE] day {:.2f} {} ({}) {} -> {} type {} r {:.3f} AU v {:.0f} m/s circ {:.0f} samples {} "
                                     "t0 {:.2f} t1 {:.2f}\n",
                w.state.game_time_s / 86400.0, ship.name, ship.class_id, mission.origin_station_id, mission.destination_station_id,
                mission.trajectory_type, r_au, v, std::sqrt(query::sun_mu(w) / derelict.epoch_position_m.length()),
                mission.sampled_path.size(), t0 / 86400.0, t1 / 86400.0);
        }
        query::add_event(w, std::format("{} disabled {:.2f} AU from the Sun after {:.1f} years in service, drifting with {}",
            ship.name, r_au, age_years, mission.cargo.empty() ? "an empty hold" : describe_cargo(mission.cargo)), "alert");
        ++w.audits.failure_audit.derelicts;
        w.state.derelicts.push_back(std::move(derelict));
    }

    // The owner's first right to the lost ship's place under the cap.
    if (w.data.universe.failures.replacement_days > 0.0) {
        w.state.replacement_claims[ship.faction_id].push_back(w.state.game_time_s + w.data.universe.failures.replacement_days * 86400.0);
    }
    // The ship's cash, or its debt, goes to the home station, as when a ship is sold.
    home.credits += ship.credits;
    home.ledger.dividends += ship.credits;
    w.state.journal.record(query::account_fleet(w, ship.faction_id), query::account_station(w, home), MoneyCategory::ShipSaleCash, ship.credits);
    ship.ledger.dividends += ship.credits;
    ship.credits = 0.0;
    ship.phase = domain::ShipMissionPhase::Idle;
    ship.active_mission = {};
    w.state.sold_ships.push_back(std::move(ship));
    w.state.ships.erase(w.state.ships.begin() + static_cast<std::ptrdiff_t>(index));
}

void step_derelicts(World& w) {
    for (std::size_t i = w.state.derelicts.size(); i-- > 0;) {
        if (w.state.game_time_s >= w.state.derelicts[i].expires_s) {
            query::add_event(w, std::format("The wreck of {} is written off: nobody reached it in {:.0f} days", w.state.derelicts[i].name,
                w.data.universe.failures.claim_days), "alert");
            ++w.audits.failure_audit.expired;
            w.state.derelicts.erase(w.state.derelicts.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    // Replacement claims run out.
    for (auto& [faction_id, expiries] : w.state.replacement_claims) {
        const auto before = expiries.size();
        std::erase_if(expiries, [&](double until_s) { return w.state.game_time_s >= until_s; });
        w.audits.failure_audit.replacement_claims_lapsed += static_cast<int>(before - expiries.size());
    }
    if (w.data.universe.failures.salvage && !w.state.derelicts.empty() && w.state.game_time_s >= w.state.next_salvage_review_s) {
        w.state.next_salvage_review_s = w.state.game_time_s + w.data.universe.failures.salvage_review_days * 86400.0;
        review_salvage(w);
    }
}

bool can_race(const World& w, const domain::ShipState& ship) {
    // Docked and free (a laid-up ship rehires its crew for it): not bound by a contract, a liner's loop or a new ship's route,
    // and not carrying a fault that is about to show.
    return (ship.phase == domain::ShipMissionPhase::Idle || ship.phase == domain::ShipMissionPhase::LaidUp) && !systems::under_contract(w, ship) && systems::liner_stops(w, ship) == nullptr
        && !(!ship.route_destination_id.empty() && w.state.game_time_s < ship.route_until_s) && !ship.fault_pending
        && ship.current_station_id.size() > 0;
}

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

SalvagePlan plan_salvage(const World& w, const domain::ShipState& ship, const domain::DerelictState& derelict,
    bool both_legs, double latest_meet_s, double fuel_cr_per_kg, double cr_per_day) {
    // The cheapest salvage run, as the trade planners pick a trip: propellant at the origin's fuel
    // price plus the ship's daily costs, less the prize where it is sold (ports differ); the way out
    // and the way home are chosen together. A race only adds the deadline `latest_meet_s` (meet the
    // wreck before the current claimant, by the margin).
    SalvagePlan plan;
    const auto& design = query::ship_design(w, ship);
    const double mu = query::sun_mu(w);
    const double now = w.state.game_time_s;
    const double meet_limit_s = std::min(derelict.expires_s, latest_meet_s);
    if (meet_limit_s - now < 2.0 * 86400.0) {
        return plan;
    }
    const auto derelict_at = [&](double t) {
        return trajectory::propagate_kepler(derelict.epoch_position_m, derelict.epoch_velocity_mps, t - derelict.failed_at_s, mu);
    };
    const double load = std::min(design.propellant_capacity_kg, ship.propellant_kg + query::purchasable_propellant_kg(w, ship));
    if (load <= 0.0) {
        return plan;
    }
    const auto taken = lots_that_fit(derelict.cargo, design.cargo_capacity_units);
    double cargo_kg = 0.0;
    for (const auto& lot : taken) {
        cargo_kg += lot.units * query::get_commodity(w, lot.commodity_id).mass_per_unit_kg;
    }
    const double dry = design.dry_mass_kg;
    const double reserve = PROPELLANT_RESERVE_FRACTION * load;
    const auto& origin = query::get_station_definition(w, ship.current_station_id);
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
            value += lot.units * w.engines.economy.reference_price(query::get_station_definition(w, port), lot.commodity_id);
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
        const auto r0 = w.engines.mechanics.get_station_position(origin, now);
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
                const auto planet_id = query::primary_body_id(w, port);
                for (double t = rendezvous_s; t <= rendezvous_s + 500.0 * 86400.0; t += 2.0 * 86400.0) {
                    planet.sampled_path.push_back(w.engines.mechanics.get_body_position(planet_id, t));
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
        const auto r0 = w.engines.mechanics.get_station_position(origin, depart);
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
            const auto planet_id = query::primary_body_id(w, port);
            const double prize = prize_kg(port);
            for (int i = 0; i < 30; ++i) {
                const double transit = 10.0 * 86400.0 * std::pow(50.0, i / 29.0);  // 10 to 500 days
                const double arrive = leg.rendezvous_s + transit;
                const auto rp = w.engines.mechanics.get_body_position(planet_id, arrive);
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

void start_salvage_run(World& w, domain::ShipState& ship, domain::DerelictState& derelict, const SalvagePlan& plan,
    double value_cr, double cost_cr) {
    const auto& design = query::ship_design(w, ship);
    auto& origin_state = query::get_station_state(w, ship.current_station_id);
    const auto& origin_def = query::get_station_definition(w, ship.current_station_id);
    const auto& destination_def = query::get_station_definition(w, plan.destination_id);
    systems::buy_propellant(w, ship, plan.propellant_load_kg - ship.propellant_kg);
    const double mission_days = (plan.arrival_s - w.state.game_time_s) / 86400.0;
    ship.leg_dose_msv = 0.0;
    systems::buy_provisions(w, ship, origin_state, mission_days * PROVISION_MARGIN + std::max(PROVISION_MIN_RETURN_DAYS, mission_days), true);
    systems::buy_provisions(w, ship, origin_state, PROVISION_ENDURANCE_DAYS, false);
    const bool plasma = design.propulsion_type == "variable_isp";
    if (!plasma) {
        // Impulsive burns: booked at the start, as for other nuclear-thermal missions.
        ship.propellant_kg -= plan.propellant_used_kg;
        if (w.data.universe.ship_environment.wear) {
            domain::TrajectoryPlan burn;
            burn.propellant_required_kg = plan.propellant_used_kg;
            ship.engine_wear = std::min(1.0, ship.engine_wear + query::drive_hours(design, burn) / query::drive_life_h(w, design));
        }
    }
    const double time_cost_per_day = query::daily_capital_cost(w, design) + systems::daily_crew_cost(w, design, origin_state)
        + query::wear_cr_per_flight_day(w, design);
    const bool waits = plan.depart_s > w.state.game_time_s;
    ship.phase = waits ? domain::ShipMissionPhase::AwaitingDeparture : domain::ShipMissionPhase::InTransit;
    domain::MissionAssignment mission;
    mission.origin_station_id = origin_def.id;
    mission.destination_station_id = plan.destination_id;
    mission.departure_time_s = plan.depart_s;
    mission.arrival_time_s = plan.arrival_s;
    mission.wait_time_s = plan.depart_s - w.state.game_time_s;
    mission.coast_time_s = plan.arrival_s - plan.depart_s;
    mission.total_travel_time_s = plan.arrival_s - w.state.game_time_s;
    mission.remaining_travel_time_s = mission.total_travel_time_s;
    mission.propellant_cost_kg = plan.propellant_used_kg;
    mission.fuel_cost = plan.propellant_used_kg / FUEL_UNITS_TO_KG * systems::station_price(w, origin_state, "fuel");
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
        for (const auto& other : w.state.ships) {
            if (other.id == derelict.claimant_ship_id) {
                overtaken = other.name;
            }
        }
    }
    derelict.claimant_ship_id = ship.id;
    derelict.claim_eta_s = plan.rendezvous_s;
    ++derelict.racers;
    ++w.audits.failure_audit.racers_sent;
    query::add_event(w, std::format("{} {} {} to salvage the wreck of {}: rendezvous day {:.0f}{}, then to {} (worth ~{:.0f} cr, costs ~{:.0f})",
        ship.name, waits ? "will leave" : "sets out from", origin_def.name, derelict.name, plan.rendezvous_s / 86400.0,
        overtakes ? std::format(", ahead of {}", overtaken.empty() ? std::string("an earlier racer") : overtaken) : std::string(),
        destination_def.name, value_cr, cost_cr), "mission");
}

std::optional<SalvageOption> best_salvage_for(const World& w, const domain::ShipState& ship, double spot_per_day) {
    const auto& f = w.data.universe.failures;
    if (!f.salvage || w.state.derelicts.empty() || !can_race(w, ship)) {
        return std::nullopt;
    }
    std::optional<SalvageOption> best;
    const auto& design = query::ship_design(w, ship);
    const auto& origin_state = query::get_station_state(w, ship.current_station_id);
    const double per_day_cost = query::daily_capital_cost(w, design) + systems::daily_crew_cost(w, design, origin_state) + query::wear_cr_per_flight_day(w, design);
    const double fuel_cr_per_kg = systems::station_price(w, origin_state, "fuel") / FUEL_UNITS_TO_KG;
    for (std::size_t i = 0; i < w.state.derelicts.size(); ++i) {
        const auto& derelict = w.state.derelicts[i];
        if (derelict.racers >= static_cast<int>(f.max_racers)) {
            continue;
        }
        // A new racer must meet the wreck sooner than the current claimant, by the margin.
        const double best_s = derelict.claimant_ship_id.empty() ? std::numeric_limits<double>::infinity()
                                                                : derelict.claim_eta_s - w.state.game_time_s;
        const double latest_meet_s = w.state.game_time_s + best_s * (1.0 - f.racer_margin);
        const auto out = plan_salvage(w, ship, derelict, false, latest_meet_s, fuel_cr_per_kg, per_day_cost);
        if (!out.feasible || out.rendezvous_s - w.state.game_time_s >= best_s * (1.0 - f.racer_margin)) {
            continue;
        }
        // Even the whole prize over the way out alone must beat the best mission: the bound
        // spares the way home's planning most of the time.
        double prize = f.scrap_fraction * derelict.ship_value_cr;
        for (const auto& lot : lots_that_fit(derelict.cargo, design.cargo_capacity_units)) {
            prize += lot.units * w.engines.economy.reference_price(query::get_station_definition(w, derelict.destination_station_id), lot.commodity_id);
        }
        if (1.5 * prize / std::max(1.0, (out.rendezvous_s - w.state.game_time_s) / 86400.0) <= std::max(0.0, spot_per_day)) {
            continue;
        }
        auto full = plan_salvage(w, ship, derelict, true, latest_meet_s, fuel_cr_per_kg, per_day_cost);
        if (!full.feasible || full.rendezvous_s - w.state.game_time_s >= best_s * (1.0 - f.racer_margin)) {
            continue;
        }
        const auto& port = query::get_station_definition(w, full.destination_id);
        double value = f.scrap_fraction * derelict.ship_value_cr;
        for (const auto& lot : lots_that_fit(derelict.cargo, design.cargo_capacity_units)) {
            value += lot.units * w.engines.economy.reference_price(port, lot.commodity_id);
        }
        const double days = std::max(1.0, (full.arrival_s - w.state.game_time_s) / 86400.0);
        const double cost = full.propellant_used_kg / FUEL_UNITS_TO_KG * systems::station_price(w, origin_state, "fuel") + days * per_day_cost;
        const double per_day = (value - cost) / days;
        if (value > cost && (!best || per_day > best->per_day)) {
            best = SalvageOption {.derelict_index = i, .plan = std::move(full), .value_cr = value, .cost_cr = cost, .per_day = per_day};
        }
    }
    return best;
}

}  // namespace

bool consider_salvage(World& w, domain::ShipState& ship, double spot_per_day) {
    // Planning a chase takes milliseconds: a ship that passed looks again after a few days.
    constexpr double kLookAgainDays = 5.0;
    if (w.state.game_time_s < ship.next_salvage_look_s) {
        return false;
    }
    ship.next_salvage_look_s = w.state.game_time_s + kLookAgainDays * 86400.0;
    const auto option = best_salvage_for(w, ship, spot_per_day);
    static const bool trace = std::getenv("SPACETRAINS_TRACE_SALVAGE") != nullptr;
    if (trace && option) {
        std::cerr << std::format("[SALVAGE] day {:.1f} {} at {}: wreck of {} pays {:.0f} cr/day against {:.0f} (worth {:.0f}, costs {:.0f})\n",
            w.state.game_time_s / 86400.0, ship.name, ship.current_station_id, w.state.derelicts[option->derelict_index].name, option->per_day,
            spot_per_day, option->value_cr, option->cost_cr);
    }
    if (!option || option->per_day <= spot_per_day) {
        return false;
    }
    if (ship.phase == domain::ShipMissionPhase::LaidUp) {
        query::add_event(w, std::format("{} rehired its crew at {}", ship.name, query::get_station_definition(w, ship.current_station_id).name), "mission");
    }
    start_salvage_run(w, ship, w.state.derelicts[option->derelict_index], option->plan, option->value_cr, option->cost_cr);
    return true;
}

namespace {

void review_salvage(World& w) {
    // Ships that found nothing to do (idle or laid up) look at the open derelicts now and then;
    // busy ones weigh salvage against their missions at dispatch (consider_salvage).
    for (std::size_t i = 0; i < w.state.ships.size(); ++i) {
        if (can_race(w, w.state.ships[i])) {
            (void)consider_salvage(w, w.state.ships[i], 0.0);
        }
    }
}

}  // namespace

void reach_derelict(World& w, domain::ShipState& ship) {
    auto& mission = ship.active_mission;
    mission.salvage_done = true;
    const auto it = std::find_if(w.state.derelicts.begin(), w.state.derelicts.end(), [&](const auto& d) { return d.id == mission.salvage_derelict_id; });
    const auto& destination = query::get_station_definition(w, mission.destination_station_id);
    if (it == w.state.derelicts.end()) {
        ++w.audits.failure_audit.races_lost;
        query::add_event(w, std::format("{} reaches the drift of a wreck already taken; heads on to {} empty", ship.name, destination.name),
            "mission");
        return;
    }
    const auto& design = query::ship_design(w, ship);
    const auto taken = lots_that_fit(it->cargo, design.cargo_capacity_units);
    for (const auto& lot : taken) {
        mission.cargo.push_back(lot);
    }
    const double scrap = w.data.universe.failures.scrap_fraction * it->ship_value_cr;
    w.state.outside_economy_credits -= scrap;
    ship.credits += scrap;
    ship.lifetime_profit += scrap;
    ship.ledger.cargo_revenue += scrap;
    w.state.journal.record(query::account_earth(), query::account_fleet(w, ship.faction_id), MoneyCategory::Salvage, scrap);
    w.audits.failure_audit.scrap_paid_cr += scrap;
    w.audits.failure_audit.cargo_recovered_cr += cargo_value_cr(w, taken, destination.id);
    ++w.audits.failure_audit.salvaged;
    query::add_event(w, std::format("{} salvages the wreck of {} ({:.0f} cr scrap{}); on to {}", ship.name, it->name, scrap,
        taken.empty() ? std::string() : ", cargo " + describe_cargo(taken), destination.name), "trade");
    w.state.derelicts.erase(it);
}

}  // namespace spacetrains::simulation::systems
