#pragma once

// Passengers, the parts that price goods (docs/plans/simulation_split.md, step 5): fares at
// a port and who boards a ship. The rest of Passengers moves in step 9.

#include <cstddef>
#include <string>
#include <vector>

#include "domain/Types.hpp"
#include "simulation/World.hpp"
#include "simulation/systems/MissionPlanning.hpp"

namespace spacetrains::simulation::systems {

// Who boards a ship leaving `origin` for `destination` on a leg of `days`: groups in order of
// what they pay over their marginal cost (provisions and the propellant their mass burns),
// up to `berths`. A group pays the reference fare adjusted by how much faster (or slower)
// and safer this trip is than the reference one, down to what it can still afford.
struct PassengerOffer {
    std::vector<MissionChoice::PassengerPick> picks;
    double people {0.0};
    double revenue_cr {0.0};
    double marginal_cr {0.0};   // provisions of those aboard (the propellant is in the plan)
    double mass_kg {0.0};
    double time_cr_per_day {0.0};  // what a day aboard costs them (time and dose): the planner's too
};

// Fuel at a port at today's price (never below base), and a reference fare at those prices.
[[nodiscard]] double port_fuel_price(const World& w, const std::string& station_id);
[[nodiscard]] double live_fare_cr(const World& w, const std::string& origin_id, const std::string& destination_id,
    const domain::PassengerDefinition::ReferenceFare& reference);
[[nodiscard]] PassengerOffer passenger_offer(const World& w, const domain::ShipState& ship,
    const domain::ShipClassDefinition& design, const domain::StationState& origin,
    const domain::StationDefinition& destination, double days, double propellant_cr_per_kg, double berths);

}  // namespace spacetrains::simulation::systems
