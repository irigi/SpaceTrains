#include "economy/EconomySystem.hpp"

#include <algorithm>

namespace spacetrains::economy {

EconomySystem::EconomySystem(const domain::UniverseDefinition& universe) : universe_(universe) {
    for (const auto& recipe : universe_.recipes) {
        recipes_by_profile_[recipe.profile_id].push_back(&recipe);
    }
}

void EconomySystem::step(std::vector<domain::StationState>& stations, double dt_s) const {
    const double dt_days = dt_s / 86400.0;
    for (auto& station : stations) {
        const auto station_it = std::find_if(
            universe_.stations.begin(),
            universe_.stations.end(),
            [&](const domain::StationDefinition& definition) { return definition.id == station.station_id; });
        if (station_it == universe_.stations.end()) {
            continue;
        }

        const auto recipe_it = recipes_by_profile_.find(station_it->economy_profile_id);
        if (recipe_it == recipes_by_profile_.end()) {
            continue;
        }

        for (const auto* recipe : recipe_it->second) {
            station.inventory[recipe->commodity_id] += recipe->units_per_day * dt_days;
            if (station.inventory[recipe->commodity_id] < 0.0) {
                station.inventory[recipe->commodity_id] = 0.0;
            }
        }
    }
}

std::unordered_map<std::string, double> EconomySystem::get_profile_net_rates(const std::string& profile_id) const {
    std::unordered_map<std::string, double> rates;
    const auto it = recipes_by_profile_.find(profile_id);
    if (it == recipes_by_profile_.end()) {
        return rates;
    }

    for (const auto* recipe : it->second) {
        rates[recipe->commodity_id] += recipe->units_per_day;
    }
    return rates;
}

}  // namespace spacetrains::economy
