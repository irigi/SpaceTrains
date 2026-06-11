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

    // Stock level (units) at which the local price equals the commodity's base price.
    // Consumers target a 21-day buffer, producers a 14-day buffer, non-traders a flat 20 units.
    [[nodiscard]] double get_target_stock(const std::string& profile_id, const std::string& commodity_id) const;

    // Local unit price: base_price * (target/stock)^elasticity, clamped to [0.25x, 4x] base.
    [[nodiscard]] double get_price(
        const std::string& profile_id,
        const std::string& commodity_id,
        double stock,
        double base_price) const;

private:
    const domain::UniverseDefinition& universe_;
    std::unordered_map<std::string, std::vector<const domain::RecipeDefinition*>> recipes_by_profile_;
};

}  // namespace spacetrains::economy
