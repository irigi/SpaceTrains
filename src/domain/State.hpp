#pragma once

// The running game: ships, stations, ledgers, orders and the UI snapshot.

#include "domain/Definitions.hpp"

namespace spacetrains::domain {

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
    // A salvage run (failures and salvage): the derelict the ship races for and when its path
    // meets it; salvage_done once it got there (won or lost).
    std::string salvage_derelict_id;
    double rendezvous_time_s {0.0};
    bool salvage_done {false};
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

// Passengers (docs/modules/ships_and_fleet.md). Why a person travels decides who pays and what
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
    // Modules fitted at the home yard (tier 2, docs/modules/ships_and_fleet.md). The
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
    // Failures (data/economy/failures.csv): when the hull was built (before the start for the
    // starting fleet) and when it fails (0: no clock). A clock that runs out in port arms
    // fault_pending: the fault then shows on the next interplanetary leg, at fault_leg_fraction of it.
    double built_s {0.0};
    double fails_at_s {0.0};
    bool fault_pending {false};
    double fault_leg_fraction {0.5};
    double next_salvage_look_s {0.0};   // a ship that passed on the derelicts looks again from then
    double last_inspected_s {-1.0e18};  // police inspections: when a cutter last inspected it
};

// A police cutter (police inspections): at its home port, on its way to meet a ship, alongside it
// for the inspection, or on its way home. Not in the fleet.
enum class PatrolPhase { Docked, Outbound, Inspecting, Returning };

struct PatrolState {
    std::string id;
    std::string name;
    std::string faction_id;
    std::string class_id;
    std::string home_station_id;
    PatrolPhase phase {PatrolPhase::Docked};
    double propellant_kg {0.0};
    std::string target_ship_id;
    std::string target_name;
    double meet_s {0.0};
    double inspect_until_s {0.0};
    double arrive_s {0.0};
    math::Vec3d meet_position_m;        // the target's state when the inspection began
    math::Vec3d meet_velocity_mps;
    std::vector<math::Vec3d> path;      // the leg flown now (outbound or home)
    std::vector<double> times;
    std::vector<double> propellant;
    double next_review_s {0.0};
    int inspections {0};
};

// A ship that failed in flight (failures and salvage): it coasts on the orbit it had, with its
// cargo, until a salvager reaches it or the claim period runs out. Not in the fleet.
struct DerelictState {
    std::string id;
    std::string name;
    std::string faction_id;
    std::string class_id;
    std::vector<std::string> fit;
    std::string home_station_id;
    std::string origin_station_id;       // of the leg it failed on
    std::string destination_station_id;
    std::vector<CargoLot> cargo;
    double ship_value_cr {0.0};
    double failed_at_s {0.0};
    double expires_s {0.0};
    math::Vec3d epoch_position_m;        // heliocentric state at failure; the coast follows from it
    math::Vec3d epoch_velocity_mps;
    std::vector<math::Vec3d> sampled_path;   // the coast, for display and the racers' targets
    std::vector<double> sampled_times_s;
    std::string claimant_ship_id;        // the racer with the earliest rendezvous
    double claim_eta_s {0.0};
    int racers {0};
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
    // Economy overhaul (households and background economies).
    double wages_paid {0.0};            // to its residents
    double household_spending {0.0};    // residents' spending at the station beyond the goods they used up
    double surface_bought {0.0};        // paid to background economies for goods (Earth at its ask)
    double surface_sold {0.0};          // paid by background economies for goods (Earth at its bid)
    double incomes {0.0};               // science budgets, services, subsidies
    double visitor_spending {0.0};      // visitors' spending during their stay
    double yard_sales {0.0};            // the yard's share of new ships
    double ship_sales {0.0};            // ships paid it for cargo, fuel, provisions, refits and repairs
    double ship_purchases {0.0};        // it paid ships for cargo they delivered
    double days_in_red {0.0};           // days its credits stood below zero
    double crew_wages {0.0};            // its ships' crews' wages, paid to its households
    double hired_from_surface {0.0};    // people who came up from its colony's surface for its jobs
    double moved_to_surface {0.0};      // its unemployed who went down to work on the surface
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
    // The residents' savings (households on): wages in, spending out.
    double household_credits {0.0};
    // What the residents may still spend on long trips: travel_share of their wages accrues to it.
    double travel_budget_cr {0.0};
    // Labour market (phase B): jobs at the station and its wage per worker per day.
    double jobs {0.0};
    double wage_cr_per_day {0.0};
    double employed_share {1.0};   // what its plants run on (jobs / workers, at most 1, in half-percent steps)
    // Phase C (not saved: set from prices and wages at the start of every tick): what each
    // output's plants run at for its price (supply responds to price), and what residents use
    // of each good they consume for its price and their income (demand responds to both).
    Inventory supply_factor {};
    Inventory demand_factor {};
};

struct SimulationSnapshot {
    double game_time_s {0.0};
    // The outside economy: residents and local producers (what they paid into the simulated
    // economy is negative) and the faction treasuries. Station + ship + outside + treasury
    // credits always equal the seeded money.
    double outside_economy_credits {0.0};
    // Background economies other than Earth (Earth is outside_economy_credits).
    std::map<std::string, double> background_credits;
    std::map<std::string, double> background_population;
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

}  // namespace spacetrains::domain
