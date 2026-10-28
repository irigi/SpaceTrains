// Police inspections (docs/modules/ships_and_fleet.md, "Police inspections").
//
// Police cutters (plasma couriers) meet ships in flight. Cutters of the "earth" police (the default
// data) are paid by Earth and inspect ships of every faction; a faction's own cutters (a base with a
// faction id) are paid by its treasury and inspect only ships bound for its stations. A cutter
// meets its target in flight, rides alongside for a few days of inspection and leaves. Legal
// assumption: the inspected ship complies but is not made to change its trajectory, so the cutter matches it.
// The meeting point is the cheapest place on the target's planned path (trajectory::plan_meeting).
// No economic effect on the inspected ship yet: the seam for customs and smuggling later.

#include "simulation/Simulation.hpp"

#include "trajectory/ChaseSolver.hpp"
#include "trajectory/KeplerPropagation.hpp"
#include "trajectory/PathMotion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <iostream>
#include <limits>

#include "simulation/SimulationInternal.hpp"

namespace spacetrains::simulation {

using namespace detail;

namespace {

constexpr double kDayS = 86400.0;
constexpr std::size_t kCandidates = 3;
// The police of the default data: paid by Earth, inspecting ships of every faction.
constexpr std::string_view kEarthPolice = "earth";

math::Vec3d circular_velocity_at(const math::Vec3d& r, double mu) {
    const double rm = std::max(1.0, r.length());
    const double vc = std::sqrt(mu / rm);
    return {-r.z / rm * vc, 0.0, r.x / rm * vc};
}

// A ship's state on its planned path (a conic per segment, trajectory/PathMotion.hpp).
trajectory::KeplerState path_state(const std::vector<math::Vec3d>& path, const std::vector<double>& times, double t, double mu) {
    return trajectory::PathMotion(path, times, mu).at(t);
}

double interpolate_scalar(const std::vector<double>& values, const std::vector<double>& times, double t) {
    return interpolate_timed_scalar(values, times, t);
}

}  // namespace

void Simulation::init_patrols() {
    const auto& p = universe_.patrols;
    if (!p.patrols) {
        return;
    }
    const auto& design = get_ship_class(p.class_id);
    int index = 0;
    for (const auto& base : p.bases) {
        std::string initials;
        for (const auto& faction : universe_.factions) {
            if (faction.id != base.faction_id) {
                continue;
            }
            bool word_start = true;
            for (const char ch : faction.name) {
                if (word_start && ch != ' ') {
                    initials += ch;
                }
                word_start = ch == ' ';
            }
        }
        for (int k = 1; k <= base.ships; ++k) {
            domain::PatrolState patrol;
            patrol.id = std::format("patrol_{}_{}_{}", base.faction_id, base.station_id, k);
            ++index;
            patrol.name = base.faction_id == kEarthPolice ? std::format("Police Cutter {}", index)
                                                          : std::format("{} Cutter {}", initials, index);
            patrol.faction_id = base.faction_id;
            patrol.class_id = p.class_id;
            patrol.home_station_id = base.station_id;
            patrol.propellant_kg = design.propellant_capacity_kg;
            patrol.next_review_s = game_time_s_ + 0.25 * kDayS * index;
            patrols_.push_back(std::move(patrol));
        }
    }
}

void Simulation::pay_patrol(const domain::PatrolState& patrol, double amount) {
    if (amount <= 0.0) {
        return;
    }
    auto& home = get_station_state(patrol.home_station_id);
    home.credits += amount;
    if (patrol.faction_id == kEarthPolice) {
        // Earth's police: the outside economy pays, as it pays the scrap of wrecks.
        outside_economy_credits_ -= amount;
        journal_.record(account_earth(), account_station(home), MoneyCategory::Patrols, amount);
    } else {
        faction_treasuries_[patrol.faction_id] -= amount;
        journal_.record(account_faction(patrol.faction_id), account_station(home), MoneyCategory::Patrols, amount);
    }
    patrol_audit_.costs_cr += amount;
}

math::Vec3d Simulation::patrol_position(const domain::PatrolState& patrol) const {
    switch (patrol.phase) {
        case domain::PatrolPhase::Docked:
            return mechanics_.get_station_position(get_station_definition(patrol.home_station_id), game_time_s_);
        case domain::PatrolPhase::Inspecting:
            for (const auto& ship : ships_) {
                if (ship.id == patrol.target_ship_id) {
                    return get_ship_render_position(ship);
                }
            }
            return trajectory::propagate_kepler(patrol.meet_position_m, patrol.meet_velocity_mps, game_time_s_ - patrol.meet_s,
                sun_mu()).position_m;
        case domain::PatrolPhase::Outbound:
        case domain::PatrolPhase::Returning:
            break;
    }
    return interpolate_timed_sampled_path(patrol.path, patrol.times, game_time_s_);
}

trajectory::ChasePlan Simulation::plan_patrol_home(const domain::PatrolState& patrol, const math::Vec3d& r,
    const math::Vec3d& v, double t_s, double propellant_kg, double reserve_kg, bool estimate_only) const {
    const auto& design = get_ship_class(patrol.class_id);
    if (propellant_kg <= reserve_kg + 1.0) {
        return {};
    }
    domain::TrajectoryPlan planet;
    const auto planet_id = primary_body_id(patrol.home_station_id);
    for (double t = t_s; t <= t_s + 500.0 * kDayS; t += 2.0 * kDayS) {
        planet.sampled_path.push_back(mechanics_.get_body_position(planet_id, t));
        planet.sampled_times_s.push_back(t);
    }
    trajectory::ChaseUnits units;
    units.mu_m3_s2 = sun_mu();
    units.length_m = std::max(0.2 * 1.495978707e11, r.length());
    const trajectory::ChaseRequest request {
        .position_m = r,
        .velocity_mps = v,
        .depart_time_s = t_s,
        .power_w = design.specific_engine_power_w_per_kg * design.dry_mass_kg,
        .initial_mass_kg = design.dry_mass_kg + propellant_kg,
        .dry_mass_kg = design.dry_mass_kg + reserve_kg,
        .end_condition = variable_isp::general::EndCondition::Rendezvous,
    };
    // The cheapest way home (propellant against the cutter's time), not the fastest.
    const auto target = trajectory::path_target(planet, t_s, units);
    if (!estimate_only) {
        return trajectory::plan_cheapest(request, target, units, kDayS, 495.0 * kDayS, patrol_kg_per_s(patrol), 10, 6, 60);
    }
    const auto candidates = trajectory::meeting_candidates(request, target, units, kDayS, 495.0 * kDayS, patrol_kg_per_s(patrol), 8);
    trajectory::ChasePlan estimate;
    if (!candidates.empty()) {
        estimate.feasible = true;
        estimate.transfer_time_s = candidates.front().transfer_time_s;
        estimate.propellant_kg = candidates.front().propellant_kg;
    }
    return estimate;
}

// A cutter's day in propellant at its home port's fuel price: hull and crew.
double Simulation::patrol_kg_per_s(const domain::PatrolState& patrol) const {
    const auto& design = get_ship_class(patrol.class_id);
    const auto& home = get_station_state(patrol.home_station_id);
    const double fuel_cr_per_kg = station_price(home, "fuel") / FUEL_UNITS_TO_KG;
    const double cr_per_day = daily_capital_cost(design) + daily_crew_cost(design, home);
    return fuel_cr_per_kg > 0.0 ? cr_per_day / fuel_cr_per_kg / kDayS : 0.0;
}

Simulation::PatrolSortie Simulation::plan_sortie(const domain::PatrolState& patrol, const math::Vec3d& r, const math::Vec3d& v,
    const domain::ShipState& target) const {
    PatrolSortie sortie;
    const auto& p = universe_.patrols;
    const auto& design = get_ship_class(patrol.class_id);
    const auto& mission = target.active_mission;
    const double now = game_time_s_;
    const double inspection_s = p.inspection_days * kDayS;
    const double earliest = 2.0 * kDayS;
    const double latest = mission.arrival_time_s - now - inspection_s - kDayS;
    const double capacity = design.propellant_capacity_kg;
    const double load = patrol.propellant_kg;
    const double dry_end = design.dry_mass_kg + load - p.outbound_share * capacity;
    if (latest <= 1.5 * earliest || dry_end <= design.dry_mass_kg) {
        return sortie;
    }
    // Time is weighed against propellant at the home port's fuel price.
    const double kg_per_s = patrol_kg_per_s(patrol);

    trajectory::ChaseUnits units;
    units.mu_m3_s2 = sun_mu();
    units.length_m = std::max(0.2 * 1.495978707e11, r.length());
    const trajectory::PathMotion target_motion_si(mission.sampled_path, mission.sampled_times_s, sun_mu());
    domain::TrajectoryPlan target_path;
    target_path.sampled_path = mission.sampled_path;
    target_path.sampled_times_s = mission.sampled_times_s;
    const trajectory::ChaseRequest request {
        .position_m = r,
        .velocity_mps = v,
        .depart_time_s = now,
        .power_w = design.specific_engine_power_w_per_kg * design.dry_mass_kg,
        .initial_mass_kg = design.dry_mass_kg + load,
        .dry_mass_kg = dry_end,
        .end_condition = variable_isp::general::EndCondition::Rendezvous,
    };
    // The meeting is chosen with the way home: the cheapest few meeting times, each with the
    // cheapest way home from where the inspection ends (an allowance kept for matching the target).
    const auto target_motion = trajectory::path_target(target_path, now, units);
    const auto candidates = trajectory::meeting_candidates(request, target_motion, units, earliest, latest, kg_per_s, 8);
    const double follow_allowance = 0.05 * capacity;
    const trajectory::MeetingCandidate* chosen = nullptr;
    double best_total = std::numeric_limits<double>::infinity();
    for (std::size_t k = 0; k < std::min<std::size_t>(3, candidates.size()); ++k) {
        const auto& c = candidates[k];
        const double end_s = now + c.transfer_time_s + inspection_s;
        const auto leaving = target_motion_si.at(end_s);
        const auto home_leg = plan_patrol_home(patrol, leaving.position_m, leaving.velocity_mps, end_s,
            load - c.propellant_kg - follow_allowance, p.return_reserve * capacity, true);
        if (!home_leg.feasible) {
            continue;
        }
        const double total = c.objective + home_leg.propellant_kg + kg_per_s * (inspection_s + home_leg.transfer_time_s);
        if (total < best_total) {
            best_total = total;
            chosen = &c;
        }
    }
    if (chosen == nullptr) {
        return sortie;
    }
    const auto meet = trajectory::refine_meeting(request, target_motion, units, kg_per_s, *chosen, 6, 40);
    const double end_s = now + meet.transfer_time_s + inspection_s;
    const auto leaving = target_motion_si.at(end_s);
    const auto home_plan = plan_patrol_home(patrol, leaving.position_m, leaving.velocity_mps, end_s,
        load - meet.propellant_kg - follow_allowance, p.return_reserve * capacity, true);
    if (!home_plan.feasible) {
        return sortie;
    }
    sortie.feasible = true;
    sortie.target_ship_id = target.id;
    sortie.meet_s = now + meet.transfer_time_s;
    sortie.propellant_kg = meet.propellant_kg;
    sortie.path = meet.path_m;
    sortie.times = meet.times_s;
    for (std::size_t i = 0; i < sortie.times.size(); ++i) {
        const double share = sortie.times.size() > 1 ? static_cast<double>(i) / (sortie.times.size() - 1) : 1.0;
        sortie.propellant.push_back(load - share * meet.propellant_kg);
    }
    sortie.objective = meet.propellant_kg + kg_per_s * meet.transfer_time_s + home_plan.propellant_kg
        + kg_per_s * (inspection_s + home_plan.transfer_time_s);
    return sortie;
}

bool Simulation::start_sortie(domain::PatrolState& patrol, const math::Vec3d& r, const math::Vec3d& v) {
    const auto& p = universe_.patrols;
    const double now = game_time_s_;
    // Ships on an interplanetary leg (bound for this faction's stations, for a faction's cutter), not inspected lately and
    // not already someone's target; the nearest few are planned.
    std::vector<std::pair<double, std::size_t>> near;
    for (std::size_t i = 0; i < ships_.size(); ++i) {
        const auto& ship = ships_[i];
        const auto& mission = ship.active_mission;
        if (ship.phase != domain::ShipMissionPhase::InTransit || mission.sampled_path.size() < 2
            || mission.arrival_time_s - now < (p.inspection_days + 4.0) * kDayS
            || now - ship.last_inspected_s < p.reinspect_days * kDayS || local_leg(mission)
            || (patrol.faction_id != kEarthPolice
                && get_station_definition(mission.destination_station_id).faction_id != patrol.faction_id)) {
            continue;
        }
        if (std::any_of(patrols_.begin(), patrols_.end(), [&](const auto& other) {
                return &other != &patrol && other.target_ship_id == ship.id
                    && (other.phase == domain::PatrolPhase::Outbound || other.phase == domain::PatrolPhase::Inspecting);
            })) {
            continue;
        }
        near.emplace_back((get_ship_render_position(ship) - r).length(), i);
    }
    std::sort(near.begin(), near.end());
    if (near.size() > kCandidates) {
        near.resize(kCandidates);
    }
    std::vector<PatrolSortie> out(near.size());
    thread_pool_->parallel_for(near.size(), [&](std::size_t k) {
        out[k] = plan_sortie(patrol, r, v, ships_[near[k].second]);
    });
    std::size_t best = out.size();
    for (std::size_t k = 0; k < out.size(); ++k) {
        if (out[k].feasible && (best == out.size() || out[k].objective < out[best].objective)) {
            best = k;
        }
    }
    if (best == out.size()) {
        ++patrol_audit_.no_target_reviews;
        return false;
    }
    auto& sortie = out[best];
    const auto& target = ships_[near[best].second];
    patrol.phase = domain::PatrolPhase::Outbound;
    patrol.target_ship_id = target.id;
    patrol.target_name = target.name;
    patrol.meet_s = sortie.meet_s;
    patrol.path = std::move(sortie.path);
    patrol.times = std::move(sortie.times);
    patrol.propellant = std::move(sortie.propellant);
    ++patrol_audit_.sorties;
    patrol_audit_.meeting_days_sum += (sortie.meet_s - now) / kDayS;
    add_event(std::format("{} sets out to inspect {} bound for {}: meets it in {:.0f} days", patrol.name, target.name,
        get_station_definition(target.active_mission.destination_station_id).name, (sortie.meet_s - now) / kDayS), "mission");
    return true;
}

void Simulation::send_patrol_home(domain::PatrolState& patrol, const math::Vec3d& r, const math::Vec3d& v) {
    const auto& p = universe_.patrols;
    const auto& design = get_ship_class(patrol.class_id);
    auto plan = plan_patrol_home(patrol, r, v, game_time_s_, patrol.propellant_kg, p.return_reserve * design.propellant_capacity_kg);
    if (!plan.feasible) {
        ++patrol_audit_.returns_replanned_short;
        plan = plan_patrol_home(patrol, r, v, game_time_s_, patrol.propellant_kg, 0.01 * design.propellant_capacity_kg);
    }
    patrol.phase = domain::PatrolPhase::Returning;
    patrol.target_ship_id.clear();
    patrol.path.clear();
    patrol.times.clear();
    patrol.propellant.clear();
    if (plan.feasible) {
        patrol.path = plan.path_m;
        patrol.times = plan.times_s;
        for (std::size_t i = 0; i < patrol.times.size(); ++i) {
            const double share = patrol.times.size() > 1 ? static_cast<double>(i) / (patrol.times.size() - 1) : 1.0;
            patrol.propellant.push_back(patrol.propellant_kg - share * plan.propellant_kg);
        }
        patrol.arrive_s = game_time_s_ + plan.transfer_time_s;
    } else {
        // No way home on what is left (should not happen: sorties check one): a tanker brings it
        // in, shown as a straight line over 200 days.
        const double arrive = game_time_s_ + 200.0 * kDayS;
        patrol.path = {r, mechanics_.get_station_position(get_station_definition(patrol.home_station_id), arrive)};
        patrol.times = {game_time_s_, arrive};
        patrol.propellant = {patrol.propellant_kg, patrol.propellant_kg};
        patrol.arrive_s = arrive;
        add_event(std::format("{} is out of propellant and is brought home by a tanker", patrol.name), "alert");
    }
}

void Simulation::step_patrols(double dt_s) {
    if (!universe_.patrols.patrols) {
        return;
    }
    const auto& p = universe_.patrols;
    for (auto& patrol : patrols_) {
        const auto& design = get_ship_class(patrol.class_id);
        auto& home = get_station_state(patrol.home_station_id);
        // Hull and crew, paid by the faction to the home port.
        pay_patrol(patrol, (daily_capital_cost(design) + daily_crew_cost(design, home)) * dt_s / kDayS);
        switch (patrol.phase) {
            case domain::PatrolPhase::Docked: {
                // Refuel from the port's stock, paid by the faction.
                const double want_units = (design.propellant_capacity_kg - patrol.propellant_kg) / FUEL_UNITS_TO_KG;
                const double units = std::clamp(want_units, 0.0, std::max(0.0, home.inventory["fuel"]));
                if (units > 1.0e-6) {
                    pay_patrol(patrol, units * station_price(home, "fuel"));
                    home.inventory["fuel"] -= units;
                    home.demand_units["fuel"] += units;
                    patrol.propellant_kg += units * FUEL_UNITS_TO_KG;
                }
                if (game_time_s_ >= patrol.next_review_s) {
                    patrol.next_review_s = game_time_s_ + p.review_days * kDayS;
                    const auto r = mechanics_.get_station_position(get_station_definition(patrol.home_station_id), game_time_s_);
                    (void)start_sortie(patrol, r, circular_velocity_at(r, sun_mu()));
                }
                break;
            }
            case domain::PatrolPhase::Outbound: {
                patrol.propellant_kg = std::max(0.0, interpolate_scalar(patrol.propellant, patrol.times, game_time_s_));
                if (game_time_s_ < patrol.meet_s) {
                    break;
                }
                const auto it = std::find_if(ships_.begin(), ships_.end(), [&](const auto& s) { return s.id == patrol.target_ship_id; });
                if (it == ships_.end() || it->phase != domain::ShipMissionPhase::InTransit
                    || it->active_mission.arrival_time_s <= game_time_s_ + 0.5 * kDayS) {
                    // The target is gone (arrived, failed): home from here.
                    const auto here = path_state(patrol.path, patrol.times, game_time_s_, sun_mu());
                    send_patrol_home(patrol, here.position_m, here.velocity_mps);
                    break;
                }
                patrol.phase = domain::PatrolPhase::Inspecting;
                patrol.inspect_until_s = std::min(game_time_s_ + p.inspection_days * kDayS,
                    it->active_mission.arrival_time_s - 0.5 * kDayS);
                const auto met = path_state(it->active_mission.sampled_path, it->active_mission.sampled_times_s, game_time_s_, sun_mu());
                patrol.meet_position_m = met.position_m;
                patrol.meet_velocity_mps = met.velocity_mps;
                add_event(std::format("{} comes alongside {} for inspection", patrol.name, it->name), "general");
                break;
            }
            case domain::PatrolPhase::Inspecting: {
                const auto it = std::find_if(ships_.begin(), ships_.end(), [&](const auto& s) { return s.id == patrol.target_ship_id; });
                const bool target_flying = it != ships_.end() && it->phase == domain::ShipMissionPhase::InTransit;
                if (target_flying && game_time_s_ < patrol.inspect_until_s) {
                    break;
                }
                const double elapsed = std::max(1.0, game_time_s_ - patrol.meet_s);
                const auto coast = trajectory::propagate_kepler(patrol.meet_position_m, patrol.meet_velocity_mps, elapsed, sun_mu());
                const auto alongside = target_flying
                    ? path_state(it->active_mission.sampled_path, it->active_mission.sampled_times_s, game_time_s_, sun_mu())
                    : coast;
                const math::Vec3d r = alongside.position_m;
                const math::Vec3d v = alongside.velocity_mps;
                // Matching the target's thrust: the velocity it gained over the coast, at constant
                // acceleration (effort = Δv² / T) on the cutter's power.
                const double dv = (v - coast.velocity_mps).length();
                const double power_w = design.specific_engine_power_w_per_kg * design.dry_mass_kg;
                const double m0 = design.dry_mass_kg + patrol.propellant_kg;
                const double m1 = 1.0 / (1.0 / m0 + dv * dv / elapsed / (2.0 * power_w));
                const double used = std::min(patrol.propellant_kg, m0 - m1);
                patrol.propellant_kg -= used;
                patrol_audit_.propellant_kg += used;
                if (it != ships_.end()) {
                    it->last_inspected_s = game_time_s_;
                    ++patrol.inspections;
                    ++patrol_audit_.inspections;
                    ++patrol_audit_.by_faction[it->faction_id];   // the inspected ship's faction
                    add_event(std::format("{} inspected {} over {:.1f} days and leaves it on its way to {}", patrol.name, it->name,
                        elapsed / kDayS, get_station_definition(it->active_mission.destination_station_id).name), "general");
                }
                patrol.target_ship_id.clear();
                if (patrol.propellant_kg >= 0.6 * design.propellant_capacity_kg && start_sortie(patrol, r, v)) {
                    break;
                }
                send_patrol_home(patrol, r, v);
                break;
            }
            case domain::PatrolPhase::Returning: {
                patrol.propellant_kg = std::max(0.0, interpolate_scalar(patrol.propellant, patrol.times, game_time_s_));
                if (game_time_s_ >= patrol.arrive_s) {
                    patrol.phase = domain::PatrolPhase::Docked;
                    patrol.path.clear();
                    patrol.times.clear();
                    patrol.propellant.clear();
                    patrol.next_review_s = game_time_s_ + p.review_days * kDayS;
                    add_event(std::format("{} is back at {}", patrol.name, get_station_definition(patrol.home_station_id).name),
                        "arrival");
                }
                break;
            }
        }
    }
}

}  // namespace spacetrains::simulation
