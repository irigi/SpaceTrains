// Passengers (docs/plans/passengers.md): people waiting at stations, the fares ships charge
// them, boarding and landing, and the reference fares the market prices against.

#include "domain/ShipDesign.hpp"
#include "simulation/Simulation.hpp"
#include "simulation/SimulationInternal.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <map>
#include <numeric>
#include <tuple>

namespace spacetrains::simulation {

using namespace detail;

namespace {
// A waiting group smaller than this is gone (the rest of its people gave up).
constexpr double MIN_GROUP_COUNT = 1.0e-6;

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

bool Simulation::employer_pays(domain::PassengerKind kind) const {
    // With the labour market on, settlers pay their own passage out of what the job gains them.
    return pays_from_employer(kind)
        || (kind == domain::PassengerKind::Migrant && universe_.passengers.settlers_sponsored != 0.0 && !labour_on());
}

double Simulation::settler_share(double payback_years) const {
    const auto& pax = universe_.passengers;
    if (payback_years <= 0.0 || pax.settler_payback_half_years <= 0.0) {
        return 1.0;
    }
    return 1.0 / (1.0 + std::pow(payback_years / pax.settler_payback_half_years, pax.settler_payback_exponent));
}

double Simulation::payback_years(const std::string& origin_id, const std::string& destination_id) const {
    const double vot = value_of_time_cr_per_day(domain::PassengerKind::Migrant);
    return vot > 0.0 ? reference_cost_cr(origin_id, destination_id, vot) / (365.0 * vot) : 0.0;
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

double Simulation::port_fuel_price(const std::string& station_id) const {
    const double base = get_commodity(economy::FUEL_ID).base_price;
    return std::max(base, station_price(get_station_state(station_id), economy::FUEL_ID));
}

// The reference fare at today's fuel prices: its propellant per seat is bought at the origin
// (and the return share at the destination) at those ports' prices instead of the base price.
// Earth makes no fuel, so trips from Earth cost several times the base-price fare's fuel.
double Simulation::live_fare_cr(const std::string& origin_id, const std::string& destination_id,
    const domain::PassengerDefinition::ReferenceFare& reference) const {
    const double base = get_commodity(economy::FUEL_ID).base_price;
    return reference.fare_cr
        + (reference.fuel_out_kg * (port_fuel_price(origin_id) - base)
              + reference.fuel_back_kg * (port_fuel_price(destination_id) - base))
        / FUEL_UNITS_TO_KG;
}

double Simulation::reference_cost_cr(const std::string& origin_id, const std::string& destination_id,
    double value_of_time) const {
    const auto* reference = reference_fare(origin_id, destination_id);
    if (reference == nullptr) {
        return std::numeric_limits<double>::infinity();
    }
    const auto& env = universe_.ship_environment;
    return live_fare_cr(origin_id, destination_id, *reference)
        + passenger_nonfare_cr(value_of_time, reference->days, env.radiation ? env.gcr_msv_per_day : 0.0);
}

std::vector<Simulation::PassengerBand> Simulation::passenger_bands(domain::PassengerKind kind,
    const std::string& origin_id, const std::string& destination_id, double count, bool fixed, const std::string& employer) const {
    std::vector<PassengerBand> out;
    if (count <= 0.0) {
        return out;
    }
    const auto& pax = universe_.passengers;
    const double value_of_time = value_of_time_cr_per_day(kind);
    const double reference = reference_cost_cr(origin_id, destination_id, value_of_time);
    if (!std::isfinite(reference)) {
        return out;  // no carrier can fly the trip
    }
    // Willingness to pay falls off as cost^elasticity: `count` people would travel at the
    // reference cost; band k holds those whose highest acceptable cost lies between its
    // maximum m_k = reference x ratio^(k-1) and the next band's.
    // Sponsored settlers: `count` is every settler the station would take; the share whose job
    // repays a passage costing m x the reference follows the payback curve.
    const int bands = std::max(1, static_cast<int>(pax.demand_bands));
    const bool sponsored = kind == domain::PassengerKind::Migrant && employer_pays(kind);
    const double payback = sponsored ? reference / (365.0 * value_of_time) : 0.0;
    // Labour market (phase B): what each traveller would pay comes from a distribution, not a
    // multiple of the reference cost. Settlers: their gain from the job; the unemployed going
    // home: an Earth wage; visitors: a share of a year's income where they live.
    using K = domain::PassengerKind;
    const bool from_income = labour_on() && kind != K::RotationOut && kind != K::RotationHome;
    const std::string home = kind == K::VisitorHome ? destination_id : origin_id;
    const double reference_tail = from_income && kind == K::Visitor ? traveller_budget_tail(home, reference) : 1.0;
    // A colony's recruits: the colony pays up to the reference cost of their passage.
    const bool recruited = kind == K::Migrant && colony_employer(employer);
    const auto share_above = [&](double multiple) {
        if (recruited) {
            return multiple <= 1.0 + 1e-9 ? 1.0 : 0.0;
        }
        if (from_income) {
            const double cost = multiple * reference;
            switch (kind) {
                case K::Migrant: return settler_gain_tail(get_station_state(destination_id), cost);
                case K::Emigrant: return returner_gain_tail(cost);
                case K::VisitorHome: return traveller_budget_tail(home, cost);
                default: return reference_tail > 0.0 ? traveller_budget_tail(home, cost) / reference_tail : 0.0;
            }
        }
        return sponsored ? settler_share(multiple * payback) : std::pow(multiple, pax.fare_elasticity);
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

void Simulation::post_travellers(domain::StationState& origin, domain::PassengerKind kind,
    const std::string& destination_id, const std::string& work_station_id, double count, bool fixed) {
    const double dose_msv = 0.0;
    for (const auto& [band, people, max_cost, value_of_time, queued, home_pays] :
        passenger_bands(kind, origin.station_id, destination_id, count, fixed, work_station_id)) {
        passenger_audit_.posted[static_cast<std::size_t>(kind)] += people;
        passenger_audit_.routes[{origin.station_id, destination_id}].posted += people;
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
                .max_cost_cr = max_cost,
                .value_of_time_cr_per_day = value_of_time,
                .dose_msv = dose_msv,
                .band = band,
            });
        } else {
            // The group's waiting time is the mean of its people's.
            group->since_s = (group->since_s * group->count + game_time_s_ * people) / (group->count + people);
            group->count += people;
            group->max_cost_cr = max_cost;
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
            if (group.kind == kind && !colony_employer(group.work_station_id) && (kind == domain::PassengerKind::Emigrant
                    ? station.station_id == station_id : group.destination_id == station_id)) {
                pending += group.count;
            }
        }
    }
    for (const auto& ship : ships_) {
        for (const auto& lot : ship.passengers) {
            if (lot.kind == kind && !colony_employer(lot.work_station_id) && (kind == domain::PassengerKind::Emigrant
                    ? lot.origin_id == station_id : lot.destination_id == station_id)) {
                pending += lot.count;
            }
        }
    }
    return pending;
}

double Simulation::colony_recruits_per_day(const domain::BackgroundEconomyDefinition& economy) const {
    // The part of a colony's growth it recruits on Earth, while it can pay for passages.
    if (!labour_on() || !migration_by_ship() || economy.immigration_share <= 0.0) {
        return 0.0;
    }
    const auto colony = background_population_.find(economy.id);
    if (colony == background_population_.end() || economy_balance(economy.id) <= 0.0) {
        return 0.0;   // it pays passages out of savings
    }
    return colony->second * economy.growth_per_year * economy.immigration_share * std::max(0.0, colony_growth_factor(economy.id))
        / 365.25;
}

void Simulation::compute_rotation_tours() {
    rotation_tours_.clear();
    if (!rotation_on()) {
        return;
    }
    const auto& pax = universe_.passengers;
    const auto& env = universe_.ship_environment;
    const double wage = value_of_time_cr_per_day(domain::PassengerKind::RotationOut);
    const double dose_rate = env.radiation ? env.gcr_msv_per_day : 0.0;
    for (const auto& station : universe_.stations) {
        if (is_earth_gateway(station.id)) {
            continue;
        }
        const auto& out_gateway = gateway_for(station.id, true);
        const auto& home_gateway = gateway_for(station.id, false);
        const auto* out = reference_fare(out_gateway, station.id);
        const auto* home = reference_fare(station.id, home_gateway);
        if (out == nullptr || home == nullptr || wage <= 0.0) {
            continue;  // no trip there or back: no rotation (the staff settle)
        }
        const double fares = live_fare_cr(out_gateway, station.id, *out) + live_fare_cr(station.id, home_gateway, *home);
        const double round_trip = fares + passenger_nonfare_cr(wage, out->days, dose_rate)
            + passenger_nonfare_cr(wage, home->days, dose_rate);
        const double tour_days = std::sqrt(round_trip * 365.0 / (pax.hardship_per_year * wage));
        rotation_tours_[station.id] = {
            .out_gateway = out_gateway,
            .home_gateway = home_gateway,
            .tour_days = tour_days,
            .fares_per_day = fares / tour_days,
        };
    }
}

const Simulation::RotationTour* Simulation::rotation_tour(const std::string& station_id) const {
    const auto it = rotation_tours_.find(station_id);
    return it == rotation_tours_.end() ? nullptr : &it->second;
}

double Simulation::rotation_positions(const domain::StationState& station) const {
    return universe_.passengers.rotation_share * station.population;
}

double Simulation::visitor_trips_per_day(const std::string& origin_id, const std::string& destination_id) const {
    if (!visits_on() || origin_id == destination_id || (is_earth_gateway(origin_id) && is_earth_gateway(destination_id))
        || reference_fare(origin_id, destination_id) == nullptr || reference_fare(destination_id, origin_id) == nullptr) {
        return 0.0;
    }
    const auto& pax = universe_.passengers;
    const double gateways = static_cast<double>(pax.earth_gateways.size());
    const auto people = [&](const std::string& station_id) {
        return is_earth_gateway(station_id) ? pax.earth_population_weight / gateways
                                            : get_station_state(station_id).population;
    };
    double others = 0.0;
    for (const auto& station : universe_.stations) {
        if (station.id != origin_id && !(is_earth_gateway(origin_id) && is_earth_gateway(station.id))) {
            others += people(station.id);
        }
    }
    if (others <= 0.0) {
        return 0.0;
    }
    const double vot = value_of_time_cr_per_day(domain::PassengerKind::Visitor);
    const double year_of_income = 365.0 * vot;
    const double reference = reference_cost_cr(origin_id, destination_id, vot);
    // Labour market: those whose trip budget covers the reference cost (income distributions).
    const double deterrence = labour_on() ? traveller_budget_tail(origin_id, reference)
                                          : std::pow(reference / year_of_income, pax.fare_elasticity);
    return pax.visit_trips_per_person_year / 365.0 * people(origin_id) * people(destination_id) / others * deterrence;
}

double Simulation::passenger_flow_per_day(const std::string& origin_id, const std::string& destination_id) const {
    double flow = 0.0;
    // Visitors out, and visitors from there going home.
    flow += visitor_trips_per_day(origin_id, destination_id) + visitor_trips_per_day(destination_id, origin_id);
    // Rotation: reliefs out to a station and the relieved home, one per position and tour.
    if (const auto* tour = rotation_tour(destination_id); tour != nullptr && tour->out_gateway == origin_id) {
        flow += rotation_positions(get_station_state(destination_id)) / tour->tour_days;
    }
    if (const auto* tour = rotation_tour(origin_id); tour != nullptr && tour->home_gateway == destination_id) {
        flow += rotation_positions(get_station_state(origin_id)) / tour->tour_days;
    }
    if (migration_by_ship() && labour_on()) {
        // Labour market: vacancies posted to Earth, the unemployed going home.
        if (is_earth_gateway(origin_id) && !is_earth_gateway(destination_id) && gateway_for(destination_id, true) == origin_id) {
            flow += labour_migrants_per_day(get_station_state(destination_id));
            for (const auto& economy : universe_.background_economies) {
                if (!economy.gateways.empty() && economy.gateways.front() == destination_id) {
                    flow += colony_recruits_per_day(economy);
                }
            }
        }
        if (!is_earth_gateway(origin_id) && is_earth_gateway(destination_id) && gateway_for(origin_id, false) == destination_id) {
            flow += labour_emigrants_per_day(get_station_state(origin_id));
        }
    } else if (migration_by_ship()) {
        const auto& growth = universe_.growth;
        const double floor_credits = universe_.open_economy.station_credit_floor;
        if (is_earth_gateway(origin_id) && !is_earth_gateway(destination_id) && gateway_for(destination_id, true) == origin_id) {
            const auto& station = get_station_state(destination_id);
            const double ceiling = growth.max_population_factor
                * static_cast<double>(get_station_definition(destination_id).population);
            if (station.supply_index >= growth.grow_above && station.credits >= floor_credits && station.population < ceiling) {
                // Those who would come at the reference cost.
                flow += growth.growth_per_year * station.population / 365.25
                    * (employer_pays(domain::PassengerKind::Migrant) ? settler_share(payback_years(origin_id, destination_id)) : 1.0);
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

std::vector<Simulation::PassengerBand> Simulation::expected_travellers(const std::string& origin_id,
    const std::string& destination_id, double window_days) const {
    // What post_travellers would post over `window_days` (the steady flows), by band.
    std::vector<PassengerBand> out;
    // Who pays: a station's residents (households on) for those who are not Earth's people and
    // whose employer does not pay.
    const auto home_pays = [&](domain::PassengerKind kind) -> std::string {
        if (!households_on() || employer_pays(kind)) {
            return {};
        }
        const std::string& home = kind == domain::PassengerKind::VisitorHome ? destination_id : origin_id;
        return is_earth_gateway(home) ? std::string() : home;
    };
    const auto add = [&](domain::PassengerKind kind, double per_day, bool fixed) {
        if (per_day > 0.0) {
            auto bands = passenger_bands(kind, origin_id, destination_id, per_day * window_days, fixed);
            for (auto& band : bands) {
                band.home_pays = home_pays(kind);
            }
            out.insert(out.end(), bands.begin(), bands.end());
        }
    };
    using K = domain::PassengerKind;
    add(K::Visitor, visitor_trips_per_day(origin_id, destination_id), false);
    add(K::VisitorHome, visitor_trips_per_day(destination_id, origin_id), true);
    if (const auto* tour = rotation_tour(destination_id); tour != nullptr && tour->out_gateway == origin_id) {
        add(K::RotationOut, rotation_positions(get_station_state(destination_id)) / tour->tour_days, true);
    }
    if (const auto* tour = rotation_tour(origin_id); tour != nullptr && tour->home_gateway == destination_id) {
        add(K::RotationHome, rotation_positions(get_station_state(origin_id)) / tour->tour_days, true);
    }
    if (migration_by_ship() && labour_on()) {
        if (is_earth_gateway(origin_id) && !is_earth_gateway(destination_id) && gateway_for(destination_id, true) == origin_id) {
            add(K::Migrant, labour_migrants_per_day(get_station_state(destination_id)), true);
            for (const auto& economy : universe_.background_economies) {
                if (!economy.gateways.empty() && economy.gateways.front() == destination_id) {
                    if (const double recruits = colony_recruits_per_day(economy); recruits > 0.0) {
                        auto bands = passenger_bands(K::Migrant, origin_id, destination_id, recruits * window_days, true,
                            "colony:" + economy.id);
                        out.insert(out.end(), bands.begin(), bands.end());
                    }
                }
            }
        }
        if (!is_earth_gateway(origin_id) && is_earth_gateway(destination_id) && gateway_for(origin_id, false) == destination_id) {
            add(K::Emigrant, labour_emigrants_per_day(get_station_state(origin_id)), true);
        }
    } else if (migration_by_ship()) {
        const auto& growth = universe_.growth;
        if (is_earth_gateway(origin_id) && !is_earth_gateway(destination_id) && gateway_for(destination_id, true) == origin_id) {
            const auto& station = get_station_state(destination_id);
            const double ceiling = growth.max_population_factor
                * static_cast<double>(get_station_definition(destination_id).population);
            if (station.supply_index >= growth.grow_above
                && station.credits >= universe_.open_economy.station_credit_floor && station.population < ceiling) {
                add(K::Migrant, growth.growth_per_year * station.population / 365.25, false);
            }
        }
        if (!is_earth_gateway(origin_id) && is_earth_gateway(destination_id) && gateway_for(origin_id, false) == destination_id) {
            const auto& station = get_station_state(origin_id);
            if (station.supply_index < growth.decline_below) {
                add(K::Emigrant, growth.decline_per_year * station.population / 365.25, false);
            }
        }
    }
    // Those already waiting: a window as long as the route commitment or longer finds each of
    // them once (they are not a flow).
    const double spread = std::min(1.0, window_days / std::max(1.0, universe_.fleet_investment.route_commitment_days));
    for (const auto& group : get_station_state(origin_id).waiting) {
        if (group.destination_id == destination_id && group.count > 0.0) {
            out.push_back({.band = group.band, .people = group.count * spread, .max_cost_cr = group.max_cost_cr,
                .value_of_time_cr_per_day = group.value_of_time_cr_per_day, .queued = true,
                .home_pays = home_pays(group.kind)});
        }
    }
    return out;
}

double Simulation::carried_passengers_per_day(const std::string& origin_id, const std::string& destination_id) const {
    // What the ships flying the route now carry, by Little's law: the people aboard over the
    // time each carrier takes to come round again (its trip out and back, taken as twice the
    // trip out).
    double rate = 0.0;
    for (const auto& ship : ships_) {
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

Simulation::PassengerRevenue Simulation::expected_passenger_revenue(const std::string& origin_id,
    const std::string& destination_id, double days, double window_days, double berths, double open_share) const {
    // The berths filled from the travellers expected over a trip's window, as passenger_offer
    // fills them: the most willing first, each paying its hedonic fare or the clearing fare,
    // never more than it is willing to pay.
    PassengerRevenue revenue;
    const auto* reference = reference_fare(origin_id, destination_id);
    if (reference == nullptr || berths < 1.0 || open_share <= 0.0) {
        return revenue;
    }
    const auto& env = universe_.ship_environment;
    const double dose_rate = env.radiation ? env.gcr_msv_per_day : 0.0;
    struct Candidate {
        double people;
        double willing;
        double hedonic;
        bool queued;
        std::string home_pays;
    };
    std::vector<Candidate> candidates;
    for (const auto& band : expected_travellers(origin_id, destination_id, window_days)) {
        const double nonfare = passenger_nonfare_cr(band.value_of_time_cr_per_day, days, dose_rate);
        const double willing = band.max_cost_cr - nonfare;
        if (willing <= 0.0) {
            continue;
        }
        // Travellers still to come wait about half the cycle for this ship, and give up as queues
        // do (at value of time / their highest cost per day): only those left board.
        const double give_up_rate = band.max_cost_cr > 0.0 ? band.value_of_time_cr_per_day / band.max_cost_cr : 0.0;
        const double staying = band.queued ? 1.0 : std::exp(-give_up_rate * 0.5 * window_days);
        candidates.push_back({band.people * open_share * staying, willing,
            live_fare_cr(origin_id, destination_id, *reference)
                + passenger_nonfare_cr(band.value_of_time_cr_per_day, reference->days, dose_rate) - nonfare,
            band.queued, band.home_pays});
    }
    // Residents travel within their travel budget over the window (this ship's open share of it).
    std::map<std::string, double> home_budgets;
    for (const auto& candidate : candidates) {
        if (!candidate.home_pays.empty() && !home_budgets.contains(candidate.home_pays)) {
            home_budgets[candidate.home_pays] = std::max(0.0, household_travel_budget(candidate.home_pays, window_days)) * open_share;
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
    const double dt_days = dt_s / 86400.0;
    // Rotation: each position is relieved once a tour. The station pays, so it sends for a
    // relief only while it can (above its credit floor by the fare); otherwise tours run long.
    for (const auto& [station_id, tour] : rotation_tours_) {
        auto& station = get_station_state(station_id);
        const auto* fare = reference_fare(tour.out_gateway, station_id);
        if (station.credits < universe_.open_economy.station_credit_floor + live_fare_cr(tour.out_gateway, station_id, *fare)) {
            continue;
        }
        post_travellers(get_station_state(tour.out_gateway), domain::PassengerKind::RotationOut, station_id, station_id,
            rotation_positions(station) / tour.tour_days * dt_days, true);
    }
    // Discretionary trips: posted at the origin; visitors whose stay is over head home.
    // With visitor_batch_days, the trips of a whole batch are posted at once when the batch
    // starts (game time has already advanced by this tick), so groups form a week at a time.
    const double batch_s = universe_.passengers.visitor_batch_days * 86400.0;
    const bool post_visitors = batch_s <= 0.0
        || std::floor(game_time_s_ / batch_s) != std::floor((game_time_s_ - dt_s) / batch_s);
    const double visitor_days = batch_s > 0.0 ? universe_.passengers.visitor_batch_days : dt_days;
    if (visits_on()) {
        for (auto& origin : stations_) {
            for (const auto& destination : universe_.stations) {
                if (!post_visitors) {
                    break;
                }
                const double trips = visitor_trips_per_day(origin.station_id, destination.id) * visitor_days;
                if (trips > 0.0) {
                    post_travellers(origin, domain::PassengerKind::Visitor, destination.id, origin.station_id, trips);
                }
            }
        }
        for (auto& station : stations_) {
            std::erase_if(station.visiting, [&](const domain::PassengerGroup& stay) {
                if (stay.since_s > game_time_s_) {
                    return false;
                }
                post_travellers(station, domain::PassengerKind::VisitorHome, stay.destination_id, stay.destination_id,
                    stay.count, true);
                return true;
            });
        }
    }
    // Waiting costs time: a person whose budget the wait would use up leaves the queue. In
    // expectation the group thins at value of time / highest cost per day.
    for (auto& station : stations_) {
        for (auto& group : station.waiting) {
            const double rate = group.max_cost_cr > 0.0 ? group.value_of_time_cr_per_day / group.max_cost_cr : 1.0;
            const double lost = group.count * (1.0 - std::exp(-rate * dt_days));
            group.count -= lost;
            passenger_audit_.gave_up[static_cast<std::size_t>(group.kind)] += lost;
            passenger_audit_.routes[{station.station_id, group.destination_id}].gave_up += lost;
        }
        std::erase_if(station.waiting, [&](const domain::PassengerGroup& group) {
            if (group.count >= MIN_GROUP_COUNT) {
                return false;
            }
            passenger_audit_.gave_up[static_cast<std::size_t>(group.kind)] += group.count;
            passenger_audit_.routes[{station.station_id, group.destination_id}].gave_up += group.count;
            return true;
        });
    }
}

bool Simulation::passenger_leg_waiting(const domain::StationState& origin, const std::string& destination_id) const {
    // A station is worth a passenger leg once enough people wait for it (min_leg_passengers;
    // at 1, any whole person); smaller groups still ride on legs flown for cargo.
    const double needed = universe_.passengers.min_leg_passengers;
    if (needed <= 1.0) {
        return std::any_of(origin.waiting.begin(), origin.waiting.end(), [&](const domain::PassengerGroup& group) {
            return group.destination_id == destination_id && group.count >= 1.0;
        });
    }
    double people = 0.0;
    for (const auto& group : origin.waiting) {
        if (group.destination_id == destination_id) {
            people += std::floor(group.count + 1e-9);
        }
    }
    return people >= needed;
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
            live_fare_cr(origin.station_id, destination.id, *reference)
                + passenger_nonfare_cr(vot, reference->days, reference_dose_rate) - nonfare});
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
        if (employer_pays(group.kind) && !group.work_station_id.empty()) {
            auto budget = budgets.find(group.work_station_id);
            if (budget == budgets.end()) {
                budget = budgets.emplace(group.work_station_id, get_station_state(group.work_station_id).credits
                    - universe_.open_economy.station_credit_floor).first;
            }
            count = std::min(count, std::floor(std::max(0.0, budget->second) / fare + 1e-9));
            if (count < 1.0) {
                continue;
            }
            budget->second -= count * fare;
        } else if (colony_employer(group.work_station_id)) {
            // A colony pays its recruits' passage while its account allows.
            const auto& economy_id = group.work_station_id.substr(std::string("colony:").size());
            auto budget = budgets.find(group.work_station_id);
            if (budget == budgets.end()) {
                budget = budgets.emplace(group.work_station_id, economy_balance(economy_id)).first;   // out of savings, not credit
            }
            count = std::min(count, std::floor(std::max(0.0, budget->second) / fare + 1e-9));
            if (count < 1.0) {
                continue;
            }
            budget->second -= count * fare;
        } else if (households_on()) {
            // Residents travel on their savings: a station's households pay only what they have.
            const std::string& home = group.kind == domain::PassengerKind::VisitorHome ? group.destination_id : origin.station_id;
            if (!is_earth_gateway(home)) {
                const auto key = "households:" + home;
                auto budget = budgets.find(key);
                if (budget == budgets.end()) {
                    // Their travel budget (or, without one, their savings beyond half their buffer).
                    budget = budgets.emplace(key, household_travel_budget(home, 0.0)).first;
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
        offer.time_cr_per_day += count * passenger_nonfare_cr(origin.waiting[candidate.group].value_of_time_cr_per_day, 1.0, dose_rate);
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
        // Rotation (and sponsored settlers) are paid by the employer, the others out of their own
        // (outside) income. The employer recovers it through its prices.
        if (employer_pays(group.kind) && !group.work_station_id.empty()) {
            auto& employer = get_station_state(group.work_station_id);
            employer.credits -= fares;
            employer.ledger.passages_paid += fares;
            journal_.record(account_station(employer), account_fleet(ship.faction_id), MoneyCategory::FaresEmployer, fares);
            if (universe_.passengers.passage_recovery_days > 0.0) {
                employer.passage_debt_cr += fares;
            }
        } else {
            pay_traveller_fare(origin, group, fares, account_fleet(ship.faction_id));
        }
        ship.credits += fares;
        ship.lifetime_profit += fares;
        ship.ledger.fares += fares;
        if (group.kind == domain::PassengerKind::Emigrant || group.kind == domain::PassengerKind::RotationHome) {
            // Those who leave take their savings with them, to Earth (households on).
            if (households_on() && group.kind == domain::PassengerKind::Emigrant) {
                const double savings = count * per_capita_savings(origin);
                origin.household_credits -= savings;
                outside_economy_credits_ += savings;
                journal_.record(account_households(origin), account_earth(), MoneyCategory::MigrantSavings, savings);
            }
            origin.population = std::max(0.0, origin.population - count);
            economy_.set_population(origin.station_id, origin.population);
        }
        passenger_audit_.boarded[kind] += count;
        passenger_audit_.fares_cr[kind] += fares;
        passenger_audit_.wait_days[kind] += count * std::max(0.0, game_time_s_ - group.since_s) / 86400.0;
        {
            auto& route = passenger_audit_.routes[{origin.station_id, destination_id}];
            route.boarded += count;
            const auto& env = universe_.ship_environment;
            if (env.radiation) {
                const double rate = leg_dose_msv_per_day(ship_design(ship), get_station_definition(origin.station_id),
                    get_station_definition(destination_id), game_time_s_);
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
        {
            auto& route = passenger_audit_.routes[{lot.origin_id, destination.station_id}];
            route.landed += lot.count;
            route.flight_days += lot.count * std::max(0.0, game_time_s_ - lot.boarded_s) / 86400.0;
        }
        if (lot.kind == domain::PassengerKind::Migrant && colony_employer(lot.work_station_id)) {
            // A colony's recruits go down to the surface, with their savings.
            const auto economy_id = lot.work_station_id.substr(std::string("colony:").size());
            background_population_[economy_id] += lot.count;
            passenger_audit_.colony_recruits_landed += lot.count;
            const double savings = lot.count * universe_.households.migrant_savings_cr;
            outside_economy_credits_ -= savings;
            economy_account(economy_id) += savings;
            journal_.record(account_earth(), account_economy(economy_id), MoneyCategory::MigrantSavings, savings);
            continue;
        }
        if (lot.kind == domain::PassengerKind::Migrant && households_on() && is_earth_gateway(lot.origin_id)) {
            // Settlers from Earth bring their savings.
            const double savings = lot.count * universe_.households.migrant_savings_cr;
            outside_economy_credits_ -= savings;
            destination.household_credits += savings;
            journal_.record(account_earth(), account_households(destination), MoneyCategory::MigrantSavings, savings);
        }
        if (lot.kind == domain::PassengerKind::Migrant || lot.kind == domain::PassengerKind::RotationOut) {
            destination.population += lot.count;
            economy_.set_population(destination.station_id, destination.population);
        }
        if (lot.kind == domain::PassengerKind::Visitor) {
            // Visitors stay a while, then go home (a stay per arrival).
            destination.visiting.push_back({
                .kind = domain::PassengerKind::VisitorHome,
                .destination_id = lot.origin_id,
                .work_station_id = lot.origin_id,
                .count = lot.count,
                .since_s = game_time_s_ + universe_.passengers.visit_days * 86400.0,
            });
        }
        if (lot.kind == domain::PassengerKind::RotationOut) {
            // Each relief sends one worker home, at the station's expense.
            if (const auto* tour = rotation_tour(destination.station_id); tour != nullptr) {
                post_travellers(destination, domain::PassengerKind::RotationHome, tour->home_gateway,
                    destination.station_id, lot.count, true);
            }
        }
    }
    double people = 0.0;
    for (const auto& lot : ship.passengers) {
        people += lot.count;
    }
    add_event(std::format("{} lands {:.0f} passengers at {}", ship.name, people, definition.name), "arrival");
    ship.passengers.clear();
}

std::string Simulation::ship_passengers_json(const domain::ShipState& ship, const domain::ShipClassDefinition& design) const {
    // Bridge fields, sent only with passengers on: berths, who is aboard, the cabin shielding.
    if (!passengers_on()) {
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
        design.berths, aboard, lots, ship.ledger.fares, cabin_shield_g_cm2(design, ship.propellant_kg));
}

std::string Simulation::station_passengers_json(const domain::StationState& station) const {
    // Bridge fields, sent only with passengers on: who waits here for which station, visitors,
    // what the station paid for passages and has still to recover through its prices.
    if (!passengers_on()) {
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
    const auto* tour = rotation_tour(station.station_id);
    return std::format("\"passengers_waiting\":{},\"visitors_here\":{:.0f},\"passages_paid\":{:.1f},"
                       "\"passages_recovered\":{:.1f},\"passage_debt_cr\":{:.1f},\"rotation_tour_days\":{:.0f},",
        list, visiting, station.ledger.passages_paid, station.ledger.passages_recovered, station.passage_debt_cr,
        tour == nullptr ? 0.0 : tour->tour_days);
}

std::vector<Simulation::ReferenceFareRow> Simulation::compute_reference_fares() const {
    const auto& pricing = universe_.pricing;
    const auto& market = universe_.market;
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
    for (const auto& ship_class : universe_.ship_classes) {
        // Liners in every tank size (more water round cabins inside the tanks), other hulls nominal.
        if (ship_class.id != ship_class.hull_id && ship_class.berths <= 0.0) {
            continue;
        }
        std::vector<const domain::ShipModuleDefinition*> shields;
        for (const auto& module : universe_.ship_modules) {
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
                // (cost to the passenger, fare, days out, propellant per seat out and back)
                std::vector<std::tuple<double, double, double, double, double>> trips;
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

}  // namespace spacetrains::simulation
