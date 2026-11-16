#pragma once

// Prices, orders and emergencies (docs/plans/simulation_split.md, step 5; docs/modules/economy.md):
// station prices along their curves, purchase orders and their bids, landed-cost reference
// prices, emergency deliveries.

#include <cstddef>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "domain/Types.hpp"
#include "simulation/World.hpp"

namespace spacetrains::simulation::systems {

// Landed-cost reference prices (step 15): for each consumer and good it buys, the base price
// plus carrier_margin x the cheapest cost per unit of carrying it from one of its two nearest
// producers, in a full hold of a standard-tank hull of at least 150 u, out loaded and back
// empty (median over departure dates in a year). Written to data by
// `spacetrains_headless --write-reference-prices`.
struct ReferencePrice {
    std::string station_id;
    std::string commodity_id;
    double reference_price {0.0};
    double base_price {0.0};
    double transport_per_unit {0.0};
    std::string producer_id;
    std::string class_id;
    double round_trip_days {0.0};
};

// A sale's value and the part of it the station's faction pays (an emergency premium).
struct SaleSplit {
    double total {0.0};
    double faction {0.0};
    double emergency_units {0.0};  // units sold at the emergency floor
    double order_units {0.0};      // units that fill an open order (step 29), and what they earn
    double order_value {0.0};
};

// An order's levels before contracts: use, safety stock, order-up-to level, the most with the extra.
struct OrderLevels {
    double use_per_day {0.0};
    double safety_units {0.0};
    double order_up_to_units {0.0};
    double max_units {0.0};
    double extra_days {0.0};
};

// What a delivery landing at `arrival_s` is paid, piecewise flat: the open quantity N at the
// order price, then the extra up to S_max at a discount on the going rate, the rest at the
// floor. An open emergency raises the order's units to its floor (the faction pays the part
// above the order price).
struct OrderQuote {
    double open_units {0.0};
    double extra_units {0.0};
    double price {0.0};
    double extra_price {0.0};
    double floor_price {0.0};
    // Oxygen and water: the station's own cap, and an open emergency's price for its units.
    double station_cap {std::numeric_limits<double>::infinity()};
    double emergency_price {0.0};
    double emergency_units {0.0};
};

// The order's bid for a delivery that finds `stock_at_arrival` (every earlier contract
// counted): the curve's shape around the reference, on the order-up-to level, up to the
// lower of today's cap and the ceiling V (its cap: `cap`).
struct OrderBid {
    double price {0.0};
    double cap {0.0};
};

// What a station starts with (step 18): its data inventory, raised to its target stock for
// every good it consumes, within 85% of its storage.
[[nodiscard]] domain::Inventory starting_inventory(const World& w, const domain::StationDefinition& station);
[[nodiscard]] std::vector<ReferencePrice> compute_reference_prices(const World& w);
// Tests and debugging: overwrite one good's stock at a station.
void set_station_stock(World& w, const std::string& station_id, const std::string& commodity_id, double units);
[[nodiscard]] std::string faction_name(const World& w, const std::string& faction_id);
[[nodiscard]] double station_price(const World& w, const domain::StationState& state, const std::string& commodity_id);
[[nodiscard]] double trade_value(const World& w, const domain::StationState& state, const std::string& commodity_id,
    double units_into_station);
[[nodiscard]] double sale_value_on_arrival(const World& w, const domain::StationState& state,
    const std::string& commodity_id, double units, double days_ahead, const std::string& seller_ship_id);
[[nodiscard]] SaleSplit sale_split_on_arrival(const World& w, const domain::StationState& state,
    const std::string& commodity_id, double units, double days_ahead, const std::string& seller_ship_id);
// Selling `units` into a station whose stock is `stock`: along its curve, raised to an
// open emergency's floor price for the units that land below its emergency cover.
[[nodiscard]] SaleSplit sale_split(const World& w, const domain::StationDefinition& station,
    const std::string& commodity_id, double stock, double units);
[[nodiscard]] const domain::Emergency* find_emergency(const World& w, const std::string& station_id,
    const std::string& commodity_id);
// Opens, raises and closes emergency deliveries of life-support goods (step 14).
void step_emergencies(World& w);
// Value of moving units into (+) or out of (-) a station, along its price curve.
// Value of selling `units` at a station `days_ahead` from now, on its forecast stock.
// The stock a delivery `days_ahead` from now lands on: today's run forward at the net
// rate, with the same good other ships deliver before then.
[[nodiscard]] double forecast_stock_on_arrival(const World& w, const domain::StationState& state,
    const std::string& commodity_id, double days_ahead, const std::string& seller_ship_id);
// Ships are paid under the station's order for the good, not along its curve: true wherever
// the station has OrderStats for it (step 29).
[[nodiscard]] bool priced_order(const domain::StationState& state, const std::string& commodity_id);
// Station purchase orders (step 29, docs/modules/economy.md). A station posts an order
// for a good it is a net consumer of, and for depot fuel where it has no factory, never at
// an export market; it then has OrderStats for the good.
[[nodiscard]] bool has_order(const World& w, const domain::StationDefinition& station, const std::string& commodity_id);
// What a station expects before its first delivery: the reference trip's lead and interval
// (21 days within one planet's system), half the lead as spread, the reference price.
[[nodiscard]] domain::OrderStats seed_order_stats(const World& w, const domain::StationDefinition& station,
    const std::string& commodity_id);
// Seeds every order a station has no observations for yet.
void seed_order_stats(World& w);
// Folds a contract (its unit price) or a delivery (its lead and the interval since the last
// one) into a station's observations; nothing if the station posts no order for the good.
void observe_contract(World& w, domain::StationState& state, const std::string& commodity_id, double units,
    double unit_price);
void observe_delivery(World& w, domain::StationState& state, const std::string& commodity_id, double contracted_s);
// V: what a unit is worth to the station (see the plan's section 4); `station_ceiling` is
// the part from its own output, before the faction tops up a life-critical good.
[[nodiscard]] double order_ceiling(const World& w, const domain::StationState& state, const std::string& commodity_id,
    std::string& kind, double& station_ceiling);
[[nodiscard]] OrderLevels order_levels(const World& w, const domain::StationState& state,
    const std::string& commodity_id, const domain::OrderStats& stats);
// The good's contracted arrivals at the station (time, units that survive the trip), sorted,
// without one ship's.
[[nodiscard]] std::vector<std::pair<double, double>> contracted_arrivals(const World& w,
    const domain::StationState& state, const std::string& commodity_id, const std::string& except_ship_id);
// The order derived from the observations, the stock and the contracts on their way.
[[nodiscard]] domain::OrderView order_view(const World& w, const domain::StationState& state,
    const std::string& commodity_id);
[[nodiscard]] OrderQuote order_quote(const World& w, const domain::StationState& state,
    const std::string& commodity_id, double arrival_s, const std::string& except_ship_id);
// The value of the units [from, from + units) of a delivery under a quote.
[[nodiscard]] SaleSplit quote_split(const OrderQuote& quote, double from, double units);
[[nodiscard]] OrderBid order_bid(const World& w, const domain::StationState& state, const std::string& commodity_id,
    const OrderLevels& levels, double stock_at_arrival);
void update_market_prices(World& w);
// Sets each order's posted price to its bid for a delivery one lead time from now (shown in
// the inspector and the audit; quotes compute their own).
void step_orders(World& w);
// What a consumer offers for a good now: its order price where it posts an order, else its price.
[[nodiscard]] double consumer_bid(const World& w, const domain::StationState& state, const std::string& commodity_id);

}  // namespace spacetrains::simulation::systems
