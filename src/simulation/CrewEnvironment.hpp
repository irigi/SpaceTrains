#pragma once

// Crew radiation (docs/modules/ships_and_fleet.md): pure functions of
// the environment settings, a ship design, the distance from the Sun and the game day.
// Doses are effective dose (mSv); "hull" means inside a habitat-class pressure hull.

#include "domain/Definitions.hpp"

#include <random>

namespace spacetrains::simulation::crew_environment {

// Solar activity on a game day: 0 at solar minimum, 1 at maximum.
[[nodiscard]] double solar_activity(const domain::ShipEnvironmentDefinition& env, double day);

// Galactic cosmic rays behind a hull, mSv/day.
[[nodiscard]] double gcr_msv_per_day(const domain::ShipEnvironmentDefinition& env, double r_au, double day);

// Extra water areal density (g/cm2) a design's storm shelter gives (0 without one).
[[nodiscard]] double shelter_g_cm2(const domain::ShipClassDefinition& design);

// Water round the passenger cabins, g/cm2, full time (passengers, docs/modules/ships_and_fleet.md):
// a cabin water wall (effect cabin_shield_g_cm2) plus, for cabins inside the propellant tanks
// (effect tank_wrap_m2: the wrapped wall area), the water in them spread over that wall.
// The crew lives behind it too.
[[nodiscard]] double cabin_shield_g_cm2(const domain::ShipClassDefinition& design, double propellant_kg);

// The share of the hull's GCR dose a crew gets with that shelter (it sleeps there).
[[nodiscard]] double gcr_factor(const domain::ShipEnvironmentDefinition& env, double shelter_g_cm2);

// Dose of an August-1972-class event at 1 AU behind the hull plus extra_g_cm2 of water:
// log-linear between the hull and the reference shelter.
[[nodiscard]] double spe_reference_msv(const domain::ShipEnvironmentDefinition& env, double extra_g_cm2);

// Expected SPE magnitude per day (in 1972 events), at this solar activity.
[[nodiscard]] double spe_magnitude_per_day(const domain::ShipEnvironmentDefinition& env, double day);

// Expected dose rate of a crew in transit between r1 and r2 (AU): cosmic rays at the mean
// distance, particle events averaged as 1/(r1 r2) (the mean of 1/r^2 on a transfer between them).
// cabin_g_cm2: full-time water round the cabins (cabin_shield_g_cm2), 0 without.
[[nodiscard]] double expected_transit_msv_per_day(const domain::ShipEnvironmentDefinition& env,
    const domain::ShipClassDefinition& design, double r1_au, double r2_au, double day, double cabin_g_cm2 = 0.0);

// GCR share in transit behind the shelter and the cabin water (both 0: 1).
[[nodiscard]] double transit_gcr_factor(const domain::ShipEnvironmentDefinition& env,
    const domain::ShipClassDefinition& design, double cabin_g_cm2);

// Expected dose rate of a crew living on a station at r (AU).
[[nodiscard]] double expected_docked_msv_per_day(const domain::ShipEnvironmentDefinition& env,
    const domain::StationRadiationDefinition& station, double r_au, double day);

// Mean of r^-n over a leg on which r runs linearly in time from r1 to r2 (AU, clamped at
// 0.05), as the simulation moves a ship between stations. r1 == r2 gives r^-n exactly.
[[nodiscard]] double mean_inverse_power(double r1_au, double r2_au, double n);

// A particle event's magnitude, drawn from the power law (two uniforms: whether, how big).
[[nodiscard]] double draw_spe_magnitude(const domain::ShipEnvironmentDefinition& env, double uniform);

}  // namespace spacetrains::simulation::crew_environment
