#pragma once

#include <unordered_map>
#include <vector>

#include "domain/Types.hpp"

namespace spacetrains::economy {

class EconomySystem {
public:
    explicit EconomySystem(const domain::UniverseDefinition& universe);

    void step(std::vector<domain::StationState>& stations, double dt_s) const;
    [[nodiscard]] std::unordered_map<std::string, double> get_profile_net_rates(const std::string& profile_id) const;

private:
    const domain::UniverseDefinition& universe_;
    std::unordered_map<std::string, std::vector<const domain::RecipeDefinition*>> recipes_by_profile_;
};

}  // namespace spacetrains::economy
