#include "simulation/query/Passengers.hpp"

#include <algorithm>
#include <cmath>

#include "simulation/SimulationInternal.hpp"
#include "simulation/query/Labour.hpp"

namespace spacetrains::simulation::query {

using namespace detail;

namespace {
bool pays_from_employer(domain::PassengerKind kind) {
    return kind == domain::PassengerKind::RotationOut || kind == domain::PassengerKind::RotationHome;
}
}  // namespace

bool employer_pays(const World& w, domain::PassengerKind kind) {
    // With the labour market on, settlers pay their own passage out of what the job gains them.
    return pays_from_employer(kind)
        || (kind == domain::PassengerKind::Migrant && w.universe_.passengers.settlers_sponsored != 0.0 && !query::labour_on(w));
}

double passenger_kg(const World& w, double days) {
    const auto& pax = w.universe_.passengers;
    return pax.body_luggage_kg + pax.consumables_kg_per_day * std::max(0.0, days);
}

double value_of_time_cr_per_day(const World& w, domain::PassengerKind kind) {
    const auto& pax = w.universe_.passengers;
    if (pays_from_employer(kind)) {
        return w.universe_.labour.wage_cr_per_day(pax.rotation_grade);
    }
    return w.universe_.labour.reference_wage_cr_per_day * pax.resident_wage_multiple;
}

double passenger_nonfare_cr(const World& w, double value_of_time, double days, double dose_msv_per_day) {
    const auto& env = w.universe_.ship_environment;
    const double dose_cost = env.radiation ? dose_msv_per_day * days * env.hazard_cr_per_msv : 0.0;
    return value_of_time * days * (1.0 + w.universe_.passengers.recovery_days_per_flight_day) + dose_cost;
}

const domain::PassengerDefinition::ReferenceFare* reference_fare(const World& w, const std::string& origin_id,
    const std::string& destination_id) {
    const auto& fares = w.universe_.passengers.reference_fares;
    const auto from = fares.find(origin_id);
    if (from == fares.end()) {
        return nullptr;
    }
    const auto to = from->second.find(destination_id);
    return to == from->second.end() ? nullptr : &to->second;
}

bool passenger_leg_waiting(const World& w, const domain::StationState& origin, const std::string& destination_id) {
    // A station is worth a passenger leg once enough people wait for it (min_leg_passengers;
    // at 1, any whole person); smaller groups still ride on legs flown for cargo.
    const double needed = w.universe_.passengers.min_leg_passengers;
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

}  // namespace spacetrains::simulation::query
