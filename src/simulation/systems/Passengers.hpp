#pragma once

// Passengers (docs/plans/simulation_split.md, steps 5 and 9; docs/modules/ships_and_fleet.md): people
// waiting at stations, the fares ships charge them, boarding and landing, rotation tours, the
// demand fleet investment sees, and the reference fares the market prices against.

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

// Travellers by willingness band (what post_travellers would post over window_days of the
// steady flows, plus those waiting spread over a route commitment), and the fares that
// berths filled from them would take.
struct PassengerBand {
    int band {0};
    double people {0.0};
    double max_cost_cr {0.0};
    double value_of_time_cr_per_day {0.0};
    bool queued {false};  // already waiting (not a flow still to come)
    std::string home_pays {};   // a station whose residents pay out of their travel budget (households on)
};
struct PassengerRevenue {
    double people {0.0};
    double fares_cr {0.0};
};

// For every station pair, the cheapest cabin carrier's cost per berth one way (carrier_margin x
// (its days out + return_share x the way back, at its daily cost, plus propellant) over berths x
// fill_ref, plus the passenger's provisions); median over departure dates. Written by
// `spacetrains_headless --write-reference-fares`.
struct ReferenceFareRow {
    std::string origin_id;
    std::string destination_id;
    double fare_cr {0.0};
    double days {0.0};
    std::string class_id;
    double fuel_out_kg {0.0};
    double fuel_back_kg {0.0};
};

// Fuel at a port at today's price (never below base), and a reference fare at those prices.
[[nodiscard]] double port_fuel_price(const World& w, const std::string& station_id);
[[nodiscard]] double live_fare_cr(const World& w, const std::string& origin_id, const std::string& destination_id,
    const domain::PassengerDefinition::ReferenceFare& reference);
[[nodiscard]] PassengerOffer passenger_offer(const World& w, const domain::ShipState& ship,
    const domain::ShipClassDefinition& design, const domain::StationState& origin,
    const domain::StationDefinition& destination, double days, double propellant_cr_per_kg, double berths);

[[nodiscard]] const char* passenger_kind_name(domain::PassengerKind kind);
// Share of a station's would-be settlers whose job repays a passage of this many years' wages.
[[nodiscard]] double settler_share(const World& w, double payback_years);
void post_travellers(World& w, domain::StationState& origin, domain::PassengerKind kind,
    const std::string& destination_id, const std::string& work_station_id, double count, bool fixed = false);
// The Earth gateway with the cheapest reference trip to or from a station.
[[nodiscard]] const std::string& gateway_for(const World& w, const std::string& station_id, bool to_station);
// People already waiting for, or flying to, a station of this kind.
[[nodiscard]] double travellers_pending(const World& w, const std::string& station_id, domain::PassengerKind kind);
[[nodiscard]] double colony_recruits_per_day(const World& w, const domain::BackgroundEconomyDefinition& economy);
void compute_rotation_tours(World& w);
[[nodiscard]] const RotationTour* rotation_tour(const World& w, const std::string& station_id);
[[nodiscard]] double rotation_positions(const World& w, const domain::StationState& station);
// People per day who would take the trip origin -> destination at about its reference cost:
// the demand a new carrier on that pair competes for (settlers posted, emigrants, and the
// queue spread over the route commitment).
[[nodiscard]] double passenger_flow_per_day(const World& w, const std::string& origin_id,
    const std::string& destination_id);
[[nodiscard]] double carried_passengers_per_day(const World& w, const std::string& origin_id,
    const std::string& destination_id);
[[nodiscard]] PassengerRevenue expected_passenger_revenue(const World& w, const std::string& origin_id,
    const std::string& destination_id, double days, double window_days, double berths, double open_share);
// The fare a carrier can expect per passenger on that pair on a leg of `days`, filling
// `berths` from `demand_per_trip` people: the reference fare adjusted for its days, or the
// fare that clears the berths when demand exceeds them.
[[nodiscard]] double expected_fare_cr(const World& w, const std::string& origin_id, const std::string& destination_id,
    double days, double demand_per_trip, double berths);
// Passengers' daily step: demand (migration instead of formula growth), waiting groups give up.
void step_passengers(World& w, double dt_s);
void board_passengers(World& w, domain::ShipState& ship, domain::StationState& origin,
    const std::string& destination_id, const std::vector<MissionChoice::PassengerPick>& picks, double days);
void land_passengers(World& w, domain::ShipState& ship, domain::StationState& destination);
[[nodiscard]] std::string ship_passengers_json(const World& w, const domain::ShipState& ship,
    const domain::ShipClassDefinition& design);
[[nodiscard]] std::string station_passengers_json(const World& w, const domain::StationState& station);
[[nodiscard]] std::vector<ReferenceFareRow> compute_reference_fares(const World& w);

}  // namespace spacetrains::simulation::systems
