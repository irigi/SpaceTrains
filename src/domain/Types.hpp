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
// depot outside its cargo storage; only fuel factories fill theirs, at their output rate.
// Other depots hold what ships deliver. Prices follow each depot's buffer.
struct FuelSupplyDefinition {
    double depot_buffer_units {0.0};          // 0 = no depots
    double factory_buffer_days {0.0};         // a factory's depot holds this many days of output (at least the depot buffer)
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
};

// Prices (data/economy/pricing.csv and reference_prices.csv, step 15).
struct PricingDefinition {
    double price_cap {16.0};            // a curve's highest multiple of its centre (empty stock)
    double carrier_margin {1.2};        // on the transport cost, in the reference prices
    double reference_departures {6.0};  // departure dates over a year the reference trips are planned at
    // A consumer's reference price (the centre of its curve) by station id and good: the
    // landed cost from the cheapest producer. Goods not listed are centred on their base price.
    std::unordered_map<std::string, std::unordered_map<std::string, double>> reference_prices;
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
};

// A station's money flows with the world outside the simulated trade (the open economy),
// all amounts positive, in credits.
struct StationLedger {
    double household_sales {0.0};       // residents and local industry paid for goods they used up
    double producer_purchases {0.0};    // the market paid local producers and its depot for new output
    double dividends {0.0};             // paid in by the ships it owns
    double subsidies {0.0};             // from the faction treasury (band floor and money-supply controller)
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
    // Diagnostics of the last economy step (not saved): the product of the upkeep penalties
    // in force, and each output's run rate (upkeep included).
    double upkeep_multiplier {1.0};
    Inventory output_factor {};
};

struct SimulationSnapshot {
    double game_time_s {0.0};
    // The outside economy: residents and local producers (what they paid into the simulated
    // economy is negative) and the faction treasuries. Station + ship + outside + treasury
    // credits always equal the seeded money.
    double outside_economy_credits {0.0};
    std::map<std::string, double> faction_treasuries;
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
