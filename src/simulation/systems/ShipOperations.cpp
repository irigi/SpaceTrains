#include "simulation/systems/ShipOperations.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "simulation/SimulationInternal.hpp"
#include "simulation/systems/Market.hpp"

namespace spacetrains::simulation::systems {

using namespace detail;

double daily_crew_cost(const World& w, const domain::ShipClassDefinition& ship_class,
    const domain::StationState& market) {
    const auto& operations = w.universe_.ship_operations;
    double cost = ship_class.crew_size * operations.wage_cr_per_crew_day;
    for (const auto& [commodity_id, units_per_crew_day] : operations.life_support_units_per_crew_day) {
        cost += ship_class.crew_size * units_per_crew_day * systems::station_price(w, market, commodity_id);
    }
    return cost;
}

}  // namespace spacetrains::simulation::systems
