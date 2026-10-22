#pragma once

#include <cstdint>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
    // Passenger berths (docs/plans/passengers.md): built-in cabins of a liner class, plus a fitted
    // cabin module's `berths` effect on a design. Liner classes load only when passengers are on.
    double berths {0.0};
    // Classes with the same hull differ only in their tanks; ships refit between them
    // at their home base. Empty = the class's own id (no refit options).
    std::string hull_id;
    std::vector<ShipComponentDefinition> components;  // in file order, parents before their parts
    // Fitted designs only (tier 2): the class they derive from, the modules and the sum of
    // their effects by key.
    std::string base_class_id;
    std::vector<std::string> fit;
    std::unordered_map<std::string, double> effects;
};

// A module a hull can fit at its home yard (data/ship_classes/modules.csv, written by
// build_ship_classes.py; tier 2, docs/plans/ship_structures_tier2.md). Its part joins the
// design's components; its effects (key=value) act through the quantities they name.
struct ShipModuleDefinition {
    std::string hull_id;
    std::string id;
    std::string slot;           // habitat | hold | drive | tank | cabin
    ShipComponentDefinition part;
    std::unordered_map<std::string, double> effects;
};

// One wage scale for all work (data/economy/labour.csv): a reference wage and grades as its
// multiples, from US BLS median-wage ratios (docs/ship_technology.md, "Labour").
struct LabourDefinition {
    double reference_wage_cr_per_day {0.5};
    double shift_hours {8.0};
    std::unordered_map<std::string, double> grades;  // grade -> multiple of the reference wage
    [[nodiscard]] double wage_cr_per_day(const std::string& grade) const {
        const auto it = grades.find(grade);
        return it == grades.end() ? 0.0 : reference_wage_cr_per_day * it->second;
    }
    [[nodiscard]] double wage_cr_per_hour(const std::string& grade) const {
        return wage_cr_per_day(grade) / shift_hours;
    }
};

// A part that wears out (data/ship_classes/spares.csv, from the ship class builder): the
// sub-part kind, what wears it (the drive's hours or meteoroids on the radiators), the goods
// it is made of (shares of its mass), and the hours to make and fit it at a yard.
struct SparePartDefinition {
    std::string kind;
    std::string wears_with;   // drive | radiators
    std::string grade;
    double fabrication_h_per_kg {0.0};
    double install_h_per_kg {0.0};
    std::vector<std::pair<std::string, double>> materials;  // commodity, share of the mass
};

// Fleet-wide operating parameters (data/economy/ship_operations.csv).
struct ShipOperationsDefinition {
    double wage_cr_per_crew_day {0.0};   // the crew grade of the labour scale
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

// The space environment acting on ships and crews (data/economy/ship_environment.csv, tier 2;
// sources and derivations in docs/ship_technology.md, "Crew radiation"). Every mechanism is a
// switch; with all off the economy is the reference.
struct ShipEnvironmentDefinition {
    bool radiation {false};
    // Galactic cosmic rays behind a habitat-class hull (~10 g/cm2): Curiosity's RAD measured
    // 1.84 mSv/day in cruise (Zeitlin et al. 2013). The solar cycle modulates it by about
    // +-30%, highest at solar minimum; it grows a few per cent per AU outward.
    double gcr_msv_per_day {1.84};
    double gcr_cycle_amplitude {0.3};
    double solar_cycle_days {4018.0};       // 11 years
    double solar_minimum_day {-1000.0};     // a solar minimum, in game days from the start
    double gcr_gradient_per_au {0.03};
    double gcr_attenuation_g_cm2 {46.0};    // extra water areal density that cuts GCR dose by 1/e
    double shelter_occupancy {0.33};        // the crew sleeps in its storm shelter
    // Solar particle events: rate of events above spe_min_magnitude (cycle mean; none at
    // solar minimum, twice the mean at maximum), magnitudes power-law distributed with index
    // spe_index up to spe_max_magnitude, in units of the August 1972 event at 1 AU. Dose to
    // the blood-forming organs of that event: ~1300 mSv inside a pressure hull, ~200 behind
    // 20 g/cm2 of water (NASA, below the 250 mSv 30-day limit); falls off as 1/r^2.
    double spe_rate_per_year {0.5};
    double spe_min_magnitude {0.01};
    double spe_max_magnitude {2.0};
    double spe_index {0.5};
    double spe_hull_msv {1300.0};
    double spe_shelter_msv {200.0};
    double spe_shelter_g_cm2 {20.0};
    // A crew is hired for one leg; no leg may take it past the career limit (ESA, 1 Sv).
    double career_limit_msv {1000.0};
    // Hazard pay, a compensating wage differential: ICRP 103 nominal risk 5.5%/Sv times a
    // ~$10M value of a statistical life, at the game's 0.00078 cr/$: 0.43 cr/mSv.
    double hazard_cr_per_msv {0.43};

    // Liquid-hydrogen boil-off (step F). Heat leaks through the tanks' multilayer insulation
    // at mli_flux_w_m2 at 1 AU, scaled by r^-mli_flux_exponent (the outer layers run colder far
    // from the Sun); it boils hydrogen at its latent heat. Tank area: a sphere of the tank
    // volume times tank_area_factor (several cylinders). A zero-boil-off cryocooler module
    // lifts up to its rated heat (effect zbo_lift_w).
    bool boiloff {false};
    double mli_flux_w_m2 {0.2};
    double mli_flux_exponent {1.0};
    double lh2_latent_j_kg {446000.0};
    double tank_area_factor {1.3};

    // Cold holds (step E): perishables (food, medicine) in a cold hold spoil at the module's
    // perishable_decay_factor times the ambient rate.
    bool cold_storage {false};

    // Wear and repairs (step H, revised). Nuclear-thermal cores are rated for 100 h of
    // full-power burn (GE-710 CERMET fuel was qualified for 100 h at 2148 C; NERVA's 10 h was
    // 1960s graphite fuel); plasma drives for 10 full-power years (electrodeless thrusters; the
    // reactor limits life, as SP-100's 7-10 and JIMO's 10+ year goals). Radiator heat pipes are holed by
    // meteoroids of 1e-5 g and up (a 0.2 mm grain through a 1 mm wall at ~20 km/s): Grun et al.
    // 1985 give 0.146 per m2 per year at 1 AU, about r^-1.3 with distance; a hit kills one
    // 0.2 m2 segment, and 30% of a panel is pipe (estimates). A drive is overhauled when 70% of
    // its life is used, radiators below 85%, at any port that has the spares: the wearing
    // parts (spares.csv) in goods at the port's price, plus the hours to make and fit them at
    // the labour scale; a team of 10 working two shifts, at least 3 days in the yard.
    bool wear {false};
    double ntr_core_life_h {100.0};
    double plasma_drive_life_h {87600.0};
    double puncture_flux_m2_yr {0.146};
    double puncture_flux_exponent {1.3};
    double radiator_vulnerable_fraction {0.3};
    double radiator_segment_m2 {0.2};
    double overhaul_wear {0.7};
    double radiator_repair_condition {0.85};
    double repair_team_size {10.0};
    double repair_team_hours_per_day {16.0};
    double min_repair_days {3.0};
    double initial_wear_max {0.5};   // the starting fleet is of mixed ages
};

// Where a docked crew lives (data/economy/station_radiation.csv): the share of the open-space
// cosmic-ray dose that reaches the station's location (planet shadow, magnetosphere,
// atmosphere, rock) and the station's own shielding beyond a habitat hull.
struct StationRadiationDefinition {
    double location_factor {1.0};
    double shield_g_cm2 {0.0};
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
    double purchase_spacing_days {1.0};    // at least this long between a review's purchases; 0 = one per tick
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

// Passengers (docs/plans/passengers.md; data/economy/passengers.csv, every key optional). Off by
// default: the economy without passengers, liner classes not loaded, growth by formula.
struct PassengerDefinition {
    double enabled {0.0};                       // master switch
    double rotation {1.0};                      // sub-switches, only with the master on
    double migration {1.0};
    double discretionary {1.0};
    double body_luggage_kg {100.0};             // EASA 2009 standard masses: ~84 kg adult + luggage
    double consumables_kg_per_day {2.0};        // linopt per passenger-day (food, water make-up, hygiene)
    double recovery_days_per_flight_day {0.25}; // NASA: 45 days of reconditioning after ~180-day flights
    double rotation_share {0.03};               // of an off-Earth station's people on rotating tours
    std::string rotation_grade {"engineer"};    // labour grade of rotating staff
    double hardship_per_year {0.2};             // eta: tour premium, share of the wage per year of tour
    double resident_wage_multiple {1.0};        // migrants' and visitors' value of time, x reference wage
    double visit_trips_per_person_year {0.05};  // discretionary trips (estimate)
    double earth_population_weight {1.0e5};     // Earth's weight in the gravity model (estimate)
    double fare_elasticity {-1.0};              // IATA/InterVISTAS 2007: -0.8 (national) to -1.4 (route)
    double demand_bands {4.0};
    double band_ratio {2.0};                    // each band's maximum cost x this the next one's
    double visit_days {90.0};
    // Cabins inside the propellant tanks: the tanks' mean fill over a leg, as a share of their
    // capacity, when a leg is planned (estimate: full at departure, near empty on arrival).
    double tank_wrap_mean_fill {0.5};
    // Settlers come for work: the station that wants them pays their passage (1), and fewer come
    // the longer the passage takes to pay back. Of the settlers a thriving station would take, the
    // share whose job repays a passage costing T years of the wage is 1 / (1 + (T / T50)^k):
    // T50 = 5 years, the middle of the 4-7 year indentures that repaid a colonial passage of
    // about a year's wages (estimate from that history); k = 2 (estimate).
    double settlers_sponsored {1.0};
    double settler_payback_half_years {5.0};
    double settler_payback_exponent {2.0};
    // A station recovers the passages it pays (settlers, rotation) from its residents through
    // its prices: the outstanding amount at 1/recovery_days per day, at most max_markup of what
    // they spend on goods (0 days: not recovered).
    double passage_recovery_days {365.0};
    double passage_max_markup {1.0};
    // Speed (2026-10-22, passengers_log): visitors are posted every batch_days (0: every tick);
    // a station becomes a passenger leg only once this many people wait for it.
    double visitor_batch_days {7.0};
    double min_leg_passengers {20.0};
    // Plans with riders aboard: riders under min_share of the ship's time cost use the plan
    // without them; above, their time is bucketed in relative_step of it.
    double rider_time_min_share {0.15};
    double rider_time_relative_step {0.25};
    std::vector<std::string> earth_gateways {"earth_orbit", "earth_l1"};
    // data/economy/reference_fares.csv (spacetrains_headless --write-reference-fares): the
    // cheapest cabin carrier's fare per berth and its days, origin -> destination.
    struct ReferenceFare {
        double fare_cr {0.0};       // at the base fuel price
        double days {0.0};
        std::string class_id;
        // The fare's propellant per seat (kg, with the carrier's margin): burned leaving the origin,
        // and the return share burned leaving the destination. The fare follows the ports' fuel
        // prices through them (Simulation::live_fare_cr); 0 in older data.
        double fuel_out_kg {0.0};
        double fuel_back_kg {0.0};
    };
    std::map<std::string, std::map<std::string, ReferenceFare>> reference_fares;
};

// Market redesign (2026-10-19, docs/plans/market_redesign.md): optional keys in
// data/economy/market.csv. These defaults are the economy before the redesign (all off); the game's
// data turns competitive pricing and rotation contracts on.
struct MarketDefinition {
    // Competitive pricing: once a day each order gets a market price, the second-lowest ask of the
    // ships free at a source (one bidder: its ask x (1 + solo_premium)); the order pays at most that
    // times an urgency factor clamp((S / projected stock)^urgency_elasticity, 1, urgency_max).
    double competitive {0.0};          // 1: on
    double ask_markup {0.3};           // on the ship's cost of the delivery
    double return_share {0.5};         // of the leg's cost added for the way back
    double fill_ref {0.65};            // share of the hold a delivery's cost is spread over (at most)
    double ask_open_units {0.0};       // 1: ... or over what the leg can carry to the open orders, if less
    double solo_premium {0.5};
    double available_days {30.0};      // ships free at a source within this many days bid
    double min_source_surplus {50.0};  // units a source must have spare to count
    double urgency_elasticity {0.0};
    double urgency_max {2.0};
    // Rotation contracts: a free ship looks for a loop of up to contract_max_stops stops every
    // contract_review_days and signs it for contract_term_days if it earns contract_accept x its best
    // spot mission; contracted deliveries load first at a fixed price.
    double contracts {0.0};            // 1: on
    double contract_max_stops {4.0};   // stops in a loop, the ship's port included (2 to 4)
    double contract_term_days {365.0};
    double contract_review_days {30.0};
    double contract_cover {0.7};       // of a station's uncontracted use a loop takes on
    double contract_accept {0.7};
    double contract_min_units {20.0};  // a delivery smaller than this per visit is left out
    double contract_refine {6.0};      // the best loops valued again with each leg dated
    double contract_cost_plus {0.0};   // >0: prices are purchase + the loop's cost share x (1 + this), at most the going rate
    // Dispatch: a follow-up leg's profit counts x exp(-days until it starts / this) (0: in full).
    double follow_up_horizon_days {0.0};
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
    std::vector<ShipModuleDefinition> ship_modules;
    ShipEnvironmentDefinition ship_environment;
    LabourDefinition labour;
    std::unordered_map<std::string, SparePartDefinition> spare_parts;  // by sub-part kind
    std::unordered_map<std::string, StationRadiationDefinition> station_radiation;
    // Module slots by hull id and slot (data/ship_classes/hull_slots.csv).
    std::unordered_map<std::string, std::unordered_map<std::string, int>> hull_slots;
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
    PassengerDefinition passengers;
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
    double boiled_propellant_kg {0.0};   // hydrogen boiled off since loading (tier 2, step F)
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
    double fares {0.0};      // passenger fares (paid at boarding)
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
};

// Passengers (docs/plans/passengers.md). Why a person travels decides who pays and what
// happens on arrival.
enum class PassengerKind {
    Migrant,       // settler from Earth: joins the destination's population (pays from the outside account)
    Emigrant,      // leaves a declining station for Earth (outside account)
    RotationOut,   // a rotating worker going to start a tour (the work station pays)
    RotationHome,  // a relieved worker going home to Earth (the work station pays)
    Visitor,       // discretionary trip out (outside account)
    VisitorHome,   // and back (outside account)
};

// People waiting at a station for a ship to one destination. Counts carry fractions (demand
// accrues continuously); whole people board.
struct PassengerGroup {
    PassengerKind kind {PassengerKind::Migrant};
    std::string destination_id;
    std::string work_station_id;           // rotation: the employer; visitors: the station they return to
    double count {0.0};
    double since_s {0.0};                  // when the group started waiting
    double max_cost_cr {0.0};              // highest generalized cost per person they accept
    double value_of_time_cr_per_day {0.0};
    double dose_msv {0.0};                 // rotation: dose already taken this career
    int band {0};                          // demand band (groups merge by kind, destination, employer, band)
};

// People aboard, one lot per group that boarded.
struct PassengerLot {
    PassengerKind kind {PassengerKind::Migrant};
    std::string origin_id;
    std::string destination_id;
    std::string work_station_id;
    double count {0.0};                    // whole people
    double fare_cr {0.0};                  // per person, paid at boarding
    double value_of_time_cr_per_day {0.0};
    double dose_msv {0.0};
    double boarded_s {0.0};
};

struct ShipState {
    std::string id;
    std::string name;
    std::string faction_id;
    std::string class_id;
    // Modules fitted at the home yard (tier 2, docs/plans/ship_structures_tier2.md). The
    // ship flies the design class_id + fit (Simulation::ship_design).
    std::vector<std::string> fit;
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
    std::vector<std::string> refit_fit;   // and the modules it leaves the yard with
    double leg_dose_msv {0.0};            // radiation dose of the crew hired for this leg
    // Wear (tier 2, step H): share of the drive's rated life used, share of radiator heat
    // pipes intact, and whether the yard visit under way is an overhaul.
    double engine_wear {0.0};
    double radiator_condition {1.0};
    bool overhaul {false};
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
    std::vector<PassengerLot> passengers;  // aboard (only in flight)
};

// A station's money flows with the world outside the simulated trade (the open economy),
// all amounts positive, in credits.
struct StationLedger {
    double household_sales {0.0};       // residents paid for goods they used up, at what the station paid (step 17)
    double producer_purchases {0.0};    // the market paid local producers and its depot for new output
    double dividends {0.0};             // paid in by the ships it owns
    double subsidies {0.0};             // from the faction treasury (core-crew top-up and money-supply controller)
    double taxes {0.0};                 // to the faction treasury (band ceiling and money-supply controller)
    double passages_paid {0.0};         // passengers' fares it paid (rotation, sponsored settlers)
    double passages_recovered {0.0};    // recovered from its residents through its prices
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
    // People waiting here for a ship (passengers), in the order their groups formed.
    std::vector<PassengerGroup> waiting {};
    // Passages paid and not yet recovered from the residents.
    double passage_debt_cr {0.0};
    // Visitors staying here until they go home (since_s: when they leave for the ship).
    std::vector<PassengerGroup> visiting {};
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
    double propellant_boiled_kg {0.0};    // hydrogen expected to boil off in flight (in the load, not burned)
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
