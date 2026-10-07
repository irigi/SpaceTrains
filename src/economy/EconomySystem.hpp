#pragma once

#include <unordered_map>
#include <vector>

#include "domain/Types.hpp"

namespace spacetrains::economy {

inline constexpr const char* FUEL_ID = "fuel";

// Recipe rates are per this many inhabitants; a station's rates scale with its population.
inline constexpr double RATE_REFERENCE_POPULATION = 10'000.0;

class EconomySystem {
public:
    explicit EconomySystem(const domain::UniverseDefinition& universe);

    void step(std::vector<domain::StationState>& stations, double dt_s) const;
    // Net units per day of the station's recipes, scaled by its population, and its fuel factory.
    [[nodiscard]] std::unordered_map<std::string, double> get_station_net_rates(const domain::StationDefinition& station) const;
    [[nodiscard]] static double population_factor(const domain::StationDefinition& station);

    // Units that count against a station's storage capacity (depot fuel does not).
    [[nodiscard]] double storage_used_units(const domain::Inventory& inventory) const;

    // Fuel a station's factory makes per day (0 where there is none).
    [[nodiscard]] double fuel_factory_output(const domain::StationDefinition& station) const;
    // A station's depot holds this much fuel: the depot buffer, more at a factory.
    [[nodiscard]] double fuel_buffer_units(const domain::StationDefinition& station) const;
    // Fuel stock expected after `days` of factory output, ignoring other trade.
    [[nodiscard]] double fuel_stock_after_days(const domain::StationDefinition& station, double stock, double days) const;

    // Stock level (units) at which the local price equals the commodity's base price.
    // Consumers target a 21-day buffer, producers a 14-day buffer, non-traders a flat 20 units.
    // Depot fuel targets the station's depot buffer.
    [[nodiscard]] double get_target_stock(const domain::StationDefinition& station, const std::string& commodity_id) const;

    // Local unit price: base_price * (target/stock)^elasticity, clamped to [0.25x, 16x] base.
    [[nodiscard]] double get_price(
        const domain::StationDefinition& station,
        const std::string& commodity_id,
        double stock,
        double base_price) const;

    // Value of moving `units_into_station` units (negative: bought from the station) at a
    // station holding `stock_before`: the price integrated over the stock as it changes,
    // so a large delivery into a starving station does not all sell at the scarcity price.
    [[nodiscard]] double get_trade_value(
        const domain::StationDefinition& station,
        const std::string& commodity_id,
        double stock_before,
        double units_into_station,
        double base_price) const;

private:
    const domain::UniverseDefinition& universe_;
    std::unordered_map<std::string, std::vector<const domain::RecipeDefinition*>> recipes_by_profile_;
};

}  // namespace spacetrains::economy
