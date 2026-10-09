#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "celestial/CelestialMechanics.hpp"
#include "domain/Types.hpp"

namespace spacetrains::trajectory {

// No planner may send a ship closer to the Sun than this (roughly a thermally
// survivable limit; Parker Solar Probe reaches 0.046 AU). Unbounded planners sent
// Lambert and variable-Isp paths straight through the Sun.
inline constexpr double kMinPerihelionM = 0.1 * 1.495978707e11;

// The body orbiting the root (Sun) that `body_id` belongs to: a planet for itself
// and its moons. Stations around different bodies of one planetary system (Earth and
// the Moon) transfer around that planet, not around the Sun.
[[nodiscard]] std::string planet_system_primary(
    const celestial::CelestialMechanics& mechanics, const std::string& body_id);

// Rendered path of a transfer around `primary_id`: half an ellipse from the origin
// station (at departure) to the destination station (at arrival), in the heliocentric
// frame (the planet keeps moving). Kepler-timed for impulsive transfers, uniform in
// time for low-thrust spirals.
struct TimedPath {
    std::vector<math::Vec3d> path;
    std::vector<double> times_s;
};
[[nodiscard]] TimedPath planet_system_path(
    const celestial::CelestialMechanics& mechanics,
    const std::string& primary_id,
    const domain::StationDefinition& origin,
    const domain::StationDefinition& destination,
    double departure_time_s,
    double transfer_time_s,
    bool kepler_timing);

// How a ship is to be planned for. All defaults reproduce the plain physics: the
// fastest transfer the propellant already aboard allows, with no payload.
struct PlanningOptions {
    // What the owner pays for propellant and for each day of wait and transit.
    // Planners that can trade time against propellant then pick the cheapest transfer.
    double propellant_cr_per_kg {0.0};
    double time_cr_per_day {0.0};
    // Cargo and provisions aboard; they ride on the rocket equation like the hull.
    double payload_kg {0.0};
    // Propellant the ship could still buy at the origin before departure (the tank
    // capacity caps it). The planner loads what the transfer needs plus the reserve,
    // never less than is already aboard.
    double purchasable_propellant_kg {0.0};
    // Propellant kept unburned on arrival, as a fraction of the burn.
    double reserve_fraction {0.0};
    // Off when only the numbers matter (scoring candidates): no rendering path is built, and
    // the variable-Isp check integration is coarser. The chosen mission is planned again
    // with its path.
    bool include_path {true};
};

// Effective exhaust velocity of a nuclear-thermal class, from its rated full-tank Δv
// (0 for classes without one, e.g. variable-Isp ships).
[[nodiscard]] double thermal_exhaust_velocity_mps(const domain::ShipClassDefinition& ship_class);

class ITrajectoryPlanner {
public:
    virtual ~ITrajectoryPlanner() = default;

    [[nodiscard]] virtual domain::TrajectoryPlan plan_transfer(
        const domain::StationDefinition& origin,
        const domain::StationDefinition& destination,
        const domain::ShipState& ship,
        const domain::ShipClassDefinition& ship_class,
        double current_time_s,
        const PlanningOptions& costs = {}) const = 0;
};

class KeplerTrajectoryPlanner final : public ITrajectoryPlanner {
public:
    KeplerTrajectoryPlanner(
        const domain::UniverseDefinition& universe,
        const celestial::CelestialMechanics& mechanics);

    [[nodiscard]] domain::TrajectoryPlan plan_transfer(
        const domain::StationDefinition& origin,
        const domain::StationDefinition& destination,
        const domain::ShipState& ship,
        const domain::ShipClassDefinition& ship_class,
        double current_time_s,
        const PlanningOptions& costs = {}) const override;

    // One cell of the Lambert departure x transit grid: geometry only, the same for any
    // ship, payload or fuel load.
    struct LambertCell {
        double wait_s {0.0};
        double transit_s {0.0};
        double dv_departure {0.0};
        double dv_arrival {0.0};
        math::Vec3d r1 {};
        math::Vec3d r2 {};
        math::Vec3d v1 {};
    };

private:
    // The usable cells (solved, above the perihelion limit) in search order. Memoised by
    // bodies and time: one mission choice plans the same pair many times (cargo sizes,
    // return fuel), and the Lambert solves are the planner's cost. Thread-safe.
    std::shared_ptr<const std::vector<LambertCell>> lambert_grid(
        const std::string& origin_body_id, const std::string& destination_body_id,
        double current_time_s, double search_window_s, double hohmann_time_s, double mu) const;

    const domain::UniverseDefinition& universe_;
    const celestial::CelestialMechanics& mechanics_;
    std::unordered_map<std::string, const domain::CelestialBodyDefinition*> bodies_by_id_;
    mutable std::mutex lambert_mutex_;
    mutable std::unordered_map<std::string, std::shared_ptr<const std::vector<LambertCell>>> lambert_grids_;
};

}  // namespace spacetrains::trajectory
