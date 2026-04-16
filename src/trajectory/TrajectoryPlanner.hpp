#pragma once

#include <unordered_map>

#include "celestial/CelestialMechanics.hpp"
#include "domain/Types.hpp"

namespace spacetrains::trajectory {

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
