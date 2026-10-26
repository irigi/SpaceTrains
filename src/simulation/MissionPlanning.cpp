// Dispatch: leg estimates and the choice of a ship's next mission.

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

Simulation::LegEstimate Simulation::estimate_leg(
    const domain::ShipClassDefinition& ship_class,
    const domain::StationDefinition& origin,
    const domain::StationDefinition& destination,
    double departure_time_s,
    double available_propellant_kg,
    double payload_kg) {
    const profiling::Scope profile_scope(profiling::Phase::EstimateLeg);
    // Follow-up legs are only estimates, so departures share 5-day buckets, available
    // fuel 10%-of-tank buckets and payloads 2 t buckets (fuel at the bucket's lower edge,
    // payload at its upper edge: conservative). Ships of one class then reuse each
    // other's plans across a whole bucket. The ship is assumed to buy what it needs.
    constexpr double BUCKET_S = 5.0 * 86400.0;
    const auto bucket = static_cast<std::int64_t>(std::floor(departure_time_s / BUCKET_S));
    const double fuel_step_kg = std::max(1.0, ship_class.propellant_capacity_kg * 0.1);
    const auto fuel_bucket = static_cast<std::int64_t>(std::floor(available_propellant_kg / fuel_step_kg));
    constexpr double PAYLOAD_STEP_KG = 2000.0;
    const auto payload_bucket = static_cast<std::int64_t>(std::ceil(std::max(0.0, payload_kg) / PAYLOAD_STEP_KG));
    leg_estimates_.erase(leg_estimates_.begin(),
        leg_estimates_.lower_bound(static_cast<std::int64_t>(std::floor(game_time_s_ / BUCKET_S))));
    auto& slot = leg_estimates_[bucket];
    const auto key = std::format("{}|{}|{}|{}|{}", ship_class.id, origin.id, destination.id, fuel_bucket, payload_bucket);
    // Measured from this caller's departure, not the bucket edge.
    // A leg that would take its crew past the career limit is not flown (tier 2 radiation).
    const auto for_caller = [&, departure_time_s](LegEstimate estimate) {
        estimate.travel_days = std::max(0.0, estimate.arrival_time_s - departure_time_s) / 86400.0;
        if (!leg_dose_allowed(ship_class, origin, destination, departure_time_s, estimate.travel_days)) {
            estimate.feasible = false;
        }
        return estimate;
    };
    if (const auto it = slot.find(key); it != slot.end()) {
        return it->second.feasible ? for_caller(it->second) : it->second;
    }
    domain::ShipState probe;
    probe.class_id = ship_class.id;
    probe.current_station_id = origin.id;
    probe.propellant_kg = 0.0;
    const auto& planner = (ship_class.propulsion_type == "variable_isp" && variable_isp_planner_)
        ? static_cast<trajectory::ITrajectoryPlanner&>(*variable_isp_planner_)
        : static_cast<trajectory::ITrajectoryPlanner&>(*kepler_planner_);
    const double purchasable_kg = static_cast<double>(fuel_bucket) * fuel_step_kg;
    if (purchasable_kg <= 0.0) {
        slot.emplace(key, LegEstimate {});
        return {};
    }
    // Plan with this class's typical costs: provisions at base prices, so an estimate does
    // not depend on the port's prices on the day the bucket was first planned. (Built only
    // here: nearly every call is answered from the cache above.)
    double typical_crew_cost = ship_class.crew_size * universe_.ship_operations.wage_cr_per_crew_day;
    for (const auto& [commodity_id, units_per_crew_day] : universe_.ship_operations.life_support_units_per_crew_day) {
        typical_crew_cost += ship_class.crew_size * units_per_crew_day * get_commodity(commodity_id).base_price;
    }
    const trajectory::PlanningOptions options {
        .propellant_cr_per_kg = get_commodity("fuel").base_price / FUEL_UNITS_TO_KG + wear_cr_per_propellant_kg(ship_class),
        .time_cr_per_day = daily_capital_cost(ship_class) + typical_crew_cost
            + hazard_cr_per_day(ship_class, origin, destination, static_cast<double>(bucket) * BUCKET_S)
            + wear_cr_per_flight_day(ship_class),
        .payload_kg = static_cast<double>(payload_bucket) * PAYLOAD_STEP_KG,
        .purchasable_propellant_kg = purchasable_kg,
        .reserve_fraction = PROPELLANT_RESERVE_FRACTION,
        // Boil-off averaged over the leg (the leak falls about as 1/r).
        .boiloff_kg_per_day = leg_boiloff_kg_per_day(ship_class, origin, destination,
            static_cast<double>(bucket) * BUCKET_S),
        .include_path = false,
    };
    // Planned from the bucket's start, whenever the bucket is first asked for, so an
    // estimate does not depend on which ship asked first (or on a save and load).
    const double plan_departure_s = static_cast<double>(bucket) * BUCKET_S;
    const auto to_estimate = [](const domain::TrajectoryPlan& plan) {
        return LegEstimate {
            .feasible = plan.feasible,
            .propellant_kg = plan.propellant_required_kg + plan.propellant_boiled_kg,
            .arrival_time_s = plan.arrival_time_s,
        };
    };
    if (deferred_plans_ != nullptr) {
        // Inside a choose_mission pass: plan later, in parallel with the others.
        defer_plan(std::format("leg|{}|{}", bucket, key), {
            .compute = [&planner, &origin, &destination, probe, &ship_class, plan_departure_s, options] {
                return planner.plan_transfer(origin, destination, probe, ship_class, plan_departure_s, options);
            },
            .commit = [this, bucket, key, to_estimate](domain::TrajectoryPlan&& plan) {
                leg_estimates_[bucket].emplace(key, to_estimate(plan));
            },
        });
        return {};
    }
    const auto estimate = to_estimate(planner.plan_transfer(origin, destination, probe, ship_class, plan_departure_s, options));
    slot.emplace(key, estimate);
    return estimate.feasible ? for_caller(estimate) : estimate;
}

void Simulation::defer_plan(const std::string& key, DeferredPlan request) {
    ++missing_plans_;
    if (deferred_keys_.insert(key).second) {
        deferred_plans_->push_back(std::move(request));
    }
}

Simulation::MissionChoice Simulation::choose_mission(
    const domain::ShipState& ship, const domain::ShipClassDefinition& ship_class, bool trace,
    double earliest_departure_s, bool cargo_only) {
    const profiling::Scope profile_scope(profiling::Phase::ChooseMission);
    std::unordered_map<std::string, domain::TrajectoryPlan> plans;
    for (;;) {
        std::vector<DeferredPlan> deferred;
        deferred_plans_ = &deferred;
        deferred_keys_.clear();
        deferred_prefix_.clear();
        std::string trace_text;
        auto choice = choose_mission_pass(ship, ship_class, trace, earliest_departure_s, cargo_only, plans, trace_text);
        deferred_plans_ = nullptr;
        if (deferred.empty()) {
            std::cerr << trace_text;
            return choice;
        }
        std::vector<domain::TrajectoryPlan> results(deferred.size());
        thread_pool_->parallel_for(deferred.size(), [&](std::size_t i) { results[i] = deferred[i].compute(); });
        for (std::size_t i = 0; i < deferred.size(); ++i) {
            deferred[i].commit(std::move(results[i]));
        }
    }
}

std::vector<Simulation::MissionChoice> Simulation::choose_missions(const std::vector<MissionRequest>& requests) {
    const profiling::Scope profile_scope(profiling::Phase::ChooseMission);
    std::vector<std::unordered_map<std::string, domain::TrajectoryPlan>> plans(requests.size());
    std::vector<MissionChoice> choices(requests.size());
    std::vector<bool> done(requests.size(), false);
    for (bool all_done = requests.empty(); !all_done;) {
        std::vector<DeferredPlan> deferred;
        deferred_plans_ = &deferred;
        deferred_keys_.clear();
        all_done = true;
        for (std::size_t r = 0; r < requests.size(); ++r) {
            if (done[r]) {
                continue;
            }
            const auto missing_before = missing_plans_;
            deferred_prefix_ = std::format("{}#", r);
            std::string trace_text;
            const auto& request = requests[r];
            choices[r] = choose_mission_pass(*request.ship, *request.ship_class, false, request.earliest_departure_s,
                request.cargo_only, plans[r], trace_text);
            // A pass that found every plan is final (a plan another request queued first in
            // this pass still counts as missing).
            done[r] = missing_plans_ == missing_before;
            all_done = all_done && done[r];
        }
        deferred_plans_ = nullptr;
        deferred_prefix_.clear();
        std::vector<domain::TrajectoryPlan> results(deferred.size());
        thread_pool_->parallel_for(deferred.size(), [&](std::size_t i) { results[i] = deferred[i].compute(); });
        for (std::size_t i = 0; i < deferred.size(); ++i) {
            deferred[i].commit(std::move(results[i]));
        }
    }
    return choices;
}

// One dispatch pass for one ship (see choose_mission_pass in Simulation.hpp): the pass state
// and its tools (planning, fuel, follow-up forecasts), then the scoring steps in order. A pass
// that asks for a plan not computed yet reads it as infeasible; choose_mission repeats it.
class Simulation::MissionPass {
public:
    MissionPass(Simulation& sim, const domain::ShipState& ship, const domain::ShipClassDefinition& ship_class,
        bool trace, double earliest_departure_s, bool cargo_only,
        std::unordered_map<std::string, domain::TrajectoryPlan>& plans, std::string& trace_text)
        : sim_(sim), ship(ship), ship_class(ship_class), trace(trace), earliest_departure_s(earliest_departure_s),
          cargo_only(cargo_only), plans(plans), trace_text(trace_text) {}

    MissionChoice run() {
        score_cargo_runs();
        score_berth_probes();
        score_cabin_module_probes();
        score_empty_legs();
        if (best_destination == nullptr && liner) {
            fly_next_stop_empty();
        }
        if (best_destination == nullptr && on_route && !liner && origin_def.id != ship.home_station_id) {
            fly_home_empty();
        }
        if (best_destination == nullptr && !cargo_only && !on_route) {
            reposition();
        }
        return finish();
    }

private:
    struct FuelAwarePlan {
        const domain::TrajectoryPlan* plan {nullptr};
        double carried_kg {0.0};  // return fuel loaded on top of the plan's own load
        bool feasible {false};
    };
    // Two-leg lookahead. A trip is only worth what the ship can do after it: a cargo run
    // into a port with nothing to carry away leaves the ship (and its costs) sitting
    // there. Every candidate leg is scored together with the best follow-up leg from its
    // destination, estimated from today's stocks and prices, or else with flying back
    // empty. Follow-ups are not urgency-weighted; they are only a forecast.
    struct FollowUp {
        double profit {0.0};
        double days {0.0};
        bool carries_cargo {false};
        std::string label {};
        std::string commodity_id {};
        double units {0.0};
    };
    struct Offer {
        std::string commodity_id;
        double surplus {0.0};
    };

    Simulation& sim_;
    const domain::ShipState& ship;
    const domain::ShipClassDefinition& ship_class;
    const bool trace;
    const double earliest_departure_s;
    const bool cargo_only;
    std::unordered_map<std::string, domain::TrajectoryPlan>& plans;
    std::string& trace_text;

    // Plans start at the earliest departure; forecasts of stocks count from now.
    const double delay_days = std::max(0.0, earliest_departure_s - sim_.game_time_s_) / 86400.0;
    domain::StationState& origin_state = sim_.get_station_state(ship.current_station_id);
    const domain::StationDefinition& origin_def = sim_.get_station_definition(ship.current_station_id);

    // Ships fuel per mission: the planner loads what the transfer burns plus the reserve,
    // from what is aboard and what this port can sell.
    const double origin_fuel_for_sale_kg = sim_.purchasable_propellant_kg(ship);
    const double provisions_kg = sim_.provisions_mass_kg(ship);

    double best_score = MIN_MISSION_SCORE_PER_DAY;
    const domain::StationDefinition* best_destination = nullptr;
    std::vector<domain::CargoLot> best_cargo;
    double best_cargo_margin = 0.0;
    const domain::TrajectoryPlan* best_plan = nullptr;
    double best_carried_kg = 0.0;
    std::string best_pickup_commodity;
    double best_pickup_units = 0.0;
    std::vector<MissionChoice::PassengerPick> best_passengers;
    std::vector<MissionChoice::CargoOption> cargo_options;

    // A newly commissioned ship works the route it was bought for: from home only to its route
    // destination, from anywhere else only home. Otherwise it left once the gap it was bought
    // for closed, the gap reopened and the treasuries ordered another ship for it.
    // A liner (step 22) flies only to the stop after the one it is at, forever.
    // A ship under a rotation contract flies its loop the same way.
    const bool liner = !cargo_only && sim_.scheduled_stops(ship) != nullptr;
    const bool on_route = liner || (!cargo_only && !ship.route_destination_id.empty() && sim_.game_time_s_ < ship.route_until_s);
    // Every day of a mission (waiting for the window included) costs capital,
    // wages and provisions; missions are compared on profit after that cost.
    const double time_cost_per_day = sim_.daily_capital_cost(ship_class) + sim_.daily_crew_cost(ship_class, origin_state)
        + sim_.hazard_cr_per_day(ship_class, origin_def, origin_def, sim_.game_time_s_) + sim_.wear_cr_per_flight_day(ship_class);
    // The crew must be provisionable from here for the whole mission.
    // (The return-leg reserve is bought too, but only if the station has it.)
    const double max_mission_days = sim_.provisionable_days(ship, origin_state) / PROVISION_MARGIN;
    // Cargo may be financed down to the credit line.
    const double spendable_credits = std::max(0.0, ship.credits + sim_.credit_line(ship_class));
    // The plan depends on the destination and the cargo mass, so plan each (destination,
    // cargo tonne) once per scoring pass rather than once per commodity. Plasma plans cost
    // ~8 ms each.
    const trajectory::ITrajectoryPlanner& planner = (ship_class.propulsion_type == "variable_isp" && sim_.variable_isp_planner_)
        ? static_cast<trajectory::ITrajectoryPlanner&>(*sim_.variable_isp_planner_)
        : static_cast<trajectory::ITrajectoryPlanner&>(*sim_.kepler_planner_);
    // Propellant is priced at what this port charges (never below base), so where fuel is
    // scarce the planners fly slower, cheaper transfers.
    const double planning_fuel_price = std::max(sim_.get_commodity("fuel").base_price, sim_.station_price(origin_state, "fuel"));
    const double exhaust_velocity_mps = trajectory::thermal_exhaust_velocity_mps(ship_class);
    // What each plan was planned with, so the chosen one can be planned again with its path
    // (its options are built in finish(), for that plan only).
    struct PlanInputs {
        const domain::StationDefinition* destination {nullptr};
        double cargo_kg {0.0};
        double rider_time_cr_per_day {0.0};
        bool full_tank {false};
    };
    std::unordered_map<const domain::TrajectoryPlan*, PlanInputs> plan_inputs;
    const double fuel_limit_kg = std::min(ship_class.propellant_capacity_kg,
        std::max(0.0, ship.propellant_kg) + origin_fuel_for_sale_kg);
    // Fuel burned on a mission comes out of the tank, already bought and not resalable,
    // so it is valued at the neutral base price, not the origin's scarcity price: a ship
    // holding 100 t of fuel at a fuel-starved port (16x) would otherwise never leave.
    // Burning it also wears a nuclear-thermal core (0 with wear off).
    const double origin_fuel_price = sim_.get_commodity("fuel").base_price + sim_.wear_cr_per_propellant_kg(ship_class) * FUEL_UNITS_TO_KG;
    // But fuel bought here for a mission is paid at this port's price: since only fuel
    // factories make fuel, a drained depot sells at up to 16x base, and that premium is a
    // real cost of leaving from here (the tanks' contents are not).
    const double origin_fuel_premium = std::max(0.0, sim_.station_price(origin_state, "fuel") - origin_fuel_price);
    std::unordered_map<std::string, std::vector<FollowUp>> follow_ups_by_destination;  // by destination, arrival and fuel
    const std::unordered_map<std::string, double>& origin_rates = sim_.economy_.get_station_net_rates(origin_def);
    std::vector<Offer> offers = origin_offers();
    // Propellant bought for the mission is paid at this port's price.
    const double marginal_fuel_price_per_kg = (origin_fuel_price + origin_fuel_premium) / FUEL_UNITS_TO_KG;
    // Passengers (docs/modules/ships_and_fleet.md) ride in berths, not the hold; they board here if a
    // trip to their destination costs them less than they will pay.
    const double berths = cargo_only ? 0.0 : ship_class.berths;
    const bool passengers_waiting = berths >= 1.0 && !origin_state.waiting.empty();
    int next_run = 0;
    bool repositioning = false;

    // Traces are kept until the pass that has every plan, so each line is printed once.
    void trace_line(const std::string& text) {
        if (trace) {
            trace_text += std::format("[trace day {:.1f}] {} ({}, {}) at {}: {}\n", sim_.game_time_s_ / 86400.0, ship.name,
                sim_.mission_phase_name(ship.phase), ship_class.id, ship.current_station_id, text);
        }
    }
    bool route_leg_allowed(const std::string& from_id, const std::string& to_id) {
        if (liner) {
            return to_id == sim_.next_scheduled_stop(ship, from_id);
        }
        return !on_route || to_id == (from_id == ship.home_station_id ? ship.route_destination_id : ship.home_station_id);
    }
    // No leg may take the crew signed on for it past its career dose limit (tier 2).
    bool dose_allows(const domain::StationDefinition& to, double days) {
        return sim_.leg_dose_allowed(ship_class, origin_def, to, sim_.game_time_s_, days);
    }
    // A ship must be able to leave its destination again: the fuel it has left on
    // arrival plus what the station can sell must cover a leg as long as this one,
    // flown without cargo. Without this, ships flew into fuel-dry consumers (Mercury)
    // and stayed there. For a nuclear-thermal ship the same Δv takes
    // m_dry * (mass_ratio - 1), with the plan's mass ratio; plasma ships are close enough
    // with the plan's own burn.
    // Returns the propellant missing for that, with `carried_kg` of return fuel aboard on top
    // of the plan's own load.
    double leave_shortfall_kg(const domain::StationDefinition& destination,
        const domain::TrajectoryPlan& plan, double payload_kg, double carried_kg, bool tankering = true) {
        const auto& dest_state = sim_.get_station_state(destination.id);
        // The load's boil-off allowance is gone on arrival.
        const double after_arrival_kg = std::max(0.0,
            plan.propellant_load_kg - plan.propellant_required_kg - plan.propellant_boiled_kg) + carried_kg;
        // Economic tankering: where the destination sells fuel at more than twice this port's
        // price, the ship carries its return fuel rather than buying it there.
        const double dest_fuel_kg = tankering && sim_.station_price(dest_state, "fuel") > TANKERING_PRICE_RATIO * planning_fuel_price
            ? 0.0
            : fuel_for_sale_on_arrival_kg(sim_.economy_, destination, dest_state,
                  delay_days + plan.travel_time_s / 86400.0);
        double needed_kg = plan.propellant_required_kg;
        if (ship_class.propulsion_type != "variable_isp" && exhaust_velocity_mps > 0.0) {
            const double planned_mass_kg = ship_class.dry_mass_kg + payload_kg + plan.propellant_load_kg;
            const double mass_ratio = planned_mass_kg
                / std::max(1.0, planned_mass_kg - plan.propellant_required_kg);
            needed_kg = (ship_class.dry_mass_kg + provisions_kg) * (mass_ratio - 1.0);
        }
        // The way back boils off too (as long a coast, the other way round).
        const double boiled_back_kg = sim_.leg_boiloff_kg_per_day(ship_class, destination, origin_def, sim_.game_time_s_)
            * plan.coast_time_s / 86400.0;
        return std::max(0.0,
            needed_kg * (1.0 + PROPELLANT_RESERVE_FRACTION) + boiled_back_kg - after_arrival_kg - dest_fuel_kg);
    }
    // Planners trade travel time against propellant at the owner's prices, and load
    // propellant for the transfer plus the reserve.
    // Hydrogen boil-off averaged over the leg (tier 2, step F).
    // Passengers aboard add their time (and dose) to the ship's: `rider_time_cr_per_day`.
    // A full-tank plan (fleet investment's passenger probes) may buy a whole tank: a route is
    // valued as it will be flown, not on the fuel this port happens to have on sale today.
    trajectory::PlanningOptions planning_options(const domain::StationDefinition& destination, double cargo_kg,
        double rider_time_cr_per_day = 0.0, bool full_tank = false) {
        return trajectory::PlanningOptions {
            .propellant_cr_per_kg = planning_fuel_price / FUEL_UNITS_TO_KG + sim_.wear_cr_per_propellant_kg(ship_class),
            .time_cr_per_day = time_cost_per_day + rider_time_cr_per_day,
            .payload_kg = provisions_kg + cargo_kg,
            .purchasable_propellant_kg = full_tank
                ? std::max(0.0, ship_class.propellant_capacity_kg - std::max(0.0, ship.propellant_kg))
                : origin_fuel_for_sale_kg,
            .reserve_fraction = PROPELLANT_RESERVE_FRACTION,
            .boiloff_kg_per_day = sim_.leg_boiloff_kg_per_day(ship_class, origin_def, destination, sim_.game_time_s_),
            .include_path = false,  // the chosen mission is planned again with its path
        };
    }
    const domain::TrajectoryPlan& plan_to(const domain::StationDefinition& destination, double cargo_kg,
        double rider_time_cr_per_day = 0.0, bool full_tank = false) {
        // Cargo rounded up to whole tonnes keeps the cache small and the estimate safe.
        const double cargo_bucket_kg = std::ceil(std::max(0.0, cargo_kg) / 1000.0) * 1000.0;
        // Riders adding less than rider_time_min_share of the ship's own time cost fly on the plan
        // without them; above, their time is bucketed in rider_time_relative_step of it.
        const auto& pax = sim_.universe_.passengers;
        const double riders = std::max(0.0, rider_time_cr_per_day);
        const double rider_step = std::max(1.0, pax.rider_time_relative_step * time_cost_per_day);
        const double rider_bucket = riders < pax.rider_time_min_share * time_cost_per_day
            ? 0.0 : std::ceil(riders / rider_step) * rider_step;
        auto key = rider_bucket > 0.0 ? std::format("{}|{:.0f}|t{:.3f}", destination.id, cargo_bucket_kg, rider_bucket)
                                      : std::format("{}|{:.0f}", destination.id, cargo_bucket_kg);
        if (full_tank) {
            key += "|full";
        }
        if (const auto it = plans.find(key); it != plans.end()) {
            plan_inputs[&it->second] = {&destination, cargo_bucket_kg, rider_bucket, full_tank};
            return it->second;
        }
        // Not planned yet: queue it and read it as infeasible for this pass.
        static const domain::TrajectoryPlan pending = [] {
            domain::TrajectoryPlan plan;
            plan.summary = "pending";
            return plan;
        }();
        sim_.defer_plan(sim_.deferred_prefix_ + "plan|" + key, {
            .compute = [&planner = planner, &origin_def = origin_def, &destination, &ship = ship, &ship_class = ship_class,
                           departure_s = std::max(sim_.game_time_s_, earliest_departure_s),
                           options = planning_options(destination, cargo_bucket_kg, rider_bucket, full_tank)] {
                return planner.plan_transfer(origin_def, destination, ship, ship_class, departure_s, options);
            },
            .commit = [&plans = plans, key](domain::TrajectoryPlan&& plan) { plans.emplace(key, std::move(plan)); },
        });
        return pending;
    }
    // Fuel-aware planning: where the destination's depot cannot refuel the ship for the next
    // leg, the ship carries the return fuel from here. That fuel rides as payload, so the leg
    // burns more; it must fit in the tanks and in what this port can sell. Depots without a
    // fuel factory only hold what tankers bring, so this is what keeps ships flying to them.
    FuelAwarePlan plan_with_return_fuel(const domain::StationDefinition& destination, double cargo_kg,
        double rider_time_cr_per_day = 0.0, bool full_tank = false) {
        const double limit_kg = full_tank ? ship_class.propellant_capacity_kg : fuel_limit_kg;
        const auto& plan = plan_to(destination, cargo_kg, rider_time_cr_per_day, full_tank);
        if (!plan.feasible) {
            return FuelAwarePlan {&plan, 0.0, false};
        }
        double carried_kg = leave_shortfall_kg(destination, plan, cargo_kg, 0.0);
        if (carried_kg <= 0.0) {
            return FuelAwarePlan {&plan, 0.0, true};
        }
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto& laden = plan_to(destination, cargo_kg + carried_kg, rider_time_cr_per_day, full_tank);
            if (!laden.feasible || laden.propellant_load_kg + carried_kg > limit_kg) {
                break;
            }
            const double missing_kg = leave_shortfall_kg(destination, laden, cargo_kg + carried_kg, carried_kg);
            if (missing_kg <= 0.0) {
                return FuelAwarePlan {&laden, carried_kg, true};
            }
            carried_kg += missing_kg;
        }
        // Tankering is a preference: a ship that cannot carry its return fuel buys it at the
        // destination, however dear (before, it could not fly to a dear port at all: no light
        // freighter went to Mars while Mars sold fuel at more than twice Earth's price).
        if (leave_shortfall_kg(destination, plan, cargo_kg, 0.0, false) <= 0.0) {
            return FuelAwarePlan {&plan, 0.0, true};
        }
        return FuelAwarePlan {&plan, 0.0, false};
    }
    double fuel_premium(const domain::TrajectoryPlan& plan, double carried_kg) {
        const double bought_kg = std::max(0.0, plan.propellant_load_kg + carried_kg - std::max(0.0, ship.propellant_kg));
        return bought_kg / FUEL_UNITS_TO_KG * origin_fuel_premium;
    }
    // A station's surplus left after the ships already docked there or inbound take their
    // loads, `days_ahead` from now (the producer keeps adding up to its stockpile limit).
    // An inbound ship takes the follow-up load it planned; the others are assumed to fill
    // their holds from the largest remaining surplus. (Before v33 every ship was assumed to
    // take the largest surplus: at Ceres a bulk good, so the platinum looked open to all.)
    std::unordered_map<std::string, double> open_surplus(const domain::StationDefinition& station_def,
        const domain::StationState& station_state, double days_ahead = 0.0) {
        const auto& rates = sim_.economy_.get_station_net_rates(station_def);
        std::unordered_map<std::string, double> surplus_by_commodity;
        for (const auto& [commodity_id, stock] : station_state.inventory) {
            const double rate = rates.contains(commodity_id) ? rates.at(commodity_id) : 0.0;
            if (rate > 0.0) {
                const double cap = sim_.economy_.production_cap_units(station_def, commodity_id, rate);
                const double forecast = std::max(stock, std::min(cap, stock + rate * days_ahead));
                surplus_by_commodity[commodity_id] = forecast - (8.0 + rate * 7.0);
            }
        }
        std::vector<const domain::ShipState*> unplanned;
        for (const auto& other : sim_.ships_) {
            if (other.id == ship.id) {
                continue;
            }
            const bool inbound = (other.phase == domain::ShipMissionPhase::InTransit
                || other.phase == domain::ShipMissionPhase::AwaitingDeparture)
                && other.active_mission.destination_station_id == station_def.id;
            const bool docked = other.current_station_id == station_def.id
                && (other.phase == domain::ShipMissionPhase::Idle || other.phase == domain::ShipMissionPhase::Refueling);
            if (inbound && other.active_mission.pickup_units > 0.0) {
                if (const auto it = surplus_by_commodity.find(other.active_mission.pickup_commodity_id);
                    it != surplus_by_commodity.end()) {
                    it->second -= std::max(0.0, std::min(it->second, other.active_mission.pickup_units));
                }
            } else if (inbound || docked) {
                unplanned.push_back(&other);
            }
        }
        for (const auto* other : unplanned) {
            if (surplus_by_commodity.empty()) {
                break;
            }
            auto largest = std::max_element(surplus_by_commodity.begin(), surplus_by_commodity.end(),
                [](const auto& a, const auto& b) { return a.second < b.second; });
            largest->second -= std::max(0.0, std::min(largest->second, sim_.ship_design(*other).cargo_capacity_units));
        }
        return surplus_by_commodity;
    }
    // A follow-up leg counts only if the ship could leave its destination again: with what it has
    // left plus what that port can sell it, for a leg as long, flown empty. (Without this, ships
    // shuttled empty between Earth L1 and Low Earth Logistics, each time for a follow-up to Mars
    // that, once there, they could not fly: no fuel at Mars to leave again.)
    bool can_leave_again(const domain::StationDefinition& next, const LegEstimate& leg,
        double departure_kg, double days_ahead) {
        const double left_kg = std::max(0.0, departure_kg - leg.propellant_kg);
        const double for_sale_kg = fuel_for_sale_on_arrival_kg(sim_.economy_, next, sim_.get_station_state(next.id),
            days_ahead + leg.travel_days);
        return left_kg + for_sale_kg >= leg.propellant_kg * (1.0 + PROPELLANT_RESERVE_FRACTION);
    }
    const std::vector<FollowUp>& follow_ups_from(const domain::StationDefinition& destination,
        const domain::TrajectoryPlan& plan, double carried_kg) {
        const auto& dest_state = sim_.get_station_state(destination.id);
        const double arrival_kg = std::max(0.0,
            plan.propellant_load_kg - plan.propellant_required_kg - plan.propellant_boiled_kg) + carried_kg;
        // A plan's travel time includes its wait for the launch window (until 2026-10-08 the wait
        // was added a second time to every arrival forecast).
        const double follow_days = delay_days + plan.travel_time_s / 86400.0;
        const double departure_kg = std::min(ship_class.propellant_capacity_kg,
            arrival_kg + fuel_for_sale_on_arrival_kg(
                sim_.economy_, destination, dest_state, delay_days + plan.travel_time_s / 86400.0));
        // The follow-ups depend on when the ship arrives and with how much fuel it can leave, so
        // a lighter load (earlier, more fuel left) gets its own (before: one set per destination,
        // from whichever plan asked first).
        const auto key = std::format("{}|{}|{}|{}", destination.id, plan.arrival_time_s, plan.travel_time_s, departure_kg);
        if (const auto it = follow_ups_by_destination.find(key); it != follow_ups_by_destination.end()) {
            return it->second;
        }
        std::vector<FollowUp> options;
        auto surplus_by_commodity = open_surplus(destination, dest_state, follow_days);
        for (const auto& [commodity_id, surplus] : surplus_by_commodity) {
            const double full_units = std::min(surplus, ship_class.cargo_capacity_units);
            if (full_units <= 1.0) {
                continue;
            }
            const double decay_per_day = sim_.cargo_decay_per_day(ship_class, commodity_id);
            for (const auto& next : sim_.universe_.stations) {
                if (next.id == destination.id || !route_leg_allowed(destination.id, next.id)) {
                    continue;
                }
                const auto& next_rates = sim_.economy_.get_station_net_rates(next);
                const auto& next_state = sim_.get_station_state(next.id);
                const bool consumes = (next_rates.contains(commodity_id) && next_rates.at(commodity_id) < 0.0)
                    || next_state.order_stats.contains(commodity_id);
                if (!consumes && sim_.station_price(next_state, commodity_id) <= sim_.get_commodity(commodity_id).base_price) {
                    continue;
                }
                // Best load: a full hold may be too heavy; where the next station posts an order,
                // its open quantity and the end of its extra are the loads that pay best.
                std::vector<double> lots {full_units, 0.5 * full_units, 0.25 * full_units};
                if (sim_.priced_order(next_state, commodity_id)) {
                    const auto light = sim_.estimate_leg(ship_class, destination, next, plan.arrival_time_s, departure_kg,
                        provisions_kg + 0.25 * full_units * sim_.get_commodity(commodity_id).mass_per_unit_kg);
                    if (light.feasible) {
                        const auto quote = sim_.order_quote(next_state, commodity_id,
                            sim_.game_time_s_ + (follow_days + light.travel_days) * 86400.0, ship.id);
                        for (const double breakpoint : {quote.open_units, quote.open_units + quote.extra_units}) {
                            if (breakpoint > 1.0 && breakpoint < full_units) {
                                lots.push_back(breakpoint);
                            }
                        }
                    }
                }
                double units = 0.0;
                double best_profit = -std::numeric_limits<double>::infinity();
                LegEstimate leg;
                for (const double lot : lots) {
                    if (lot <= 1.0) {
                        continue;
                    }
                    const auto estimate = sim_.estimate_leg(ship_class, destination, next, plan.arrival_time_s, departure_kg,
                        provisions_kg + lot * sim_.get_commodity(commodity_id).mass_per_unit_kg);
                    if (!estimate.feasible || !can_leave_again(next, estimate, departure_kg, follow_days)) {
                        continue;
                    }
                    const double lot_profit = sim_.sale_value_on_arrival(next_state, commodity_id,
                            lot * std::pow(1.0 - decay_per_day, estimate.travel_days), follow_days + estimate.travel_days, ship.id)
                        - sim_.trade_value(dest_state, commodity_id, -lot)
                        - (estimate.propellant_kg / FUEL_UNITS_TO_KG) * origin_fuel_price
                        - time_cost_per_day * estimate.travel_days;
                    if (lot_profit > best_profit) {
                        best_profit = lot_profit;
                        units = lot;
                        leg = estimate;
                    }
                }
                if (!leg.feasible) {
                    continue;
                }
                const double revenue = sim_.sale_value_on_arrival(next_state, commodity_id,
                    units * std::pow(1.0 - decay_per_day, leg.travel_days), follow_days + leg.travel_days, ship.id);
                const double cost = sim_.trade_value(dest_state, commodity_id, -units) + (leg.propellant_kg / FUEL_UNITS_TO_KG) * origin_fuel_price
                    + time_cost_per_day * leg.travel_days;
                options.push_back({
                    .profit = revenue - cost,
                    .days = leg.travel_days,
                    .carries_cargo = true,
                    .label = std::format("then {:.0f}u {} -> {}", units, commodity_id, next.id),
                    .commodity_id = commodity_id,
                    .units = units,
                });
            }
        }
        // Passengers waiting there for another station (a forecast from who waits now).
        if (!cargo_only && ship_class.berths >= 1.0 && !dest_state.waiting.empty()) {
            for (const auto& next : sim_.universe_.stations) {
                if (next.id == destination.id || !route_leg_allowed(destination.id, next.id)
                    || !sim_.passenger_leg_waiting(dest_state, next.id)) {
                    continue;
                }
                const auto leg = sim_.estimate_leg(ship_class, destination, next, plan.arrival_time_s, departure_kg,
                    provisions_kg + ship_class.berths * sim_.passenger_kg(sim_.economy_.transfer_days(destination, next)));
                if (!leg.feasible || !can_leave_again(next, leg, departure_kg, follow_days)) {
                    continue;
                }
                const auto riders = sim_.passenger_offer(ship, ship_class, dest_state, next, leg.travel_days, 0.0, ship_class.berths);
                if (riders.people < 1.0) {
                    continue;
                }
                options.push_back({
                    .profit = riders.revenue_cr - riders.marginal_cr
                        - (leg.propellant_kg / FUEL_UNITS_TO_KG) * origin_fuel_price - time_cost_per_day * leg.travel_days,
                    .days = leg.travel_days,
                    .carries_cargo = true,
                    .label = std::format("then {:.0f} passengers -> {}", riders.people, next.id),
                });
            }
        }
        // Dead end: fly back empty, or, if even that is impossible, sit out the lay-up delay.
        const auto back = sim_.estimate_leg(ship_class, destination, origin_def, plan.arrival_time_s, departure_kg, provisions_kg);
        if (back.feasible) {
            options.push_back({
                .profit = -((back.propellant_kg / FUEL_UNITS_TO_KG) * origin_fuel_price
                    + time_cost_per_day * back.travel_days),
                .days = back.travel_days,
                .label = "then back empty",
            });
        } else {
            options.push_back({.profit = -time_cost_per_day * IDLE_LAYUP_DAYS, .days = IDLE_LAYUP_DAYS, .label = "then idle"});
        }
        return follow_ups_by_destination.emplace(key, std::move(options)).first->second;
    }
    // Profit per day over both legs, with the best follow-up for this leg.
    double two_leg_score(const domain::StationDefinition& destination, const domain::TrajectoryPlan& plan,
        double carried_kg, double leg_profit, double leg_days, bool cargo_follow_up_only, FollowUp& follow) {
        double best = -std::numeric_limits<double>::infinity();
        for (const auto& option : follow_ups_from(destination, plan, carried_kg)) {
            if (cargo_follow_up_only && !option.carries_cargo) {
                continue;
            }
            // A follow-up is a forecast; one that starts years from now is worth less (a light
            // freighter at Mars waited out 1,385 days for science samples forecast at Ceres).
            const double horizon = sim_.universe_.market.follow_up_horizon_days;
            const double weight = horizon > 0.0 && option.profit > 0.0 ? std::exp(-leg_days / horizon) : 1.0;
            const double score = (leg_profit + weight * option.profit) / std::max(1.0, leg_days + option.days);
            if (score > best) {
                best = score;
                follow = option;
            }
        }
        return best;
    }
    // What this port has to sell: goods it makes, above a small working stock.
    std::vector<Offer> origin_offers() {
        std::vector<Offer> offers;
        for (const auto& [commodity_id, stock] : origin_state.inventory) {
            const double rate = origin_rates.contains(commodity_id) ? origin_rates.at(commodity_id) : 0.0;
            const double surplus = rate > 0.0 ? std::max(0.0, stock - (8.0 + rate * 7.0)) : 0.0;
            if (surplus > 1.0) {
                offers.push_back({commodity_id, surplus});
            }
        }
        // Fleet investment values a hull fitted with a cabin module for its berths only (the bare
        // hull is the cargo candidate), so its probe plans no cargo runs.
        if (cargo_only && sim_.passengers_on() && ship_class.berths >= 1.0 && !ship_class.fit.empty()) {
            offers.clear();
        }
        return offers;
    }

    // The manifest (step 29, section 5b). For each destination the ship fills its hold with the
    // units that earn the most each: what the destination pays for the next unit (an open order,
    // then the extra at a discount, then the floor; along its curve where it posts no order),
    // less what the unit costs here along the producer's curve and the propellant its mass burns,
    // until the next unit would lose money or the hold, the credit or the surplus runs out. Once
    // an order slice has paid for the trip, units that just cover their price and propellant top
    // the hold up. The manifest is then planned and scored over two legs, whole and as its first
    // half and quarter in the order it was loaded: a full hold may be too heavy to fly or to
    // leave again. (Until step 29: one good at 100, 50 or 25% of the hold, and a separate mixed
    // hold.) Fleet investment probes (cargo_only) get each manifest of several goods as lots
    // sharing a run number.
    void score_cargo_runs() {
        for (const auto& destination : sim_.universe_.stations) {
            if (offers.empty() && !passengers_waiting) {
                break;
            }
            if (destination.id == origin_def.id || !route_leg_allowed(origin_def.id, destination.id)) {
                continue;
            }
            const auto& destination_state = sim_.get_station_state(destination.id);
            const auto& destination_rates = sim_.economy_.get_station_net_rates(destination);
            struct Good {
                const Offer* offer {nullptr};
                const domain::CommodityDefinition* commodity {nullptr};
                bool ordered {false};
                OrderQuote quote {};
                double stock_on_arrival {0.0};  // where the destination posts no order
                double urgency {1.0};
                double loaded {0.0};
                double buy_cost {0.0};  // of `loaded`, along the origin's curve
                double contract_units {0.0};  // the first of `loaded`, at the contract's price
                double contract_price {0.0};
            };
            std::vector<Good> goods;
            double heaviest_kg = 0.0;
            for (const auto& offer : offers) {
                const auto& commodity = sim_.get_commodity(offer.commodity_id);
                const double rate = destination_rates.contains(offer.commodity_id) ? destination_rates.at(offer.commodity_id) : 0.0;
                const bool ordered = sim_.priced_order(destination_state, offer.commodity_id);
                // Follow the price signal, not just the recipe: a station short of something it does
                // not consume itself (water drunk by visiting crews) still bids it up.
                if (!ordered && rate >= 0.0 && sim_.station_price(destination_state, offer.commodity_id) <= commodity.base_price) {
                    continue;
                }
                const auto stock_it = destination_state.inventory.find(offer.commodity_id);
                const double stock = stock_it == destination_state.inventory.end() ? 0.0 : stock_it->second;
                // An export market never starves: Earth's economy takes what arrives. The price clamp
                // saturates, so a starving station cannot bid any higher; the score is weighted by
                // how few days of stock it has left.
                const double days_left = rate < 0.0 && !sim_.economy_.is_export_market(destination, offer.commodity_id)
                    ? stock / std::abs(rate) : std::numeric_limits<double>::infinity();
                goods.push_back({.offer = &offer, .commodity = &commodity, .ordered = ordered,
                    .urgency = std::clamp(14.0 / std::max(days_left, 0.5), 1.0, 5.0)});
                heaviest_kg = std::max(heaviest_kg, commodity.mass_per_unit_kg);
            }
            // Riders board any leg flown there; a leg for passengers alone needs min_leg_passengers.
            const bool riders_here = passengers_waiting
                && std::any_of(origin_state.waiting.begin(), origin_state.waiting.end(), [&](const domain::PassengerGroup& group) {
                       return group.destination_id == destination.id && group.count >= 1.0;
                   });
            const bool passengers_here = riders_here && sim_.passenger_leg_waiting(origin_state, destination.id);
            if (goods.empty() && !passengers_here) {
                continue;
            }
            // The empty-hold transfer dates the forecasts (a laden one differs a little); a hold of
            // the heaviest good (or half of one) gives the propellant per kg of cargo. All three are
            // asked for in one pass.
            const auto& probe = plan_to(destination, 0.0);
            // Without goods the empty plan gives the propellant per kg (berths weigh little; at the
            // same mass ratio, propellant grows with the mass it moves).
            const bool from_empty = goods.empty();
            const double laden_kg = goods.empty() ? berths * sim_.passenger_kg(sim_.economy_.transfer_days(origin_def, destination))
                                                  : ship_class.cargo_capacity_units * heaviest_kg;
            const auto& laden = from_empty ? probe : plan_to(destination, laden_kg);
            const auto& half_laden = from_empty ? probe : plan_to(destination, 0.5 * laden_kg);
            if (!probe.feasible) {
                continue;
            }
            double propellant_per_kg = 0.0;
            if (from_empty) {
                propellant_per_kg = probe.propellant_required_kg / std::max(1.0, ship_class.dry_mass_kg + provisions_kg);
            } else {
                for (const auto* plan : {&laden, &half_laden}) {
                    if (plan->feasible) {
                        const double kg = plan == &laden ? laden_kg : 0.5 * laden_kg;
                        propellant_per_kg = std::max(0.0, plan->propellant_required_kg - probe.propellant_required_kg) / std::max(1.0, kg);
                        break;
                    }
                }
            }
            const double probe_days = probe.travel_time_s / 86400.0;
            const double arrival_s = sim_.game_time_s_ + (delay_days + probe_days) * 86400.0;
            const auto riders = riders_here
                ? sim_.passenger_offer(ship, ship_class, origin_state, destination, probe_days,
                      propellant_per_kg * marginal_fuel_price_per_kg, berths)
                : PassengerOffer {};
            for (auto& good : goods) {
                if (good.ordered) {
                    good.quote = sim_.order_quote(destination_state, good.offer->commodity_id, arrival_s, ship.id);
                } else {
                    good.stock_on_arrival = sim_.forecast_stock_on_arrival(destination_state, good.offer->commodity_id,
                        delay_days + probe_days, ship.id);
                }
            }
            double hold_left = ship_class.cargo_capacity_units;
            if (destination.storage_capacity_units > 0.0) {
                hold_left = std::min(hold_left, std::max(0.0,
                    destination.storage_capacity_units - sim_.economy_.storage_used_units(destination, destination_state.inventory)));
            }
            double credits_left = spendable_credits;
            const double chunk = std::max(1.0, ship_class.cargo_capacity_units / 40.0);
            std::vector<std::pair<std::size_t, double>> picks;  // (good, units) in the order loaded
            // A rotation contract's deliveries on this leg go aboard first, at their fixed price.
            if (liner && sim_.under_contract(ship)) {
                for (const auto& delivery : ship.contract.deliveries) {
                    if (delivery.to_station_id != destination.id) {
                        continue;
                    }
                    for (std::size_t g = 0; g < goods.size(); ++g) {
                        auto& good = goods[g];
                        if (good.offer->commodity_id != delivery.commodity_id || good.contract_units > 0.0) {
                            continue;
                        }
                        const double units = std::min({delivery.units, good.offer->surplus, hold_left});
                        const double buy = units >= 1.0 ? sim_.trade_value(origin_state, good.offer->commodity_id, -units) : 0.0;
                        if (units < 1.0 || buy > credits_left) {
                            continue;
                        }
                        good.loaded = units;
                        good.buy_cost = buy;
                        good.contract_units = units;
                        good.contract_price = delivery.unit_price;
                        hold_left -= units;
                        credits_left -= buy;
                        picks.emplace_back(g, units);
                    }
                }
            }
            while (hold_left >= 0.5) {
                std::size_t best_good = goods.size();
                double best_gain = 0.0;
                double best_units = 0.0;
                double best_buy = 0.0;
                for (std::size_t g = 0; g < goods.size(); ++g) {
                    auto& good = goods[g];
                    const double units = std::min({chunk, good.offer->surplus - good.loaded, hold_left});
                    if (units < 0.5) {
                        continue;
                    }
                    const double surviving = std::pow(1.0 - sim_.cargo_decay_per_day(ship_class, good.commodity->id), probe_days);
                    const double spot_loaded = good.loaded - good.contract_units;
                    const double sale = good.ordered
                        ? sim_.quote_split(good.quote, spot_loaded * surviving, units * surviving).total
                        : sim_.sale_split(destination, good.offer->commodity_id, good.stock_on_arrival + spot_loaded * surviving,
                              units * surviving).total;
                    const double buy = sim_.trade_value(origin_state, good.offer->commodity_id, -(good.loaded + units)) - good.buy_cost;
                    const double propellant = propellant_per_kg * units * good.commodity->mass_per_unit_kg * marginal_fuel_price_per_kg;
                    const double gain = (sale - buy - propellant) / units;
                    if (gain > best_gain && buy <= credits_left) {
                        best_good = g;
                        best_gain = gain;
                        best_units = units;
                        best_buy = buy;
                    }
                }
                if (best_good == goods.size()) {
                    break;
                }
                goods[best_good].loaded += best_units;
                goods[best_good].buy_cost += best_buy;
                hold_left -= best_units;
                credits_left -= best_buy;
                picks.emplace_back(best_good, best_units);
            }
            // The whole manifest, and its first half and quarter (passengers ride with each; with
            // passengers and no goods, the passengers alone).
            std::size_t last_count = std::numeric_limits<std::size_t>::max();
            for (const double fraction : {1.0, 0.5, 0.25}) {
                const auto count = static_cast<std::size_t>(std::ceil(static_cast<double>(picks.size()) * fraction));
                if ((count == 0 && riders.people <= 0.0) || count == last_count) {
                    continue;
                }
                last_count = count;
                std::vector<double> units_by_good(goods.size(), 0.0);
                for (std::size_t i = 0; i < count; ++i) {
                    units_by_good[picks[i].first] += picks[i].second;
                }
                std::vector<domain::CargoLot> lots;
                std::vector<double> lot_urgency;
                double cargo_kg = 0.0;
                for (std::size_t g = 0; g < goods.size(); ++g) {
                    double units = units_by_good[g];
                    if (units > 1.0) {
                        cargo_kg += units * goods[g].commodity->mass_per_unit_kg;
                    }
                    if (const double contracted = std::min(units, goods[g].contract_units); contracted >= 1.0) {
                        lots.push_back({.commodity_id = goods[g].offer->commodity_id, .units = contracted,
                            .fixed_unit_price = goods[g].contract_price});
                        lot_urgency.push_back(1.0);
                        units -= contracted;
                    }
                    if (units > 1.0) {
                        lots.push_back({.commodity_id = goods[g].offer->commodity_id, .units = units});
                        lot_urgency.push_back(goods[g].urgency);
                    }
                }
                if (lots.empty() && riders.people <= 0.0) {
                    continue;
                }
                cargo_kg += riders.mass_kg;
                const auto cargo_label = riders.people > 0.0
                    ? std::format("cargo {} + {:.0f} passengers -> {}", describe_cargo(lots), riders.people, destination.id)
                    : std::format("cargo {} -> {}", describe_cargo(lots), destination.id);
                // Tankers too: a tanker that bought its return fuel from the cargo it just delivered
                // would drain the depot it supplies.
                const auto fuel_plan = plan_with_return_fuel(destination, cargo_kg, riders.time_cr_per_day);
                const auto& plan = *fuel_plan.plan;
                if (!plan.feasible) {
                    trace_line(cargo_label + ": no feasible trajectory (" + plan.summary + ")");
                    continue;
                }
                if (!fuel_plan.feasible) {
                    trace_line(cargo_label + ": could not refuel to leave again, nor carry the fuel");
                    continue;
                }
                const double travel_days = plan.travel_time_s / 86400.0;
                // Passengers eat from the same stores.
                const double crew = std::max(1.0, ship_class.crew_size);
                const double provisionable = max_mission_days * crew / (crew + riders.people);
                if (travel_days > provisionable) {
                    trace_line(std::format("{}: {:.0f} days exceeds provisionable {:.0f}", cargo_label, travel_days, provisionable));
                    continue;
                }
                if (!dose_allows(destination, travel_days)) {
                    trace_line(std::format("{}: {:.0f} days exceeds the crew's dose limit", cargo_label, travel_days));
                    continue;
                }
                if (!sim_.drive_life_allows(ship, ship_class, plan)) {
                    trace_line(cargo_label + ": the drive's remaining life is too short");
                    continue;
                }
                const double fuel_cost = (consumed_propellant_kg(plan) / FUEL_UNITS_TO_KG) * origin_fuel_price
                    + fuel_premium(plan, fuel_plan.carried_kg);
                if (cargo_only) {
                    const int run = lots.size() > 1 ? next_run++ : -1;
                    for (std::size_t l = 0; l < lots.size(); ++l) {
                        cargo_options.push_back({.destination = &destination, .commodity_id = lots[l].commodity_id,
                            .cargo_units = lots[l].units, .travel_days = travel_days,
                            .wait_days = plan.wait_time_s / 86400.0, .fuel_cost = l == 0 ? fuel_cost : 0.0, .run = run});
                    }
                }
                const double trip_cost = fuel_cost + time_cost_per_day * travel_days;
                const double total_units = domain::total_units(lots);
                // Fares, less the passengers' provisions; with no goods aboard they carry the trip.
                double revenue = riders.revenue_cr;
                double purchases = riders.marginal_cr;
                double weighted = riders.revenue_cr - riders.marginal_cr - (lots.empty() ? trip_cost : 0.0);
                for (std::size_t l = 0; l < lots.size(); ++l) {
                    const auto& lot = lots[l];
                    const double surviving = lot.units
                        * std::pow(1.0 - sim_.cargo_decay_per_day(ship_class, lot.commodity_id), travel_days);
                    const double lot_revenue = lot.fixed_unit_price > 0.0 ? surviving * lot.fixed_unit_price
                        : sim_.sale_value_on_arrival(destination_state, lot.commodity_id, surviving, delay_days + travel_days, ship.id);
                    const double lot_cost = sim_.trade_value(origin_state, lot.commodity_id, -lot.units);
                    revenue += lot_revenue;
                    purchases += lot_cost;
                    // Each lot's margin, less its share of the trip, weighted by its urgency.
                    weighted += lot_urgency[l] * (lot_revenue - lot_cost - trip_cost * lot.units / total_units);
                }
                FollowUp follow;
                // Cargo-only probes (fleet investment) value the run on its own and skip the
                // follow-up forecast, the expensive part of a dispatch pass.
                const double score = cargo_only
                    ? weighted / std::max(1.0, travel_days)
                    : two_leg_score(destination, plan, fuel_plan.carried_kg, weighted, travel_days, false, follow);
                trace_line(std::format("{}: {:.0f} days revenue {:.0f} cost {:.0f} (trip {:.0f}), {}: score {:.1f}{}",
                    cargo_label, travel_days, revenue, purchases + trip_cost, trip_cost, follow.label, score,
                    score > best_score ? " (best so far)" : ""));
                if (score > best_score) {
                    best_score = score;
                    best_destination = &destination;
                    best_cargo = lots;
                    best_cargo_margin = revenue - purchases - fuel_cost;
                    best_plan = &plan;
                    best_carried_kg = fuel_plan.carried_kg;
                    best_pickup_commodity = follow.commodity_id;
                    best_pickup_units = follow.units;
                    best_passengers = riders.picks;
                }
            }
        }
    }

    // Investment probes plan passenger legs as the reference fares and dispatch do, with the
    // riders' time (full berths at the reference fill, a resident's time and dose per day), and
    // on a full tank (see planning_options).
    double probe_rider_day_cr() const {
        return sim_.universe_.market.fill_ref
            * sim_.passenger_nonfare_cr(sim_.value_of_time_cr_per_day(domain::PassengerKind::Migrant), 1.0,
                sim_.universe_.ship_environment.radiation ? sim_.universe_.ship_environment.gcr_msv_per_day : 0.0);
    }

    // Fleet investment probes of a ship with berths: one passenger option per destination it can
    // carry full berths to, valued later at the sustained flow of travellers (not today's queue).
    void score_berth_probes() {
        const double probe_rider_day_cr = this->probe_rider_day_cr();
        if (cargo_only && sim_.passengers_on() && ship_class.berths >= 1.0) {
            for (const auto& destination : sim_.universe_.stations) {
                if (destination.id == origin_def.id || sim_.reference_fare(origin_def.id, destination.id) == nullptr) {
                    continue;
                }
                const double load_kg = ship_class.berths * sim_.passenger_kg(sim_.economy_.transfer_days(origin_def, destination));
                const auto fuel_plan = plan_with_return_fuel(destination, load_kg, ship_class.berths * probe_rider_day_cr, true);
                const auto& plan = *fuel_plan.plan;
                if (!plan.feasible || !fuel_plan.feasible) {
                    continue;
                }
                const double travel_days = plan.travel_time_s / 86400.0;
                const double crew = std::max(1.0, ship_class.crew_size);
                if (travel_days > max_mission_days * crew / (crew + ship_class.berths) || !dose_allows(destination, travel_days)
                    || !sim_.drive_life_allows(ship, ship_class, plan)) {
                    continue;
                }
                cargo_options.push_back({.destination = &destination, .commodity_id = PASSENGER_GOOD,
                    .cargo_units = ship_class.berths, .travel_days = travel_days, .wait_days = plan.wait_time_s / 86400.0,
                    .fuel_cost = (consumed_propellant_kg(plan) / FUEL_UNITS_TO_KG) * origin_fuel_price
                        + fuel_premium(plan, fuel_plan.carried_kg),
                    .run = -1,
                    .return_fuel_kg = fuel_plan.carried_kg > 0.0 ? -1.0 : consumed_propellant_kg(plan)});
            }
        }
    }

    // A bare hull's probe also values the hull with each of its cabin modules (fleet investment
    // probes no cabin design of its own): the same transfers with the module's mass and full
    // berths as payload, in this probe's plans; the limits are the fitted design's.
    void score_cabin_module_probes() {
        const double probe_rider_day_cr = this->probe_rider_day_cr();
        if (cargo_only && sim_.passengers_on() && ship_class.berths <= 0.0
            && ship_class.fit.empty()) {
            for (const auto& module : sim_.universe_.ship_modules) {
                if (module.hull_id != ship_class.id || module.slot != "cabin" || !sim_.module_has_active_effect(module)) {
                    continue;
                }
                const auto& design = sim_.register_design(ship_class.id, {module.id});
                if (design.berths < 1.0) {
                    continue;
                }
                const double module_kg = std::max(0.0, design.dry_mass_kg - ship_class.dry_mass_kg);
                for (const auto& destination : sim_.universe_.stations) {
                    if (destination.id == origin_def.id || sim_.reference_fare(origin_def.id, destination.id) == nullptr) {
                        continue;
                    }
                    const double load_kg = module_kg + design.berths * sim_.passenger_kg(sim_.economy_.transfer_days(origin_def, destination));
                    const auto fuel_plan = plan_with_return_fuel(destination, load_kg, design.berths * probe_rider_day_cr, true);
                    const auto& plan = *fuel_plan.plan;
                    if (!plan.feasible || !fuel_plan.feasible) {
                        continue;
                    }
                    const double travel_days = plan.travel_time_s / 86400.0;
                    const double crew = std::max(1.0, design.crew_size);
                    if (travel_days > max_mission_days * crew / (crew + design.berths)
                        || !sim_.leg_dose_allowed(design, origin_def, destination, sim_.game_time_s_, travel_days)
                        || !sim_.drive_life_allows(ship, design, plan)) {
                        continue;
                    }
                    cargo_options.push_back({.destination = &destination, .commodity_id = PASSENGER_GOOD,
                        .cargo_units = design.berths, .travel_days = travel_days, .wait_days = plan.wait_time_s / 86400.0,
                        .fuel_cost = (consumed_propellant_kg(plan) / FUEL_UNITS_TO_KG) * origin_fuel_price
                            + fuel_premium(plan, fuel_plan.carried_kg),
                        .run = -1, .module_id = module.id,
                        .return_fuel_kg = fuel_plan.carried_kg > 0.0 ? -1.0 : consumed_propellant_kg(plan)});
                }
            }
        }
    }

    // Empty legs toward a better pickup compete with cargo runs on the same two-leg terms.
    void score_empty_legs() {
        for (const auto& destination : sim_.universe_.stations) {
            if (cargo_only || destination.id == origin_def.id || !route_leg_allowed(origin_def.id, destination.id)) {
                continue;
            }
            const auto fuel_plan = plan_with_return_fuel(destination, 0.0);
            if (!fuel_plan.feasible) {
                continue;
            }
            const auto& plan = *fuel_plan.plan;
            const double leg_days = plan.travel_time_s / 86400.0;
            if (leg_days > max_mission_days || !dose_allows(destination, leg_days) || !sim_.drive_life_allows(ship, ship_class, plan)) {
                continue;
            }
            const double leg_cost = (consumed_propellant_kg(plan) / FUEL_UNITS_TO_KG) * origin_fuel_price
                + fuel_premium(plan, fuel_plan.carried_kg)
                + time_cost_per_day * leg_days;
            FollowUp follow;
            const double score = two_leg_score(destination, plan, fuel_plan.carried_kg, -leg_cost, leg_days, true, follow);
            if (follow.label.empty()) {
                continue;
            }
            trace_line(std::format("empty -> {}: {:.0f} days cost {:.0f}, {}: score {:.1f}{}",
                destination.id, leg_days, leg_cost, follow.label, score, score > best_score ? " (best so far)" : ""));
            if (score > best_score) {
                best_score = score;
                best_destination = &destination;
                best_cargo.clear();
                best_cargo_margin = 0.0;
                best_plan = &plan;
                best_carried_kg = fuel_plan.carried_kg;
                best_pickup_commodity = follow.commodity_id;
                best_pickup_units = follow.units;
                best_passengers.clear();
            }
        }
    }

    // A liner with nothing that pays for its next leg flies it empty: its value is the schedule.
    void fly_next_stop_empty() {
        const auto& next = sim_.get_station_definition(sim_.next_scheduled_stop(ship, origin_def.id));
        const auto fuel_plan = plan_with_return_fuel(next, 0.0);
        if (fuel_plan.plan->feasible && fuel_plan.feasible && fuel_plan.plan->travel_time_s / 86400.0 <= max_mission_days
            && dose_allows(next, fuel_plan.plan->travel_time_s / 86400.0)
            && sim_.drive_life_allows(ship, ship_class, *fuel_plan.plan)) {
            trace_line("liner: next stop empty to " + next.id);
            best_pickup_commodity.clear();
            best_pickup_units = 0.0;
            best_score = 0.0;
            best_destination = &next;
            best_plan = fuel_plan.plan;
            best_carried_kg = fuel_plan.carried_kg;
        }
    }

    // A ship on its route away from home with nothing to carry back flies home empty.
    void fly_home_empty() {
        const auto& home = sim_.get_station_definition(ship.home_station_id);
        const auto& plan = plan_to(home, 0.0);
        if (plan.feasible && plan.travel_time_s / 86400.0 <= max_mission_days
            && dose_allows(home, plan.travel_time_s / 86400.0) && sim_.drive_life_allows(ship, ship_class, plan)) {
            trace_line("on route: back home empty to " + home.id);
            best_pickup_commodity.clear();
            best_pickup_units = 0.0;
            best_score = 0.0;
            best_destination = &home;
            best_plan = &plan;
            best_carried_kg = 0.0;
        }
    }

    // Nothing worth flying: move toward a better pickup.
    void reposition() {
        repositioning = true;
        best_pickup_commodity.clear();
        best_pickup_units = 0.0;
        best_score = 0.0;  // repositioning scores are in urgency units, not credits/day

        // Sourcing score: does this station have surplus goods urgently needed
        // elsewhere? An empty ship is only useful where there is something to
        // pick up, so repositioning targets producers; starving stations are
        // served by the cargo loop's urgency weighting once a ship is loaded.
        const auto sourcing_score_for = [&](const domain::StationDefinition& station_def,
                                            const domain::StationState& station_state) {
            double sourcing_score = 0.0;
            for (const auto& [commodity_id, surplus] : open_surplus(station_def, station_state)) {
                if (surplus <= 1.0) continue;
                // Sum up how urgently this commodity is needed across the whole system.
                double system_urgency = 0.0;
                for (const auto& other : sim_.universe_.stations) {
                    if (other.id == station_def.id) continue;
                    const auto& other_rates = sim_.economy_.get_station_net_rates(other);
                    const double other_rate = other_rates.count(commodity_id) ? other_rates.at(commodity_id) : 0.0;
                    if (other_rate >= 0.0) continue;
                    const auto& other_state = sim_.get_station_state(other.id);
                    const double other_stock = other_state.inventory.count(commodity_id)
                        ? other_state.inventory.at(commodity_id) : 0.0;
                    const double days_rem = sim_.economy_.is_export_market(other, commodity_id) ? 30.0
                        : other_stock > 0.0 ? other_stock / std::abs(other_rate) : 0.0;
                    system_urgency += std::max(1.0, 30.0 / std::max(1.0, days_rem)) * std::abs(other_rate);
                }
                sourcing_score += std::min(surplus, ship_class.cargo_capacity_units) * system_urgency / 100.0;
            }
            return sourcing_score;
        };
        const double origin_sourcing_score = sourcing_score_for(origin_def, origin_state);

        // What one full load picked up at `station_def` could earn, in credits, at
        // today's prices (the best spread to any consumer). A repositioning trip
        // must at least pay for itself out of that.
        const auto sourcing_value_cr = [&](const domain::StationDefinition& station_def,
                                           const domain::StationState& station_state) {
            // What the ships already there or on their way leave of it.
            double best_value = 0.0;
            for (const auto& [commodity_id, surplus] : open_surplus(station_def, station_state)) {
                const double load = std::min(surplus, ship_class.cargo_capacity_units);
                if (load <= 1.0) continue;
                const double buy_cost = sim_.trade_value(station_state, commodity_id, -load);
                for (const auto& other : sim_.universe_.stations) {
                    if (other.id == station_def.id) continue;
                    const auto& other_rates = sim_.economy_.get_station_net_rates(other);
                    const auto& other_state = sim_.get_station_state(other.id);
                    const bool consumes = (other_rates.contains(commodity_id) && other_rates.at(commodity_id) < 0.0)
                        || other_state.order_stats.contains(commodity_id);
                    if (!consumes && sim_.station_price(other_state, commodity_id) <= sim_.get_commodity(commodity_id).base_price) continue;
                    best_value = std::max(best_value, sim_.trade_value(other_state, commodity_id, load) - buy_cost);
                }
            }
            return best_value;
        };

        for (const auto& destination : sim_.universe_.stations) {
            if (destination.id == origin_def.id) {
                continue;
            }
            const auto& dest_state = sim_.get_station_state(destination.id);

            const double destination_score = sourcing_score_for(destination, dest_state);
            // Only burn propellant relocating if the destination is a clearly
            // better pickup spot than where the ship already sits — otherwise
            // the fleet thrashes between comparable producers, hauling nothing.
            if (destination_score <= 0.0 || destination_score < origin_sourcing_score * 1.5) {
                trace_line(std::format("reposition -> {}: sourcing score {:.1f} vs here {:.1f}",
                    destination.id, destination_score, origin_sourcing_score));
                continue;
            }
            const auto fuel_plan = plan_with_return_fuel(destination, 0.0);
            const auto& plan = *fuel_plan.plan;
            if (!plan.feasible) {
                trace_line("reposition -> " + destination.id + ": no feasible trajectory (" + plan.summary + ")");
                continue;
            }
            const double reposition_days = plan.travel_time_s / 86400.0;
            if (reposition_days > max_mission_days || !dose_allows(destination, reposition_days)
                || !sim_.drive_life_allows(ship, ship_class, plan)) {
                trace_line(std::format("reposition -> {}: {:.0f} days exceeds provisionable {:.0f}",
                    destination.id, reposition_days, max_mission_days));
                continue;
            }
            if (!fuel_plan.feasible) {
                trace_line(std::format("reposition -> {}: could not refuel to leave again nor carry the fuel (burns {:.0f} of {:.0f} kg, port sells {:.0f} kg)",
                    destination.id, plan.propellant_required_kg, plan.propellant_load_kg,
                    fuel_for_sale_on_arrival_kg(sim_.economy_, destination, sim_.get_station_state(destination.id),
                        delay_days + reposition_days)));
                continue;
            }
            const double reposition_cost = (consumed_propellant_kg(plan) / FUEL_UNITS_TO_KG) * origin_fuel_price
                + fuel_premium(plan, fuel_plan.carried_kg)
                + time_cost_per_day * reposition_days;
            if (sourcing_value_cr(destination, dest_state) <= reposition_cost) {
                trace_line(std::format("reposition -> {}: load worth {:.0f} does not cover trip cost {:.0f}",
                    destination.id, sourcing_value_cr(destination, dest_state), reposition_cost));
                continue;
            }
            // Crowd penalty: don't send the whole fleet to the same "best" station.
            int congestion = 0;
            for (const auto& other : sim_.ships_) {
                if (other.id == ship.id) {
                    continue;
                }
                const bool docked_there = other.current_station_id == destination.id
                    && other.phase != domain::ShipMissionPhase::InTransit;
                const bool inbound = (other.phase == domain::ShipMissionPhase::InTransit
                    || other.phase == domain::ShipMissionPhase::AwaitingDeparture)
                    && other.active_mission.destination_station_id == destination.id;
                if (docked_there || inbound) {
                    ++congestion;
                }
            }
            const double score = destination_score
                / std::max(1.0, plan.travel_time_s / 86400.0)
                / (1.0 + static_cast<double>(congestion));
            if (score > best_score) {
                best_score = score;
                best_destination = &destination;
                best_cargo.clear();
                best_cargo_margin = 0.0;
                best_plan = &plan;
                best_carried_kg = fuel_plan.carried_kg;
            }
        }
    }

    MissionChoice finish() {
        MissionChoice choice;
        choice.cargo_options = std::move(cargo_options);
        if (best_destination == nullptr) {
            return choice;
        }
        choice.kind = repositioning ? MissionChoice::Kind::Reposition : MissionChoice::Kind::Mission;
        choice.score = best_score;
        choice.destination = best_destination;
        choice.cargo = best_cargo;
        choice.cargo_margin = best_cargo_margin;
        choice.plan = *best_plan;
        if (const auto it = plan_inputs.find(best_plan); it != plan_inputs.end()) {
            const auto& inputs = it->second;
            choice.plan_options = planning_options(*inputs.destination, inputs.cargo_kg, inputs.rider_time_cr_per_day,
                inputs.full_tank);
            choice.plan_departure_s = std::max(sim_.game_time_s_, earliest_departure_s);
        }
        choice.carried_propellant_kg = best_carried_kg;
        choice.pickup_commodity_id = best_pickup_commodity;
        choice.pickup_units = best_pickup_units;
        choice.passengers = best_passengers;
        return choice;
    }
};

Simulation::MissionChoice Simulation::choose_mission_pass(
    const domain::ShipState& ship, const domain::ShipClassDefinition& ship_class, bool trace,
    double earliest_departure_s, bool cargo_only, std::unordered_map<std::string, domain::TrajectoryPlan>& plans,
    std::string& trace_text) {
    return MissionPass(*this, ship, ship_class, trace, earliest_departure_s, cargo_only, plans, trace_text).run();
}

}  // namespace spacetrains::simulation
