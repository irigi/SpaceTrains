#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "domain/Types.hpp"

namespace spacetrains::economy {

inline constexpr const char* FUEL_ID = "fuel";

// The lowest price a station pays, as a multiple of its curve centre (or its going rate, step 29).
inline constexpr double PRICE_FLOOR_MULTIPLIER = 0.25;
// Price elasticity: how sharply prices respond to stock deviating from target.
inline constexpr double PRICE_ELASTICITY = 1.3;

// Recipe rates are per this many inhabitants; a station's rates scale with its population.
inline constexpr double RATE_REFERENCE_POPULATION = 10'000.0;

class EconomySystem {
public:
    explicit EconomySystem(const domain::UniverseDefinition& universe);

    void step(std::vector<domain::StationState>& stations, double dt_s) const;
    // Net units per day of the station's recipes, scaled by its population, and its fuel factory.
    // Kept per station and rebuilt when its population is set (the only input that changes).
    [[nodiscard]] const std::unordered_map<std::string, double>& get_station_net_rates(const domain::StationDefinition& station) const;
    // Rates scale with the station's residents: its current population (set_population, step
    // 27) over RATE_REFERENCE_POPULATION, the seeded one until set.
    [[nodiscard]] double population_factor(const domain::StationDefinition& station) const;
    void set_population(const std::string& station_id, double population);
    // Labour market (economy overhaul): plants run with the workers employed, so outputs and the
    // inputs that feed them scale with employed workers / workers (at most 1; 1 until set).
    void set_employment(const std::string& station_id, double employed_share);
    [[nodiscard]] double employment(const std::string& station_id) const;

    // Units that count against a station's storage capacity (depot fuel does not, nor what a
    // background economy keeps stocked at its gateway).
    [[nodiscard]] double storage_used_units(const domain::Inventory& inventory) const;
    [[nodiscard]] double storage_used_units(const domain::StationDefinition& station, const domain::Inventory& inventory) const;

    // Background economies (docs/plans/economy_overhaul.md): what one sells or buys at this
    // gateway (nullptr: none). A surface market that sells in any amount keeps the station's
    // stock at surface_depth_units, priced at its ask.
    [[nodiscard]] const domain::SurfaceMarketDefinition* surface_market(
        const domain::StationDefinition& station, const std::string& commodity_id) const;
    [[nodiscard]] const std::vector<const domain::SurfaceMarketDefinition*>& surface_markets_at(const std::string& station_id) const;
    // The surface keeps this good stocked here in any amount (Earth).
    [[nodiscard]] bool surface_supplied(const domain::StationDefinition& station, const std::string& commodity_id) const;
    // What the surface pays for it here (0: it does not buy).
    [[nodiscard]] double surface_bid(const domain::StationDefinition& station, const std::string& commodity_id) const;
    [[nodiscard]] double surface_depth_units() const { return surface_depth_units_; }

    // Fuel a station's factory makes per day (0 where there is none).
    [[nodiscard]] double fuel_factory_output(const domain::StationDefinition& station) const;
    // A fuel factory's depot holds this much fuel: factory_buffer_days of output (0 elsewhere).
    [[nodiscard]] double fuel_buffer_units(const domain::StationDefinition& station) const;
    // A producer stops adding to its stock of a good it makes at this many units.
    [[nodiscard]] double production_cap_units(const domain::StationDefinition& station, const std::string& commodity_id,
        double units_per_day) const;
    // Fuel stock expected after `days` of factory output, ignoring other trade.
    [[nodiscard]] double fuel_stock_after_days(const domain::StationDefinition& station, double stock, double days) const;

    // Earth's economy buys this good here at its base price, in any amount.
    [[nodiscard]] bool is_export_market(const domain::StationDefinition& station, const std::string& commodity_id) const;

    // Stock level (units) at which the curve's price equals its centre: producers a 14-day
    // buffer, non-traders a flat 20 units, a fuel factory its buffer. Consumers and depot fuel buy
    // under station orders (step 29); for them it is a nominal three weeks.
    [[nodiscard]] double get_target_stock(const domain::StationDefinition& station, const std::string& commodity_id) const;

    // Local unit price: base_price * (target/stock)^elasticity, clamped to [0.25x, 16x] base;
    // the base price at an export market, and 0.25x base for an export good where it is
    // neither made nor exported (nobody there needs platinum).
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

    // The centre of a station's price curve for a good: the landed cost (base price plus the
    // cost of carrying it from the nearest producer, set by set_reference_prices) at a
    // consumer, the base price elsewhere.
    [[nodiscard]] double reference_price(const domain::StationDefinition& station, const std::string& commodity_id) const;
    // Map-level one-way transfer estimate between two stations, in days: a Hohmann half-orbit
    // between their parent planets, a few days within one planet's system.
    [[nodiscard]] double transfer_days(const domain::StationDefinition& a, const domain::StationDefinition& b) const;
    // The station consumes this good as upkeep (life support and crew needs).
    [[nodiscard]] bool is_upkeep(const domain::StationDefinition& station, const std::string& commodity_id) const;
    // The highest multiple of its centre a station's curve for a good reaches (at an empty
    // stock): price_cap. Station orders cap their bids themselves (step 29).
    [[nodiscard]] double price_cap(const domain::StationDefinition& station, const std::string& commodity_id) const;
    void set_reference_prices(std::unordered_map<std::string, std::unordered_map<std::string, double>> prices);

    // The profile's recipes and the station's own.
    [[nodiscard]] const std::vector<const domain::RecipeDefinition*>& recipes_of(const domain::StationDefinition& station) const;

private:
    // The multiple of a recipe's rate the events running at a station allow (step 28).
    [[nodiscard]] double event_factor(const domain::StationState& station, const domain::RecipeDefinition& recipe) const;
    // reference_price for a caller that passes the base price.
    [[nodiscard]] double curve_centre(
        const domain::StationDefinition& station, const std::string& commodity_id, double base_price) const;
    [[nodiscard]] double commodity_base_price(const std::string& commodity_id) const;
    // A price that does not move with the stock: the base price at an export market (a surface
    // economy's bid where it only buys), the floor price for an export good anywhere that neither
    // makes nor exports it (0 otherwise).
    [[nodiscard]] double flat_price(const domain::StationDefinition& station, const std::string& commodity_id, double base_price) const;
    // The lowest and highest multiple of the curve centre (the surface ask and bid bound it).
    [[nodiscard]] double price_floor_multiplier(const domain::StationDefinition& station, const std::string& commodity_id) const;
    [[nodiscard]] std::unordered_map<std::string, double> compute_net_rates(const domain::StationDefinition& station) const;

    const domain::UniverseDefinition& universe_;
    // The profile's recipes and the station's own, by station id.
    std::unordered_map<std::string, std::vector<const domain::RecipeDefinition*>> recipes_by_station_;
    // Export goods by station id.
    std::unordered_map<std::string, std::unordered_map<std::string, double>> export_markets_;
    std::unordered_set<std::string> export_goods_;
    // Surface markets by station id (economy overhaul), and the flat bids of those that only buy.
    std::unordered_map<std::string, std::vector<const domain::SurfaceMarketDefinition*>> surface_by_station_;
    std::unordered_map<std::string, std::unordered_map<std::string, double>> export_bids_;
    bool legacy_export_markets_ {false};
    std::unordered_set<std::string> science_stations_;   // stations whose budget is science (station_incomes kind science_station)
    double surface_depth_units_ {0.0};
    // Landed-cost reference prices by station id and good (empty: base prices).
    std::unordered_map<std::string, std::unordered_map<std::string, double>> reference_prices_;
    // Events by id (step 28).
    std::unordered_map<std::string, const domain::EventDefinition*> events_by_id_;
    // Current populations by station id (step 27).
    std::unordered_map<std::string, double> populations_;
    // Employed share of the workers by station id (economy overhaul; 1 if absent).
    std::unordered_map<std::string, double> employment_;
    // get_station_net_rates by station id, rebuilt by set_population.
    std::unordered_map<std::string, std::unordered_map<std::string, double>> net_rates_;
};

}  // namespace spacetrains::economy
