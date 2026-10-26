// A ship's day by phase: idle, refitting, awaiting departure, in transit, arrival.

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

void Simulation::step_idle_ship(domain::ShipState& ship) {
    const profiling::Scope profile_scope(profiling::Phase::ShipReview);
    // Planning is expensive, so docked ships look for work every few hours, and laid-up
    // ships (no crew) every few days, not on every tick.
    const bool laid_up = ship.phase == domain::ShipMissionPhase::LaidUp;
    if (game_time_s_ < ship.next_review_s) {
        return;
    }
    ship.next_review_s = game_time_s_ + (laid_up ? LAYUP_REVIEW_DAYS * 86400.0 : IDLE_REVIEW_S);

    // Debug aid: SPACETRAINS_TRACE_SHIP="<ship name>" logs every candidate mission of
    // that ship and why it was rejected (stderr).
    static const char* const trace_ship = std::getenv("SPACETRAINS_TRACE_SHIP");
    const bool trace = trace_ship != nullptr && ship.name == trace_ship;

    if (consider_repair(ship)) {
        return;
    }
    const auto& ship_class = ship_design(ship);
    const auto review_start = std::chrono::steady_clock::now();
    // A rotation contract that ran out ends here.
    if (!ship.contract.stops.empty() && game_time_s_ >= ship.contract.until_s) {
        add_event(std::format("{}'s rotation contract ended at {}", ship.name,
            get_station_definition(ship.current_station_id).name), "mission");
        ship.contract = {};
    }
    // A loop's wait in port at its first stop, counted from arrival (cycle minus the legs).
    if (under_contract(ship) && ship.current_station_id == ship.contract.stops.front()) {
        const double ready_s = ship.idle_since_s + contract_dwell_days(ship.contract) * 86400.0;
        if (game_time_s_ < ready_s) {
            ship.next_review_s = std::min(ship.next_review_s, ready_s);
            return;
        }
    }
    auto choice = choose_mission(ship, ship_class, trace, game_time_s_);
    // Rotation contracts: a free ship looks for a loop now and then, and signs one that earns
    // about as much as the best spot mission (a repositioning earns nothing).
    const auto& market = universe_.market;
    if (market.contracts > 0.0 && ship.contract.stops.empty() && liner_stops(ship) == nullptr
        && !(!ship.route_destination_id.empty() && game_time_s_ < ship.route_until_s)
        && game_time_s_ >= ship.next_contract_review_s) {
        // Reviews are spread over the review period by ship (never the whole fleet at once).
        if (ship.next_contract_review_s <= 0.0) {
            const auto spread = static_cast<double>(std::hash<std::string> {}(ship.id) % 1000) / 1000.0;
            ship.next_contract_review_s = game_time_s_ + spread * market.contract_review_days * 86400.0;
        }
    }
    if (market.contracts > 0.0 && ship.contract.stops.empty() && liner_stops(ship) == nullptr
        && !(!ship.route_destination_id.empty() && game_time_s_ < ship.route_until_s)
        && game_time_s_ >= ship.next_contract_review_s) {
        ship.next_contract_review_s = game_time_s_ + market.contract_review_days * 86400.0;
        auto [contract, score] = find_contract(ship, ship_class);
        const double spot = choice.kind == MissionChoice::Kind::Mission ? choice.score : 0.0;
        if (!contract.stops.empty() && score > MIN_MISSION_SCORE_PER_DAY && score >= market.contract_accept * spot) {
            std::string route;
            for (const auto& stop : contract.stops) {
                route += get_station_definition(stop).name + " > ";
            }
            std::string goods;
            for (const auto& delivery : contract.deliveries) {
                goods += std::format("{}{:.0f}u {} to {} at {:.0f}", goods.empty() ? "" : ", ", delivery.units,
                    delivery.commodity_id, delivery.to_station_id, delivery.unit_price);
            }
            add_event(std::format("{} signs a rotation contract: {}({:.0f}-day loop, {:.0f} cr/day against {:.0f} spot): {}",
                ship.name, route, contract.cycle_days, score, spot, goods), "mission");
            ship.contract = std::move(contract);
            choice = choose_mission(ship, ship_class, trace, game_time_s_);
        }
    }
    // Debug aid: with SPACETRAINS_PROFILE, reviews over half a second are reported (stderr).
    if (profiling::enabled()) {
        const double review_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - review_start).count();
        if (review_s > 0.5) {
            std::cerr << std::format("[slow review day {:.1f}] {} ({}) at {}: {:.2f} s\n", game_time_s_ / 86400.0, ship.name,
                ship_class.id, ship.current_station_id, review_s);
        }
    }
    if (scheduled_stops(ship) == nullptr && consider_refit(ship, choice, trace)) {
        return;
    }

    auto& origin_state = get_station_state(ship.current_station_id);
    const auto& origin_def = get_station_definition(ship.current_station_id);
    if (choice.kind == MissionChoice::Kind::None && under_contract(ship)) {
        // The loop cannot be flown from here (no fuel, no trajectory): the contract lapses.
        add_event(std::format("{} gives up its rotation contract at {}", ship.name, origin_def.name), "alert");
        ship.contract = {};
        return;
    }
    if (choice.kind == MissionChoice::Kind::None) {
        if (ship.propellant_kg <= ship_class.propellant_capacity_kg * 0.01 && purchasable_propellant_kg(ship) <= 0.0) {
            ship.phase = domain::ShipMissionPhase::Stranded;
            add_event(std::format("{} is stranded at {} due to fuel shortage", ship.name, origin_def.name), "alert");
        } else if (!laid_up && liner_stops(ship) == nullptr
            && (ship.credits < 0.0 || game_time_s_ - ship.idle_since_s > IDLE_LAYUP_DAYS * 86400.0)) {
            // Nothing worth flying and either in debt or idle for a month: stop paying the crew.
            ship.phase = domain::ShipMissionPhase::LaidUp;
            ship.laid_up_since_s = game_time_s_;
            ship.next_review_s = game_time_s_ + LAYUP_REVIEW_DAYS * 86400.0;
            add_event(ship.credits < 0.0
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
        const auto& planner = (ship_class.propulsion_type == "variable_isp" && variable_isp_planner_)
            ? static_cast<trajectory::ITrajectoryPlanner&>(*variable_isp_planner_)
            : static_cast<trajectory::ITrajectoryPlanner&>(*kepler_planner_);
        plan = planner.plan_transfer(origin_def, *best_destination, ship, ship_class, choice.plan_departure_s, options);
    }
    if (!plan.feasible) {
        return;
    }
    const double time_cost_per_day = daily_capital_cost(ship_class) + daily_crew_cost(ship_class, origin_state)
        + hazard_cr_per_day(ship_class, origin_def, origin_def, game_time_s_) + wear_cr_per_flight_day(ship_class);
    if (trajectory_audit_enabled_) {
        record_trajectory_audit(ship, origin_def, *best_destination, plan, plan.propellant_load_kg);
    }
    // Load the mission's propellant (the plan's burn plus the reserve, and any return fuel).
    buy_propellant(ship, plan.propellant_load_kg + choice.carried_propellant_kg - ship.propellant_kg);

    if (laid_up) {
        add_event(std::format("{} rehired its crew at {}", ship.name, origin_def.name), "mission");
    }
    ship.leg_dose_msv = 0.0;  // a crew is signed on for each leg
    const double mission_days = plan.travel_time_s / 86400.0;
    {
        auto& route = passenger_audit_.routes[{origin_state.station_id, best_destination->id}];
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
        board_passengers(ship, origin_state, best_destination->id, choice.passengers, mission_days);
    }
    // What the mission needs may come out of the whole stock; the rest of the endurance
    // only out of what the station can spare.
    buy_provisions(ship, origin_state,
        mission_days * PROVISION_MARGIN + std::max(PROVISION_MIN_RETURN_DAYS, mission_days), true);
    buy_provisions(ship, origin_state, PROVISION_ENDURANCE_DAYS, false);

    double purchase_cost = 0.0;
    double expected_revenue = 0.0;
    const auto& destination_state = get_station_state(best_destination->id);
    // Contracts: each lot's price is agreed now, from the destination's forecast stock on
    // arrival (other ships' earlier deliveries included), for the units that will survive the
    // trip. The ship knows what it will earn; the station bears the forecast's error.
    auto contracted_cargo = cargo;
    const double arrival_days = std::max(0.0, plan.arrival_time_s - game_time_s_) / 86400.0;
    for (auto& lot : contracted_cargo) {
        const double surviving = lot.units
            * std::pow(1.0 - cargo_decay_per_day(ship_class, lot.commodity_id), plan.travel_time_s / 86400.0);
        if (lot.fixed_unit_price > 0.0) {
            lot.contract_value = surviving * lot.fixed_unit_price;
            observe_contract(get_station_state(best_destination->id), lot.commodity_id, surviving, lot.fixed_unit_price);
            continue;
        }
        const auto split = sale_split_on_arrival(destination_state, lot.commodity_id, surviving, arrival_days, ship.id);
        lot.contract_value = split.total;
        lot.emergency_premium = split.faction;
        // The going rate learns from order slices.
        if (split.order_units > 0.0) {
            observe_contract(get_station_state(best_destination->id), lot.commodity_id, split.order_units,
                split.order_value / split.order_units);
        }
        if (split.faction > 0.0) {
            for (auto& emergency : emergencies_) {
                if (emergency.station_id == best_destination->id && emergency.commodity_id == lot.commodity_id) {
                    emergency.last_raise_s = game_time_s_;
                    emergency.units_open = std::max(0.0, emergency.units_open - split.emergency_units);
                }
            }
            add_event(std::format("{} takes an emergency delivery of {:.0f}u {} to {} ({:.0f} cr from {})",
                ship.name, surviving, get_commodity(lot.commodity_id).name, best_destination->name, split.faction,
                faction_name(best_destination->faction_id)), "alert");
        }
    }
    for (const auto& lot : cargo) {
        // Buy at origin along the price curve as the stock falls.
        const double cost = trade_value(origin_state, lot.commodity_id, -lot.units);
        origin_state.inventory[lot.commodity_id] -= lot.units;
        origin_state.export_units_per_day[lot.commodity_id] += lot.units / TRADE_FLOW_DAYS;
        ship.credits -= cost;
        ship.lifetime_profit -= cost;
        ship.ledger.cargo_purchases += cost;
        origin_state.credits += cost;
        origin_state.ledger.ship_sales += cost;
        journal_.record(account_fleet(ship.faction_id), account_station(origin_state), MoneyCategory::Cargo, cost);
        purchase_cost += cost;
        record_trade({
            .time_s = game_time_s_,
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
    if (liner_stops(ship) != nullptr) {
        add_event(std::format("{} (liner) leaves {} on schedule for {}, departing day {:.0f}, arriving day {:.0f}{}",
            ship.name, origin_def.name, best_destination->name, plan.departure_time_s / 86400.0,
            plan.arrival_time_s / 86400.0, cargo.empty() ? " (empty)" : ""), "mission");
    }
    // Nuclear-thermal ships: deduct propellant at mission start (instantaneous burns).
    // Variable-Isp ships: propellant is consumed continuously during transit and
    // tracked per-sample, so we don't deduct here — ship.propellant_kg is updated
    // in step_in_transit_ship from sampled_propellant_kg.
    if (ship_class.propulsion_type != "variable_isp") {
        ship.propellant_kg -= plan.propellant_required_kg;
        if (universe_.ship_environment.wear) {
            ship.engine_wear = std::min(1.0, ship.engine_wear + drive_hours(ship_class, plan) / drive_life_h(ship_class));
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
        .fuel_cost = (consumed_propellant_kg(plan) / FUEL_UNITS_TO_KG) * station_price(origin_state, "fuel"),
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
        add_event(std::format("{} carries {:.0f} kg of return fuel to {}", ship.name, choice.carried_propellant_kg,
            best_destination->name), "fuel");
    }
    if (plan.wait_time_s > 0.0) {
        if (best_cargo_units > 0.0) {
            add_event(std::format(
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
            add_event(std::format(
                "{} repositioning {}->{} in {:.1f}d",
                ship.name,
                origin_def.name,
                best_destination->name,
                plan.wait_time_s / 86400.0),
                "mission");
        }
    } else if (best_cargo_units > 0.0) {
        add_event(std::format(
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
        add_event(std::format("{} repositioned from {} to {}", ship.name, origin_def.name, best_destination->name), "mission");
    }
}

double Simulation::refit_bill(
    const domain::ShipClassDefinition& from, const domain::ShipClassDefinition& to) const {
    return universe_.ship_operations.refit_cost_fraction * std::abs(to.ship_value_cr - from.ship_value_cr);
}

bool Simulation::consider_refit(domain::ShipState& ship, const MissionChoice& current, bool trace) {
    const profiling::Scope profile_scope(profiling::Phase::ConsiderRefit);
    if (ship.current_station_id != ship.home_station_id || game_time_s_ < ship.next_refit_review_s) {
        return false;
    }
    ship.next_refit_review_s = game_time_s_ + REFIT_REVIEW_DAYS * 86400.0;
    const auto& ship_class = ship_design(ship);
    const double refit_days = universe_.ship_operations.refit_days;
    // What the ship earns as it is: dispatch's profit rate, or nothing (a repositioning
    // score is not a profit, and such a ship has no paying work here either).
    const double current_score = current.kind == MissionChoice::Kind::Mission ? std::max(0.0, current.score) : 0.0;

    // One yard visit: the next tank size either way, or one module in or out.
    const auto options = neighbouring_fits(ship.class_id, ship.fit, radiator_bucket(ship));
    const FitOption* best_option = nullptr;
    const domain::ShipClassDefinition* best_class = nullptr;
    double best_gain = 0.0;
    for (const auto& fit_option : options) {
        const auto& candidate = *fit_option.design;
        const double bill = refit_bill(ship_class, candidate);
        if (ship.credits - bill < -credit_line(candidate)) {
            continue;
        }
        domain::ShipState probe = ship;
        probe.class_id = fit_option.class_id;
        probe.fit = fit_option.fit;
        probe.propellant_kg = std::min(ship.propellant_kg, candidate.propellant_capacity_kg);
        // Scored on departures after the yard is done: launch windows close meanwhile.
        const auto option = choose_mission(probe, candidate, trace, game_time_s_ + refit_days * 86400.0);
        if (option.kind != MissionChoice::Kind::Mission) {
            continue;
        }
        // The better missions must repay the yard bill and the earnings lost in the yard.
        // The scores already carry each variant's capital charge.
        const double gain = (option.score - current_score) * REFIT_PAYBACK_DAYS - bill - current_score * refit_days;
        if (trace) {
            std::cerr << std::format("[trace day {:.1f}] {}: refit to {}: score {:.1f} vs {:.1f}, bill {:.0f}, gain {:.0f}\n",
                game_time_s_ / 86400.0, ship.name, candidate.id, option.score, current_score, bill, gain);
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

    auto& yard = get_station_state(ship.current_station_id);
    // Propellant that no longer fits goes back to the station's depot.
    const double excess_kg = ship.propellant_kg - best_class->propellant_capacity_kg;
    if (excess_kg > 0.0) {
        const double units = excess_kg / FUEL_UNITS_TO_KG;
        const double value = trade_value(yard, "fuel", units);
        yard.inventory["fuel"] += units;
        yard.credits -= value;
        yard.ledger.ship_purchases += value;
        ship.propellant_kg -= excess_kg;
        ship.credits += value;
        ship.lifetime_profit += value;
        ship.ledger.fuel -= value;
        journal_.record(account_station(yard), account_fleet(ship.faction_id), MoneyCategory::FuelBuyback, value);
        record_trade({
            .time_s = game_time_s_,
            .ship_id = ship.id,
            .station_id = yard.station_id,
            .commodity_id = "fuel",
            .kind = "sell",
            .units = units,
            .unit_price = value / units,
            .total = value,
        });
    }
    const double bill = refit_bill(ship_class, *best_class);
    ship.credits -= bill;
    ship.lifetime_profit -= bill;
    ship.ledger.refits += bill;
    yard.credits += bill;
    yard.ledger.ship_sales += bill;
    journal_.record(account_fleet(ship.faction_id), account_station(yard), MoneyCategory::RefitsRepairs, bill);
    ship.refit_class_id = best_option->class_id;
    ship.refit_fit = best_option->fit;
    ship.refit_done_s = game_time_s_ + refit_days * 86400.0;
    // The refit was justified by a payback period of better missions: give it that long
    // before reconsidering, or ships swap tanks back and forth on each new forecast.
    ship.next_refit_review_s = ship.refit_done_s + REFIT_PAYBACK_DAYS * 86400.0;
    ship.phase = domain::ShipMissionPhase::Refitting;
    add_event(std::format("{} refitting at {}: {} -> {} ({:.0f} cr, {:.0f} days)",
        ship.name, get_station_definition(ship.current_station_id).name, ship_class.name, best_class->name,
        bill, refit_days), "mission");
    return true;
}

void Simulation::step_refitting_ship(domain::ShipState& ship) {
    if (game_time_s_ < ship.refit_done_s) {
        return;
    }
    if (ship.overhaul) {
        ship.engine_wear = 0.0;
        ship.radiator_condition = 1.0;
        ship.overhaul = false;
    }
    (void)register_design(ship.refit_class_id, ship.refit_fit, radiator_bucket(ship));
    ship.class_id = ship.refit_class_id;
    ship.fit = std::move(ship.refit_fit);
    ship.refit_class_id.clear();
    ship.refit_fit.clear();
    ship.phase = domain::ShipMissionPhase::Idle;
    ship.idle_since_s = game_time_s_;
    ship.next_review_s = game_time_s_;
    add_event(std::format("{} left the yard at {} as {}",
        ship.name, get_station_definition(ship.current_station_id).name, ship_design(ship).name), "mission");
}

void Simulation::step_awaiting_departure_ship(domain::ShipState& ship) {
    ship.active_mission.remaining_travel_time_s = std::max(0.0, ship.active_mission.arrival_time_s - game_time_s_);
    if (game_time_s_ < ship.active_mission.departure_time_s) {
        return;
    }
    ship.phase = domain::ShipMissionPhase::InTransit;
    const auto& origin = get_station_definition(ship.active_mission.origin_station_id);
    const auto& destination = get_station_definition(ship.active_mission.destination_station_id);
    if (!ship.active_mission.cargo.empty()) {
        add_event(std::format("{} departed {} for {} carrying {}", ship.name, origin.name, destination.name,
            describe_cargo(ship.active_mission.cargo)), "mission");
    } else {
        add_event(std::format("{} departed {} for {}", ship.name, origin.name, destination.name), "mission");
    }
}

void Simulation::step_in_transit_ship(domain::ShipState& ship, double dt_s) {
    (void)dt_s;
    ship.active_mission.remaining_travel_time_s = std::max(0.0, ship.active_mission.arrival_time_s - game_time_s_);

    // Update propellant continuously from the pre-computed mass samples (variable-Isp drives only).
    // The samples run from initial propellant at departure to final propellant at arrival,
    // giving a smooth display rather than a step-change at mission assignment.
    if (!ship.active_mission.sampled_propellant_kg.empty()
        && ship.active_mission.sampled_times_s.size() == ship.active_mission.sampled_propellant_kg.size()) {
        ship.propellant_kg = interpolate_timed_scalar(
            ship.active_mission.sampled_propellant_kg,
            ship.active_mission.sampled_times_s,
            game_time_s_) + ship.active_mission.carried_propellant_kg - ship.active_mission.boiled_propellant_kg;
        ship.propellant_kg = std::max(0.0, ship.propellant_kg);
    }

    if (ship.active_mission.remaining_travel_time_s > 0.0) {
        return;
    }
    if (const auto& samples = ship.active_mission.sampled_propellant_kg;
        samples.size() >= 2 && ship_design(ship).propulsion_type != "variable_isp") {
        // The last sample is after the arrival burn; the planned reserve is a share of the burns.
        const double left_kg = samples.back() + ship.active_mission.carried_propellant_kg
            - ship.active_mission.boiled_propellant_kg;
        const double reserve_kg = PROPELLANT_RESERVE_FRACTION * std::max(0.0, samples.front() - samples.back());
        ++radiation_audit_.thermal_arrivals;
        if (left_kg < -1.0) {
            ++radiation_audit_.arrivals_short_of_burn;
        } else if (left_kg < 0.5 * reserve_kg) {
            ++radiation_audit_.arrivals_below_half_reserve;
        }
        if (reserve_kg > 1.0) {
            radiation_audit_.lowest_reserve_share = std::min(radiation_audit_.lowest_reserve_share, left_kg / reserve_kg);
        }
    }

    ship.current_station_id = ship.active_mission.destination_station_id;
    ship.phase = domain::ShipMissionPhase::Idle;
    ship.idle_since_s = game_time_s_;
    auto& destination = get_station_state(ship.current_station_id);
    if (!ship.active_mission.cargo.empty()) {
        const auto& destination_def = get_station_definition(ship.current_station_id);
        const double transit_days = ship.active_mission.total_travel_time_s / 86400.0;
        // Deliveries beyond the destination's free storage are jettisoned.
        double free_capacity = destination_def.storage_capacity_units > 0.0
            ? std::max(0.0, destination_def.storage_capacity_units - economy_.storage_used_units(destination_def, destination.inventory))
            : std::numeric_limits<double>::infinity();
        double total_revenue = 0.0;
        double total_spoiled = 0.0;
        std::vector<domain::CargoLot> sold;
        for (const auto& lot : ship.active_mission.cargo) {
            // Some goods (food, medicine) spoil in transit.
            const double decay_per_day = cargo_decay_per_day(ship_design(ship), lot.commodity_id);
            const double surviving = decay_per_day > 0.0 ? std::max(0.0, std::pow(1.0 - decay_per_day, transit_days)) : 1.0;
            double arrived = lot.units * surviving;
            total_spoiled += lot.units - arrived;
            if (arrived > free_capacity) {
                add_event(std::format("{} jettisoned {:.1f}u {} at {} — storage full",
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
                : trade_value(destination, lot.commodity_id, arrived);
            // An emergency premium comes from the station's faction, the rest from the station.
            const double faction_share = lot.emergency_premium * delivered_share;
            if (faction_share > 0.0) {
                faction_treasuries_[destination_def.faction_id] -= faction_share;
                emergency_paid_ += faction_share;
                for (auto& emergency : emergencies_) {
                    if (emergency.station_id == destination.station_id && emergency.commodity_id == lot.commodity_id) {
                        emergency.faction_paid += faction_share;
                    }
                }
            }
            // The station's import cost: what it paid per unit, averaged over about 30 days of its use.
            if (arrived > 0.0 && !economy_.is_export_market(destination_def, lot.commodity_id)) {
                const auto& rates = economy_.get_station_net_rates(destination_def);
                const auto rate = rates.find(lot.commodity_id);
                const double history = rate == rates.end() ? 0.0 : std::max(0.0, -rate->second) * IMPORT_COST_DAYS;
                const auto known = destination.import_unit_cost.find(lot.commodity_id);
                const double previous = known == destination.import_unit_cost.end()
                    ? economy_.reference_price(destination_def, lot.commodity_id) : known->second;
                destination.import_unit_cost[lot.commodity_id] =
                    (previous * history + (revenue - faction_share)) / (history + arrived);
            }
            destination.inventory[lot.commodity_id] += arrived;
            destination.import_units_per_day[lot.commodity_id] += arrived / TRADE_FLOW_DAYS;
            // Debug aid: SPACETRAINS_TRACE_DELIVERIES=1 logs every delivery (stderr).
            if (static const bool trace_deliveries = std::getenv("SPACETRAINS_TRACE_DELIVERIES") != nullptr;
                trace_deliveries && arrived > 0.0) {
                std::cerr << std::format("[DELIVERY] day {:.0f} {} {} {:.0f}u at {:.1f}/u by {} ({}) lead {:.0f}d\n",
                    game_time_s_ / 86400.0, destination.station_id, lot.commodity_id, arrived, revenue / arrived, ship.name,
                    ship.class_id, (game_time_s_ - ship.active_mission.departure_time_s + ship.active_mission.wait_time_s) / 86400.0);
            }
            if (arrived > 0.0) {
                observe_delivery(destination, lot.commodity_id,
                    ship.active_mission.departure_time_s - ship.active_mission.wait_time_s);
                // Audit only: what stations pay against the landed reference price.
                const auto& refs = universe_.pricing.reference_prices;
                if (const auto at = refs.find(destination.station_id); at != refs.end()) {
                    if (const auto ref = at->second.find(lot.commodity_id); ref != at->second.end()) {
                        delivered_paid_cr_ += revenue;
                        delivered_reference_cr_ += arrived * ref->second;
                    }
                }
            }
            destination.credits -= revenue - faction_share;
            destination.ledger.ship_purchases += revenue - faction_share;
            ship.credits += revenue;
            journal_.record(account_station(destination), account_fleet(ship.faction_id), MoneyCategory::Cargo, revenue - faction_share);
            if (faction_share > 0.0) {
                journal_.record(account_faction(destination_def.faction_id), account_fleet(ship.faction_id),
                    MoneyCategory::EmergencyPremium, faction_share);
            }
            ship.lifetime_profit += revenue;
            ship.ledger.cargo_revenue += revenue;
            total_revenue += revenue;
            if (arrived > 0.0) {
                record_trade({
                    .time_s = game_time_s_,
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
        add_event(total_spoiled > 0.1
            ? std::format("{} arrived at {} with {} for {:.0f} cr ({:.1f}u spoiled in {:.0f}d transit)",
                  ship.name, destination_def.name, describe_cargo(sold), total_revenue, total_spoiled, transit_days)
            : std::format("{} arrived at {} and sold {} for {:.0f} cr", ship.name, destination_def.name,
                  describe_cargo(sold), total_revenue),
            "arrival");
    } else {
        add_event(std::format("{} arrived at {}", ship.name, get_station_definition(ship.current_station_id).name), "arrival");
    }
    land_passengers(ship, destination);
    ship.active_mission = {};
}

}  // namespace spacetrains::simulation
