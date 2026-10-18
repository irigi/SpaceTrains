#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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
        double current_time_s,
        const PlanningOptions& costs = {}) const override;

    // The station-targeting refinement of one atlas cell for one route and plan time, before
    // any fuel check: the path does not depend on the ship (kappa only decides the fuel), and
    // most plans repeat a route and time with another payload or fuel load.
    struct Departure {
        double wait_s {0.0};
        double r_scale {1.0};
        double t_scale {1.0};
        double phi {0.0};
    };
    struct Refinement {
        std::vector<std::pair<Departure, variable_isp::ShootingResult>> by_wait;  // in the order tried
    };

private:
    const domain::UniverseDefinition& universe_;
    const celestial::CelestialMechanics& mechanics_;
    const variable_isp::VariableIspAtlas& atlas_;
    variable_isp::VariableIspIntegrator integrator_;
    std::unordered_map<std::string, const domain::CelestialBodyDefinition*> bodies_by_id_;
    mutable std::mutex refinements_mutex_;
    mutable std::unordered_map<std::string, std::shared_ptr<const Refinement>> refinements_;
};

}  // namespace spacetrains::trajectory
