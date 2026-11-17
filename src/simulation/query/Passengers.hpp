#pragma once

// Passengers, the read-only leaves (docs/plans/simulation_split.md, step 4): which kinds of
// travel are on, the mass and value of time of a passenger, reference fares, who waits where.

#include <string>

#include "domain/Types.hpp"
#include "simulation/World.hpp"

namespace spacetrains::simulation::query {

[[nodiscard]] inline bool passengers_on(const World& w) { return w.data.universe_.passengers.enabled != 0.0; }
[[nodiscard]] inline bool migration_by_ship(const World& w) {
    return passengers_on(w) && w.data.universe_.passengers.migration != 0.0;
}
[[nodiscard]] inline bool rotation_on(const World& w) {
    return passengers_on(w) && w.data.universe_.passengers.rotation != 0.0;
}
[[nodiscard]] inline bool visits_on(const World& w) {
    return passengers_on(w) && w.data.universe_.passengers.discretionary != 0.0;
}
[[nodiscard]] bool employer_pays(const World& w, domain::PassengerKind kind);
[[nodiscard]] double passenger_kg(const World& w, double days);
[[nodiscard]] double value_of_time_cr_per_day(const World& w, domain::PassengerKind kind);
[[nodiscard]] double passenger_nonfare_cr(const World& w, double value_of_time, double days, double dose_msv_per_day);
[[nodiscard]] const domain::PassengerDefinition::ReferenceFare* reference_fare(const World& w,
    const std::string& origin_id, const std::string& destination_id);
[[nodiscard]] bool passenger_leg_waiting(const World& w, const domain::StationState& origin,
    const std::string& destination_id);

}  // namespace spacetrains::simulation::query
