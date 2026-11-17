#pragma once

// The running game's state: everything Simulation holds, as plain data (docs/plans/simulation_split.md).
// Step 1 of the split: the members are here verbatim and Simulation inherits them privately; the
// rules still live in Simulation's member functions. Later steps move the rules into systems that
// take World& (or const World&), group these members, and hold the World behind a pointer.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "celestial/CelestialMechanics.hpp"
#include "domain/Types.hpp"
#include "economy/EconomySystem.hpp"
#include "simulation/FlowRecord.hpp"
#include "simulation/MoneyJournal.hpp"
#include "trajectory/TrajectoryAudit.hpp"
#include "trajectory/TrajectoryPlanner.hpp"
#include "trajectory/VariableIspTrajectoryPlanner.hpp"
#include "util/ThreadPool.hpp"
#include "variable_isp/VariableIsp.hpp"

namespace spacetrains::simulation {

// Audit (tier 2, crew radiation): since start or load, not saved.
struct RadiationAudit {
    double crew_dose_msv {0.0};      // person-mSv over the fleet
    double hazard_pay_cr {0.0};
    double highest_leg_dose_msv {0.0};
    int particle_events {0};
    double largest_event_magnitude {0.0};
    double hydrogen_boiled_kg {0.0};   // step F
    // Nuclear-thermal arrivals: short of the arrival burn, or with under half the reserve left.
    int thermal_arrivals {0};
    int arrivals_short_of_burn {0};
    int arrivals_below_half_reserve {0};
    double lowest_reserve_share {1.0e9};   // fuel left on arrival / planned reserve
    int overhauls {0};                 // step H
    double repair_bills_cr {0.0};
    std::map<std::string, double> spares_units;
    int repairs_waiting_for_spares {0};
    int thermal_overhauls {0};
    int plasma_overhauls {0};
    double repair_spares_cr {0.0};
    double repair_labour_cr {0.0};
    double repair_days {0.0};
};

// Audit (failures and salvage): since start or load, not saved.
struct FailureAudit {
    int failures {0};
    int local_recoveries {0};          // failed on a leg near one planet: towed in by the station
    int faults_armed_in_port {0};      // clocks that ran out docked (the fault shows on the next leg)
    double age_years_sum {0.0};
    std::map<std::string, int> by_class;
    double hulls_lost_cr {0.0};
    double cargo_lost_cr {0.0};        // what the owners paid for the cargo aboard at failure
    int derelicts {0};
    int salvaged {0};
    int expired {0};
    int racers_sent {0};
    int races_lost {0};
    double cargo_recovered_cr {0.0};   // at reference value at the salvager's destination
    double scrap_paid_cr {0.0};
    int replacements_bought {0};
    int replacement_claims_lapsed {0};
    // Exposure: daily samples of the cargo value aboard ships in flight and of the fleet's hull
    // value, for the mean-life factor (cargo at risk per unit of hull value).
    double cargo_aboard_cr_days {0.0};
    double hull_value_cr_days {0.0};
};

// Audit (police inspections): since start or load, not saved.
struct PatrolAudit {
    int inspections {0};
    int sorties {0};
    int no_target_reviews {0};
    int returns_replanned_short {0};   // a way home that needed the reserve, or none found
    double meeting_days_sum {0.0};     // from setting out to meeting
    double propellant_kg {0.0};
    double costs_cr {0.0};             // running costs and fuel, paid by the treasuries
    std::map<std::string, int> by_faction;
};

// Audit (passengers): since start or load, not saved. Index by PassengerKind.
struct PassengerAudit {
    std::array<double, 6> posted {};       // people who began to wait
    std::array<double, 6> boarded {};
    std::array<double, 6> landed {};
    std::array<double, 6> gave_up {};
    std::array<double, 6> fares_cr {};
    std::array<double, 6> wait_days {};    // person-days waited by those who boarded
    std::array<double, 6> flight_days {};  // person-days aboard
    double berth_days {0.0};               // berths flown (load factor = flight_days / berth_days)
    double colony_recruits_landed {0.0};   // settlers a colony recruited, landed at its gateway (they go down)
    int departures_with_passengers {0};
    // Per route (origin, destination), all kinds together.
    struct Route {
        double posted {0.0};
        double boarded {0.0};
        double landed {0.0};
        double gave_up {0.0};
        double flight_days {0.0};      // person-days aboard of those who landed
        double max_leg_dose_msv {0.0}; // highest planned leg dose of a boarding
        std::map<std::string, int> carriers;  // boardings by ship design
        int departures {0};             // every leg flown on this route
        int berth_departures {0};       // legs by ships with berths
        int empty_with_waiting {0};     // ...that left with nobody aboard while people waited
    };
    std::map<std::pair<std::string, std::string>, Route> routes;
};

// Rotation (step 4): each off-Earth station's tour, which minimises its cost per day on site,
// T* = sqrt(C x 365 / (eta x wage)), C the round trip's fares, time and dose; and the fares
// per position-day it costs. From the reference fares; empty without rotation.
struct RotationTour {
    std::string out_gateway;
    std::string home_gateway;
    double tour_days {0.0};
    double fares_per_day {0.0};   // per position
};

// A colony's welfare (docs/modules/economy.md, "Colonies"), by economy id.
struct ColonyState {
    double satisfaction {1.0};         // imports bought / its base imports (value), averaged
    double throttle {1.0};             // its affordability now (1: buys in full)
    double income_per_day {0.0};       // its sales and Earth's investment, averaged
    double base_imports_per_day {0.0};   // its base imports' value (people-scaled) at the bid
    double wage_median {0.0};          // its wage median now (cr per day)
};

// A plan a pass asked for: computed on a worker thread, then stored on the simulation thread.
struct DeferredPlan {
    std::function<domain::TrajectoryPlan()> compute;
    std::function<void(domain::TrajectoryPlan&&)> commit;
};

// Two-leg mission scoring: a cheap, cached estimate of a follow-up leg.
struct LegEstimate {
    bool feasible {false};
    double travel_days {0.0};      // from the caller's departure
    double propellant_kg {0.0};
    double arrival_time_s {0.0};   // cached; travel_days is derived per caller
};

// One cargo run a fleet review's probe found for a new ship (World::review_probes_).
struct ProbedRun {
    std::string destination_id;
    std::string commodity_id;
    double cargo_units {0.0};
    double travel_days {0.0};
    double wait_days {0.0};
    double fuel_cost {0.0};
    int run {-1};
    std::string module_id {};  // a passenger run for the hull with this cabin module
    double return_fuel_kg {-1.0};  // see MissionChoice::CargoOption
};

// The simulation advances in fixed ticks of TICK_S game seconds (Simulation::TICK_S).
inline constexpr double TICK_S = 0.1 * 86400.0;

// The pseudo-good fleet investment's probes and route commitments name passenger runs by
// (Simulation::PASSENGER_GOOD).
inline constexpr const char* PASSENGER_GOOD = "passengers";

struct World {
    // The engines keep references into universe_ and mechanics_, so these three come first.
    explicit World(domain::UniverseDefinition universe)
        : universe_(std::move(universe)),
          mechanics_(universe_),
          economy_(universe_) {}

    domain::UniverseDefinition universe_;
    celestial::CelestialMechanics mechanics_;
    economy::EconomySystem economy_;
    variable_isp::VariableIspAtlas atlas_;
    bool atlas_loaded_ {false};
    std::unique_ptr<trajectory::KeplerTrajectoryPlanner> kepler_planner_;
    std::unique_ptr<trajectory::VariableIspTrajectoryPlanner> variable_isp_planner_;
    double game_time_s_ {0.0};
    double untimed_s_ {0.0};   // game time step() was given that no tick has used yet
    double outside_economy_credits_ {0.0};   // Earth's economy (the outside account)
    // The other background economies' accounts (economy overhaul), by id.
    std::map<std::string, double> background_credits_;
    // The colonies' people (labour market), by economy id.
    std::map<std::string, double> background_population_;
    std::map<std::string, ColonyState> colony_state_;
    std::map<std::string, double> faction_treasuries_;
    // The money journal and its account indices (MoneyJournal.hpp).
    MoneyJournal journal_;
    FlowRecord flows_;
    std::unordered_map<std::string, int> colony_accounts_;
    std::unordered_map<std::string, int> faction_accounts_;
    std::unordered_map<std::string, int> fleet_accounts_;
    int first_station_account_ {0};
    int first_household_account_ {0};
    double seeded_money_supply_ {0.0};   // grows by the working capital of new ships
    double next_investment_review_s_ {0.0};
    int investment_purchases_left_ {0};   // of the current review, one per tick
    double next_purchase_s_ {0.0};        // no purchase before this (purchase_spacing_days)
    // A review's probes, by "hull class|yard": the cargo runs a new ship could fly. Saved with
    // the game (the review spans several ticks).
    std::map<std::string, std::vector<ProbedRun>> review_probes_;
    domain::FleetInvestmentLedger investment_ledger_;
    // Step 17: taxes each faction collects per day (365-day average) and interest paid on debt.
    std::map<std::string, double> faction_tax_per_day_;
    double faction_interest_paid_ {0.0};
    // Emergency deliveries open now, and the totals since the start.
    std::vector<domain::Emergency> emergencies_;
    int emergencies_opened_ {0};
    // Random events (step 28): the generator (seeded from the data and the start date) and
    // how many of each event have struck.
    std::mt19937_64 event_rng_ {0};
    bool events_enabled_ {true};
    std::map<std::string, int> events_started_;
    double emergency_paid_ {0.0};   // by the faction treasuries, in premiums
    std::vector<domain::ShipState> sold_ships_;
    // Failures and salvage: derelicts drifting now, and each faction's replacement claims (when
    // each runs out). Saved.
    std::vector<domain::DerelictState> derelicts_;
    std::map<std::string, std::vector<double>> replacement_claims_;
    double next_salvage_review_s_ {0.0};
    bool replacement_tried_ {false};   // this review's replacement purchase found nothing worth buying
    double next_exposure_sample_s_ {0.0};
    FailureAudit failure_audit_;           // audit only, not saved
    std::vector<domain::PatrolState> patrols_;   // police cutters (saved)
    PatrolAudit patrol_audit_;             // audit only, not saved
    double timewarp_factor_ {3600.0};
    std::vector<domain::StationState> stations_;
    std::vector<domain::ShipState> ships_;
    std::vector<domain::EventEntry> recent_events_;
    // Each station's stock at the start (from the data, so a loaded game rebuilds the same):
    // lifetime profit counts the stock's value gain against it. (Recomputed from today's rates
    // it grew with the population, and the reported profit drifted.)
    std::unordered_map<std::string, domain::Inventory> starting_inventories_;
    std::vector<domain::TradeEntry> recent_trades_;
    double delivered_paid_cr_ {0.0};       // audit only, not saved
    RadiationAudit radiation_audit_;       // audit only, not saved
    PassengerAudit passenger_audit_;       // audit only, not saved
    std::map<std::string, RotationTour> rotation_tours_;  // from data, not saved
    mutable std::unordered_map<std::string, std::pair<double, double>> station_radius_cache_;  // time, AU
    double delivered_reference_cr_ {0.0};
    bool trajectory_audit_enabled_ {false};
    std::vector<trajectory::TrajectoryAuditRecord> trajectory_audit_records_;
    // estimate_leg() cache: departure bucket -> "class|from|to|fuel bucket" -> estimate.
    std::map<std::int64_t, std::unordered_map<std::string, LegEstimate>> leg_estimates_;
    // Plans the current choose_mission pass is missing (null outside choose_mission).
    std::vector<DeferredPlan>* deferred_plans_ {nullptr};
    std::unordered_set<std::string> deferred_keys_;
    std::string deferred_prefix_;   // keeps one request's private plan keys apart from another's
    std::size_t missing_plans_ {0};  // plans read as missing, queued now or by an earlier request
    std::unique_ptr<util::ThreadPool> thread_pool_;
    std::unordered_map<std::string, const domain::StationDefinition*> station_defs_by_id_;
    std::unordered_map<std::string, const domain::ShipClassDefinition*> ship_classes_by_id_;
    // Fitted designs by design id. A std::map, so references handed out stay valid; derived
    // from data and fits only, never saved.
    std::map<std::string, domain::ShipClassDefinition> designs_;
    std::unordered_map<std::string, const domain::ShipModuleDefinition*> modules_by_key_;  // "hull|module"
    std::unordered_set<std::string> active_effects_;  // module effect keys switched on
    // Solar particle events draw from their own stream, so station events stay as they were.
    std::mt19937_64 environment_rng_;
};

}  // namespace spacetrains::simulation
