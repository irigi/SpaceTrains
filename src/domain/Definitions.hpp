#pragma once

// The universe as loaded from data: bodies, stations, goods, ship classes and every economy setting.

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
    // Passenger berths (docs/modules/ships_and_fleet.md): built-in cabins of a liner class, plus a fitted
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
// build_ship_classes.py; tier 2, docs/modules/ships_and_fleet.md). Its part joins the
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

// Ship failures, derelicts and salvage (data/economy/failures.csv; docs/modules/ships_and_fleet.md,
// "Failures and salvage"). Off by default: with `failures` off nothing below acts.
struct FailureDefinition {
    bool failures {false};
    bool salvage {false};               // derelicts can be salvaged (otherwise they drift until they expire)
    double weibull_shape {3.0};         // k: hazard grows as age^(k-1), so new ships rarely fail
    // Mean life = lifetime_years (the capital charge's amortisation) x this factor: the charge then
    // covers, in expectation, the hull and the cargo lost with it (see the module doc).
    double mean_life_factor {1.1};
    double scrap_fraction {0.2};        // of the ship value, paid by the outside economy to the salvager
    double claim_days {365.0};          // an unsalvaged derelict is written off after this
    double max_racers {3.0};            // ships that may set out for one derelict
    double racer_margin {0.05};         // a new racer must beat the best rendezvous time by this share
    double salvage_review_days {0.5};   // how often idle ships look at open derelicts
    double replacement_days {180.0};    // the owner's first right to the lost ship's place under the cap
};

// Police inspections (data/economy/patrols.csv and patrol_bases.csv; docs/modules/ships_and_fleet.md,
// "Police inspections"): faction cutters meet ships bound for the faction's stations in flight and
// inspect them for a few days. The inspected ship keeps its trajectory (the law does not make it
// change course); the cutter matches it.
struct PatrolBaseDefinition {
    std::string faction_id;
    std::string station_id;
    int ships {1};
};

struct PatrolDefinition {
    bool patrols {false};
    std::string class_id {"plasma_courier"};   // cutters are couriers in faction service
    double inspection_days {3.0};
    double reinspect_days {120.0};     // a ship inspected is left alone this long
    double review_days {2.0};          // a cutter in port looks for a target this often
    double outbound_share {0.4};       // of the load the way to a meeting may burn
    double return_reserve {0.1};       // of the load kept when back home
    std::vector<PatrolBaseDefinition> bases;
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
    std::string site {"orbit"};   // where the habitat is: orbit, surface, underground or aerostat (display only)
};

// The station systems view (data/stations/works.csv, station_systems.csv; display only, see
// docs/plans/station_structures.md, "Light view"). A works makes one or more of a station's
// outputs; its jobs are its share of the station's jobs, split by kind with a typical staffing.
struct StationWorksDefinition {
    std::string profile_id;      // "*" for every profile without a row of its own
    std::string commodity_id;
    std::string works_id;
    std::string name;
    std::string kind;            // farm, mine, refinery, plant, fab, lab, fuel_works
    double labour_share {1.0};   // jobs per unit of output value, relative
    std::map<std::string, double> staffing;   // pop kind -> share of the works' jobs
};

struct StationSystemsDefinition {
    double services_share {0.0};                       // least share of the station's jobs in life support, port, administration
    double works_jobs_per_value {0.0};                 // works jobs per cr/day of output value x labour share
    std::map<std::string, double> services_staffing;   // pop kind -> share
    std::map<std::string, double> need_kg_per_person_day;
    std::vector<std::string> life_needs;
};

// Propellant supply (data/economy/fuel_supply.csv, fuel_factories.csv). Every station has a
// depot outside its cargo storage; only fuel factories fill theirs, at their output rate, and
// price it on their buffer. Other depots hold what ships deliver, bought under orders (step 29).
struct FuelSupplyDefinition {
    double factory_buffer_days {0.0};         // a factory's depot holds this many days of output
    std::unordered_map<std::string, double> factory_output_units_per_day;  // by station id
    // What a factory's fuel costs where it is made, per unit (economy overhaul: local propellant is far
    // cheaper than Earth's launched price); absent: the base price.
    std::unordered_map<std::string, double> factory_price_cr;
};

// The open economy (data/economy/open_economy.csv). Faction treasuries keep each station's
// balance inside a band, ships pay the cash above a working reserve to their owner (the home
// station), and a slow controller transfers money per head of population to hold the money
// supply in stations and ships near the seeded amount.
struct OpenEconomyDefinition {
    double station_credit_floor {0.0};     // subsidies close the gap below this
    double station_credit_ceiling {0.0};   // taxes take the excess above this; 0 = no taxes
    double station_credit_ceiling_in_debt {0.0};   // the ceiling while the faction is in debt; 0 = the same
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

// Station purchase orders (step 29, docs/modules/economy.md; data/economy/orders.csv).
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

// Passengers (docs/modules/ships_and_fleet.md; data/economy/passengers.csv, every key optional). Off by
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

// Market redesign (2026-10-19, docs/modules/economy.md): optional keys in
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

// Background economies (docs/modules/economy.md): Earth's surface and a few small
// colonies the game does not simulate in detail (data/economy/background_economies.csv). Each
// has an account and trades with its gateway stations at its surface price plus launch (it
// sells) or minus landing (it buys); Earth is the old outside account.
struct BackgroundEconomyDefinition {
    std::string id;
    std::string name;
    std::vector<std::string> gateways;
    double launch_cr_per_kg {0.0};
    double landing_cr_per_kg {0.0};
    double initial_credits {0.0};        // how far it may spend below zero (its credit line); Earth has no limit
    double income_cr_per_day {0.0};      // Earth's investment in it (paid from Earth's account)
    // A colony's people (labour market): they take the gateway station's jobs when the wage gain beats
    // the ascent, and the station's unemployed go down to work on the surface. Earth: 0 (not modelled).
    double population {0.0};
    double growth_per_year {0.0};
    double wage_median_cr_per_day {0.0};
    double wage_sigma {0.0};
    double search_per_year {0.0};        // share of its people looking for a better job each year
    double immigration_share {0.0};      // of its growth, settlers it recruits on Earth and pays passage for
};

// One good a background economy trades at a gateway (data/economy/surface_markets.csv,
// prices derived at load from surface $/kg, the launch and landing costs and credits_per_usd).
struct SurfaceMarketDefinition {
    std::string economy_id;
    std::string station_id;
    std::string commodity_id;
    double ask_cr {0.0};               // it sells to the station at this price per unit (0: does not sell)
    double bid_cr {0.0};               // it buys from the station at this price per unit (0: does not buy)
    double sell_units_per_day {0.0};   // most it sells a day; 0 = any amount (keeps the stock topped up)
    double buy_units_per_day {0.0};    // most it buys a day; 0 = any amount (takes the whole stock)
    // A bid set from the station's own reference price instead (small colonies' imports; Earth's
    // propellant for its own traffic): bid = bid_share x reference price (0: bid_cr as given).
    double bid_share {0.0};
};

// Money a station receives from a background economy every day for what it does there
// (data/economy/station_incomes.csv): science budgets, relay and traffic services,
// settlement subsidies.
struct StationIncomeDefinition {
    std::string station_id;
    std::string kind;
    std::string payer_id;   // background economy
    double cr_per_day {0.0};
    // A grant paid to need (science, settlement): full while the station holds less than half the
    // credit ceiling, falling to nothing at the ceiling. Otherwise a paid service (relay, tourism).
    bool need_based {false};
};

// Residents as an account of their own (docs/modules/economy.md): the station pays
// them wages; they pay for what they use up and spend the rest at the station, keeping a
// buffer of savings (data/economy/households.csv).
struct HouseholdDefinition {
    double enabled {0.0};
    double participation {0.6};          // workers per resident
    double savings_days {90.0};          // the buffer households keep, in days of their wages
    double spend_days {60.0};            // time to spend savings above the buffer
    std::unordered_map<std::string, double> wage_multiple;   // station id -> multiple of the reference wage
    double migrant_savings_cr {0.0};     // a settler from a background economy brings this
    double visitor_spend_cr_per_day {0.0};   // a visitor spends this at the station per day of the stay
    double credits_per_usd {0.00078};    // the hardware price scale (ships) used for surface prices
    double surface_depth_units {0.0};    // stock a surface keeps at its gateway of each good it sells in any amount
    double yard_share {0.0};             // of a new ship's price paid to its yard station (the rest to Earth)
    double yard_materials_share {0.0};   // of the price, the goods the yard builds it from (from its stock)
    double travel_share {0.0};           // of residents' wages they put aside for long trips (0: no limit beyond savings)
    double science_price_premium {0.0};  // a station with a science budget centres its import prices this much above landed cost
    std::vector<std::string> yards;      // stations that build ships
    // Phase C: supply and demand respond to prices and incomes (0 elasticities: off).
    double supply_price_elasticity {0.0};    // plants run at (price / reference)^e of capacity...
    double min_utilization {0.1};            // ...but never below this
    double consume_income_elasticity {0.0};  // residents' use of `consume` goods: (wage / starting wage)^e...
    double consume_price_elasticity {0.0};   // ...x (price / reference)^e
    double upkeep_price_elasticity {0.0};    // life support and upkeep goods: (price / reference)^e
    // Colonies (the small background economies; docs/modules/economy.md, "Colonies"). 0: off.
    double colony_centre_markup {0.0};       // a gateway centres a good its colony sells (and it does not make) at ask x this
    double colony_stock_days {0.0};          // ...and stocks this many days of the colony's sales (0: 14 days of its net rate)
    std::unordered_map<std::string, double> colony_stock_days_by_good;   // per good (small, valuable lots: a year)
    double colony_import_buffer_days {0.0};  // a colony buys in full above this many days of its imports in hand
    double colony_satisfaction_days {90.0};  // its import satisfaction is averaged over this many days
    double colony_people_scaling {0.0};      // 1: its sales and imports scale with its people
    double colony_growth_follows_satisfaction {0.0};   // 1: growth x (2 x satisfaction - 1)
    double colony_wage_follows_income {0.0};           // 1: its wage median follows its income per head
    double colony_wage_relaxation_days {365.0};
    std::vector<std::string> colony_essentials;  // goods a short colony keeps buying first (food, medicine)
};

// Labour market (docs/modules/economy.md, phase B; data/economy/labour_market.csv).
// Every station off Earth has jobs and a wage per worker; wages follow vacancies; people move
// for the gain a job brings them, and pay their passage out of that gain. Wages and incomes are
// lognormal distributions, so a low-paid worker on Earth may gain from a job a well-paid one
// would not take.
struct LabourMarketDefinition {
    double enabled {0.0};
    double earth_wage_median_cr_per_day {0.107};   // per worker ($50k a year)
    double earth_wage_sigma {0.9};                 // log-spread of Earth wages (and incomes)
    double station_wage_sigma {0.35};              // log-spread of a station's wages around its level
    double stay_years {5.0};                       // how long a settler expects to work there
    double hardship_share {0.2};                   // of the station wage, what life there costs a worker in comfort
    double wage_adjust_per_year {1.0};             // d ln(wage)/dt per unit vacancy rate
    double min_wage_multiple {0.3};                // of the reference wage
    double vacancy_fill_days {120.0};              // vacancies are posted to Earth's job seekers over this time
    double unemployed_leave_days {180.0};          // the unemployed look for a passage home over this time
    double unemployment_benefit {0.3};             // of the wage, paid by the station to its unemployed
    double visitor_budget_years {0.5};             // a trip's budget, in years of income
    double visitor_income_sigma {1.2};             // log-spread of travellers' budgets
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
    FailureDefinition failures;
    PatrolDefinition patrols;
    LabourDefinition labour;
    std::unordered_map<std::string, SparePartDefinition> spare_parts;  // by sub-part kind
    std::unordered_map<std::string, StationRadiationDefinition> station_radiation;
    std::vector<StationWorksDefinition> station_works;   // empty: no systems view
    StationSystemsDefinition station_systems;
    // Module slots by hull id and slot (data/ship_classes/hull_slots.csv).
    std::unordered_map<std::string, std::unordered_map<std::string, int>> hull_slots;
    std::vector<StationDefinition> stations;
    std::vector<RecipeDefinition> recipes;
    std::vector<ExportMarketDefinition> export_markets;
    std::vector<BackgroundEconomyDefinition> background_economies;
    std::vector<SurfaceMarketDefinition> surface_markets;
    std::vector<StationIncomeDefinition> station_incomes;
    HouseholdDefinition households;
    LabourMarketDefinition labour_market;
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

}  // namespace spacetrains::domain
