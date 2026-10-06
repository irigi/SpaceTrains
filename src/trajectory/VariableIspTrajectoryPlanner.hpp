#pragma once

#include <unordered_map>

#include "celestial/CelestialMechanics.hpp"
#include "domain/Types.hpp"
#include "trajectory/TrajectoryPlanner.hpp"
#include "variable_isp/VariableIsp.hpp"

namespace spacetrains::trajectory {

class VariableIspTrajectoryPlanner final : public ITrajectoryPlanner {
public:
    VariableIspTrajectoryPlanner(
        const domain::UniverseDefinition& universe,
        const celestial::CelestialMechanics& mechanics,
        const variable_isp::VariableIspAtlas& atlas);

    [[nodiscard]] domain::TrajectoryPlan plan_transfer(
        const domain::StationDefinition& origin,
        const domain::StationDefinition& destination,
        const domain::ShipState& ship,
        const domain::ShipClassDefinition& ship_class,
        double current_time_s) const override;

private:
    const domain::UniverseDefinition& universe_;
    const celestial::CelestialMechanics& mechanics_;
    const variable_isp::VariableIspAtlas& atlas_;
    variable_isp::VariableIspIntegrator integrator_;
    std::unordered_map<std::string, const domain::CelestialBodyDefinition*> bodies_by_id_;
};

}  // namespace spacetrains::trajectory
