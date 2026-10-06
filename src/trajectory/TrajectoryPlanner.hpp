#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "celestial/CelestialMechanics.hpp"
#include "domain/Types.hpp"

namespace spacetrains::trajectory {

// No planner may send a ship closer to the Sun than this (roughly a thermally
// survivable limit; Parker Solar Probe reaches 0.046 AU). Unbounded planners sent
// Lambert and ion paths straight through the Sun.
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

class ITrajectoryPlanner {
public:
    virtual ~ITrajectoryPlanner() = default;

    [[nodiscard]] virtual domain::TrajectoryPlan plan_transfer(
        const domain::StationDefinition& origin,
        const domain::StationDefinition& destination,
        const domain::ShipState& ship,
        const domain::ShipClassDefinition& ship_class,
        double current_time_s) const = 0;
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
        double current_time_s) const override;

private:
    const domain::UniverseDefinition& universe_;
    const celestial::CelestialMechanics& mechanics_;
    std::unordered_map<std::string, const domain::CelestialBodyDefinition*> bodies_by_id_;
};

}  // namespace spacetrains::trajectory
