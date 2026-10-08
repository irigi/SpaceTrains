#include "economy/EconomySystem.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdlib>
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
// Export goods wait for ships that come by once in a year or two.
constexpr double EXPORT_STOCKPILE_DAYS = 720.0;

// Price elasticity: how sharply prices respond to stock deviating from target.
constexpr double PRICE_ELASTICITY = 1.3;
constexpr double PRICE_MIN_MULTIPLIER = 0.25;
// A starving station must be able to bid up to the delivered cost of the goods
// (fuel plus ship time); 4x made outer-system supply runs impossible to pay for.
constexpr double PRICE_MAX_MULTIPLIER = 16.0;
}  // namespace

EconomySystem::EconomySystem(const domain::UniverseDefinition& universe) : universe_(universe) {
    for (const auto& station : universe_.stations) {
        auto& recipes = recipes_by_station_[station.id];
        for (const auto& recipe : universe_.recipes) {
            if (recipe.station_id.empty() ? recipe.profile_id == station.economy_profile_id : recipe.station_id == station.id) {
                recipes.push_back(&recipe);
            }
        }
    }
    for (const auto& market : universe_.export_markets) {
        export_markets_[market.station_id][market.commodity_id] = market.units_per_day;
        export_goods_.insert(market.commodity_id);
    }
    compute_cover_days();
}

// A consumer's target stock (where its price is the base price) covers three weeks of
// consumption, or, where the nearest producer is far, 1.4 x the one-way transfer time (at
// most a year): a delivery has to last until the next one can come. With three weeks
// everywhere, a hold big enough for a 200-day route flooded a Mars price down to a quarter
// of base, so nobody supplied the distant stations (v36: about 45,000 u of holds would be
// needed for steady supply, the fleet had 5,000-10,000 u). Transfer times are Hohmann
// half-orbits between the parent planets (a few days within one planet's system), a
// map-level estimate that does not depend on where the planets are.
void EconomySystem::compute_cover_days() {
    const auto body_of = [&](const std::string& id) -> const domain::CelestialBodyDefinition* {
        for (const auto& body : universe_.bodies) {
            if (body.id == id) {
                return &body;
            }
        }
        return nullptr;
    };
    const auto planet_of = [&](const std::string& body_id) {
        const auto* body = body_of(body_id);
        while (body != nullptr && !body->orbit.parent_id.empty()) {
            const auto* parent = body_of(body->orbit.parent_id);
            if (parent == nullptr || parent->orbit.parent_id.empty()) {
                break;
            }
            body = parent;
        }
        return body;
    };
    double mu_sun = 0.0;
    for (const auto& body : universe_.bodies) {
        if (body.orbit.parent_id.empty()) {
            mu_sun = body.mu_m3_s2;
        }
    }
    const auto one_way_days = [&](const domain::StationDefinition& a, const domain::StationDefinition& b) {
        const auto* pa = planet_of(a.parent_body_id);
        const auto* pb = planet_of(b.parent_body_id);
        if (pa == nullptr || pb == nullptr || mu_sun <= 0.0) {
            return 0.0;
        }
        if (pa == pb) {
            return a.parent_body_id == b.parent_body_id ? 0.5 : 5.0;
        }
        const double axis = 0.5 * (pa->orbit.semi_major_axis_m + pb->orbit.semi_major_axis_m);
        return 3.14159265358979323846 * std::sqrt(axis * axis * axis / mu_sun) / 86400.0;
    };
    for (const auto& consumer : universe_.stations) {
        for (const auto* recipe : recipes_of(consumer)) {
            if (recipe->units_per_day >= 0.0) {
                continue;
            }
            double nearest = std::numeric_limits<double>::infinity();
            for (const auto& producer : universe_.stations) {
                for (const auto* other : recipes_of(producer)) {
                    if (other->commodity_id == recipe->commodity_id && other->units_per_day > 0.0) {
                        nearest = std::min(nearest, one_way_days(producer, consumer));
                    }
                }
            }
            if (std::isfinite(nearest)) {
                cover_days_[consumer.id][recipe->commodity_id] = std::clamp(1.4 * nearest, 21.0, 365.0);
            }
        }
    }
}

double EconomySystem::cover_days(const domain::StationDefinition& station, const std::string& commodity_id) const {
    const auto station_it = cover_days_.find(station.id);
    if (station_it == cover_days_.end()) {
        return 21.0;
    }
    const auto it = station_it->second.find(commodity_id);
    return it == station_it->second.end() ? 21.0 : it->second;
}

double EconomySystem::flat_price_multiplier(const domain::StationDefinition& station, const std::string& commodity_id) const {
    if (!export_goods_.contains(commodity_id)) {
        return 0.0;
    }
    if (is_export_market(station, commodity_id)) {
        return 1.0;
    }
    const auto& recipes = recipes_of(station);
    const bool made_here = std::any_of(recipes.begin(), recipes.end(),
        [&](const domain::RecipeDefinition* recipe) { return recipe->commodity_id == commodity_id; });
    return made_here ? 0.0 : PRICE_MIN_MULTIPLIER;
}

const std::vector<const domain::RecipeDefinition*>& EconomySystem::recipes_of(const domain::StationDefinition& station) const {
    static const std::vector<const domain::RecipeDefinition*> none;
    const auto it = recipes_by_station_.find(station.id);
    return it == recipes_by_station_.end() ? none : it->second;
}

bool EconomySystem::is_export_market(const domain::StationDefinition& station, const std::string& commodity_id) const {
    const auto it = export_markets_.find(station.id);
    return it != export_markets_.end() && it->second.contains(commodity_id);
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

        // Earth's economy takes everything delivered to its export markets.
        if (const auto market_it = export_markets_.find(station.station_id); market_it != export_markets_.end()) {
            for (const auto& [commodity_id, units_per_day] : market_it->second) {
                double& stock = station.inventory[commodity_id];
                if (stock > 0.0) {
                    station.market_sold_units[commodity_id] += stock;
                    stock = 0.0;
                }
            }
        }

        const auto& recipes = recipes_of(*station_it);

        const double factor = population_factor(*station_it);

        // Production dependencies (step 14). Each consumed good's availability is its stock over
        // a 7-day buffer, in [0, 1]. Upkeep goods (life support, crew needs) penalise every output:
        // each by 1 - (1 - full_shortage_multiplier) x (1 - availability), and the penalties
        // multiply. Each output then runs at the lowest availability of its own material inputs,
        // times the upkeep multiplier. No penalty is total, so a station never stops for good.
        // (v37's value-weighted average over all inputs let a smelter with food but no ore run at
        // 90%; the minimum over all inputs before it let 0.75 u/day of medicine stop 100 u/day of
        // metals, and shortages cascaded.)
        const auto stock_of = [&](const std::string& commodity_id) {
            const auto it = station.inventory.find(commodity_id);
            return it == station.inventory.end() ? 0.0 : it->second;
        };
        std::unordered_map<std::string, double> availability;
        double upkeep = 1.0;
        for (const auto* recipe : recipes) {
            if (recipe->units_per_day >= 0.0) {
                continue;
            }
            const double buffer = std::abs(recipe->units_per_day * factor) * 7.0;
            const double available = std::clamp(stock_of(recipe->commodity_id) / std::max(0.001, buffer), 0.0, 1.0);
            availability[recipe->commodity_id] = available;
            if (recipe->role == domain::RecipeRole::Upkeep) {
                const auto penalty = universe_.upkeep_penalties.find(recipe->commodity_id);
                if (penalty != universe_.upkeep_penalties.end()) {
                    upkeep *= 1.0 - (1.0 - penalty->second) * (1.0 - available);
                }
            }
        }

        // Storage cap: a full station halts production of new units (consumption continues),
        // so gluts back up the supply chain instead of accumulating without consequence.
        // Production stops at 85% so the station keeps headroom to receive imports —
        // a producer that fills itself to the brim can no longer be resupplied at all.
        const double capacity = station_it->storage_capacity_units;
        const bool storage_full = capacity > 0.0 && storage_used_units(station.inventory) >= capacity * 0.85;
        const double ceiling = storage_full ? 0.0 : upkeep;

        // Each output's run rate, and the input that limits it (for the unmet-demand count).
        struct OutputRun {
            double run {1.0};
            std::string limited_by;
            double limit {1.0};
        };
        std::unordered_map<std::string, OutputRun> runs;
        for (const auto* recipe : recipes) {
            if (recipe->units_per_day > 0.0) {
                runs.try_emplace(recipe->commodity_id);
            }
        }
        for (const auto* recipe : recipes) {
            if (recipe->role != domain::RecipeRole::Input) {
                continue;
            }
            for (const auto& output : recipe->feeds) {
                auto& run = runs[output];
                const double available = availability[recipe->commodity_id];
                if (available < run.limit) {
                    run.limit = available;
                    run.limited_by = recipe->commodity_id;
                }
            }
        }
        station.upkeep_multiplier = upkeep;
        station.output_factor.clear();
        for (auto& [commodity_id, run] : runs) {
            run.run = ceiling * run.limit;
            station.output_factor[commodity_id] = run.run;
        }

        for (const auto* recipe : recipes) {
            const double units_per_day = recipe->units_per_day * factor;
            double rate = units_per_day;
            // Lost consumption that is a shortage, not a choice: the part of an input's use its own
            // stock-out took from the outputs it limits.
            double input_shortfall_per_day = 0.0;
            if (units_per_day > 0.0) {
                rate = units_per_day * runs[recipe->commodity_id].run;
            } else if (recipe->role == domain::RecipeRole::Input && !recipe->feeds.empty()) {
                // A plant that is not running uses no feedstock.
                double mean_run = 0.0;
                for (const auto& output : recipe->feeds) {
                    const auto& run = runs[output];
                    mean_run += run.run;
                    if (run.limited_by == recipe->commodity_id) {
                        input_shortfall_per_day += ceiling * (1.0 - run.limit);
                    }
                }
                const double feeds = static_cast<double>(recipe->feeds.size());
                rate = units_per_day * mean_run / feeds;
                input_shortfall_per_day *= -units_per_day / feeds;
            }
            double& stock = station.inventory[recipe->commodity_id];
            stock += rate * dt_days;

            // Inventory cap for produced commodities: prevents unbounded accumulation when
            // ships can't distribute fast enough.
            if (recipe->units_per_day > 0.0) {
                const double cap = production_cap_units(*station_it, recipe->commodity_id, units_per_day);
                if (stock > cap) {
                    stock = cap;
                }
            }

            if (units_per_day < 0.0) {
                station.demand_units[recipe->commodity_id] -= units_per_day * dt_days;
                if (input_shortfall_per_day > 0.0) {
                    station.unmet_units[recipe->commodity_id] += input_shortfall_per_day * dt_days;
                }
            }
            if (stock < 0.0) {
                if (recipe->units_per_day < 0.0) {
                    station.unmet_units[recipe->commodity_id] -= stock;
                    // Debug aid, on with SPACETRAINS_TRACE_STARVED=1 (the audit's
                    // "Unmet Demand" table sums the same shortfalls).
                    static const bool trace_starved = std::getenv("SPACETRAINS_TRACE_STARVED") != nullptr;
                    if (trace_starved) std::cerr << std::format(
                        "[STARVED] station={} commodity={} shortfall={:.2f}u\n",
                        station.station_id,
                        recipe->commodity_id,
                        -stock);
                }
                stock = 0.0;
            }
        }

        // Fuel factory: fills its depot toward the buffer at its output rate.
        if (fuel_factory_output(*station_it) > 0.0) {
            double& fuel = station.inventory[FUEL_ID];
            fuel = fuel_stock_after_days(*station_it, fuel, dt_days);
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

double EconomySystem::fuel_factory_output(const domain::StationDefinition& station) const {
    const auto& outputs = universe_.fuel_supply.factory_output_units_per_day;
    const auto it = outputs.find(station.id);
    return it == outputs.end() ? 0.0 : it->second;
}

double EconomySystem::fuel_buffer_units(const domain::StationDefinition& station) const {
    const auto& fuel_supply = universe_.fuel_supply;
    if (fuel_supply.depot_buffer_units <= 0.0) {
        return 0.0;
    }
    return std::max(fuel_supply.depot_buffer_units, fuel_factory_output(station) * fuel_supply.factory_buffer_days);
}

double EconomySystem::fuel_stock_after_days(const domain::StationDefinition& station, double stock, double days) const {
    const double buffer = fuel_buffer_units(station);
    const double output = fuel_factory_output(station);
    if (output <= 0.0 || stock >= buffer) {
        return stock;
    }
    return std::min(buffer, std::max(0.0, stock) + output * std::max(0.0, days));
}

double EconomySystem::commodity_base_price(const std::string& commodity_id) const {
    for (const auto& commodity : universe_.commodities) {
        if (commodity.id == commodity_id) {
            return commodity.base_price;
        }
    }
    return 1.0;
}

double EconomySystem::get_target_stock(const domain::StationDefinition& station, const std::string& commodity_id) const {
    // A depot's fuel sells at base price when its buffer is full.
    if (commodity_id == FUEL_ID && universe_.fuel_supply.depot_buffer_units > 0.0) {
        return fuel_buffer_units(station);
    }
    const auto rates = get_station_net_rates(station);
    const double net_rate = rates.contains(commodity_id) ? rates.at(commodity_id) : 0.0;
    if (net_rate < 0.0) {
        return std::abs(net_rate) * cover_days(station, commodity_id);
    }
    if (net_rate > 0.0) {
        return net_rate * 14.0;
    }
    return 20.0;
}

double EconomySystem::curve_centre(
    const domain::StationDefinition& station, const std::string& commodity_id, double base_price) const {
    const auto station_it = reference_prices_.find(station.id);
    if (station_it == reference_prices_.end()) {
        return base_price;
    }
    const auto it = station_it->second.find(commodity_id);
    return it == station_it->second.end() ? base_price : it->second;
}

double EconomySystem::reference_price(const domain::StationDefinition& station, const std::string& commodity_id) const {
    return curve_centre(station, commodity_id, commodity_base_price(commodity_id));
}

void EconomySystem::set_reference_prices(std::unordered_map<std::string, std::unordered_map<std::string, double>> prices) {
    reference_prices_ = std::move(prices);
}

bool EconomySystem::is_upkeep(const domain::StationDefinition& station, const std::string& commodity_id) const {
    const auto& recipes = recipes_of(station);
    return std::any_of(recipes.begin(), recipes.end(), [&](const domain::RecipeDefinition* recipe) {
        return recipe->commodity_id == commodity_id && recipe->role == domain::RecipeRole::Upkeep;
    });
}

double EconomySystem::price_cap() const {
    return PRICE_MAX_MULTIPLIER;
}

double EconomySystem::stock_at_multiplier(
    const domain::StationDefinition& station, const std::string& commodity_id, double multiplier) const {
    return get_target_stock(station, commodity_id) * std::pow(std::max(multiplier, 1.0e-9), -1.0 / PRICE_ELASTICITY);
}

double EconomySystem::get_price(
    const domain::StationDefinition& station,
    const std::string& commodity_id,
    double stock,
    double base_price) const {
    if (const double flat = flat_price_multiplier(station, commodity_id); flat > 0.0) {
        return base_price * flat;
    }
    const double target = get_target_stock(station, commodity_id);
    const double ratio = target / std::max(stock, 0.5);
    const double multiplier = std::clamp(std::pow(ratio, PRICE_ELASTICITY), PRICE_MIN_MULTIPLIER, PRICE_MAX_MULTIPLIER);
    return curve_centre(station, commodity_id, base_price) * multiplier;
}

double EconomySystem::production_cap_units(
    const domain::StationDefinition& station, const std::string& commodity_id, double units_per_day) const {
    const double days = export_goods_.contains(commodity_id) ? EXPORT_STOCKPILE_DAYS : PRODUCTION_CAP_DAYS;
    return std::max(units_per_day * days, commodity_id == FUEL_ID ? fuel_buffer_units(station) : 0.0);
}

double EconomySystem::get_trade_value(
    const domain::StationDefinition& station,
    const std::string& commodity_id,
    double stock_before,
    double units_into_station,
    double base_price) const {
    if (const double flat = flat_price_multiplier(station, commodity_id); flat > 0.0) {
        return base_price * flat * std::abs(units_into_station);
    }
    // Exact integral of get_price over the traded stock range. The multiplier is
    // (target / max(s, 0.5))^e clamped to [min, max]: constant below s = 0.5, at the
    // upper clamp up to s_hi, (target/s)^e up to s_lo, at the lower clamp beyond.
    const double target = get_target_stock(station, commodity_id);
    const double e = PRICE_ELASTICITY;
    const auto clamp_multiplier = [&](double stock) {
        return std::clamp(std::pow(target / std::max(stock, 0.5), e), PRICE_MIN_MULTIPLIER, PRICE_MAX_MULTIPLIER);
    };
    const double s_hi = std::max(0.5, target * std::pow(PRICE_MAX_MULTIPLIER, -1.0 / e));
    const double s_lo = std::max(0.5, target * std::pow(PRICE_MIN_MULTIPLIER, -1.0 / e));
    const auto power_integral = [&](double a, double b) {  // ∫ (target/s)^e ds over [a, b]
        return std::pow(target, e) * (std::pow(b, 1.0 - e) - std::pow(a, 1.0 - e)) / (1.0 - e);
    };
    // ∫ multiplier ds over [0, x].
    const auto cumulative = [&](double x) {
        x = std::max(0.0, x);
        double total = clamp_multiplier(0.0) * std::min(x, 0.5);
        if (x > 0.5) {
            total += PRICE_MAX_MULTIPLIER * (std::min(x, s_hi) - 0.5);
        }
        if (x > s_hi) {
            total += power_integral(s_hi, std::min(x, s_lo));
        }
        if (x > s_lo) {
            total += PRICE_MIN_MULTIPLIER * (x - s_lo);
        }
        return total;
    };
    const double a = stock_before;
    const double b = stock_before + units_into_station;
    return curve_centre(station, commodity_id, base_price) * std::abs(cumulative(b) - cumulative(a));
}

double EconomySystem::population_factor(const domain::StationDefinition& station) {
    return static_cast<double>(station.population) / RATE_REFERENCE_POPULATION;
}

std::unordered_map<std::string, double> EconomySystem::get_station_net_rates(const domain::StationDefinition& station) const {
    std::unordered_map<std::string, double> rates;
    if (const double output = fuel_factory_output(station); output > 0.0) {
        rates[FUEL_ID] += output;
    }
    const double factor = population_factor(station);
    for (const auto* recipe : recipes_of(station)) {
        rates[recipe->commodity_id] += recipe->units_per_day * factor;
    }
    if (const auto it = export_markets_.find(station.id); it != export_markets_.end()) {
        for (const auto& [commodity_id, units_per_day] : it->second) {
            rates[commodity_id] -= units_per_day;
        }
    }
    return rates;
}

}  // namespace spacetrains::economy
