#pragma once

#include <unordered_map>

#include "celestial/CelestialMechanics.hpp"
#include "domain/Types.hpp"

namespace spacetrains::trajectory {

// No planner may send a ship closer to the Sun than this (roughly a thermally
// survivable limit; Parker Solar Probe reaches 0.046 AU). Unbounded planners sent
// Lambert and ion paths straight through the Sun.
inline constexpr double kMinPerihelionM = 0.1 * 1.495978707e11;

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
