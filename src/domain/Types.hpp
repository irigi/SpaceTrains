#pragma once

#include <cstdint>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "math/Vec3d.hpp"

namespace spacetrains::domain {

// Ordered, so iteration (and every choice that depends on it) does not depend on insertion
// history: a game continued after a load plays exactly as one that never stopped.
using Inventory = std::map<std::string, double>;

struct OrbitDefinition {
    std::string parent_id;
    double semi_major_axis_m {0.0};
    double eccentricity {0.0};
    double orbital_period_s {0.0};
    double phase_at_epoch_rad {0.0};
};

struct CelestialBodyDefinition {
    std::string id;
    std::string name;
    double radius_m {0.0};
    double mu_m3_s2 {0.0};
    OrbitDefinition orbit;
};

struct FactionDefinition {
    std::string id;
    std::string name;
    std::string color_hex;  // RRGGBB, no leading '#'
};

struct CommodityDefinition {
    std::string id;
    std::string name;
    double mass_per_unit_kg {1.0};
    double decay_fraction_per_day {0.0};  // fraction of cargo lost per day in transit
    double base_price {10.0};             // credits per unit at target stock level
};

// One part of a ship class (data/ship_classes/ship_components.csv, written by
// build_ship_classes.py): engine, radiator, tank, habitat, hold, structure, and the
// habitat's own parts (parent_id "habitat"). Top-level parts add up to the class's dry mass
// and value. Tier 1 only describes the ship; the simulation still runs on the class totals.
// Zero means "not applicable" for the physical fields.
struct ShipComponentDefinition {
    std::string id;           // "engine", "habitat.life_support", ...
    std::string parent_id;    // "" for top-level parts
    std::string kind;         // engine | radiator | tank | habitat | hold | structure | habitat part kinds
    std::string name;
    double mass_kg {0.0};
    double price_cr {0.0};
    double power_w {0.0};       // engines: jet power; habitat: electrical draw
    double heat_w {0.0};        // waste heat produced (engine, habitat) or rejected (radiators)
    double temperature_k {0.0}; // operating temperature (reactor core, radiator panel, tank, cabin)
    double volume_m3 {0.0};
    double area_m2 {0.0};       // radiator panel area (two-sided)
    double capacity_kg {0.0};   // tanks: propellant; hold: rated cargo
    double crew_capacity {0.0};
    double isp_s {0.0};
    double thrust_n {0.0};
};

struct ShipClassDefinition {
    std::string id;
    std::string name;
    std::string propulsion_type {"nuclear_thermal"};   // "nuclear_thermal" or "variable_isp"
    double dry_mass_kg {0.0};
    double propellant_capacity_kg {0.0};
    double cargo_capacity_units {0.0};
    // Nuclear-thermal propulsion fields:
    double max_delta_v_mps {0.0};
    double cruise_accel_mps2 {0.0};
    // Variable-Isp (plasma) propulsion fields:
    double specific_engine_power_w_per_kg {0.0};  // alpha [W/kg_dry]
    // Operating economics:
    double ship_value_cr {0.0};   // capital tied up in the ship (amortised + interest)
    double crew_size {0.0};
    // Classes with the same hull differ only in their tanks; ships refit between them
    // at their home base. Empty = the class's own id (no refit options).
    std::string hull_id;
    std::vector<ShipComponentDefinition> components;  // in file order, parents before their parts
};

// Fleet-wide operating parameters (data/economy/ship_operations.csv).
struct ShipOperationsDefinition {
    double wage_cr_per_crew_day {0.0};
    double interest_rate_per_year {0.0};
    double lifetime_years {30.0};
    // Tank refits at the home base: days in the yard, and the yard's bill as a fraction
    // of the hardware value added or removed (the owner finances the hardware itself,
    // through the capital charge on the new ship value).
    double refit_days {20.0};
    double refit_cost_fraction {0.3};
    // Life support per crew-day, in commodity units, bought as real goods.
    std::unordered_map<std::string, double> life_support_units_per_crew_day;
};

// Propellant supply (data/economy/fuel_supply.csv, fuel_factories.csv). Every station has a
// depot outside its cargo storage; only fuel factories fill theirs, at their output rate, and
// price it on their buffer. Other depots hold what ships deliver, bought under orders (step 29).
struct FuelSupplyDefinition {
    double factory_buffer_days {0.0};         // a factory's depot holds this many days of output
    std::unordered_map<std::string, double> factory_output_units_per_day;  // by station id
};

// The open economy (data/economy/open_economy.csv). Faction treasuries keep each station's
// balance inside a band, ships pay the cash above a working reserve to their owner (the home
// station), and a slow controller transfers money per head of population to hold the money
// supply in stations and ships near the seeded amount.
struct OpenEconomyDefinition {
    double station_credit_floor {0.0};     // subsidies close the gap below this
    double station_credit_ceiling {0.0};   // taxes take the excess above this; 0 = no taxes
    double station_balance_days {90.0};    // time to close a subsidy or tax gap
    double ship_cash_reserve {0.0};        // dividends take the excess above this; 0 = no dividends
    double dividend_days {30.0};
    double money_supply_days {0.0};        // controller time constant; 0 = no controller
    // Station money (step 17).
    double core_crew_fraction {0.0};       // of the population whose upkeep the faction guarantees
    double faction_loan_to_value {0.0};    // factions borrow this share of their fleet's hull value...
    double faction_tax_years {0.0};        // ...plus this many years of their tax income
};

// Fleet investment (data/economy/fleet_investment.csv). Faction treasuries buy new ships
// from the outside economy for the routes that pay best, and owners sell ships that have
// been laid up for long back to it for their salvage value.
struct FleetInvestmentDefinition {
    double review_days {0.0};              // between commissioning reviews; 0 = no new ships
    double hurdle_return_per_year {0.0};   // expected profit after costs / ship price
    double working_capital {0.0};          // cash a new ship starts with, from its treasury
    double build_days {0.0};               // in the yard before its first mission
    double layup_sale_days {0.0};          // laid up this long: sold; 0 = never
    double salvage_fraction {0.0};         // of the ship's value, paid to its treasury
    double route_commitment_days {0.0};    // a new ship works the route it was bought for this long
    double max_ships_per_review {1.0};     // ships bought per review while candidates clear the hurdle
    double max_fleet_size {0.0};           // no purchase while this many ships are in service; 0 = no limit
    double max_fleet_hold_units {0.0};     // no purchase that takes the fleet's holds past this; 0 = no limit (step 21)
};

// Station growth (data/economy/growth.csv, step 27). A station's population follows how well
// its residents are supplied: it grows while every upkeep good has been in stock (an average
// over window_days of the worst one's availability at least grow_above) and the station pays
// its way, and shrinks (emigration) while that average is below decline_below. It stays
// between the core crew and max_population_factor x the seeded population.
struct GrowthDefinition {
    double growth_per_year {0.0};
    double decline_per_year {0.0};
    double max_population_factor {1.0};
    double window_days {90.0};
    double grow_above {0.95};
    double decline_below {0.7};
};

// Random events (data/events/events.csv, step 28). Each may strike its station at random,
// rate_per_year on average, while it is not already running there, for a duration drawn
// between min_days and max_days:
//   outage        the station's output of the good runs at `magnitude` (0 = halted);
//   boom          its output of the good runs at `magnitude` x;
//   demand_spike  its use of the good runs at `magnitude` x;
//   migration     its population grows by `magnitude` (a share) at once.
// Prices, emergencies and ships react through the market; nothing else special-cases them.
enum class EventKind { Outage, Boom, DemandSpike, Migration };

struct EventDefinition {
    std::string id;
    EventKind kind {EventKind::Outage};
    std::string station_id;
    std::string commodity_id;  // empty for migration
    double rate_per_year {0.0};
    double min_days {0.0};
    double max_days {0.0};
    double magnitude {1.0};
    std::string headline;
    std::string end_headline;
};

// Prices (data/economy/pricing.csv and reference_prices.csv, step 15).
struct PricingDefinition {
    double price_cap {16.0};            // a curve's highest multiple of its centre (empty stock)
    double carrier_margin {1.2};        // on the transport cost, in the reference prices
    double reference_departures {6.0};  // departure dates over a year the reference trips are planned at
    // An order-up-to level covers at most this many days of use (step 29).
    double max_cover_days {365.0};
    // A consumer's reference price (the centre of its curve) by station id and good: the
    // landed cost from the cheapest producer. Goods not listed are centred on their base price.
    std::unordered_map<std::string, std::unordered_map<std::string, double>> reference_prices;
    // The reference trip's round-trip days, same keys (the last column of reference_prices.csv).
    std::unordered_map<std::string, std::unordered_map<std::string, double>> round_trip_days;
};

// Station purchase orders (step 29, docs/plans/station_orders.md; data/economy/orders.csv).
struct OrdersDefinition {
    double safety_factor {1.5};        // k: the safety stock is k x the spread of a delivery cycle
    double ema_deliveries {4.0};       // the observed lead, interval and spread average over this many deliveries
    double extra_discount {0.1};       // d: the station buys past its order-up-to level only this far below its going rate
    double max_extra_days {90.0};      // the extra covers at most this many days of use (and one interval)
    double resale_margin {0.1};        // a consumer sells to ships (provisions, depot fuel) at its bid on its stock plus this
    double keep_share {0.75};          // of an input's marginal profit the station keeps (the rest is its ceiling)
    double life_value {24.0};          // x reference: what the faction pays for oxygen and water at most
    double consume_value {4.0};        // x reference: the ceiling for goods used up without output (construction, science)
    double depot_fuel_value {8.0};     // x reference: the cap for fuel at a depot without a factory (not min with price_cap)
    double input_cap {12.0};           // x reference: an input's bid goes at most this high (its ceiling V permitting)
};

// Market redesign switches (2026-10-19 night session); optional keys in data/economy/market.csv.
// The defaults reproduce the economy before the redesign.
struct MarketDefinition {
    double follow_up_horizon_days {0.0};  // >0: a follow-up leg's profit counts x exp(-days until it starts / this)
    double lead_cap_factor {0.0};      // >0: a delivery's lead sample counts at most this x the nearest source's lead
    double bid_scale {1.0};            // multiplies the stock-ratio bid (before its cap)
    double bid_elasticity {1.3};       // e in (S / stock)^e
    // Competitive pricing (tender): ships near a source ask their cost plus a markup; an order
    // pays at most the second-lowest ask (one bidder: its ask x (1 + solo_premium)).
    double competitive {0.0};          // 1: on
    double ask_markup {0.3};           // on the ship's cost of the delivery
    double return_share {0.5};         // of the leg's cost added for the way back
    double fill_ref {0.65};            // share of the hold a delivery's cost is spread over
    double solo_premium {0.5};
    double available_days {30.0};      // ships free at a source within this many days bid
    double min_source_surplus {50.0};  // units a source must have spare to count
    double ask_open_units {0.0};
    double anchor_reference {0.0};     // >0: the market price is this x the reference price (no asks)
    double own_ask_floor {0.0};        // 1: a ship is paid at least its own ask (within the stock bid)
    double tender_awards {0.0};        // 1: each open order goes to the cheapest docked ship at a source (binding)       // 1: spread a trip over what the leg can carry to open orders, not the hold
    double urgency_elasticity {0.0};   // the market price times clamp((S / stock)^this, 1, urgency_max)
    double urgency_max {2.0};
    double urgency_on_safety {0.0};
    double escalation_rate {0.0};      // >0: the market price x an escalation that grows this much a day while the order is open
    double escalation_relax {0.1};     // ... and falls this share a day toward 1 once nothing is open    // 1: urgency against the safety stock instead of the order-up-to level
    // Rotation contracts.
    double contracts {0.0};            // 1: on
    double contract_max_stops {4.0};   // stops in a loop, the ship's port included
    double contract_term_days {365.0};
    double contract_review_days {30.0};  // how often a ship without a contract looks for one
    double contract_review_when_idle {0.0};  // 1: ... and at once whenever it has no paying work
    double contract_cover {0.7};       // of a station's uncontracted use a loop takes on
    double contract_accept {0.7};      // signs if the loop earns this x the best spot mission's score
    double contract_vs_unweighted {0.0};  // 1: ... against the spot mission's profit per day, not its urgency-weighted score
    double contract_min_units {20.0};  // a delivery smaller than this per visit is left out
    double contract_refine {6.0};      // loops valued again with each leg dated
    double contract_at_going_rate {0.0};  // 1: contracts are signed at the going rate even under competitive prices
    double contract_cost_plus {0.0};
    double contract_reserve_days {0.0};   // >0: a source holds back what contract ships load there within this many days   // >0: contract prices are purchase + loop cost share x (1 + this)
};

struct StationDefinition {
    std::string id;
    std::string name;
    std::string faction_id;
    std::string parent_body_id;
    double altitude_m {0.0};
    double theta_rad {0.0};
    std::int64_t population {0};
    std::string economy_profile_id;
    double storage_capacity_units {0.0};  // total units across all commodities; 0 = unlimited
    double initial_credits {0.0};
    Inventory initial_inventory;
};

// A recipe of an economy profile, or of one station (local resources such as Ceres's
// platinum, data/recipes/station_recipes.csv), when station_id is set.
// What a consumed good is for (step 14). Outputs have no role.
enum class RecipeRole {
    Output,
    Input,    // material input of the outputs in `feeds`: they stop without it
    Upkeep,   // life support and crew needs: a shortage penalises every output of the station
    Consume,  // demand with no effect on output (colony construction, research)
};

struct RecipeDefinition {
    std::string profile_id;
    std::string commodity_id;
    double units_per_day {0.0};
    std::string station_id {};
    RecipeRole role {RecipeRole::Output};
    std::vector<std::string> feeds {};  // outputs of the same station, for an Input
};

// Earth's economy (the outside account) buys any amount of an export good delivered to
// this station at its base price (data/economy/export_markets.csv). Planners see the
// market as a consumer of units_per_day.
struct ExportMarketDefinition {
    std::string station_id;
    std::string commodity_id;
    double units_per_day {0.0};
};

struct ShipSeedDefinition {
    std::string id;
    std::string name;
    std::string faction_id;
    std::string class_id;
    std::string home_station_id;
    std::string start_station_id;
    double initial_propellant_kg {0.0};
    double initial_credits {0.0};
};

struct UniverseDefinition {
    std::vector<CelestialBodyDefinition> bodies;
    std::vector<FactionDefinition> factions;
    std::vector<CommodityDefinition> commodities;
    std::vector<ShipClassDefinition> ship_classes;
    std::vector<StationDefinition> stations;
    std::vector<RecipeDefinition> recipes;
    std::vector<ExportMarketDefinition> export_markets;
    std::vector<ShipSeedDefinition> ship_seeds;
    ShipOperationsDefinition ship_operations;
    FuelSupplyDefinition fuel_supply;
    OpenEconomyDefinition open_economy;
    FleetInvestmentDefinition fleet_investment;
    PricingDefinition pricing;
    OrdersDefinition orders;
    MarketDefinition market;
    GrowthDefinition growth;
    std::vector<EventDefinition> events;
    std::uint64_t event_seed {0};
    // Scheduled liners (data/economy/liners.csv, step 22): seed ship id -> the stations of its
    // loop, in order. A liner flies only to the stop after the one it is at, forever.
    std::unordered_map<std::string, std::vector<std::string>> liners;
    // Output multiplier at a full shortage of an upkeep good (data/economy/upkeep_penalties.csv).
    std::unordered_map<std::string, double> upkeep_penalties;
    // Upkeep goods whose shortage makes the owning faction pay for an emergency delivery.
    std::unordered_set<std::string> emergency_goods;
};

enum class ShipMissionPhase {
    Idle,
    AwaitingDeparture,
    InTransit,
    Refueling,
    Stranded,
    LaidUp,     // out of money: crew discharged, docked until a mission pays
    Refitting   // in the home yard for new tanks; crew discharged until it is done
};

struct EventEntry {
    double time_s {0.0};
    std::string text;
    std::string category {"general"};  // mission | arrival | trade | fuel | alert | general
};

struct TradeEntry {
    double time_s {0.0};
    std::string ship_id;
    std::string station_id;
    std::string commodity_id;
    std::string kind;  // "buy" | "sell" | "fuel"
    double units {0.0};
    double unit_price {0.0};
    double total {0.0};
};

// One good in a ship's hold.
struct CargoLot {
    std::string commodity_id;
    double units {0.0};
    // The price agreed at departure for this lot as it will arrive (after spoilage): the
    // destination's forecast price along its curve, the same forecast dispatch planned with.
    // The station pays it on arrival whatever the market did meanwhile (0: sold at market).
    double contract_value {0.0};
    // The part of contract_value the destination's faction pays: an emergency delivery's
    // premium over the station's own price (step 14).
    double emergency_premium {0.0};
    // A rotation contract's delivery: the price per unit fixed when the contract was signed
    // (0: priced by the order at departure). Only used until departure; not saved.
    double fixed_unit_price {0.0};
};

// An emergency delivery the owning faction pays for (step 14): the station is about to run
// out of a life-support good. While it is open, the station's price for that good, for the
// units still open (30 days of its use) and below 30 days of stock, is at least `premium`
// times its reference price; the faction pays the part above the station's own curve.
struct Emergency {
    std::string station_id;
    std::string commodity_id;
    double premium {3.0};
    double units_open {0.0};    // still to be contracted at the floor price (30 days of use at opening)
    double opened_s {0.0};
    double last_raise_s {0.0};  // when the premium last rose, or a ship last took a delivery
    double faction_paid {0.0};
};

[[nodiscard]] inline double total_units(const std::vector<CargoLot>& cargo) {
    double total = 0.0;
    for (const auto& lot : cargo) {
        total += lot.units;
    }
    return total;
}

[[nodiscard]] inline double units_of(const std::vector<CargoLot>& cargo, const std::string& commodity_id) {
    double total = 0.0;
    for (const auto& lot : cargo) {
        if (lot.commodity_id == commodity_id) {
            total += lot.units;
        }
    }
    return total;
}

struct MissionAssignment {
    std::string origin_station_id;
    std::string destination_station_id;
    // The hold: several goods for the destination (v37 mixed cargo); empty when flying empty.
    std::vector<CargoLot> cargo;
    double departure_time_s {0.0};
    double arrival_time_s {0.0};
    double wait_time_s {0.0};
    double coast_time_s {0.0};
    double total_travel_time_s {0.0};
    double remaining_travel_time_s {0.0};
    double propellant_cost_kg {0.0};
    double purchase_cost {0.0};      // credits paid for cargo at origin
    double fuel_cost {0.0};          // credits paid for propellant attributed to this mission
    double expected_revenue {0.0};   // estimated sale value at destination when planned
    double operating_cost {0.0};     // expected capital + crew + provisions over wait + transit
    std::vector<math::Vec3d> sampled_path;
    std::vector<double> sampled_times_s;
    std::vector<double> sampled_propellant_kg;
    std::string trajectory_type;
    // Return fuel carried for a port that cannot refuel the ship; the samples do not include it.
    double carried_propellant_kg {0.0};
    // The follow-up load the ship planned to pick up at its destination (none if empty):
    // other ships' forecasts leave it to this ship.
    std::string pickup_commodity_id;
    double pickup_units {0.0};
};

// Where a ship's money went over its lifetime (all amounts positive, in credits).
struct ShipLedger {
    double cargo_revenue {0.0};
    double cargo_purchases {0.0};
    double fuel {0.0};
    double wages {0.0};
    double capital {0.0};
    double provisions {0.0};
    double refits {0.0};
    double dividends {0.0};  // surplus cash paid to the owner; not a cost, so not in lifetime_profit
    double subsidies {0.0};  // a liner's cash top-ups from its faction (step 22); not income, so not in lifetime_profit
};

// A rotation contract (market redesign, 2026-10-19): the ship flies a loop of up to four stops
// for a term, delivering fixed amounts at fixed prices on each leg; spare hold space is sold
// at market. Saved.
struct ShipContract {
    struct Delivery {
        std::string to_station_id;     // loaded at the stop before it in the loop
        std::string commodity_id;
        double units {0.0};            // per visit
        double unit_price {0.0};
    };
    std::vector<std::string> stops;    // empty: no contract
    std::vector<double> leg_days;      // stops[i] -> stops[i + 1], as estimated at signing
    std::vector<Delivery> deliveries;
    double signed_s {0.0};
    double until_s {0.0};
    double cycle_days {0.0};
    bool award {false};                // a tender won: one leg, ends on delivery
};

struct ShipState {
    std::string id;
    std::string name;
    std::string faction_id;
    std::string class_id;
    std::string home_station_id;
    std::string current_station_id;
    ShipMissionPhase phase {ShipMissionPhase::Idle};
    double propellant_kg {0.0};
    double credits {0.0};
    double lifetime_profit {0.0};
    MissionAssignment active_mission;
    Inventory provisions;              // life-support stock carried for the crew
    ShipLedger ledger;
    double next_review_s {0.0};  // when a docked ship next looks for a mission
    double idle_since_s {0.0};         // when the ship last became idle (crewed, docked)
    // Tank refits (part C): the class the ship becomes when the yard is done.
    std::string refit_class_id;
    double refit_done_s {0.0};
    double next_refit_review_s {0.0};
    double laid_up_since_s {0.0};      // when the ship was last laid up
    double commissioned_s {0.0};       // 0 for the starting fleet
    // Route commitment (fleet investment): until route_until_s a new ship only shuttles
    // between its home and route_destination_id. The commodity and flow it was bought for
    // count against that destination's demand when the treasuries value the next ship.
    std::string route_destination_id;
    std::string route_commodity_id;
    double route_units_per_day {0.0};
    double route_until_s {0.0};
    ShipContract contract;
    double next_contract_review_s {0.0};
};

// A station's money flows with the world outside the simulated trade (the open economy),
// all amounts positive, in credits.
struct StationLedger {
    double household_sales {0.0};       // residents paid for goods they used up, at what the station paid (step 17)
    double producer_purchases {0.0};    // the market paid local producers and its depot for new output
    double dividends {0.0};             // paid in by the ships it owns
    double subsidies {0.0};             // from the faction treasury (core-crew top-up and money-supply controller)
    double taxes {0.0};                 // to the faction treasury (band ceiling and money-supply controller)
};

// The treasuries' trade in ships with the outside economy (fleet investment), all amounts
// positive, in credits.
struct FleetInvestmentLedger {
    int ships_commissioned {0};
    int ships_sold {0};
    double hulls_bought {0.0};      // treasuries -> outside economy
    double working_capital {0.0};   // treasuries -> new ships
    double salvage {0.0};           // outside economy -> treasuries
};

struct ActiveEvent {
    std::string event_id;
    double start_s {0.0};
    double end_s {0.0};
};

// What a station has observed about resupplying one good it consumes, and its order for it
// (step 29). Saved.
struct OrderStats {
    double ship_sales_per_day {0.0};   // provisions sold to ships, units per day (depot fuel: ship_fuel_units_per_day)
    double lead_days {0.0};            // L: from a contract to its delivery
    double interval_days {0.0};        // I: between deliveries
    double spread_days {0.0};          // sigma: mean absolute deviation of L and I samples
    double going_rate {0.0};           // G: what it has agreed to pay per unit, units-weighted
    double last_delivery_s {-1.0};     // -1: none yet
    int deliveries {0};
    // The order's bid for a delivery one lead time from now (set each tick; quotes make their own).
    double order_price {0.0};
    // Competitive pricing: the second-lowest ask of the ships near a source (0: none), set daily.
    double market_price {0.0};
    int market_bidders {0};
    // Escalation: rises while the order stays open with nobody taking it, falls back once taken.
    double escalation {1.0};
};

// The order a station would post for a good, derived from its OrderStats, its stock and the
// contracts on their way (step 29). Not saved; filled for snapshots.
struct OrderView {
    std::string ceiling_kind;          // input | upkeep | life | consume | depot_fuel
    double use_per_day {0.0};          // u: recipe use plus sales to ships
    double lead_days {0.0};
    double interval_days {0.0};
    double spread_days {0.0};
    double going_rate {0.0};
    double reference_price {0.0};
    double order_price {0.0};          // P
    double safety_units {0.0};         // B = u k sigma
    double reorder_units {0.0};        // R = u (L + k sigma)
    double order_up_to_units {0.0};    // S = R + u I, capped by storage and max_cover_days
    double extra_days {0.0};           // E: days past S the discount pays for (capped)
    double max_units {0.0};            // S_max = S + u E, same caps
    double stock {0.0};
    double contracted_units {0.0};     // on their way under contract
    double open_units {0.0};           // N for a delivery one lead time from now
    double days_to_safety {-1.0};      // when the projected stock falls below B (-1: not within the lead time)
    double ceiling {0.0};              // V per unit
    double station_ceiling {0.0};      // V from the station's own output (life goods: before the faction tops it up)
};

struct StationState {
    std::string station_id;
    Inventory inventory;
    double credits {0.0};
    StationLedger ledger;
    // Fuel ships bought here, units per day, averaged with an exponential window: the demand
    // a tanker serving the station would meet.
    double ship_fuel_units_per_day {0.0};
    // Cargo ships delivered here and bought here, units per day, averaged the same way: the
    // flow a new ship competes with.
    Inventory import_units_per_day {};
    Inventory export_units_per_day {};
    // Units the station's consumers asked for since the start, and the part they went without
    // because the stock had run out.
    Inventory demand_units {};
    Inventory unmet_units {};
    // Units an export market sold on to the outside economy since the start.
    Inventory market_sold_units {};
    // What the station paid ships per unit, averaged over about 30 days of its use (step 17):
    // its residents pay this for what they use up (the reference price until a delivery).
    Inventory import_unit_cost {};
    // Step 29: what the station has observed about resupplying each good it consumes, and (for
    // snapshots only) the orders derived from it.
    std::map<std::string, OrderStats> order_stats {};
    std::map<std::string, OrderView> orders {};
    // Diagnostics of the last economy step (not saved): the product of the upkeep penalties
    // in force, and each output's run rate (upkeep included).
    double upkeep_multiplier {1.0};
    Inventory output_factor {};
    std::map<std::string, std::string> output_limited_by {};  // the input that limits each output, if any
    Inventory upkeep_availability {};                         // stock over a 7-day buffer, per upkeep good
    // Residents now (step 27; the definition's population is the seeded one), the average
    // availability of the worst-supplied upkeep good, and the population last announced.
    double population {0.0};
    double supply_index {1.0};
    double population_announced {0.0};
    // Events running here (step 28): the definition's id, and when each ends.
    std::vector<ActiveEvent> events {};
};

struct SimulationSnapshot {
    double game_time_s {0.0};
    // The outside economy: residents and local producers (what they paid into the simulated
    // economy is negative) and the faction treasuries. Station + ship + outside + treasury
    // credits always equal the seeded money.
    double outside_economy_credits {0.0};
    std::map<std::string, double> faction_treasuries;
    // What each faction may borrow (negative treasury) and has paid in interest (step 17).
    std::map<std::string, double> faction_credit_limits;
    double faction_interest_paid {0.0};
    // What the money-supply controller holds stations + ships at: the seeded money plus the
    // working capital the treasuries gave new ships.
    double money_supply_target {0.0};
    FleetInvestmentLedger fleet_investment;
    std::vector<StationState> stations;
    std::vector<ShipState> ships;
    std::vector<ShipState> sold_ships;   // as they were when sold, for the audit
    std::vector<EventEntry> recent_events;
    // Emergency deliveries (step 14): open now, opened since the start, premiums paid.
    std::vector<Emergency> emergencies;
    int emergencies_opened {0};
    std::map<std::string, int> events_started;  // by event id (step 28)
    double emergency_paid {0.0};
};

// Planner-internal facts that the final path no longer shows (endpoints are
// snapped onto the stations). Used by the trajectory audit.
struct TrajectoryDiagnostics {
    double start_miss_m {0.0};      // unsnapped first sample -> origin station
    double endpoint_miss_m {0.0};   // unsnapped last sample -> destination station
    // VariableISP only.
    double rho {0.0};
    double kappa {0.0};
    double theta_target_rad {0.0};  // launch-window theta from the atlas grid
    double theta_actual_rad {0.0};  // theta reached by the integrated trajectory
    double r_end_canonical_ratio {0.0};  // integrated final radius / canonical rho
    std::string seed_source;        // refined (atlas cell seed after endpoint shooting)
    std::size_t refine_iterations {0};
    std::size_t windows_tried {0};  // launch windows attempted before one converged
};

struct TrajectoryPlan {
    bool feasible {false};
    double departure_time_s {0.0};
    double arrival_time_s {0.0};
    double wait_time_s {0.0};
    double coast_time_s {0.0};
    double travel_time_s {0.0};
    double propellant_required_kg {0.0};  // burned on the transfer
    double propellant_load_kg {0.0};      // aboard at departure (what was aboard plus any purchase)
    std::vector<math::Vec3d> sampled_path;
    std::vector<double> sampled_times_s;
    std::vector<double> sampled_propellant_kg;
    std::string summary;
    std::string trajectory_type;  // keplerian_local | keplerian_lambert | keplerian_hohmann | variable_isp
    TrajectoryDiagnostics diagnostics;
    // False when planned without a rendering path (PlanningOptions::include_path off).
    bool has_render_path {true};
};

}  // namespace spacetrains::domain
