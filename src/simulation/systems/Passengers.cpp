// Passengers (docs/modules/ships_and_fleet.md): people waiting at stations, the fares ships charge
// them, boarding and landing, and the reference fares the market prices against.

#include "simulation/systems/Passengers.hpp"

#include "domain/ShipDesign.hpp"
#include "simulation/SimulationInternal.hpp"
#include "simulation/query/Accounts.hpp"
#include "simulation/query/Events.hpp"
#include "simulation/query/Finance.hpp"
#include "simulation/query/Labour.hpp"
#include "simulation/query/Lookups.hpp"
#include "simulation/query/Passengers.hpp"
#include "simulation/query/Positions.hpp"
#include "simulation/query/ShipRates.hpp"
#include "simulation/systems/Households.hpp"
#include "simulation/systems/Market.hpp"
#include "simulation/systems/ShipOperations.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <numeric>
#include <tuple>

namespace spacetrains::simulation::systems {

using namespace detail;

namespace {

// Defined below, file-local.
// The reference trip's cost to a settler in years of the resident wage.
[[nodiscard]] double payback_years(const World& w, const std::string& origin_id, const std::string& destination_id);
// The generalized cost of the reference trip for someone with this value of time.
[[nodiscard]] double reference_cost_cr(const World& w, const std::string& origin_id,
    const std::string& destination_id, double value_of_time);
// Adds `count` people who want to travel at reference cost, spread over the demand bands
// (fare elasticity): more come if trips are cheaper, fewer if dearer. `fixed`: exactly
// `count` people travel (a relief, a worker going home), their budgets spread the same way.
[[nodiscard]] std::vector<PassengerBand> passenger_bands(const World& w, domain::PassengerKind kind,
    const std::string& origin_id, const std::string& destination_id, double count, bool fixed,
    const std::string& employer = {});
// Discretionary trips (step 5) per day origin -> destination at reference cost: a gravity
// model, visit_trips_per_person_year x the origin's people x the destination's share of the
// others, deterred by the trip's cost against a year's income (elasticity). Earth weighs in
// as earth_population_weight, shared by its gateways. 0 without a fare both ways.
[[nodiscard]] double visitor_trips_per_day(const World& w, const std::string& origin_id,
    const std::string& destination_id);
[[nodiscard]] std::vector<PassengerBand> expected_travellers(const World& w, const std::string& origin_id,
    const std::string& destination_id, double window_days);

}  // namespace

namespace {
// A waiting group smaller than this is gone (the rest of its people gave up).
constexpr double MIN_GROUP_COUNT = 1.0e-6;

}  // namespace

const char* passenger_kind_name(domain::PassengerKind kind) {
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

double settler_share(const World& w, double payback_years) {
    const auto& pax = w.data.universe_.passengers;
    if (payback_years <= 0.0 || pax.settler_payback_half_years <= 0.0) {
        return 1.0;
    }
    return 1.0 / (1.0 + std::pow(payback_years / pax.settler_payback_half_years, pax.settler_payback_exponent));
}

namespace {

double payback_years(const World& w, const std::string& origin_id, const std::string& destination_id) {
    const double vot = query::value_of_time_cr_per_day(w, domain::PassengerKind::Migrant);
    return vot > 0.0 ? reference_cost_cr(w, origin_id, destination_id, vot) / (365.0 * vot) : 0.0;
}

}  // namespace

double port_fuel_price(const World& w, const std::string& station_id) {
    const double base = query::get_commodity(w, economy::FUEL_ID).base_price;
    return std::max(base, systems::station_price(w, query::get_station_state(w, station_id), economy::FUEL_ID));
}

// The reference fare at today's fuel prices: its propellant per seat is bought at the origin
// (and the return share at the destination) at those ports' prices instead of the base price.
// Earth makes no fuel, so trips from Earth cost several times the base-price fare's fuel.
double live_fare_cr(const World& w, const std::string& origin_id, const std::string& destination_id,
    const domain::PassengerDefinition::ReferenceFare& reference) {
    const double base = query::get_commodity(w, economy::FUEL_ID).base_price;
    return reference.fare_cr
        + (reference.fuel_out_kg * (systems::port_fuel_price(w, origin_id) - base)
              + reference.fuel_back_kg * (systems::port_fuel_price(w, destination_id) - base))
        / FUEL_UNITS_TO_KG;
}

namespace {

double reference_cost_cr(const World& w, const std::string& origin_id, const std::string& destination_id,
    double value_of_time) {
    const auto* reference = query::reference_fare(w, origin_id, destination_id);
    if (reference == nullptr) {
        return std::numeric_limits<double>::infinity();
    }
    const auto& env = w.data.universe_.ship_environment;
    return systems::live_fare_cr(w, origin_id, destination_id, *reference)
        + query::passenger_nonfare_cr(w, value_of_time, reference->days, env.radiation ? env.gcr_msv_per_day : 0.0);
}

std::vector<PassengerBand> passenger_bands(const World& w, domain::PassengerKind kind,
    const std::string& origin_id, const std::string& destination_id, double count, bool fixed, const std::string& employer) {
    std::vector<PassengerBand> out;
    if (count <= 0.0) {
        return out;
    }
    const auto& pax = w.data.universe_.passengers;
    const double value_of_time = query::value_of_time_cr_per_day(w, kind);
    const double reference = reference_cost_cr(w, origin_id, destination_id, value_of_time);
    if (!std::isfinite(reference)) {
        return out;  // no carrier can fly the trip
    }
    // Willingness to pay falls off as cost^elasticity: `count` people would travel at the
    // reference cost; band k holds those whose highest acceptable cost lies between its
    // maximum m_k = reference x ratio^(k-1) and the next band's.
    // Sponsored settlers: `count` is every settler the station would take; the share whose job
    // repays a passage costing m x the reference follows the payback curve.
    const int bands = std::max(1, static_cast<int>(pax.demand_bands));
    const bool sponsored = kind == domain::PassengerKind::Migrant && query::employer_pays(w, kind);
    const double payback = sponsored ? reference / (365.0 * value_of_time) : 0.0;
    // Labour market (phase B): what each traveller would pay comes from a distribution, not a
    // multiple of the reference cost. Settlers: their gain from the job; the unemployed going
    // home: an Earth wage; visitors: a share of a year's income where they live.
    using K = domain::PassengerKind;
    const bool from_income = query::labour_on(w) && kind != K::RotationOut && kind != K::RotationHome;
    const std::string home = kind == K::VisitorHome ? destination_id : origin_id;
    const double reference_tail = from_income && kind == K::Visitor ? traveller_budget_tail(w, home, reference) : 1.0;
    // A colony's recruits: the colony pays up to the reference cost of their passage.
    const bool recruited = kind == K::Migrant && query::colony_employer(employer);
    const auto share_above = [&](double multiple) {
        if (recruited) {
            return multiple <= 1.0 + 1e-9 ? 1.0 : 0.0;
        }
        if (from_income) {
            const double cost = multiple * reference;
            switch (kind) {
                case K::Migrant: return settler_gain_tail(w, query::get_station_state(w, destination_id), cost);
                case K::Emigrant: return returner_gain_tail(w, cost);
                case K::VisitorHome: return traveller_budget_tail(w, home, cost);
                default: return reference_tail > 0.0 ? traveller_budget_tail(w, home, cost) / reference_tail : 0.0;
            }
        }
        return sponsored ? settler_share(w, multiple * payback) : std::pow(multiple, pax.fare_elasticity);
    };
    if (recruited || (from_income && (kind == K::Migrant || kind == K::Emigrant || kind == K::VisitorHome))) {
        fixed = true;   // a given number of people, spread over their budgets
        if (share_above(std::pow(pax.band_ratio, -1.0)) <= 0.0) {
            return out;   // nobody among them can pay even the cheapest band
        }
    }
    // A fixed number of travellers is spread over the bands in the same proportions.
    const double scale = fixed ? 1.0 / share_above(std::pow(pax.band_ratio, -1.0)) : 1.0;
    for (int band = 0; band < bands; ++band) {
        const double multiple = std::pow(pax.band_ratio, band - 1);
        const double share = share_above(multiple) - (band + 1 < bands ? share_above(multiple * pax.band_ratio) : 0.0);
        const double people = count * share * scale;
        if (people > 0.0) {
            out.push_back({.band = band, .people = people, .max_cost_cr = reference * multiple,
                .value_of_time_cr_per_day = value_of_time});
        }
    }
    return out;
}

}  // namespace

void post_travellers(World& w, domain::StationState& origin, domain::PassengerKind kind,
    const std::string& destination_id, const std::string& work_station_id, double count, bool fixed) {
    const double dose_msv = 0.0;
    for (const auto& [band, people, max_cost, value_of_time, queued, home_pays] :
        passenger_bands(w, kind, origin.station_id, destination_id, count, fixed, work_station_id)) {
        w.audits.passenger_audit_.posted[static_cast<std::size_t>(kind)] += people;
        w.audits.passenger_audit_.routes[{origin.station_id, destination_id}].posted += people;
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
                .since_s = w.state.game_time_s_,
                .max_cost_cr = max_cost,
                .value_of_time_cr_per_day = value_of_time,
                .dose_msv = dose_msv,
                .band = band,
            });
        } else {
            // The group's waiting time is the mean of its people's.
            group->since_s = (group->since_s * group->count + w.state.game_time_s_ * people) / (group->count + people);
            group->count += people;
            group->max_cost_cr = max_cost;
        }
    }
}

const std::string& gateway_for(const World& w, const std::string& station_id, bool to_station) {
    const auto& gateways = w.data.universe_.passengers.earth_gateways;
    const std::string* best = &gateways.front();
    double best_fare = std::numeric_limits<double>::infinity();
    for (const auto& gateway : gateways) {
        const auto* fare = to_station ? query::reference_fare(w, gateway, station_id) : query::reference_fare(w, station_id, gateway);
        if (fare != nullptr && fare->fare_cr < best_fare) {
            best_fare = fare->fare_cr;
            best = &gateway;
        }
    }
    return *best;
}

double travellers_pending(const World& w, const std::string& station_id, domain::PassengerKind kind) {
    double pending = 0.0;
    for (const auto& station : w.state.stations_) {
        for (const auto& group : station.waiting) {
            if (group.kind == kind && !query::colony_employer(group.work_station_id) && (kind == domain::PassengerKind::Emigrant
                    ? station.station_id == station_id : group.destination_id == station_id)) {
                pending += group.count;
            }
        }
    }
    for (const auto& ship : w.state.ships_) {
        for (const auto& lot : ship.passengers) {
            if (lot.kind == kind && !query::colony_employer(lot.work_station_id) && (kind == domain::PassengerKind::Emigrant
                    ? lot.origin_id == station_id : lot.destination_id == station_id)) {
                pending += lot.count;
            }
        }
    }
    return pending;
}

double colony_recruits_per_day(const World& w, const domain::BackgroundEconomyDefinition& economy) {
    // The part of a colony's growth it recruits on Earth, while it can pay for passages.
    if (!query::labour_on(w) || !query::migration_by_ship(w) || economy.immigration_share <= 0.0) {
        return 0.0;
    }
    const auto colony = w.state.background_population_.find(economy.id);
    if (colony == w.state.background_population_.end() || query::economy_balance(w, economy.id) <= 0.0) {
        return 0.0;   // it pays passages out of savings
    }
    return colony->second * economy.growth_per_year * economy.immigration_share * std::max(0.0, colony_growth_factor(w, economy.id))
        / 365.25;
}

void compute_rotation_tours(World& w) {
    w.data.rotation_tours_.clear();
    if (!query::rotation_on(w)) {
        return;
    }
    const auto& pax = w.data.universe_.passengers;
    const auto& env = w.data.universe_.ship_environment;
    const double wage = query::value_of_time_cr_per_day(w, domain::PassengerKind::RotationOut);
    const double dose_rate = env.radiation ? env.gcr_msv_per_day : 0.0;
    for (const auto& station : w.data.universe_.stations) {
        if (query::is_earth_gateway(w, station.id)) {
            continue;
        }
        const auto& out_gateway = gateway_for(w, station.id, true);
        const auto& home_gateway = gateway_for(w, station.id, false);
        const auto* out = query::reference_fare(w, out_gateway, station.id);
        const auto* home = query::reference_fare(w, station.id, home_gateway);
        if (out == nullptr || home == nullptr || wage <= 0.0) {
            continue;  // no trip there or back: no rotation (the staff settle)
        }
        const double fares = systems::live_fare_cr(w, out_gateway, station.id, *out) + systems::live_fare_cr(w, station.id, home_gateway, *home);
        const double round_trip = fares + query::passenger_nonfare_cr(w, wage, out->days, dose_rate)
            + query::passenger_nonfare_cr(w, wage, home->days, dose_rate);
        const double tour_days = std::sqrt(round_trip * 365.0 / (pax.hardship_per_year * wage));
        w.data.rotation_tours_[station.id] = {
            .out_gateway = out_gateway,
            .home_gateway = home_gateway,
            .tour_days = tour_days,
            .fares_per_day = fares / tour_days,
        };
    }
}

const RotationTour* rotation_tour(const World& w, const std::string& station_id) {
    const auto it = w.data.rotation_tours_.find(station_id);
    return it == w.data.rotation_tours_.end() ? nullptr : &it->second;
}

double rotation_positions(const World& w, const domain::StationState& station) {
    return w.data.universe_.passengers.rotation_share * station.population;
}

namespace {

double visitor_trips_per_day(const World& w, const std::string& origin_id, const std::string& destination_id) {
    if (!query::visits_on(w) || origin_id == destination_id || (query::is_earth_gateway(w, origin_id) && query::is_earth_gateway(w, destination_id))
        || query::reference_fare(w, origin_id, destination_id) == nullptr || query::reference_fare(w, destination_id, origin_id) == nullptr) {
        return 0.0;
    }
    const auto& pax = w.data.universe_.passengers;
    const double gateways = static_cast<double>(pax.earth_gateways.size());
    const auto people = [&](const std::string& station_id) {
        return query::is_earth_gateway(w, station_id) ? pax.earth_population_weight / gateways
                                            : query::get_station_state(w, station_id).population;
    };
    double others = 0.0;
    for (const auto& station : w.data.universe_.stations) {
        if (station.id != origin_id && !(query::is_earth_gateway(w, origin_id) && query::is_earth_gateway(w, station.id))) {
            others += people(station.id);
        }
    }
    if (others <= 0.0) {
        return 0.0;
    }
    const double vot = query::value_of_time_cr_per_day(w, domain::PassengerKind::Visitor);
    const double year_of_income = 365.0 * vot;
    const double reference = reference_cost_cr(w, origin_id, destination_id, vot);
    // Labour market: those whose trip budget covers the reference cost (income distributions).
    const double deterrence = query::labour_on(w) ? traveller_budget_tail(w, origin_id, reference)
                                          : std::pow(reference / year_of_income, pax.fare_elasticity);
    return pax.visit_trips_per_person_year / 365.0 * people(origin_id) * people(destination_id) / others * deterrence;
}

}  // namespace

double passenger_flow_per_day(const World& w, const std::string& origin_id, const std::string& destination_id) {
    double flow = 0.0;
    // Visitors out, and visitors from there going home.
    flow += visitor_trips_per_day(w, origin_id, destination_id) + visitor_trips_per_day(w, destination_id, origin_id);
    // Rotation: reliefs out to a station and the relieved home, one per position and tour.
    if (const auto* tour = rotation_tour(w, destination_id); tour != nullptr && tour->out_gateway == origin_id) {
        flow += rotation_positions(w, query::get_station_state(w, destination_id)) / tour->tour_days;
    }
    if (const auto* tour = rotation_tour(w, origin_id); tour != nullptr && tour->home_gateway == destination_id) {
        flow += rotation_positions(w, query::get_station_state(w, origin_id)) / tour->tour_days;
    }
    if (query::migration_by_ship(w) && query::labour_on(w)) {
        // Labour market: vacancies posted to Earth, the unemployed going home.
        if (query::is_earth_gateway(w, origin_id) && !query::is_earth_gateway(w, destination_id) && gateway_for(w, destination_id, true) == origin_id) {
            flow += labour_migrants_per_day(w, query::get_station_state(w, destination_id));
            for (const auto& economy : w.data.universe_.background_economies) {
                if (!economy.gateways.empty() && economy.gateways.front() == destination_id) {
                    flow += colony_recruits_per_day(w, economy);
                }
            }
        }
        if (!query::is_earth_gateway(w, origin_id) && query::is_earth_gateway(w, destination_id) && gateway_for(w, origin_id, false) == destination_id) {
            flow += labour_emigrants_per_day(w, query::get_station_state(w, origin_id));
        }
    } else if (query::migration_by_ship(w)) {
        const auto& growth = w.data.universe_.growth;
        const double floor_credits = w.data.universe_.open_economy.station_credit_floor;
        if (query::is_earth_gateway(w, origin_id) && !query::is_earth_gateway(w, destination_id) && gateway_for(w, destination_id, true) == origin_id) {
            const auto& station = query::get_station_state(w, destination_id);
            const double ceiling = growth.max_population_factor
                * static_cast<double>(query::get_station_definition(w, destination_id).population);
            if (station.supply_index >= growth.grow_above && station.credits >= floor_credits && station.population < ceiling) {
                // Those who would come at the reference cost.
                flow += growth.growth_per_year * station.population / 365.25
                    * (query::employer_pays(w, domain::PassengerKind::Migrant) ? settler_share(w, payback_years(w, origin_id, destination_id)) : 1.0);
            }
        }
        if (!query::is_earth_gateway(w, origin_id) && query::is_earth_gateway(w, destination_id) && gateway_for(w, origin_id, false) == destination_id) {
            const auto& station = query::get_station_state(w, origin_id);
            if (station.supply_index < growth.decline_below) {
                flow += growth.decline_per_year * station.population / 365.25;
            }
        }
    }
    // Those already waiting, spread over a new ship's route commitment.
    for (const auto& group : query::get_station_state(w, origin_id).waiting) {
        if (group.destination_id == destination_id) {
            flow += group.count / std::max(1.0, w.data.universe_.fleet_investment.route_commitment_days);
        }
    }
    return flow;
}

namespace {

std::vector<PassengerBand> expected_travellers(const World& w, const std::string& origin_id,
    const std::string& destination_id, double window_days) {
    // What post_travellers would post over `window_days` (the steady flows), by band.
    std::vector<PassengerBand> out;
    // Who pays: a station's residents (households on) for those who are not Earth's people and
    // whose employer does not pay.
    const auto home_pays = [&](domain::PassengerKind kind) -> std::string {
        if (!query::households_on(w) || query::employer_pays(w, kind)) {
            return {};
        }
        const std::string& home = kind == domain::PassengerKind::VisitorHome ? destination_id : origin_id;
        return query::is_earth_gateway(w, home) ? std::string() : home;
    };
    const auto add = [&](domain::PassengerKind kind, double per_day, bool fixed) {
        if (per_day > 0.0) {
            auto bands = passenger_bands(w, kind, origin_id, destination_id, per_day * window_days, fixed);
            for (auto& band : bands) {
                band.home_pays = home_pays(kind);
            }
            out.insert(out.end(), bands.begin(), bands.end());
        }
    };
    using K = domain::PassengerKind;
    add(K::Visitor, visitor_trips_per_day(w, origin_id, destination_id), false);
    add(K::VisitorHome, visitor_trips_per_day(w, destination_id, origin_id), true);
    if (const auto* tour = rotation_tour(w, destination_id); tour != nullptr && tour->out_gateway == origin_id) {
        add(K::RotationOut, rotation_positions(w, query::get_station_state(w, destination_id)) / tour->tour_days, true);
    }
    if (const auto* tour = rotation_tour(w, origin_id); tour != nullptr && tour->home_gateway == destination_id) {
        add(K::RotationHome, rotation_positions(w, query::get_station_state(w, origin_id)) / tour->tour_days, true);
    }
    if (query::migration_by_ship(w) && query::labour_on(w)) {
        if (query::is_earth_gateway(w, origin_id) && !query::is_earth_gateway(w, destination_id) && gateway_for(w, destination_id, true) == origin_id) {
            add(K::Migrant, labour_migrants_per_day(w, query::get_station_state(w, destination_id)), true);
            for (const auto& economy : w.data.universe_.background_economies) {
                if (!economy.gateways.empty() && economy.gateways.front() == destination_id) {
                    if (const double recruits = colony_recruits_per_day(w, economy); recruits > 0.0) {
                        auto bands = passenger_bands(w, K::Migrant, origin_id, destination_id, recruits * window_days, true,
                            "colony:" + economy.id);
                        out.insert(out.end(), bands.begin(), bands.end());
                    }
                }
            }
        }
        if (!query::is_earth_gateway(w, origin_id) && query::is_earth_gateway(w, destination_id) && gateway_for(w, origin_id, false) == destination_id) {
            add(K::Emigrant, labour_emigrants_per_day(w, query::get_station_state(w, origin_id)), true);
        }
    } else if (query::migration_by_ship(w)) {
        const auto& growth = w.data.universe_.growth;
        if (query::is_earth_gateway(w, origin_id) && !query::is_earth_gateway(w, destination_id) && gateway_for(w, destination_id, true) == origin_id) {
            const auto& station = query::get_station_state(w, destination_id);
            const double ceiling = growth.max_population_factor
                * static_cast<double>(query::get_station_definition(w, destination_id).population);
            if (station.supply_index >= growth.grow_above
                && station.credits >= w.data.universe_.open_economy.station_credit_floor && station.population < ceiling) {
                add(K::Migrant, growth.growth_per_year * station.population / 365.25, false);
            }
        }
        if (!query::is_earth_gateway(w, origin_id) && query::is_earth_gateway(w, destination_id) && gateway_for(w, origin_id, false) == destination_id) {
            const auto& station = query::get_station_state(w, origin_id);
            if (station.supply_index < growth.decline_below) {
                add(K::Emigrant, growth.decline_per_year * station.population / 365.25, false);
            }
        }
    }
    // Those already waiting: a window as long as the route commitment or longer finds each of
    // them once (they are not a flow).
    const double spread = std::min(1.0, window_days / std::max(1.0, w.data.universe_.fleet_investment.route_commitment_days));
    for (const auto& group : query::get_station_state(w, origin_id).waiting) {
        if (group.destination_id == destination_id && group.count > 0.0) {
            out.push_back({.band = group.band, .people = group.count * spread, .max_cost_cr = group.max_cost_cr,
                .value_of_time_cr_per_day = group.value_of_time_cr_per_day, .queued = true,
                .home_pays = home_pays(group.kind)});
        }
    }
    return out;
}

}  // namespace

double carried_passengers_per_day(const World& w, const std::string& origin_id, const std::string& destination_id) {
    // What the ships flying the route now carry, by Little's law: the people aboard over the
    // time each carrier takes to come round again (its trip out and back, taken as twice the
    // trip out).
    double rate = 0.0;
    for (const auto& ship : w.state.ships_) {
        for (const auto& lot : ship.passengers) {
            if (lot.origin_id != origin_id || lot.destination_id != destination_id) {
                continue;
            }
            const double trip_days = std::max(1.0, (ship.active_mission.arrival_time_s - lot.boarded_s) / 86400.0);
            rate += lot.count / (2.0 * trip_days);
        }
    }
    return rate;
}

PassengerRevenue expected_passenger_revenue(const World& w, const std::string& origin_id,
    const std::string& destination_id, double days, double window_days, double berths, double open_share) {
    // The berths filled from the travellers expected over a trip's window, as passenger_offer
    // fills them: the most willing first, each paying its hedonic fare or the clearing fare,
    // never more than it is willing to pay.
    PassengerRevenue revenue;
    const auto* reference = query::reference_fare(w, origin_id, destination_id);
    if (reference == nullptr || berths < 1.0 || open_share <= 0.0) {
        return revenue;
    }
    const auto& env = w.data.universe_.ship_environment;
    const double dose_rate = env.radiation ? env.gcr_msv_per_day : 0.0;
    struct Candidate {
        double people;
        double willing;
        double hedonic;
        bool queued;
        std::string home_pays;
    };
    std::vector<Candidate> candidates;
    for (const auto& band : expected_travellers(w, origin_id, destination_id, window_days)) {
        const double nonfare = query::passenger_nonfare_cr(w, band.value_of_time_cr_per_day, days, dose_rate);
        const double willing = band.max_cost_cr - nonfare;
        if (willing <= 0.0) {
            continue;
        }
        // Travellers still to come wait about half the cycle for this ship, and give up as queues
        // do (at value of time / their highest cost per day): only those left board.
        const double give_up_rate = band.max_cost_cr > 0.0 ? band.value_of_time_cr_per_day / band.max_cost_cr : 0.0;
        const double staying = band.queued ? 1.0 : std::exp(-give_up_rate * 0.5 * window_days);
        candidates.push_back({band.people * open_share * staying, willing,
            systems::live_fare_cr(w, origin_id, destination_id, *reference)
                + query::passenger_nonfare_cr(w, band.value_of_time_cr_per_day, reference->days, dose_rate) - nonfare,
            band.queued, band.home_pays});
    }
    // Residents travel within their travel budget over the window (this ship's open share of it).
    std::map<std::string, double> home_budgets;
    for (const auto& candidate : candidates) {
        if (!candidate.home_pays.empty() && !home_budgets.contains(candidate.home_pays)) {
            home_budgets[candidate.home_pays] = std::max(0.0, query::household_travel_budget(w, candidate.home_pays, window_days)) * open_share;
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) { return a.willing > b.willing; });
    // The fare clears above the hedonic fare only when the queue already waiting outnumbers the
    // berths: the travellers still to come board whichever ship calls first, so a long window
    // does not let this ship pick the most willing of them.
    double clearing = 0.0;
    {
        double queue_left = berths;
        for (const auto& candidate : candidates) {
            if (!candidate.queued) {
                continue;
            }
            if (candidate.people > queue_left) {
                clearing = candidate.willing;
                break;
            }
            queue_left -= candidate.people;
        }
    }
    double left = berths;
    std::size_t boarding = 0;
    for (const auto& candidate : candidates) {
        if (left <= 0.0) {
            break;
        }
        left -= std::min(candidate.people, left);
        ++boarding;
    }
    left = berths;
    for (std::size_t c = 0; c < boarding; ++c) {
        const auto& candidate = candidates[c];
        double count = std::min(candidate.people, left);
        const double fare = std::min(candidate.willing, std::max(candidate.hedonic, clearing));
        if (!candidate.home_pays.empty() && fare > 0.0) {
            double& budget = home_budgets[candidate.home_pays];
            count = std::min(count, budget / fare);
            budget -= count * fare;
        }
        left -= count;
        revenue.people += count;
        revenue.fares_cr += count * fare;
    }
    return revenue;
}

double expected_fare_cr(const World& w, const std::string& origin_id, const std::string& destination_id, double days,
    double demand_per_trip, double berths) {
    const auto* reference = query::reference_fare(w, origin_id, destination_id);
    if (reference == nullptr || berths < 1.0) {
        return 0.0;
    }
    const auto& pax = w.data.universe_.passengers;
    const auto& env = w.data.universe_.ship_environment;
    const double dose_rate = env.radiation ? env.gcr_msv_per_day : 0.0;
    const double vot = query::value_of_time_cr_per_day(w, domain::PassengerKind::Migrant);
    const double nonfare = query::passenger_nonfare_cr(w, vot, days, dose_rate);
    const double hedonic = reference_cost_cr(w, origin_id, destination_id, vot) - nonfare;
    // With willingness to pay falling off as cost^elasticity (-1), berths filled from a demand
    // `demand_per_trip` times larger clear at that multiple of the reference cost, up to the
    // highest band.
    const double top = std::pow(pax.band_ratio, std::max(0.0, pax.demand_bands - 2.0));
    const double multiple = std::clamp(std::pow(demand_per_trip / berths, -1.0 / pax.fare_elasticity), 1.0, top);
    return std::max(hedonic, multiple * reference_cost_cr(w, origin_id, destination_id, vot) - nonfare);
}

void step_passengers(World& w, double dt_s) {
    const double dt_days = dt_s / 86400.0;
    // Rotation: each position is relieved once a tour. The station pays, so it sends for a
    // relief only while it can (above its credit floor by the fare); otherwise tours run long.
    for (const auto& [station_id, tour] : w.data.rotation_tours_) {
        auto& station = query::get_station_state(w, station_id);
        const auto* fare = query::reference_fare(w, tour.out_gateway, station_id);
        if (station.credits < w.data.universe_.open_economy.station_credit_floor + systems::live_fare_cr(w, tour.out_gateway, station_id, *fare)) {
            continue;
        }
        post_travellers(w, query::get_station_state(w, tour.out_gateway), domain::PassengerKind::RotationOut, station_id, station_id,
            rotation_positions(w, station) / tour.tour_days * dt_days, true);
    }
    // Discretionary trips: posted at the origin; visitors whose stay is over head home.
    // With visitor_batch_days, the trips of a whole batch are posted at once when the batch
    // starts (game time has already advanced by this tick), so groups form a week at a time.
    const double batch_s = w.data.universe_.passengers.visitor_batch_days * 86400.0;
    const bool post_visitors = batch_s <= 0.0
        || std::floor(w.state.game_time_s_ / batch_s) != std::floor((w.state.game_time_s_ - dt_s) / batch_s);
    const double visitor_days = batch_s > 0.0 ? w.data.universe_.passengers.visitor_batch_days : dt_days;
    if (query::visits_on(w)) {
        for (auto& origin : w.state.stations_) {
            for (const auto& destination : w.data.universe_.stations) {
                if (!post_visitors) {
                    break;
                }
                const double trips = visitor_trips_per_day(w, origin.station_id, destination.id) * visitor_days;
                if (trips > 0.0) {
                    post_travellers(w, origin, domain::PassengerKind::Visitor, destination.id, origin.station_id, trips);
                }
            }
        }
        for (auto& station : w.state.stations_) {
            std::erase_if(station.visiting, [&](const domain::PassengerGroup& stay) {
                if (stay.since_s > w.state.game_time_s_) {
                    return false;
                }
                post_travellers(w, station, domain::PassengerKind::VisitorHome, stay.destination_id, stay.destination_id,
                    stay.count, true);
                return true;
            });
        }
    }
    // Waiting costs time: a person whose budget the wait would use up leaves the queue. In
    // expectation the group thins at value of time / highest cost per day.
    for (auto& station : w.state.stations_) {
        for (auto& group : station.waiting) {
            const double rate = group.max_cost_cr > 0.0 ? group.value_of_time_cr_per_day / group.max_cost_cr : 1.0;
            const double lost = group.count * (1.0 - std::exp(-rate * dt_days));
            group.count -= lost;
            w.audits.passenger_audit_.gave_up[static_cast<std::size_t>(group.kind)] += lost;
            w.audits.passenger_audit_.routes[{station.station_id, group.destination_id}].gave_up += lost;
        }
        std::erase_if(station.waiting, [&](const domain::PassengerGroup& group) {
            if (group.count >= MIN_GROUP_COUNT) {
                return false;
            }
            w.audits.passenger_audit_.gave_up[static_cast<std::size_t>(group.kind)] += group.count;
            w.audits.passenger_audit_.routes[{station.station_id, group.destination_id}].gave_up += group.count;
            return true;
        });
    }
}

PassengerOffer passenger_offer(const World& w, const domain::ShipState& ship,
    const domain::ShipClassDefinition& design, const domain::StationState& origin,
    const domain::StationDefinition& destination, double days, double propellant_cr_per_kg, double berths) {
    (void)ship;
    PassengerOffer offer;
    if (berths < 1.0 || origin.waiting.empty()) {
        return offer;
    }
    const auto* reference = query::reference_fare(w, origin.station_id, destination.id);
    if (reference == nullptr) {
        return offer;
    }
    const auto& env = w.data.universe_.ship_environment;
    const auto& origin_def = query::get_station_definition(w, origin.station_id);
    const double dose_rate = env.radiation ? query::leg_dose_msv_per_day(w, design, origin_def, destination, w.state.game_time_s_) : 0.0;
    const double reference_dose_rate = env.radiation ? env.gcr_msv_per_day : 0.0;
    double provisions_per_day = 0.0;
    for (const auto& [commodity_id, units_per_day] : w.data.universe_.ship_operations.life_support_units_per_crew_day) {
        provisions_per_day += units_per_day * systems::station_price(w, origin, commodity_id);
    }
    const double marginal = provisions_per_day * days + query::passenger_kg(w, days) * propellant_cr_per_kg;
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
        const double nonfare = query::passenger_nonfare_cr(w, vot, days, dose_rate);
        const double willing = group.max_cost_cr - nonfare;
        if (willing <= marginal) {
            continue;
        }
        candidates.push_back({g, whole, willing,
            systems::live_fare_cr(w, origin.station_id, destination.id, *reference)
                + query::passenger_nonfare_cr(w, vot, reference->days, reference_dose_rate) - nonfare});
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
    // An employer pays only what it has above its credit floor.
    std::map<std::string, double> budgets;
    for (std::size_t c = 0; c < boarding; ++c) {
        const auto& candidate = candidates[c];
        const double fare = std::min(candidate.willing, std::max({candidate.hedonic, clearing, marginal}));
        if (fare <= marginal) {
            continue;
        }
        double count = std::min(candidate.whole, left);
        const auto& group = origin.waiting[candidate.group];
        if (query::employer_pays(w, group.kind) && !group.work_station_id.empty()) {
            auto budget = budgets.find(group.work_station_id);
            if (budget == budgets.end()) {
                budget = budgets.emplace(group.work_station_id, query::get_station_state(w, group.work_station_id).credits
                    - w.data.universe_.open_economy.station_credit_floor).first;
            }
            count = std::min(count, std::floor(std::max(0.0, budget->second) / fare + 1e-9));
            if (count < 1.0) {
                continue;
            }
            budget->second -= count * fare;
        } else if (query::colony_employer(group.work_station_id)) {
            // A colony pays its recruits' passage while its account allows.
            const auto& economy_id = group.work_station_id.substr(std::string("colony:").size());
            auto budget = budgets.find(group.work_station_id);
            if (budget == budgets.end()) {
                budget = budgets.emplace(group.work_station_id, query::economy_balance(w, economy_id)).first;   // out of savings, not credit
            }
            count = std::min(count, std::floor(std::max(0.0, budget->second) / fare + 1e-9));
            if (count < 1.0) {
                continue;
            }
            budget->second -= count * fare;
        } else if (query::households_on(w)) {
            // Residents travel on their savings: a station's households pay only what they have.
            const std::string& home = group.kind == domain::PassengerKind::VisitorHome ? group.destination_id : origin.station_id;
            if (!query::is_earth_gateway(w, home)) {
                const auto key = "households:" + home;
                auto budget = budgets.find(key);
                if (budget == budgets.end()) {
                    // Their travel budget (or, without one, their savings beyond half their buffer).
                    budget = budgets.emplace(key, query::household_travel_budget(w, home, 0.0)).first;
                }
                count = std::min(count, std::floor(std::max(0.0, budget->second) / fare + 1e-9));
                if (count < 1.0) {
                    continue;
                }
                budget->second -= count * fare;
            }
        }
        left -= count;
        offer.picks.push_back({.group = candidate.group, .count = count, .fare_cr = fare});
        offer.people += count;
        offer.revenue_cr += count * fare;
        offer.marginal_cr += count * provisions_per_day * days;
        offer.time_cr_per_day += count * query::passenger_nonfare_cr(w, origin.waiting[candidate.group].value_of_time_cr_per_day, 1.0, dose_rate);
    }
    offer.mass_kg = offer.people * query::passenger_kg(w, days);
    return offer;
}

void board_passengers(World& w, domain::ShipState& ship, domain::StationState& origin,
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
        // Rotation (and sponsored settlers) are paid by the employer, the others out of their own
        // (outside) income. The employer recovers it through its prices.
        if (query::employer_pays(w, group.kind) && !group.work_station_id.empty()) {
            auto& employer = query::get_station_state(w, group.work_station_id);
            employer.credits -= fares;
            employer.ledger.passages_paid += fares;
            w.state.journal_.record(query::account_station(w, employer), query::account_fleet(w, ship.faction_id), MoneyCategory::FaresEmployer, fares);
            if (w.data.universe_.passengers.passage_recovery_days > 0.0) {
                employer.passage_debt_cr += fares;
            }
        } else {
            pay_traveller_fare(w, origin, group, fares, query::account_fleet(w, ship.faction_id));
        }
        ship.credits += fares;
        ship.lifetime_profit += fares;
        ship.ledger.fares += fares;
        if (group.kind == domain::PassengerKind::Emigrant || group.kind == domain::PassengerKind::RotationHome) {
            // Those who leave take their savings with them, to Earth (households on).
            if (query::households_on(w) && group.kind == domain::PassengerKind::Emigrant) {
                const double savings = count * per_capita_savings(origin);
                origin.household_credits -= savings;
                w.state.outside_economy_credits_ += savings;
                w.state.journal_.record(query::account_households(w, origin), query::account_earth(), MoneyCategory::MigrantSavings, savings);
            }
            origin.population = std::max(0.0, origin.population - count);
            w.engines.economy_.set_population(origin.station_id, origin.population);
        }
        w.audits.passenger_audit_.boarded[kind] += count;
        w.audits.passenger_audit_.fares_cr[kind] += fares;
        w.audits.passenger_audit_.wait_days[kind] += count * std::max(0.0, w.state.game_time_s_ - group.since_s) / 86400.0;
        {
            auto& route = w.audits.passenger_audit_.routes[{origin.station_id, destination_id}];
            route.boarded += count;
            const auto& env = w.data.universe_.ship_environment;
            if (env.radiation) {
                const double rate = query::leg_dose_msv_per_day(w, query::ship_design(w, ship), query::get_station_definition(w, origin.station_id),
                    query::get_station_definition(w, destination_id), w.state.game_time_s_);
                route.max_leg_dose_msv = std::max(route.max_leg_dose_msv, rate * days);
            }
            std::string design = ship.class_id;
            for (const auto& module : ship.fit) {
                design += "+" + module;
            }
            ++route.carriers[design];
        }
        ship.passengers.push_back({
            .kind = group.kind,
            .origin_id = origin.station_id,
            .destination_id = destination_id,
            .work_station_id = group.work_station_id,
            .count = count,
            .fare_cr = pick.fare_cr,
            .value_of_time_cr_per_day = group.value_of_time_cr_per_day,
            .dose_msv = group.dose_msv,
            .boarded_s = w.state.game_time_s_,
        });
    }
    std::erase_if(origin.waiting, [](const domain::PassengerGroup& group) { return group.count < MIN_GROUP_COUNT; });
    if (!ship.passengers.empty()) {
        ++w.audits.passenger_audit_.departures_with_passengers;
        w.audits.passenger_audit_.berth_days += query::ship_design(w, ship).berths * days;
        double people = 0.0;
        for (const auto& lot : ship.passengers) {
            people += lot.count;
        }
        query::add_event(w, std::format("{} boards {:.0f} passengers at {} for {}", ship.name, people,
            query::get_station_definition(w, origin.station_id).name, query::get_station_definition(w, destination_id).name), "mission");
    }
}

void land_passengers(World& w, domain::ShipState& ship, domain::StationState& destination) {
    if (ship.passengers.empty()) {
        return;
    }
    const auto& definition = query::get_station_definition(w, destination.station_id);
    for (const auto& lot : ship.passengers) {
        const auto kind = static_cast<std::size_t>(lot.kind);
        {
            const char* group = "visitors";
            switch (lot.kind) {
            case domain::PassengerKind::Migrant: group = "settlers"; break;
            case domain::PassengerKind::Emigrant: group = "emigrants"; break;
            case domain::PassengerKind::RotationOut:
            case domain::PassengerKind::RotationHome: group = "rotation"; break;
            case domain::PassengerKind::Visitor:
            case domain::PassengerKind::VisitorHome: break;
            }
            // A colony's recruits go on down to its surface.
            const bool recruits = lot.kind == domain::PassengerKind::Migrant && query::colony_employer(lot.work_station_id);
            w.state.flows_.record(FlowKind::Passengers, lot.origin_id,
                recruits ? "surface:" + lot.work_station_id.substr(std::string("colony:").size()) : destination.station_id,
                group, lot.count, lot.count * lot.fare_cr);
        }
        w.audits.passenger_audit_.landed[kind] += lot.count;
        w.audits.passenger_audit_.flight_days[kind] += lot.count * std::max(0.0, w.state.game_time_s_ - lot.boarded_s) / 86400.0;
        {
            auto& route = w.audits.passenger_audit_.routes[{lot.origin_id, destination.station_id}];
            route.landed += lot.count;
            route.flight_days += lot.count * std::max(0.0, w.state.game_time_s_ - lot.boarded_s) / 86400.0;
        }
        if (lot.kind == domain::PassengerKind::Migrant && query::colony_employer(lot.work_station_id)) {
            // A colony's recruits go down to the surface, with their savings.
            const auto economy_id = lot.work_station_id.substr(std::string("colony:").size());
            w.state.background_population_[economy_id] += lot.count;
            w.audits.passenger_audit_.colony_recruits_landed += lot.count;
            const double savings = lot.count * w.data.universe_.households.migrant_savings_cr;
            w.state.outside_economy_credits_ -= savings;
            economy_account(w, economy_id) += savings;
            w.state.journal_.record(query::account_earth(), query::account_economy(w, economy_id), MoneyCategory::MigrantSavings, savings);
            continue;
        }
        if (lot.kind == domain::PassengerKind::Migrant && query::households_on(w) && query::is_earth_gateway(w, lot.origin_id)) {
            // Settlers from Earth bring their savings.
            const double savings = lot.count * w.data.universe_.households.migrant_savings_cr;
            w.state.outside_economy_credits_ -= savings;
            destination.household_credits += savings;
            w.state.journal_.record(query::account_earth(), query::account_households(w, destination), MoneyCategory::MigrantSavings, savings);
        }
        if (lot.kind == domain::PassengerKind::Migrant || lot.kind == domain::PassengerKind::RotationOut) {
            destination.population += lot.count;
            w.engines.economy_.set_population(destination.station_id, destination.population);
        }
        if (lot.kind == domain::PassengerKind::Visitor) {
            // Visitors stay a while, then go home (a stay per arrival).
            destination.visiting.push_back({
                .kind = domain::PassengerKind::VisitorHome,
                .destination_id = lot.origin_id,
                .work_station_id = lot.origin_id,
                .count = lot.count,
                .since_s = w.state.game_time_s_ + w.data.universe_.passengers.visit_days * 86400.0,
            });
        }
        if (lot.kind == domain::PassengerKind::RotationOut) {
            // Each relief sends one worker home, at the station's expense.
            if (const auto* tour = rotation_tour(w, destination.station_id); tour != nullptr) {
                post_travellers(w, destination, domain::PassengerKind::RotationHome, tour->home_gateway,
                    destination.station_id, lot.count, true);
            }
        }
    }
    double people = 0.0;
    for (const auto& lot : ship.passengers) {
        people += lot.count;
    }
    query::add_event(w, std::format("{} lands {:.0f} passengers at {}", ship.name, people, definition.name), "arrival");
    ship.passengers.clear();
}

std::string ship_passengers_json(const World& w, const domain::ShipState& ship, const domain::ShipClassDefinition& design) {
    // Bridge fields, sent only with passengers on: berths, who is aboard, the cabin shielding.
    if (!query::passengers_on(w)) {
        return {};
    }
    double aboard = 0.0;
    std::string lots = "[";
    for (const auto& lot : ship.passengers) {
        aboard += lot.count;
        lots += std::format("{}{{\"kind\":\"{}\",\"to\":\"{}\",\"count\":{:.0f},\"fare_cr\":{:.1f}}}", lots.size() > 1 ? "," : "",
            passenger_kind_name(lot.kind), json_escape(lot.destination_id), lot.count, lot.fare_cr);
    }
    lots += "]";
    return std::format("\"berths\":{},\"passengers_aboard\":{:.0f},\"passenger_lots\":{},\"fares_cr\":{:.1f},"
                       "\"cabin_shield_g_cm2\":{:.1f},",
        design.berths, aboard, lots, ship.ledger.fares, query::cabin_shield_g_cm2(w, design, ship.propellant_kg));
}

std::string station_passengers_json(const World& w, const domain::StationState& station) {
    // Bridge fields, sent only with passengers on: who waits here for which station, visitors,
    // what the station paid for passages and has still to recover through its prices.
    if (!query::passengers_on(w)) {
        return {};
    }
    std::map<std::pair<std::string, std::string>, double> waiting;
    for (const auto& group : station.waiting) {
        waiting[{passenger_kind_name(group.kind), group.destination_id}] += group.count;
    }
    std::string list = "[";
    for (const auto& [key, count] : waiting) {
        if (count >= 0.5) {
            list += std::format("{}{{\"kind\":\"{}\",\"to\":\"{}\",\"count\":{:.0f}}}", list.size() > 1 ? "," : "",
                key.first, json_escape(key.second), count);
        }
    }
    list += "]";
    double visiting = 0.0;
    for (const auto& stay : station.visiting) {
        visiting += stay.count;
    }
    const auto* tour = rotation_tour(w, station.station_id);
    return std::format("\"passengers_waiting\":{},\"visitors_here\":{:.0f},\"passages_paid\":{:.1f},"
                       "\"passages_recovered\":{:.1f},\"passage_debt_cr\":{:.1f},\"rotation_tour_days\":{:.0f},",
        list, visiting, station.ledger.passages_paid, station.ledger.passages_recovered, station.passage_debt_cr,
        tour == nullptr ? 0.0 : tour->tour_days);
}

std::vector<ReferenceFareRow> compute_reference_fares(const World& w) {
    const auto& pricing = w.data.universe_.pricing;
    const auto& market = w.data.universe_.market;
    const auto departures = static_cast<int>(pricing.reference_departures);
    // Cabin carriers: every hull with each of its cabin modules (plain, shielded), and the
    // liners bare and with their cabin shields (a water wall, the cabins inside the tanks, both).
    std::vector<domain::ShipClassDefinition> carriers;
    const auto add_carrier = [&](const domain::ShipClassDefinition& hull, const std::vector<const domain::ShipModuleDefinition*>& fit) {
        std::vector<domain::ShipComponentDefinition> parts;
        std::vector<std::string> ids;
        for (const auto* module : fit) {
            parts.push_back(module->part);
            ids.push_back(module->id);
        }
        auto design = domain::derive_design(hull, parts, domain::design_id(hull.id, ids));
        design.base_class_id = hull.id;
        design.fit = ids;
        for (const auto* module : fit) {
            for (const auto& [effect, value] : module->effects) {
                design.effects[effect] += value;
            }
            if (const auto berths = module->effects.find("berths"); module->slot == "cabin" && berths != module->effects.end()) {
                design.berths += berths->second;
            }
        }
        carriers.push_back(std::move(design));
    };
    for (const auto& ship_class : w.data.universe_.ship_classes) {
        // Liners in every tank size (more water round cabins inside the tanks), other hulls nominal.
        if (ship_class.id != ship_class.hull_id && ship_class.berths <= 0.0) {
            continue;
        }
        std::vector<const domain::ShipModuleDefinition*> shields;
        for (const auto& module : w.data.universe_.ship_modules) {
            if (module.hull_id != ship_class.hull_id) {
                continue;
            }
            if (module.slot == "cabin" && module.effects.contains("berths") && ship_class.id == ship_class.hull_id) {
                add_carrier(ship_class, {&module});
            } else if (ship_class.berths > 0.0
                && (module.effects.contains("cabin_shield_g_cm2") || module.effects.contains("tank_wrap_m2"))) {
                shields.push_back(&module);
            }
        }
        if (ship_class.berths > 0.0) {
            carriers.push_back(ship_class);
            for (const auto* shield : shields) {
                add_carrier(ship_class, {shield});
            }
            if (shields.size() > 1) {
                add_carrier(ship_class, shields);
            }
        }
    }
    const double fuel_cr_per_kg = query::get_commodity(w, economy::FUEL_ID).base_price / FUEL_UNITS_TO_KG;
    double provisions_per_day = 0.0;
    for (const auto& [commodity_id, units_per_day] : w.data.universe_.ship_operations.life_support_units_per_crew_day) {
        provisions_per_day += units_per_day * query::get_commodity(w, commodity_id).base_price;
    }
    const auto daily_cost = [&](const domain::ShipClassDefinition& design) {
        return query::daily_capital_cost(w, design) + design.crew_size * w.data.universe_.ship_operations.wage_cr_per_crew_day
            + design.crew_size * provisions_per_day;
    };
    // A resident's time and dose per day aboard: the reference trip is the one with the lowest
    // cost to the passenger (fare plus time), so its planner trades propellant against the
    // ship's time and the passengers' too.
    const auto& env = w.data.universe_.ship_environment;
    const double person_day_cr = query::passenger_nonfare_cr(w, query::value_of_time_cr_per_day(w, domain::PassengerKind::Migrant), 1.0,
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
    for (const auto& from : w.data.universe_.stations) {
        for (const auto& to : w.data.universe_.stations) {
            if (from.id == to.id) {
                continue;
            }
            for (const auto& carrier : carriers) {
                for (int d = 0; d < departures; ++d) {
                    legs.push_back({&from, &to, &carrier, departure_s(d),
                        carrier.berths * query::passenger_kg(w, w.engines.economy_.transfer_days(from, to))});
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
            .boiloff_kg_per_day = query::leg_boiloff_kg_per_day(w, *leg.carrier, *leg.from, *leg.to, leg.departure_s),
            .include_path = false,
        };
        const auto& planner = (leg.carrier->propulsion_type == "variable_isp" && w.engines.variable_isp_planner_)
            ? static_cast<const trajectory::ITrajectoryPlanner&>(*w.engines.variable_isp_planner_)
            : static_cast<const trajectory::ITrajectoryPlanner&>(*w.engines.kepler_planner_);
        return planner.plan_transfer(*leg.from, *leg.to, probe, *leg.carrier, leg.departure_s, options);
    };
    std::vector<domain::TrajectoryPlan> outbound(legs.size());
    w.engines.thread_pool_->parallel_for(legs.size(), [&](std::size_t i) { outbound[i] = plan_leg(legs[i]); });
    std::vector<Leg> returns(legs.size());
    for (std::size_t i = 0; i < legs.size(); ++i) {
        returns[i] = {legs[i].to, legs[i].from, legs[i].carrier,
            outbound[i].feasible ? outbound[i].arrival_time_s : legs[i].departure_s, 0.0};
    }
    std::vector<domain::TrajectoryPlan> inbound(returns.size());
    w.engines.thread_pool_->parallel_for(returns.size(), [&](std::size_t i) {
        if (outbound[i].feasible) {
            inbound[i] = plan_leg(returns[i]);
        }
    });

    std::vector<ReferenceFareRow> rows;
    std::size_t i = 0;
    for (const auto& from : w.data.universe_.stations) {
        for (const auto& to : w.data.universe_.stations) {
            if (from.id == to.id) {
                continue;
            }
            ReferenceFareRow best {.origin_id = from.id, .destination_id = to.id,
                .fare_cr = std::numeric_limits<double>::infinity()};
            double best_cost = std::numeric_limits<double>::infinity();
            for (const auto& carrier : carriers) {
                // (cost to the passenger, fare, days out, propellant per seat out and back)
                std::vector<std::tuple<double, double, double, double, double>> trips;
                for (int d = 0; d < departures; ++d, ++i) {
                    const auto& out = outbound[i];
                    const auto& back = inbound[i];
                    const double out_days = out.travel_time_s / 86400.0;
                    if (!out.feasible || !back.feasible
                        || !query::leg_dose_allowed(w, carrier, from, to, departure_s(d), out_days)) {
                        continue;
                    }
                    const double back_days = back.travel_time_s / 86400.0;
                    const double cost = daily_cost(carrier) * (out_days + market.return_share * back_days)
                        + (consumed_propellant_kg(out) + market.return_share * consumed_propellant_kg(back)) * fuel_cr_per_kg;
                    const double per_seat = pricing.carrier_margin / (carrier.berths * market.fill_ref);
                    const double fare = per_seat * cost + provisions_per_day * out_days;
                    trips.emplace_back(fare + person_day_cr * out_days, fare, out_days, per_seat * consumed_propellant_kg(out),
                        per_seat * market.return_share * consumed_propellant_kg(back));
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
                    best.fuel_out_kg = std::get<3>(median);
                    best.fuel_back_kg = std::get<4>(median);
                }
            }
            if (std::isfinite(best.fare_cr)) {
                rows.push_back(std::move(best));
            }
        }
    }
    return rows;
}

}  // namespace spacetrains::simulation::systems
