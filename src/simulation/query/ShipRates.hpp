#pragma once

// Ship cost and environment rates, the read-only leaves of running a ship
// (docs/plans/simulation_split.md, step 4): capital cost, wear, hazard pay and the radiation
// dose of a leg, boil-off, cargo decay, how far a station or ship is from the Sun.

#include <map>
#include <string>

#include "domain/Types.hpp"
#include "simulation/World.hpp"

namespace spacetrains::simulation::query {

// What an overhaul replaces and costs (tier 2): the wearing parts' goods, the hours to
// make and fit them and their wages, and the days in the yard.
struct OverhaulBill {
    std::map<std::string, double> spares_units;
    double hours {0.0};
    double labour_cr {0.0};
    double days {0.0};
};

[[nodiscard]] double daily_capital_cost(const World& w, const domain::ShipClassDefinition& ship_class);
[[nodiscard]] double station_r_au(const World& w, const domain::StationDefinition& station, double time_s);
[[nodiscard]] double docked_dose_msv_per_day(const World& w, const std::string& station_id, double time_s);
[[nodiscard]] double cabin_shield_g_cm2(const World& w, const domain::ShipClassDefinition& design,
    double propellant_kg);
[[nodiscard]] double leg_dose_msv_per_day(const World& w, const domain::ShipClassDefinition& design,
    const domain::StationDefinition& from, const domain::StationDefinition& to, double time_s);
[[nodiscard]] double hazard_cr_per_day(const World& w, const domain::ShipClassDefinition& design,
    const domain::StationDefinition& from, const domain::StationDefinition& to, double time_s);
[[nodiscard]] bool leg_dose_allowed(const World& w, const domain::ShipClassDefinition& design,
    const domain::StationDefinition& from, const domain::StationDefinition& to, double time_s, double days);
[[nodiscard]] double cargo_decay_per_day(const World& w, const domain::ShipClassDefinition& design,
    const std::string& commodity_id);
[[nodiscard]] double drive_life_h(const World& w, const domain::ShipClassDefinition& design);
[[nodiscard]] double drive_hours(const domain::ShipClassDefinition& design,
    const domain::TrajectoryPlan& plan);
[[nodiscard]] OverhaulBill overhaul_bill(const World& w, const domain::ShipClassDefinition& design,
    double drive_share, double radiator_share);
[[nodiscard]] double wear_cr_per_drive_hour(const World& w, const domain::ShipClassDefinition& design);
[[nodiscard]] double wear_cr_per_flight_day(const World& w, const domain::ShipClassDefinition& design);
[[nodiscard]] double wear_cr_per_propellant_kg(const World& w, const domain::ShipClassDefinition& design);
[[nodiscard]] double ship_r_au(const World& w, const domain::ShipState& ship);
[[nodiscard]] double boiloff_kg_per_day(const World& w, const domain::ShipClassDefinition& design, double r_au);
[[nodiscard]] double leg_boiloff_kg_per_day(const World& w, const domain::ShipClassDefinition& design, double r1_au,
    double r2_au);
[[nodiscard]] double leg_boiloff_kg_per_day(const World& w, const domain::ShipClassDefinition& design,
    const domain::StationDefinition& from, const domain::StationDefinition& to, double time_s);
[[nodiscard]] double purchasable_propellant_kg(const World& w, const domain::ShipState& ship);

[[nodiscard]] double credit_line(const domain::ShipClassDefinition& ship_class);
[[nodiscard]] double persons_aboard(const World& w, const domain::ShipState& ship);
[[nodiscard]] double provisionable_days(const World& w, const domain::ShipState& ship,
    const domain::StationState& market);
[[nodiscard]] bool drive_life_allows(const World& w, const domain::ShipState& ship,
    const domain::ShipClassDefinition& design, const domain::TrajectoryPlan& plan);
[[nodiscard]] double provisions_mass_kg(const World& w, const domain::ShipState& ship);

}  // namespace spacetrains::simulation::query
