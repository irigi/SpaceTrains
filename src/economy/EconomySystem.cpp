#include "economy/EconomySystem.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <iostream>

namespace spacetrains::economy {

namespace {
// Machinery degrades at this fraction per day independent of production/consumption.
// Represents tools wearing out, seals failing, components requiring periodic replacement.
constexpr double MACHINERY_WEAR_PER_DAY = 0.0005;

// Produced commodities are capped at this many days of production to prevent
// infinite accumulation when ships can't keep up with distribution.
constexpr double PRODUCTION_CAP_DAYS = 90.0;

// Price elasticity: how sharply prices respond to stock deviating from target.
constexpr double PRICE_ELASTICITY = 1.3;
constexpr double PRICE_MIN_MULTIPLIER = 0.25;
// A starving station must be able to bid up to the delivered cost of the goods
// (fuel plus ship time); 4x made outer-system supply runs impossible to pay for.
constexpr double PRICE_MAX_MULTIPLIER = 16.0;
}  // namespace

EconomySystem::EconomySystem(const domain::UniverseDefinition& universe) : universe_(universe) {
    for (const auto& recipe : universe_.recipes) {
        recipes_by_profile_[recipe.profile_id].push_back(&recipe);
    }
}

void EconomySystem::step(std::vector<domain::StationState>& stations, double dt_s) const {
    const double dt_days = dt_s / 86400.0;
    const auto& fuel_supply = universe_.fuel_supply;
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

        // Production gating: production efficiency scales smoothly from 0.1 (inputs at zero)
        // to 1.0 (inputs at 7-day buffer). Never drops to 0 — stations retain 10% productivity
        // via manual/emergency operations (fallback farming, improvised repairs, etc.).
        double efficiency = 1.0;
        for (const auto* recipe : recipe_it->second) {
            if (recipe->units_per_day < 0.0) {
                const double stock = station.inventory.count(recipe->commodity_id)
                    ? station.inventory.at(recipe->commodity_id) : 0.0;
                const double buffer = std::abs(recipe->units_per_day) * 7.0;
                // Linear ramp: 0.1 at stock=0, 1.0 at stock≥buffer
                const double ratio = stock / std::max(0.001, buffer);
                const double input_efficiency = 0.1 + 0.9 * std::min(1.0, ratio);
                efficiency = std::min(efficiency, input_efficiency);
            }
        }

        // Storage cap: a full station halts production of new units (consumption continues),
        // so gluts back up the supply chain instead of accumulating without consequence.
        // Production stops at 85% so the station keeps headroom to receive imports —
        // a producer that fills itself to the brim can no longer be resupplied at all.
        const double capacity = station_it->storage_capacity_units;
        const bool storage_full = capacity > 0.0 && storage_used_units(station.inventory) >= capacity * 0.85;

        for (const auto* recipe : recipe_it->second) {
            const double rate = (recipe->units_per_day > 0.0)
                ? (storage_full ? 0.0 : recipe->units_per_day * efficiency)  // production scales with input availability
                : recipe->units_per_day;              // consumption is unaffected by efficiency
            double& stock = station.inventory[recipe->commodity_id];
            stock += rate * dt_days;

            // Inventory cap for produced commodities: prevents unbounded accumulation when
            // ships can't distribute fast enough.
            if (recipe->units_per_day > 0.0) {
                const double cap = std::max(recipe->units_per_day * PRODUCTION_CAP_DAYS,
                    recipe->commodity_id == FUEL_ID ? fuel_supply.depot_buffer_units : 0.0);
                if (stock > cap) {
                    stock = cap;
                }
            }

            if (stock < 0.0) {
                if (recipe->units_per_day < 0.0) {
                    std::cerr << std::format(
                        "[STARVED] station={} commodity={} shortfall={:.2f}u\n",
                        station.station_id,
                        recipe->commodity_id,
                        -stock);
                }
                stock = 0.0;
            }
        }

        // Propellant depot: refills toward its buffer at a bounded rate.
        if (fuel_supply.depot_buffer_units > 0.0) {
            double& fuel = station.inventory[FUEL_ID];
            fuel = fuel_stock_after_days(fuel, dt_days);
        }

        // Machinery wear: independent of recipes, represents physical deterioration.
        auto& machinery_stock = station.inventory["machinery"];
        if (machinery_stock > 0.0) {
            machinery_stock *= std::max(0.0, 1.0 - MACHINERY_WEAR_PER_DAY * dt_days);
        }
    }
}

double EconomySystem::storage_used_units(const domain::Inventory& inventory) const {
    const bool depot_fuel = universe_.fuel_supply.depot_buffer_units > 0.0;
    double stored = 0.0;
    for (const auto& [commodity_id, units] : inventory) {
        if (!(depot_fuel && commodity_id == FUEL_ID)) {
            stored += std::max(0.0, units);
        }
    }
    return stored;
}

double EconomySystem::fuel_stock_after_days(double stock, double days) const {
    const auto& fuel_supply = universe_.fuel_supply;
    if (fuel_supply.depot_buffer_units <= 0.0 || stock >= fuel_supply.depot_buffer_units) {
        return stock;
    }
    return std::min(fuel_supply.depot_buffer_units,
        std::max(0.0, stock) + fuel_supply.depot_output_units_per_day * std::max(0.0, days));
}

double EconomySystem::get_target_stock(const std::string& profile_id, const std::string& commodity_id) const {
    // A depot's fuel sells at base price when its buffer is full.
    if (commodity_id == FUEL_ID && universe_.fuel_supply.depot_buffer_units > 0.0) {
        return universe_.fuel_supply.depot_buffer_units;
    }
    const auto it = recipes_by_profile_.find(profile_id);
    double net_rate = 0.0;
    if (it != recipes_by_profile_.end()) {
        for (const auto* recipe : it->second) {
            if (recipe->commodity_id == commodity_id) {
                net_rate += recipe->units_per_day;
            }
        }
    }
    if (net_rate < 0.0) {
        return std::abs(net_rate) * 21.0;
    }
    if (net_rate > 0.0) {
        return net_rate * 14.0;
    }
    return 20.0;
}

double EconomySystem::get_price(
    const std::string& profile_id,
    const std::string& commodity_id,
    double stock,
    double base_price) const {
    const double target = get_target_stock(profile_id, commodity_id);
    const double ratio = target / std::max(stock, 0.5);
    const double multiplier = std::clamp(std::pow(ratio, PRICE_ELASTICITY), PRICE_MIN_MULTIPLIER, PRICE_MAX_MULTIPLIER);
    return base_price * multiplier;
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
