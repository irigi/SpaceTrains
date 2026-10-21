// Passengers (docs/plans/passengers.md): people waiting at stations, the fares ships charge
// them, boarding and landing, and the reference fares the market prices against.

#include "domain/ShipDesign.hpp"
#include "simulation/Simulation.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <numeric>
#include <tuple>

namespace spacetrains::simulation {

namespace {
// As in Simulation.cpp.
constexpr double FUEL_UNITS_TO_KG = 100.0;
constexpr double PROPELLANT_RESERVE_FRACTION = 0.1;
// A waiting group smaller than this is gone (the rest of its people gave up).
constexpr double MIN_GROUP_COUNT = 1.0e-6;

double consumed_propellant_kg(const domain::TrajectoryPlan& plan) {
    return plan.propellant_required_kg + plan.propellant_boiled_kg;
}

bool pays_from_employer(domain::PassengerKind kind) {
    return kind == domain::PassengerKind::RotationOut || kind == domain::PassengerKind::RotationHome;
}
}  // namespace

const char* Simulation::passenger_kind_name(domain::PassengerKind kind) {
    switch (kind) {
        case domain::PassengerKind::Migrant: return "settlers";
        case domain::PassengerKind::Emigrant: return "emigrants";
        case domain::PassengerKind::RotationOut: return "rotation out";
        case domain::PassengerKind::RotationHome: return "rotation home";
        case domain::PassengerKind::Visitor: return "visitors";
        case domain::PassengerKind::VisitorHome: return "visitors home";
    }
    return "passengers";
}

bool Simulation::is_earth_gateway(const std::string& station_id) const {
    const auto& gateways = universe_.passengers.earth_gateways;
    return std::find(gateways.begin(), gateways.end(), station_id) != gateways.end();
}

double Simulation::passenger_kg(double days) const {
    const auto& pax = universe_.passengers;
    return pax.body_luggage_kg + pax.consumables_kg_per_day * std::max(0.0, days);
}

double Simulation::value_of_time_cr_per_day(domain::PassengerKind kind) const {
    const auto& pax = universe_.passengers;
    if (pays_from_employer(kind)) {
        return universe_.labour.wage_cr_per_day(pax.rotation_grade);
    }
    return universe_.labour.reference_wage_cr_per_day * pax.resident_wage_multiple;
}

double Simulation::passenger_nonfare_cr(double value_of_time, double days, double dose_msv_per_day) const {
    const auto& env = universe_.ship_environment;
    const double dose_cost = env.radiation ? dose_msv_per_day * days * env.hazard_cr_per_msv : 0.0;
    return value_of_time * days * (1.0 + universe_.passengers.recovery_days_per_flight_day) + dose_cost;
}

const domain::PassengerDefinition::ReferenceFare* Simulation::reference_fare(
    const std::string& origin_id, const std::string& destination_id) const {
    const auto& fares = universe_.passengers.reference_fares;
    const auto from = fares.find(origin_id);
    if (from == fares.end()) {
        return nullptr;
    }
    const auto to = from->second.find(destination_id);
    return to == from->second.end() ? nullptr : &to->second;
}

double Simulation::reference_cost_cr(const std::string& origin_id, const std::string& destination_id,
    double value_of_time) const {
    const auto* reference = reference_fare(origin_id, destination_id);
    if (reference == nullptr) {
        return std::numeric_limits<double>::infinity();
    }
    const auto& env = universe_.ship_environment;
    return reference->fare_cr + passenger_nonfare_cr(value_of_time, reference->days, env.radiation ? env.gcr_msv_per_day : 0.0);
}

void Simulation::post_travellers(domain::StationState& origin, domain::PassengerKind kind,
    const std::string& destination_id, const std::string& work_station_id, double count, double dose_msv) {
    if (count <= 0.0) {
        return;
    }
    const auto& pax = universe_.passengers;
    const double value_of_time = value_of_time_cr_per_day(kind);
    const double reference = reference_cost_cr(origin.station_id, destination_id, value_of_time);
    if (!std::isfinite(reference)) {
        return;  // no carrier can fly the trip
    }
    // Willingness to pay falls off as cost^elasticity: `count` people would travel at the
    // reference cost; band k holds those whose highest acceptable cost lies between its
    // maximum m_k = reference x ratio^(k-1) and the next band's.
    const int bands = std::max(1, static_cast<int>(pax.demand_bands));
    const auto share_above = [&](double multiple) { return std::pow(multiple, pax.fare_elasticity); };
    for (int band = 0; band < bands; ++band) {
        const double multiple = std::pow(pax.band_ratio, band - 1);
        const double share = share_above(multiple) - (band + 1 < bands ? share_above(multiple * pax.band_ratio) : 0.0);
        const double people = count * share;
        if (people <= 0.0) {
            continue;
        }
        passenger_audit_.posted[static_cast<std::size_t>(kind)] += people;
        auto group = std::find_if(origin.waiting.begin(), origin.waiting.end(), [&](const domain::PassengerGroup& g) {
            return g.kind == kind && g.band == band && g.destination_id == destination_id
                && g.work_station_id == work_station_id && g.dose_msv == dose_msv;
        });
        if (group == origin.waiting.end()) {
            origin.waiting.push_back({
                .kind = kind,
                .destination_id = destination_id,
                .work_station_id = work_station_id,
                .count = people,
                .since_s = game_time_s_,
                .max_cost_cr = reference * multiple,
                .value_of_time_cr_per_day = value_of_time,
                .dose_msv = dose_msv,
                .band = band,
            });
        } else {
            // The group's waiting time is the mean of its people's.
            group->since_s = (group->since_s * group->count + game_time_s_ * people) / (group->count + people);
            group->count += people;
            group->max_cost_cr = reference * multiple;
        }
    }
}

const std::string& Simulation::gateway_for(const std::string& station_id, bool to_station) const {
    const auto& gateways = universe_.passengers.earth_gateways;
    const std::string* best = &gateways.front();
    double best_fare = std::numeric_limits<double>::infinity();
    for (const auto& gateway : gateways) {
        const auto* fare = to_station ? reference_fare(gateway, station_id) : reference_fare(station_id, gateway);
        if (fare != nullptr && fare->fare_cr < best_fare) {
            best_fare = fare->fare_cr;
            best = &gateway;
        }
    }
    return *best;
}

double Simulation::travellers_pending(const std::string& station_id, domain::PassengerKind kind) const {
    double pending = 0.0;
    for (const auto& station : stations_) {
        for (const auto& group : station.waiting) {
            if (group.kind == kind && (kind == domain::PassengerKind::Emigrant
                    ? station.station_id == station_id : group.destination_id == station_id)) {
                pending += group.count;
            }
        }
    }
    for (const auto& ship : ships_) {
        for (const auto& lot : ship.passengers) {
            if (lot.kind == kind && (kind == domain::PassengerKind::Emigrant
                    ? lot.origin_id == station_id : lot.destination_id == station_id)) {
                pending += lot.count;
            }
        }
    }
    return pending;
}

double Simulation::passenger_flow_per_day(const std::string& origin_id, const std::string& destination_id) const {
    double flow = 0.0;
    if (migration_by_ship()) {
        const auto& growth = universe_.growth;
        const double floor_credits = universe_.open_economy.station_credit_floor;
        if (is_earth_gateway(origin_id) && !is_earth_gateway(destination_id) && gateway_for(destination_id, true) == origin_id) {
            const auto& station = get_station_state(destination_id);
            const double ceiling = growth.max_population_factor
                * static_cast<double>(get_station_definition(destination_id).population);
            if (station.supply_index >= growth.grow_above && station.credits >= floor_credits && station.population < ceiling) {
                flow += growth.growth_per_year * station.population / 365.25;
            }
        }
        if (!is_earth_gateway(origin_id) && is_earth_gateway(destination_id) && gateway_for(origin_id, false) == destination_id) {
            const auto& station = get_station_state(origin_id);
            if (station.supply_index < growth.decline_below) {
                flow += growth.decline_per_year * station.population / 365.25;
            }
        }
    }
    // Those already waiting, spread over a new ship's route commitment.
    for (const auto& group : get_station_state(origin_id).waiting) {
        if (group.destination_id == destination_id) {
            flow += group.count / std::max(1.0, universe_.fleet_investment.route_commitment_days);
        }
    }
    return flow;
}

double Simulation::expected_fare_cr(const std::string& origin_id, const std::string& destination_id, double days,
    double demand_per_trip, double berths) const {
    const auto* reference = reference_fare(origin_id, destination_id);
    if (reference == nullptr || berths < 1.0) {
        return 0.0;
    }
    const auto& pax = universe_.passengers;
    const auto& env = universe_.ship_environment;
    const double dose_rate = env.radiation ? env.gcr_msv_per_day : 0.0;
    const double vot = value_of_time_cr_per_day(domain::PassengerKind::Migrant);
    const double nonfare = passenger_nonfare_cr(vot, days, dose_rate);
    const double hedonic = reference_cost_cr(origin_id, destination_id, vot) - nonfare;
    // With willingness to pay falling off as cost^elasticity (-1), berths filled from a demand
    // `demand_per_trip` times larger clear at that multiple of the reference cost, up to the
    // highest band.
    const double top = std::pow(pax.band_ratio, std::max(0.0, pax.demand_bands - 2.0));
    const double multiple = std::clamp(std::pow(demand_per_trip / berths, -1.0 / pax.fare_elasticity), 1.0, top);
    return std::max(hedonic, multiple * reference_cost_cr(origin_id, destination_id, vot) - nonfare);
}

void Simulation::step_passengers(double dt_s) {
    // Waiting costs time: a person whose budget the wait would use up leaves the queue. In
    // expectation the group thins at value of time / highest cost per day.
    const double dt_days = dt_s / 86400.0;
    for (auto& station : stations_) {
        for (auto& group : station.waiting) {
            const double rate = group.max_cost_cr > 0.0 ? group.value_of_time_cr_per_day / group.max_cost_cr : 1.0;
            const double lost = group.count * (1.0 - std::exp(-rate * dt_days));
            group.count -= lost;
            passenger_audit_.gave_up[static_cast<std::size_t>(group.kind)] += lost;
        }
        std::erase_if(station.waiting, [&](const domain::PassengerGroup& group) {
            if (group.count >= MIN_GROUP_COUNT) {
                return false;
            }
            passenger_audit_.gave_up[static_cast<std::size_t>(group.kind)] += group.count;
            return true;
        });
    }
}

Simulation::PassengerOffer Simulation::passenger_offer(const domain::ShipState& ship,
    const domain::ShipClassDefinition& design, const domain::StationState& origin,
    const domain::StationDefinition& destination, double days, double propellant_cr_per_kg, double berths) const {
    (void)ship;
    PassengerOffer offer;
    if (berths < 1.0 || origin.waiting.empty()) {
        return offer;
    }
    const auto* reference = reference_fare(origin.station_id, destination.id);
    if (reference == nullptr) {
        return offer;
    }
    const auto& env = universe_.ship_environment;
    const auto& origin_def = get_station_definition(origin.station_id);
    const double dose_rate = env.radiation ? leg_dose_msv_per_day(design, origin_def, destination, game_time_s_) : 0.0;
    const double reference_dose_rate = env.radiation ? env.gcr_msv_per_day : 0.0;
    double provisions_per_day = 0.0;
    for (const auto& [commodity_id, units_per_day] : universe_.ship_operations.life_support_units_per_crew_day) {
        provisions_per_day += units_per_day * station_price(origin, commodity_id);
    }
    const double marginal = provisions_per_day * days + passenger_kg(days) * propellant_cr_per_kg;
    // Each group pays what this trip is worth to it against the reference trip (hedonic), or,
    // where more people wait than there are berths, the fare that clears the berths: what the
    // first person left behind would have paid (a uniform-price auction). Never more than its
    // own budget allows, never less than what carrying it costs.
    struct Candidate {
        std::size_t group;
        double whole;
        double willing;   // highest fare the group pays for this trip
        double hedonic;
    };
    std::vector<Candidate> candidates;
    for (std::size_t g = 0; g < origin.waiting.size(); ++g) {
        const auto& group = origin.waiting[g];
        const double whole = std::floor(group.count + 1e-9);
        if (group.destination_id != destination.id || whole < 1.0) {
            continue;
        }
        if (env.radiation && group.dose_msv + dose_rate * days > env.career_limit_msv) {
            continue;
        }
        const double vot = group.value_of_time_cr_per_day;
        const double nonfare = passenger_nonfare_cr(vot, days, dose_rate);
        const double willing = group.max_cost_cr - nonfare;
        if (willing <= marginal) {
            continue;
        }
        candidates.push_back({g, whole, willing,
            reference->fare_cr + passenger_nonfare_cr(vot, reference->days, reference_dose_rate) - nonfare});
    }
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) { return a.willing > b.willing; });
    double left = std::floor(berths + 1e-9);
    double clearing = 0.0;
    std::size_t boarding = 0;
    for (const auto& candidate : candidates) {
        if (left < 1.0) {
            clearing = candidate.willing;
            break;
        }
        const double take = std::min(candidate.whole, left);
        left -= take;
        ++boarding;
        if (take < candidate.whole) {
            clearing = candidate.willing;  // the rest of this group stays behind
            break;
        }
    }
    left = std::floor(berths + 1e-9);
    for (std::size_t c = 0; c < boarding; ++c) {
        const auto& candidate = candidates[c];
        const double fare = std::min(candidate.willing, std::max({candidate.hedonic, clearing, marginal}));
        if (fare <= marginal) {
            continue;
        }
        const double count = std::min(candidate.whole, left);
        left -= count;
        offer.picks.push_back({.group = candidate.group, .count = count, .fare_cr = fare});
        offer.people += count;
        offer.revenue_cr += count * fare;
        offer.marginal_cr += count * provisions_per_day * days;
    }
    offer.mass_kg = offer.people * passenger_kg(days);
    return offer;
}

void Simulation::board_passengers(domain::ShipState& ship, domain::StationState& origin,
    const std::string& destination_id, const std::vector<MissionChoice::PassengerPick>& picks, double days) {
    for (const auto& pick : picks) {
        if (pick.group >= origin.waiting.size()) {
            continue;
        }
        auto& group = origin.waiting[pick.group];
        const double count = std::min(pick.count, std::floor(group.count + 1e-9));
        if (group.destination_id != destination_id || count < 1.0) {
            continue;
        }
        group.count -= count;
        const auto kind = static_cast<std::size_t>(group.kind);
        const double fares = count * pick.fare_cr;
        // Rotation is paid by the employer, everyone else pays out of their own (outside) income.
        if (pays_from_employer(group.kind)) {
            get_station_state(group.work_station_id).credits -= fares;
        } else {
            outside_economy_credits_ -= fares;
        }
        ship.credits += fares;
        ship.lifetime_profit += fares;
        ship.ledger.fares += fares;
        if (group.kind == domain::PassengerKind::Emigrant) {
            origin.population = std::max(0.0, origin.population - count);
            economy_.set_population(origin.station_id, origin.population);
        }
        passenger_audit_.boarded[kind] += count;
        passenger_audit_.fares_cr[kind] += fares;
        passenger_audit_.wait_days[kind] += count * std::max(0.0, game_time_s_ - group.since_s) / 86400.0;
        ship.passengers.push_back({
            .kind = group.kind,
            .origin_id = origin.station_id,
            .destination_id = destination_id,
            .work_station_id = group.work_station_id,
            .count = count,
            .fare_cr = pick.fare_cr,
            .value_of_time_cr_per_day = group.value_of_time_cr_per_day,
            .dose_msv = group.dose_msv,
            .boarded_s = game_time_s_,
        });
    }
    std::erase_if(origin.waiting, [](const domain::PassengerGroup& group) { return group.count < MIN_GROUP_COUNT; });
    if (!ship.passengers.empty()) {
        ++passenger_audit_.departures_with_passengers;
        passenger_audit_.berth_days += ship_design(ship).berths * days;
        double people = 0.0;
        for (const auto& lot : ship.passengers) {
            people += lot.count;
        }
        add_event(std::format("{} boards {:.0f} passengers at {} for {}", ship.name, people,
            get_station_definition(origin.station_id).name, get_station_definition(destination_id).name), "mission");
    }
}

void Simulation::land_passengers(domain::ShipState& ship, domain::StationState& destination) {
    if (ship.passengers.empty()) {
        return;
    }
    const auto& definition = get_station_definition(destination.station_id);
    for (const auto& lot : ship.passengers) {
        const auto kind = static_cast<std::size_t>(lot.kind);
        passenger_audit_.landed[kind] += lot.count;
        passenger_audit_.flight_days[kind] += lot.count * std::max(0.0, game_time_s_ - lot.boarded_s) / 86400.0;
        if (lot.kind == domain::PassengerKind::Migrant) {
            destination.population += lot.count;
            economy_.set_population(destination.station_id, destination.population);
        }
    }
    double people = 0.0;
    for (const auto& lot : ship.passengers) {
        people += lot.count;
    }
    add_event(std::format("{} lands {:.0f} passengers at {}", ship.name, people, definition.name), "arrival");
    ship.passengers.clear();
}

std::vector<Simulation::ReferenceFareRow> Simulation::compute_reference_fares() const {
    const auto& pricing = universe_.pricing;
    const auto& market = universe_.market;
    const auto departures = static_cast<int>(pricing.reference_departures);
    // Cabin carriers: every hull with its cabin module, and the liners.
    std::vector<domain::ShipClassDefinition> carriers;
    for (const auto& ship_class : universe_.ship_classes) {
        if (ship_class.id != ship_class.hull_id) {
            continue;
        }
        if (ship_class.berths > 0.0) {
            carriers.push_back(ship_class);
            continue;
        }
        for (const auto& module : universe_.ship_modules) {
            const auto berths = module.effects.find("berths");
            if (module.hull_id != ship_class.id || module.slot != "cabin" || berths == module.effects.end()) {
                continue;
            }
            auto design = domain::derive_design(ship_class, {module.part}, domain::design_id(ship_class.id, {module.id}));
            design.berths += berths->second;
            design.base_class_id = ship_class.id;
            design.fit = {module.id};
            design.effects = module.effects;
            carriers.push_back(std::move(design));
        }
    }
    const double fuel_cr_per_kg = get_commodity(economy::FUEL_ID).base_price / FUEL_UNITS_TO_KG;
    double provisions_per_day = 0.0;
    for (const auto& [commodity_id, units_per_day] : universe_.ship_operations.life_support_units_per_crew_day) {
        provisions_per_day += units_per_day * get_commodity(commodity_id).base_price;
    }
    const auto daily_cost = [&](const domain::ShipClassDefinition& design) {
        return daily_capital_cost(design) + design.crew_size * universe_.ship_operations.wage_cr_per_crew_day
            + design.crew_size * provisions_per_day;
    };
    // A resident's time and dose per day aboard: the reference trip is the one with the lowest
    // cost to the passenger (fare plus time), so its planner trades propellant against the
    // ship's time and the passengers' too.
    const auto& env = universe_.ship_environment;
    const double person_day_cr = passenger_nonfare_cr(value_of_time_cr_per_day(domain::PassengerKind::Migrant), 1.0,
        env.radiation ? env.gcr_msv_per_day : 0.0);
    struct Leg {
        const domain::StationDefinition* from {nullptr};
        const domain::StationDefinition* to {nullptr};
        const domain::ShipClassDefinition* carrier {nullptr};
        double departure_s {0.0};
        double payload_kg {0.0};
    };
    std::vector<Leg> legs;
    const auto departure_s = [&](int d) { return d * 365.25 * 86400.0 / departures; };
    for (const auto& from : universe_.stations) {
        for (const auto& to : universe_.stations) {
            if (from.id == to.id) {
                continue;
            }
            for (const auto& carrier : carriers) {
                for (int d = 0; d < departures; ++d) {
                    legs.push_back({&from, &to, &carrier, departure_s(d),
                        carrier.berths * passenger_kg(economy_.transfer_days(from, to))});
                }
            }
        }
    }
    const auto plan_leg = [&](const Leg& leg) {
        domain::ShipState probe;
        probe.class_id = leg.carrier->base_class_id.empty() ? leg.carrier->id : leg.carrier->base_class_id;
        probe.fit = leg.carrier->fit;
        probe.current_station_id = leg.from->id;
        const trajectory::PlanningOptions options {
            .propellant_cr_per_kg = fuel_cr_per_kg,
            .time_cr_per_day = daily_cost(*leg.carrier)
                + (leg.payload_kg > 0.0 ? leg.carrier->berths * market.fill_ref * person_day_cr : 0.0),
            .payload_kg = leg.payload_kg,
            .purchasable_propellant_kg = leg.carrier->propellant_capacity_kg,
            .reserve_fraction = PROPELLANT_RESERVE_FRACTION,
            .boiloff_kg_per_day = leg_boiloff_kg_per_day(*leg.carrier, *leg.from, *leg.to, leg.departure_s),
            .include_path = false,
        };
        const auto& planner = (leg.carrier->propulsion_type == "variable_isp" && variable_isp_planner_)
            ? static_cast<const trajectory::ITrajectoryPlanner&>(*variable_isp_planner_)
            : static_cast<const trajectory::ITrajectoryPlanner&>(*kepler_planner_);
        return planner.plan_transfer(*leg.from, *leg.to, probe, *leg.carrier, leg.departure_s, options);
    };
    std::vector<domain::TrajectoryPlan> outbound(legs.size());
    thread_pool_->parallel_for(legs.size(), [&](std::size_t i) { outbound[i] = plan_leg(legs[i]); });
    std::vector<Leg> returns(legs.size());
    for (std::size_t i = 0; i < legs.size(); ++i) {
        returns[i] = {legs[i].to, legs[i].from, legs[i].carrier,
            outbound[i].feasible ? outbound[i].arrival_time_s : legs[i].departure_s, 0.0};
    }
    std::vector<domain::TrajectoryPlan> inbound(returns.size());
    thread_pool_->parallel_for(returns.size(), [&](std::size_t i) {
        if (outbound[i].feasible) {
            inbound[i] = plan_leg(returns[i]);
        }
    });

    std::vector<ReferenceFareRow> rows;
    std::size_t i = 0;
    for (const auto& from : universe_.stations) {
        for (const auto& to : universe_.stations) {
            if (from.id == to.id) {
                continue;
            }
            ReferenceFareRow best {.origin_id = from.id, .destination_id = to.id,
                .fare_cr = std::numeric_limits<double>::infinity()};
            double best_cost = std::numeric_limits<double>::infinity();
            for (const auto& carrier : carriers) {
                std::vector<std::tuple<double, double, double>> trips;  // (cost to the passenger, fare, days out)
                for (int d = 0; d < departures; ++d, ++i) {
                    const auto& out = outbound[i];
                    const auto& back = inbound[i];
                    const double out_days = out.travel_time_s / 86400.0;
                    if (!out.feasible || !back.feasible
                        || !leg_dose_allowed(carrier, from, to, departure_s(d), out_days)) {
                        continue;
                    }
                    const double back_days = back.travel_time_s / 86400.0;
                    const double cost = daily_cost(carrier) * (out_days + market.return_share * back_days)
                        + (consumed_propellant_kg(out) + market.return_share * consumed_propellant_kg(back)) * fuel_cr_per_kg;
                    const double fare = pricing.carrier_margin * cost / (carrier.berths * market.fill_ref)
                        + provisions_per_day * out_days;
                    trips.emplace_back(fare + person_day_cr * out_days, fare, out_days);
                }
                if (trips.size() * 2 <= static_cast<std::size_t>(departures)) {
                    continue;
                }
                std::sort(trips.begin(), trips.end());
                const auto& median = trips[trips.size() / 2];
                if (std::get<0>(median) < best_cost) {
                    best_cost = std::get<0>(median);
                    best.fare_cr = std::get<1>(median);
                    best.days = std::get<2>(median);
                    best.class_id = carrier.id;
                }
            }
            if (std::isfinite(best.fare_cr)) {
                rows.push_back(std::move(best));
            }
        }
    }
    return rows;
}

}  // namespace spacetrains::simulation
