// A ship's day by phase: idle, refitting, awaiting departure, in transit, arrival.

#include "simulation/systems/ShipPhases.hpp"

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
#include "simulation/query/Accounts.hpp"
#include "simulation/query/Events.hpp"
#include "simulation/query/Finance.hpp"
#include "simulation/query/Lookups.hpp"
#include "simulation/query/ShipRates.hpp"
#include "simulation/systems/Contracts.hpp"
#include "simulation/systems/Failures.hpp"
#include "simulation/systems/Market.hpp"
#include "simulation/systems/MissionPlanning.hpp"
#include "simulation/systems/Passengers.hpp"
#include "simulation/systems/ShipOperations.hpp"
#include "trajectory/TrajectoryAudit.hpp"

namespace spacetrains::simulation::systems {

using namespace detail;

namespace {

// A refit or build option: a class (tank variant) and the modules fitted with it.
struct FitOption {
    const domain::ShipClassDefinition* design {nullptr};
    std::string class_id;
    std::vector<std::string> fit;
};

// Defined below, file-local.
[[nodiscard]] double refit_bill(const World& w, const domain::ShipClassDefinition& from,
    const domain::ShipClassDefinition& to);
// Part C: at its home base a ship may refit to another tank variant of its hull when
// the missions that opens repay the yard bill. Returns true if the ship went into the yard.
bool consider_refit(World& w, domain::ShipState& ship, const MissionChoice& current, bool trace);
// The fits one yard visit can reach from class_id + fit: the next tank size either way, and
// one module added (into a free slot) or removed. Registers the designs (serial code only).
std::vector<FitOption> neighbouring_fits(World& w, const std::string& class_id, const std::vector<std::string>& fit,
    double radiator_bucket = 1.0);
void record_trajectory_audit(World& w, const domain::ShipState& ship, const domain::StationDefinition& origin,
    const domain::StationDefinition& destination, const domain::TrajectoryPlan& plan, double planning_propellant_kg);

}  // namespace

void step_idle_ship(World& w, domain::ShipState& ship) {
    const profiling::Scope profile_scope(profiling::Phase::ShipReview);
    // Planning is expensive, so docked ships look for work every few hours, and laid-up
    // ships (no crew) every few days, not on every tick.
    const bool laid_up = ship.phase == domain::ShipMissionPhase::LaidUp;
    if (w.state.game_time_s_ < ship.next_review_s) {
        return;
    }
    ship.next_review_s = w.state.game_time_s_ + (laid_up ? LAYUP_REVIEW_DAYS * 86400.0 : IDLE_REVIEW_S);

    // Debug aid: SPACETRAINS_TRACE_SHIP="<ship name>" logs every candidate mission of
    // that ship and why it was rejected (stderr).
    static const char* const trace_ship = std::getenv("SPACETRAINS_TRACE_SHIP");
    const bool trace = trace_ship != nullptr && ship.name == trace_ship;

    if (systems::consider_repair(w, ship)) {
        return;
    }
    const auto& ship_class = query::ship_design(w, ship);
    const auto review_start = std::chrono::steady_clock::now();
    // A rotation contract that ran out ends here.
    if (!ship.contract.stops.empty() && w.state.game_time_s_ >= ship.contract.until_s) {
        query::add_event(w, std::format("{}'s rotation contract ended at {}", ship.name,
            query::get_station_definition(w, ship.current_station_id).name), "mission");
        ship.contract = {};
    }
    // A loop's wait in port at its first stop, counted from arrival (cycle minus the legs).
    if (systems::under_contract(w, ship) && ship.current_station_id == ship.contract.stops.front()) {
        const double ready_s = ship.idle_since_s + systems::contract_dwell_days(ship.contract) * 86400.0;
        if (w.state.game_time_s_ < ready_s) {
            ship.next_review_s = std::min(ship.next_review_s, ready_s);
            return;
        }
    }
    auto choice = systems::choose_mission(w, ship, ship_class, trace, w.state.game_time_s_);
    // Rotation contracts: a free ship looks for a loop now and then, and signs one that earns
    // about as much as the best spot mission (a repositioning earns nothing).
    const auto& market = w.data.universe_.market;
    if (market.contracts > 0.0 && ship.contract.stops.empty() && systems::liner_stops(w, ship) == nullptr
        && !(!ship.route_destination_id.empty() && w.state.game_time_s_ < ship.route_until_s)
        && w.state.game_time_s_ >= ship.next_contract_review_s) {
        // Reviews are spread over the review period by ship (never the whole fleet at once).
        if (ship.next_contract_review_s <= 0.0) {
            const auto spread = static_cast<double>(std::hash<std::string> {}(ship.id) % 1000) / 1000.0;
            ship.next_contract_review_s = w.state.game_time_s_ + spread * market.contract_review_days * 86400.0;
        }
    }
    if (market.contracts > 0.0 && ship.contract.stops.empty() && systems::liner_stops(w, ship) == nullptr
        && !(!ship.route_destination_id.empty() && w.state.game_time_s_ < ship.route_until_s)
        && w.state.game_time_s_ >= ship.next_contract_review_s) {
        ship.next_contract_review_s = w.state.game_time_s_ + market.contract_review_days * 86400.0;
        auto [contract, score] = systems::find_contract(w, ship, ship_class);
        const double spot = choice.kind == MissionChoice::Kind::Mission ? choice.score : 0.0;
        if (!contract.stops.empty() && score > MIN_MISSION_SCORE_PER_DAY && score >= market.contract_accept * spot) {
            std::string route;
            for (const auto& stop : contract.stops) {
                route += query::get_station_definition(w, stop).name + " > ";
            }
            std::string goods;
            for (const auto& delivery : contract.deliveries) {
                goods += std::format("{}{:.0f}u {} to {} at {:.0f}", goods.empty() ? "" : ", ", delivery.units,
                    delivery.commodity_id, delivery.to_station_id, delivery.unit_price);
            }
            query::add_event(w, std::format("{} signs a rotation contract: {}({:.0f}-day loop, {:.0f} cr/day against {:.0f} spot): {}",
                ship.name, route, contract.cycle_days, score, spot, goods), "mission");
            ship.contract = std::move(contract);
            choice = systems::choose_mission(w, ship, ship_class, trace, w.state.game_time_s_);
        }
    }
    // Debug aid: with SPACETRAINS_PROFILE, reviews over half a second are reported (stderr).
    if (profiling::enabled()) {
        const double review_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - review_start).count();
        if (review_s > 0.5) {
            std::cerr << std::format("[slow review day {:.1f}] {} ({}) at {}: {:.2f} s\n", w.state.game_time_s_ / 86400.0, ship.name,
                ship_class.id, ship.current_station_id, review_s);
        }
    }
    if (systems::scheduled_stops(w, ship) == nullptr && consider_refit(w, ship, choice, trace)) {
        return;
    }
    // Failures: a derelict within reach may pay more than the best mission.
    if (w.data.universe_.failures.salvage && !w.state.derelicts_.empty()
        && systems::consider_salvage(w, ship, choice.kind == MissionChoice::Kind::Mission ? choice.score : 0.0)) {
        return;
    }

    auto& origin_state = query::get_station_state(w, ship.current_station_id);
    const auto& origin_def = query::get_station_definition(w, ship.current_station_id);
    if (choice.kind == MissionChoice::Kind::None && systems::under_contract(w, ship)) {
        // The loop cannot be flown from here (no fuel, no trajectory): the contract lapses.
        query::add_event(w, std::format("{} gives up its rotation contract at {}", ship.name, origin_def.name), "alert");
        ship.contract = {};
        return;
    }
    if (choice.kind == MissionChoice::Kind::None) {
        if (ship.propellant_kg <= ship_class.propellant_capacity_kg * 0.01 && query::purchasable_propellant_kg(w, ship) <= 0.0) {
            ship.phase = domain::ShipMissionPhase::Stranded;
            query::add_event(w, std::format("{} is stranded at {} due to fuel shortage", ship.name, origin_def.name), "alert");
        } else if (!laid_up && systems::liner_stops(w, ship) == nullptr
            && (ship.credits < 0.0 || w.state.game_time_s_ - ship.idle_since_s > IDLE_LAYUP_DAYS * 86400.0)) {
            // Nothing worth flying and either in debt or idle for a month: stop paying the crew.
            ship.phase = domain::ShipMissionPhase::LaidUp;
            ship.laid_up_since_s = w.state.game_time_s_;
            ship.next_review_s = w.state.game_time_s_ + LAYUP_REVIEW_DAYS * 86400.0;
            query::add_event(w, ship.credits < 0.0
                ? std::format("{} laid up at {} ({:.0f} cr in debt)", ship.name, origin_def.name, -ship.credits)
                : std::format("{} laid up at {} (no profitable work for {:.0f} days)",
                      ship.name, origin_def.name, IDLE_LAYUP_DAYS),
                "alert");
        }
        return;
    }

    const auto* best_destination = choice.destination;
    const auto& cargo = choice.cargo;
    const double best_cargo_units = domain::total_units(cargo);
    // Candidates are planned without a rendering path; plan the chosen one again with it
    // (same inputs, so the same transfer; a variable-Isp check on the denser path can still
    // reject it, and the ship then waits for its next review).
    domain::TrajectoryPlan plan = choice.plan;
    if (!plan.has_render_path) {
        auto options = choice.plan_options;
        options.include_path = true;
        const auto& planner = (ship_class.propulsion_type == "variable_isp" && w.engines.variable_isp_planner_)
            ? static_cast<trajectory::ITrajectoryPlanner&>(*w.engines.variable_isp_planner_)
            : static_cast<trajectory::ITrajectoryPlanner&>(*w.engines.kepler_planner_);
        plan = planner.plan_transfer(origin_def, *best_destination, ship, ship_class, choice.plan_departure_s, options);
    }
    if (!plan.feasible) {
        return;
    }
    const double time_cost_per_day = query::daily_capital_cost(w, ship_class) + systems::daily_crew_cost(w, ship_class, origin_state)
        + query::hazard_cr_per_day(w, ship_class, origin_def, origin_def, w.state.game_time_s_) + query::wear_cr_per_flight_day(w, ship_class);
    if (w.audits.trajectory_audit_enabled_) {
        record_trajectory_audit(w, ship, origin_def, *best_destination, plan, plan.propellant_load_kg);
    }
    // Load the mission's propellant (the plan's burn plus the reserve, and any return fuel).
    systems::buy_propellant(w, ship, plan.propellant_load_kg + choice.carried_propellant_kg - ship.propellant_kg);

    if (laid_up) {
        query::add_event(w, std::format("{} rehired its crew at {}", ship.name, origin_def.name), "mission");
    }
    ship.leg_dose_msv = 0.0;  // a crew is signed on for each leg
    const double mission_days = plan.travel_time_s / 86400.0;
    {
        auto& route = w.audits.passenger_audit_.routes[{origin_state.station_id, best_destination->id}];
        ++route.departures;
        if (ship_class.berths >= 1.0) {
            ++route.berth_departures;
            if (choice.passengers.empty()
                && std::any_of(origin_state.waiting.begin(), origin_state.waiting.end(), [&](const auto& group) {
                       return group.destination_id == best_destination->id && group.count >= 1.0;
                   })) {
                ++route.empty_with_waiting;
            }
        }
    }
    // Passengers board (and pay) before the stores are bought: they eat from them too.
    if (!choice.passengers.empty()) {
        systems::board_passengers(w, ship, origin_state, best_destination->id, choice.passengers, mission_days);
    }
    // What the mission needs may come out of the whole stock; the rest of the endurance
    // only out of what the station can spare.
    systems::buy_provisions(w, ship, origin_state,
        mission_days * PROVISION_MARGIN + std::max(PROVISION_MIN_RETURN_DAYS, mission_days), true);
    systems::buy_provisions(w, ship, origin_state, PROVISION_ENDURANCE_DAYS, false);

    double purchase_cost = 0.0;
    double expected_revenue = 0.0;
    const auto& destination_state = query::get_station_state(w, best_destination->id);
    // Contracts: each lot's price is agreed now, from the destination's forecast stock on
    // arrival (other ships' earlier deliveries included), for the units that will survive the
    // trip. The ship knows what it will earn; the station bears the forecast's error.
    auto contracted_cargo = cargo;
    const double arrival_days = std::max(0.0, plan.arrival_time_s - w.state.game_time_s_) / 86400.0;
    for (auto& lot : contracted_cargo) {
        const double surviving = lot.units
            * std::pow(1.0 - query::cargo_decay_per_day(w, ship_class, lot.commodity_id), plan.travel_time_s / 86400.0);
        if (lot.fixed_unit_price > 0.0) {
            lot.contract_value = surviving * lot.fixed_unit_price;
            systems::observe_contract(w, query::get_station_state(w, best_destination->id), lot.commodity_id, surviving, lot.fixed_unit_price);
            continue;
        }
        const auto split = systems::sale_split_on_arrival(w, destination_state, lot.commodity_id, surviving, arrival_days, ship.id);
        lot.contract_value = split.total;
        lot.emergency_premium = split.faction;
        // The going rate learns from order slices.
        if (split.order_units > 0.0) {
            systems::observe_contract(w, query::get_station_state(w, best_destination->id), lot.commodity_id, split.order_units,
                split.order_value / split.order_units);
        }
        if (split.faction > 0.0) {
            for (auto& emergency : w.state.emergencies_) {
                if (emergency.station_id == best_destination->id && emergency.commodity_id == lot.commodity_id) {
                    emergency.last_raise_s = w.state.game_time_s_;
                    emergency.units_open = std::max(0.0, emergency.units_open - split.emergency_units);
                }
            }
            query::add_event(w, std::format("{} takes an emergency delivery of {:.0f}u {} to {} ({:.0f} cr from {})",
                ship.name, surviving, query::get_commodity(w, lot.commodity_id).name, best_destination->name, split.faction,
                systems::faction_name(w, best_destination->faction_id)), "alert");
        }
    }
    for (const auto& lot : cargo) {
        // Buy at origin along the price curve as the stock falls.
        const double cost = systems::trade_value(w, origin_state, lot.commodity_id, -lot.units);
        origin_state.inventory[lot.commodity_id] -= lot.units;
        origin_state.export_units_per_day[lot.commodity_id] += lot.units / TRADE_FLOW_DAYS;
        ship.credits -= cost;
        ship.lifetime_profit -= cost;
        ship.ledger.cargo_purchases += cost;
        origin_state.credits += cost;
        origin_state.ledger.ship_sales += cost;
        w.state.journal_.record(query::account_fleet(w, ship.faction_id), query::account_station(w, origin_state), MoneyCategory::Cargo, cost);
        w.state.flows_.record(FlowKind::Loaded, origin_state.station_id, best_destination->id, lot.commodity_id, lot.units, cost);
        purchase_cost += cost;
        query::record_trade(w, {
            .time_s = w.state.game_time_s_,
            .ship_id = ship.id,
            .station_id = origin_state.station_id,
            .commodity_id = lot.commodity_id,
            .kind = "buy",
            .units = lot.units,
            .unit_price = cost / lot.units,
            .total = cost,
        });
    }
    for (const auto& lot : contracted_cargo) {
        expected_revenue += lot.contract_value;
    }
    if (systems::liner_stops(w, ship) != nullptr) {
        query::add_event(w, std::format("{} (liner) leaves {} on schedule for {}, departing day {:.0f}, arriving day {:.0f}{}",
            ship.name, origin_def.name, best_destination->name, plan.departure_time_s / 86400.0,
            plan.arrival_time_s / 86400.0, cargo.empty() ? " (empty)" : ""), "mission");
    }
    // Nuclear-thermal ships: deduct propellant at mission start (instantaneous burns).
    // Variable-Isp ships: propellant is consumed continuously during transit and
    // tracked per-sample, so we don't deduct here — ship.propellant_kg is updated
    // in step_in_transit_ship from sampled_propellant_kg.
    if (ship_class.propulsion_type != "variable_isp") {
        ship.propellant_kg -= plan.propellant_required_kg;
        if (w.data.universe_.ship_environment.wear) {
            ship.engine_wear = std::min(1.0, ship.engine_wear + query::drive_hours(ship_class, plan) / query::drive_life_h(w, ship_class));
        }
    }
    auto sampled_propellant = plan.sampled_propellant_kg;
    ship.phase = plan.wait_time_s > 0.0 ? domain::ShipMissionPhase::AwaitingDeparture : domain::ShipMissionPhase::InTransit;
    ship.active_mission = {
        .origin_station_id = origin_def.id,
        .destination_station_id = best_destination->id,
        .cargo = contracted_cargo,
        .departure_time_s = plan.departure_time_s,
        .arrival_time_s = plan.arrival_time_s,
        .wait_time_s = plan.wait_time_s,
        .coast_time_s = plan.coast_time_s,
        .total_travel_time_s = plan.travel_time_s,
        .remaining_travel_time_s = plan.travel_time_s,
        .propellant_cost_kg = consumed_propellant_kg(plan),
        .purchase_cost = purchase_cost,
        .fuel_cost = (consumed_propellant_kg(plan) / FUEL_UNITS_TO_KG) * systems::station_price(w, origin_state, "fuel"),
        .expected_revenue = expected_revenue,
        .operating_cost = time_cost_per_day * mission_days,
        .sampled_path = plan.sampled_path,
        .sampled_times_s = plan.sampled_times_s,
        .sampled_propellant_kg = std::move(sampled_propellant),
        .trajectory_type = plan.trajectory_type,
        .carried_propellant_kg = choice.carried_propellant_kg,
        .pickup_commodity_id = choice.pickup_commodity_id,
        .pickup_units = choice.pickup_units,
    };
    if (choice.carried_propellant_kg > 0.0) {
        query::add_event(w, std::format("{} carries {:.0f} kg of return fuel to {}", ship.name, choice.carried_propellant_kg,
            best_destination->name), "fuel");
    }
    if (plan.wait_time_s > 0.0) {
        if (best_cargo_units > 0.0) {
            query::add_event(w, std::format(
                "{} scheduled {}->{} ({}) in {:.1f}d  est. profit {:.0f} cr ({:.1f}d travel)",
                ship.name,
                origin_def.name,
                best_destination->name,
                describe_cargo(cargo),
                plan.wait_time_s / 86400.0,
                expected_revenue - purchase_cost,
                plan.travel_time_s / 86400.0),
                "mission");
        } else {
            query::add_event(w, std::format(
                "{} repositioning {}->{} in {:.1f}d",
                ship.name,
                origin_def.name,
                best_destination->name,
                plan.wait_time_s / 86400.0),
                "mission");
        }
    } else if (best_cargo_units > 0.0) {
        query::add_event(w, std::format(
            "{} departed {}->{} ({})  est. profit {:.0f} cr ({:.1f}d transit, prop={:.0f}kg)",
            ship.name,
            origin_def.name,
            best_destination->name,
            describe_cargo(cargo),
            expected_revenue - purchase_cost,
            plan.travel_time_s / 86400.0,
            plan.propellant_required_kg),
            "mission");
    } else {
        query::add_event(w, std::format("{} repositioned from {} to {}", ship.name, origin_def.name, best_destination->name), "mission");
    }
}

namespace {

double refit_bill(const World& w, 
    const domain::ShipClassDefinition& from, const domain::ShipClassDefinition& to) {
    return w.data.universe_.ship_operations.refit_cost_fraction * std::abs(to.ship_value_cr - from.ship_value_cr);
}

bool consider_refit(World& w, domain::ShipState& ship, const MissionChoice& current, bool trace) {
    const profiling::Scope profile_scope(profiling::Phase::ConsiderRefit);
    if (ship.current_station_id != ship.home_station_id || w.state.game_time_s_ < ship.next_refit_review_s) {
        return false;
    }
    ship.next_refit_review_s = w.state.game_time_s_ + REFIT_REVIEW_DAYS * 86400.0;
    const auto& ship_class = query::ship_design(w, ship);
    const double refit_days = w.data.universe_.ship_operations.refit_days;
    // What the ship earns as it is: dispatch's profit rate, or nothing (a repositioning
    // score is not a profit, and such a ship has no paying work here either).
    const double current_score = current.kind == MissionChoice::Kind::Mission ? std::max(0.0, current.score) : 0.0;

    // One yard visit: the next tank size either way, or one module in or out.
    const auto options = neighbouring_fits(w, ship.class_id, ship.fit, query::radiator_bucket(w, ship));
    const FitOption* best_option = nullptr;
    const domain::ShipClassDefinition* best_class = nullptr;
    double best_gain = 0.0;
    for (const auto& fit_option : options) {
        const auto& candidate = *fit_option.design;
        const double bill = refit_bill(w, ship_class, candidate);
        if (ship.credits - bill < -query::credit_line(candidate)) {
            continue;
        }
        domain::ShipState probe = ship;
        probe.class_id = fit_option.class_id;
        probe.fit = fit_option.fit;
        probe.propellant_kg = std::min(ship.propellant_kg, candidate.propellant_capacity_kg);
        // Scored on departures after the yard is done: launch windows close meanwhile.
        const auto option = systems::choose_mission(w, probe, candidate, trace, w.state.game_time_s_ + refit_days * 86400.0);
        if (option.kind != MissionChoice::Kind::Mission) {
            continue;
        }
        // The better missions must repay the yard bill and the earnings lost in the yard.
        // The scores already carry each variant's capital charge.
        const double gain = (option.score - current_score) * REFIT_PAYBACK_DAYS - bill - current_score * refit_days;
        if (trace) {
            std::cerr << std::format("[trace day {:.1f}] {}: refit to {}: score {:.1f} vs {:.1f}, bill {:.0f}, gain {:.0f}\n",
                w.state.game_time_s_ / 86400.0, ship.name, candidate.id, option.score, current_score, bill, gain);
        }
        if (gain > best_gain) {
            best_gain = gain;
            best_class = &candidate;
            best_option = &fit_option;
        }
    }
    if (best_class == nullptr) {
        return false;
    }

    auto& yard = query::get_station_state(w, ship.current_station_id);
    // Propellant that no longer fits goes back to the station's depot.
    const double excess_kg = ship.propellant_kg - best_class->propellant_capacity_kg;
    if (excess_kg > 0.0) {
        const double units = excess_kg / FUEL_UNITS_TO_KG;
        const double value = systems::trade_value(w, yard, "fuel", units);
        yard.inventory["fuel"] += units;
        yard.credits -= value;
        yard.ledger.ship_purchases += value;
        ship.propellant_kg -= excess_kg;
        ship.credits += value;
        ship.lifetime_profit += value;
        ship.ledger.fuel -= value;
        w.state.journal_.record(query::account_station(w, yard), query::account_fleet(w, ship.faction_id), MoneyCategory::FuelBuyback, value);
        query::record_trade(w, {
            .time_s = w.state.game_time_s_,
            .ship_id = ship.id,
            .station_id = yard.station_id,
            .commodity_id = "fuel",
            .kind = "sell",
            .units = units,
            .unit_price = value / units,
            .total = value,
        });
    }
    const double bill = refit_bill(w, ship_class, *best_class);
    ship.credits -= bill;
    ship.lifetime_profit -= bill;
    ship.ledger.refits += bill;
    yard.credits += bill;
    yard.ledger.ship_sales += bill;
    w.state.journal_.record(query::account_fleet(w, ship.faction_id), query::account_station(w, yard), MoneyCategory::RefitsRepairs, bill);
    ship.refit_class_id = best_option->class_id;
    ship.refit_fit = best_option->fit;
    ship.refit_done_s = w.state.game_time_s_ + refit_days * 86400.0;
    // The refit was justified by a payback period of better missions: give it that long
    // before reconsidering, or ships swap tanks back and forth on each new forecast.
    ship.next_refit_review_s = ship.refit_done_s + REFIT_PAYBACK_DAYS * 86400.0;
    ship.phase = domain::ShipMissionPhase::Refitting;
    query::add_event(w, std::format("{} refitting at {}: {} -> {} ({:.0f} cr, {:.0f} days)",
        ship.name, query::get_station_definition(w, ship.current_station_id).name, ship_class.name, best_class->name,
        bill, refit_days), "mission");
    return true;
}

}  // namespace

void step_refitting_ship(World& w, domain::ShipState& ship) {
    if (w.state.game_time_s_ < ship.refit_done_s) {
        return;
    }
    if (ship.overhaul) {
        ship.engine_wear = 0.0;
        ship.radiator_condition = 1.0;
        ship.overhaul = false;
    }
    (void)query::register_design(w, ship.refit_class_id, ship.refit_fit, query::radiator_bucket(w, ship));
    ship.class_id = ship.refit_class_id;
    ship.fit = std::move(ship.refit_fit);
    ship.refit_class_id.clear();
    ship.refit_fit.clear();
    ship.phase = domain::ShipMissionPhase::Idle;
    ship.idle_since_s = w.state.game_time_s_;
    ship.next_review_s = w.state.game_time_s_;
    query::add_event(w, std::format("{} left the yard at {} as {}",
        ship.name, query::get_station_definition(w, ship.current_station_id).name, query::ship_design(w, ship).name), "mission");
}

void step_awaiting_departure_ship(World& w, domain::ShipState& ship) {
    ship.active_mission.remaining_travel_time_s = std::max(0.0, ship.active_mission.arrival_time_s - w.state.game_time_s_);
    if (w.state.game_time_s_ < ship.active_mission.departure_time_s) {
        return;
    }
    ship.phase = domain::ShipMissionPhase::InTransit;
    const auto& origin = query::get_station_definition(w, ship.active_mission.origin_station_id);
    const auto& destination = query::get_station_definition(w, ship.active_mission.destination_station_id);
    if (!ship.active_mission.cargo.empty()) {
        query::add_event(w, std::format("{} departed {} for {} carrying {}", ship.name, origin.name, destination.name,
            describe_cargo(ship.active_mission.cargo)), "mission");
    } else {
        query::add_event(w, std::format("{} departed {} for {}", ship.name, origin.name, destination.name), "mission");
    }
}

void step_in_transit_ship(World& w, domain::ShipState& ship, double dt_s) {
    (void)dt_s;
    ship.active_mission.remaining_travel_time_s = std::max(0.0, ship.active_mission.arrival_time_s - w.state.game_time_s_);

    // Update propellant continuously from the pre-computed mass samples (variable-Isp drives only).
    // The samples run from initial propellant at departure to final propellant at arrival,
    // giving a smooth display rather than a step-change at mission assignment.
    if (!ship.active_mission.sampled_propellant_kg.empty()
        && ship.active_mission.sampled_times_s.size() == ship.active_mission.sampled_propellant_kg.size()) {
        ship.propellant_kg = interpolate_timed_scalar(
            ship.active_mission.sampled_propellant_kg,
            ship.active_mission.sampled_times_s,
            w.state.game_time_s_) + ship.active_mission.carried_propellant_kg - ship.active_mission.boiled_propellant_kg;
        ship.propellant_kg = std::max(0.0, ship.propellant_kg);
    }

    if (!ship.active_mission.salvage_derelict_id.empty() && !ship.active_mission.salvage_done
        && w.state.game_time_s_ >= ship.active_mission.rendezvous_time_s) {
        systems::reach_derelict(w, ship);
    }
    if (ship.active_mission.remaining_travel_time_s > 0.0) {
        return;
    }
    if (const auto& samples = ship.active_mission.sampled_propellant_kg;
        samples.size() >= 2 && query::ship_design(w, ship).propulsion_type != "variable_isp") {
        // The last sample is after the arrival burn; the planned reserve is a share of the burns.
        const double left_kg = samples.back() + ship.active_mission.carried_propellant_kg
            - ship.active_mission.boiled_propellant_kg;
        const double reserve_kg = PROPELLANT_RESERVE_FRACTION * std::max(0.0, samples.front() - samples.back());
        ++w.audits.radiation_audit_.thermal_arrivals;
        if (left_kg < -1.0) {
            ++w.audits.radiation_audit_.arrivals_short_of_burn;
        } else if (left_kg < 0.5 * reserve_kg) {
            ++w.audits.radiation_audit_.arrivals_below_half_reserve;
        }
        if (reserve_kg > 1.0) {
            w.audits.radiation_audit_.lowest_reserve_share = std::min(w.audits.radiation_audit_.lowest_reserve_share, left_kg / reserve_kg);
        }
    }

    ship.current_station_id = ship.active_mission.destination_station_id;
    ship.phase = domain::ShipMissionPhase::Idle;
    ship.idle_since_s = w.state.game_time_s_;
    auto& destination = query::get_station_state(w, ship.current_station_id);
    if (!ship.active_mission.cargo.empty()) {
        const auto& destination_def = query::get_station_definition(w, ship.current_station_id);
        const double transit_days = ship.active_mission.total_travel_time_s / 86400.0;
        // Deliveries beyond the destination's free storage are jettisoned.
        double free_capacity = destination_def.storage_capacity_units > 0.0
            ? std::max(0.0, destination_def.storage_capacity_units - w.engines.economy_.storage_used_units(destination_def, destination.inventory))
            : std::numeric_limits<double>::infinity();
        double total_revenue = 0.0;
        double total_spoiled = 0.0;
        std::vector<domain::CargoLot> sold;
        for (const auto& lot : ship.active_mission.cargo) {
            // Some goods (food, medicine) spoil in transit.
            const double decay_per_day = query::cargo_decay_per_day(w, query::ship_design(w, ship), lot.commodity_id);
            const double surviving = decay_per_day > 0.0 ? std::max(0.0, std::pow(1.0 - decay_per_day, transit_days)) : 1.0;
            double arrived = lot.units * surviving;
            total_spoiled += lot.units - arrived;
            if (arrived > free_capacity) {
                query::add_event(w, std::format("{} jettisoned {:.1f}u {} at {} — storage full",
                    ship.name, arrived - free_capacity, lot.commodity_id, destination_def.name), "alert");
                arrived = free_capacity;
            }
            free_capacity -= arrived;
            // The agreed price for what arrives (less if storage forced a jettison), or, without a
            // contract, the market's along the price curve as the delivery lands.
            const double expected_units = lot.units * surviving;
            const double delivered_share = expected_units > 0.0 ? std::min(1.0, arrived / expected_units) : 0.0;
            const double revenue = lot.contract_value > 0.0 && expected_units > 0.0
                ? lot.contract_value * delivered_share
                : systems::trade_value(w, destination, lot.commodity_id, arrived);
            // An emergency premium comes from the station's faction, the rest from the station.
            const double faction_share = lot.emergency_premium * delivered_share;
            if (faction_share > 0.0) {
                w.state.faction_treasuries_[destination_def.faction_id] -= faction_share;
                w.state.emergency_paid_ += faction_share;
                for (auto& emergency : w.state.emergencies_) {
                    if (emergency.station_id == destination.station_id && emergency.commodity_id == lot.commodity_id) {
                        emergency.faction_paid += faction_share;
                    }
                }
            }
            // The station's import cost: what it paid per unit, averaged over about 30 days of its use.
            if (arrived > 0.0 && !w.engines.economy_.is_export_market(destination_def, lot.commodity_id)) {
                const auto& rates = w.engines.economy_.get_station_net_rates(destination_def);
                const auto rate = rates.find(lot.commodity_id);
                const double history = rate == rates.end() ? 0.0 : std::max(0.0, -rate->second) * IMPORT_COST_DAYS;
                const auto known = destination.import_unit_cost.find(lot.commodity_id);
                const double previous = known == destination.import_unit_cost.end()
                    ? w.engines.economy_.reference_price(destination_def, lot.commodity_id) : known->second;
                destination.import_unit_cost[lot.commodity_id] =
                    (previous * history + (revenue - faction_share)) / (history + arrived);
            }
            destination.inventory[lot.commodity_id] += arrived;
            destination.import_units_per_day[lot.commodity_id] += arrived / TRADE_FLOW_DAYS;
            // Debug aid: SPACETRAINS_TRACE_DELIVERIES=1 logs every delivery (stderr).
            if (static const bool trace_deliveries = std::getenv("SPACETRAINS_TRACE_DELIVERIES") != nullptr;
                trace_deliveries && arrived > 0.0) {
                std::cerr << std::format("[DELIVERY] day {:.0f} {} {} {:.0f}u at {:.1f}/u by {} ({}) lead {:.0f}d\n",
                    w.state.game_time_s_ / 86400.0, destination.station_id, lot.commodity_id, arrived, revenue / arrived, ship.name,
                    ship.class_id, (w.state.game_time_s_ - ship.active_mission.departure_time_s + ship.active_mission.wait_time_s) / 86400.0);
            }
            if (arrived > 0.0) {
                systems::observe_delivery(w, destination, lot.commodity_id,
                    ship.active_mission.departure_time_s - ship.active_mission.wait_time_s);
                // Audit only: what stations pay against the landed reference price.
                const auto& refs = w.data.universe_.pricing.reference_prices;
                if (const auto at = refs.find(destination.station_id); at != refs.end()) {
                    if (const auto ref = at->second.find(lot.commodity_id); ref != at->second.end()) {
                        w.audits.delivered_paid_cr_ += revenue;
                        w.audits.delivered_reference_cr_ += arrived * ref->second;
                    }
                }
            }
            destination.credits -= revenue - faction_share;
            destination.ledger.ship_purchases += revenue - faction_share;
            ship.credits += revenue;
            w.state.journal_.record(query::account_station(w, destination), query::account_fleet(w, ship.faction_id), MoneyCategory::Cargo, revenue - faction_share);
            if (faction_share > 0.0) {
                w.state.journal_.record(query::account_faction(w, destination_def.faction_id), query::account_fleet(w, ship.faction_id),
                    MoneyCategory::EmergencyPremium, faction_share);
            }
            ship.lifetime_profit += revenue;
            ship.ledger.cargo_revenue += revenue;
            total_revenue += revenue;
            if (arrived > 0.0) {
                w.state.flows_.record(FlowKind::Delivered, ship.active_mission.origin_station_id, destination.station_id,
                    lot.commodity_id, arrived, revenue);
                query::record_trade(w, {
                    .time_s = w.state.game_time_s_,
                    .ship_id = ship.id,
                    .station_id = destination.station_id,
                    .commodity_id = lot.commodity_id,
                    .kind = "sell",
                    .units = arrived,
                    .unit_price = revenue / arrived,
                    .total = revenue,
                });
                sold.push_back({.commodity_id = lot.commodity_id, .units = arrived});
            }
        }
        query::add_event(w, total_spoiled > 0.1
            ? std::format("{} arrived at {} with {} for {:.0f} cr ({:.1f}u spoiled in {:.0f}d transit)",
                  ship.name, destination_def.name, describe_cargo(sold), total_revenue, total_spoiled, transit_days)
            : std::format("{} arrived at {} and sold {} for {:.0f} cr", ship.name, destination_def.name,
                  describe_cargo(sold), total_revenue),
            "arrival");
    } else {
        query::add_event(w, std::format("{} arrived at {}", ship.name, query::get_station_definition(w, ship.current_station_id).name), "arrival");
    }
    {
        // The lane's transport work: cargo unit-days and the propellant the plan burned, by class
        // (the porkchop plots show the classes that fly a route).
        double unit_days = 0.0;
        for (const auto& lot : ship.active_mission.cargo) {
            unit_days += lot.units * std::max(0.0, ship.active_mission.total_travel_time_s) / 86400.0;
        }
        w.state.flows_.record(FlowKind::Lane, ship.active_mission.origin_station_id, destination.station_id, ship.class_id, unit_days,
            ship.active_mission.propellant_cost_kg);
    }
    systems::land_passengers(w, ship, destination);
    ship.active_mission = {};
}

// From Simulation.cpp (split step 12): used only by the phases.

namespace {

std::vector<FitOption> neighbouring_fits(World& w,
    const std::string& class_id, const std::vector<std::string>& fit, double radiator_bucket) {
    const auto& current = query::get_ship_class(w, class_id);
    std::vector<FitOption> options;
    const auto add = [&](const std::string& option_class, std::vector<std::string> option_fit) {
        try {
            const auto& design = query::register_design(w, option_class, option_fit, radiator_bucket);
            options.push_back({.design = &design, .class_id = option_class, .fit = std::move(option_fit)});
        } catch (const std::runtime_error&) {
            // The fit does not suit this hull (slots full): not an option.
        }
    };
    // Only the neighbouring tank sizes: larger steps take several refits.
    const domain::ShipClassDefinition* next_smaller = nullptr;
    const domain::ShipClassDefinition* next_larger = nullptr;
    for (const auto& other : w.data.universe_.ship_classes) {
        if (other.hull_id != current.hull_id) {
            continue;
        }
        const double tank_kg = other.propellant_capacity_kg;
        if (tank_kg < current.propellant_capacity_kg
            && (next_smaller == nullptr || tank_kg > next_smaller->propellant_capacity_kg)) {
            next_smaller = &other;
        }
        if (tank_kg > current.propellant_capacity_kg
            && (next_larger == nullptr || tank_kg < next_larger->propellant_capacity_kg)) {
            next_larger = &other;
        }
    }
    for (const auto* tank : {next_smaller, next_larger}) {
        if (tank != nullptr) {
            add(tank->id, fit);
        }
    }
    for (const auto& module : w.data.universe_.ship_modules) {
        if (module.hull_id != current.hull_id) {
            continue;
        }
        const bool fitted = std::find(fit.begin(), fit.end(), module.id) != fit.end();
        if (fitted) {
            std::vector<std::string> without;
            std::copy_if(fit.begin(), fit.end(), std::back_inserter(without),
                [&](const auto& id) { return id != module.id; });
            add(class_id, std::move(without));
        } else if (query::module_has_active_effect(w, module)) {
            auto with = fit;
            with.push_back(module.id);
            std::sort(with.begin(), with.end());
            add(class_id, std::move(with));
        }
    }
    return options;
}

void record_trajectory_audit(
    World& w,
    const domain::ShipState& ship,
    const domain::StationDefinition& origin,
    const domain::StationDefinition& destination,
    const domain::TrajectoryPlan& plan,
    double planning_propellant_kg) {
    const double r_origin_m = w.engines.mechanics_.get_heliocentric_radius(origin.parent_body_id, plan.departure_time_s);
    const double r_dest_m = w.engines.mechanics_.get_heliocentric_radius(destination.parent_body_id, plan.arrival_time_s);
    auto metrics = trajectory::audit_trajectory(plan);
    const bool flagged = !metrics.flags.empty();
    w.audits.trajectory_audit_records_.push_back({
        .planned_at_s = w.state.game_time_s_,
        .ship_name = ship.name,
        .class_id = ship.class_id,
        .origin_station_id = origin.id,
        .destination_station_id = destination.id,
        .trajectory_type = plan.trajectory_type,
        .planning_propellant_kg = planning_propellant_kg,
        .wait_time_s = plan.wait_time_s,
        .coast_time_s = plan.coast_time_s,
        .r_origin_m = r_origin_m,
        .r_dest_m = r_dest_m,
        .diagnostics = plan.diagnostics,
        .metrics = std::move(metrics),
        .sampled_path = flagged ? plan.sampled_path : std::vector<math::Vec3d>{},
        .sampled_times_s = flagged ? plan.sampled_times_s : std::vector<double>{},
    });
}

}  // namespace

}  // namespace spacetrains::simulation::systems
