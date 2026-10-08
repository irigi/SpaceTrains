#pragma once

#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "celestial/CelestialMechanics.hpp"
#include "data_loader/DataLoader.hpp"
#include "domain/Types.hpp"
#include "economy/EconomySystem.hpp"
#include "trajectory/TrajectoryAudit.hpp"
#include "trajectory/TrajectoryPlanner.hpp"
#include "trajectory/VariableIspTrajectoryPlanner.hpp"
#include "util/ThreadPool.hpp"
#include "variable_isp/VariableIsp.hpp"

namespace spacetrains::simulation {

class Simulation {
public:
    explicit Simulation(domain::UniverseDefinition universe);

    [[nodiscard]] static Simulation from_data_root(const std::string& data_root);

    // The simulation advances in fixed ticks of TICK_S game seconds, whatever the timewarp
    // or the caller's step: step() adds real_dt_s x timewarp and runs the whole ticks due.
    // Headless runs and the UI therefore simulate the same game.
    static constexpr double TICK_S = 0.1 * 86400.0;
    void step(double real_dt_s);
    void set_timewarp(double timewarp_factor);

    [[nodiscard]] const domain::UniverseDefinition& universe() const;
    [[nodiscard]] const economy::EconomySystem& economy_system() const;
    [[nodiscard]] domain::SimulationSnapshot snapshot() const;
    [[nodiscard]] double game_time_s() const { return game_time_s_; }
    // Starts a fresh game at another date (planets elsewhere along their orbits): the same
    // economy in a different geometry, for benchmarks over several starts. Call before the
    // first step.
    void start_at(double time_s);

    // Save games (src/persistence/SimulationPersistence.cpp): the mutable state as JSON.
    // load_state_json() expects a Simulation built from the same data files (checked by
    // fingerprint) and throws, leaving the simulation unchanged, on a bad or foreign save.
    [[nodiscard]] std::string save_state_json() const;
    void load_state_json(const std::string& text);
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
    [[nodiscard]] std::vector<ReferencePrice> compute_reference_prices() const;
    // Tests and debugging: overwrite one good's stock at a station.
    void set_station_stock(const std::string& station_id, const std::string& commodity_id, double units);
    [[nodiscard]] std::string data_fingerprint() const;
    [[nodiscard]] std::string build_report() const;
    // The UI snapshot. Without paths, ships in flight carry only a "path_id"; their planned
    // paths are in build_bridge_paths_json(), which the bridge writes when
    // bridge_paths_signature() changes (paths were 70% of a 600 KB snapshot at 90 ships).
    [[nodiscard]] std::string build_bridge_snapshot_json(bool paused, std::uint64_t snapshot_seq, double snapshot_real_time_s,
        bool with_paths = true) const;
    [[nodiscard]] std::string build_bridge_paths_json() const;
    [[nodiscard]] std::string bridge_paths_signature() const;
    [[nodiscard]] double timewarp_factor() const;
    [[nodiscard]] math::Vec3d get_ship_render_position(const domain::ShipState& ship) const;
    [[nodiscard]] const std::vector<domain::TradeEntry>& recent_trades() const { return recent_trades_; }

    // When enabled, every accepted mission plan is checked by audit_trajectory()
    // and recorded (headless --trajectory-audit).
    void set_trajectory_audit_enabled(bool enabled) { trajectory_audit_enabled_ = enabled; }
    [[nodiscard]] const std::vector<trajectory::TrajectoryAuditRecord>& trajectory_audit_records() const {
        return trajectory_audit_records_;
    }

private:
    [[nodiscard]] const domain::ShipClassDefinition& get_ship_class(const std::string& class_id) const;
    [[nodiscard]] const domain::CelestialBodyDefinition& get_body_definition(const std::string& body_id) const;
    [[nodiscard]] const domain::StationDefinition& get_station_definition(const std::string& station_id) const;
    [[nodiscard]] domain::StationState& get_station_state(const std::string& station_id);
    [[nodiscard]] const domain::StationState& get_station_state(const std::string& station_id) const;
    [[nodiscard]] std::string mission_phase_name(domain::ShipMissionPhase phase) const;
    [[nodiscard]] std::string bridge_path_id(const domain::ShipState& ship) const;
    void write_ship_path_json(std::ostream& output, const domain::ShipState& ship) const;
    [[nodiscard]] const domain::CommodityDefinition& get_commodity(const std::string& commodity_id) const;
    [[nodiscard]] std::string faction_name(const std::string& faction_id) const;
    // What a station starts with (step 18): its data inventory, raised to its target stock for
    // every good it consumes, within 85% of its storage.
    [[nodiscard]] domain::Inventory starting_inventory(const domain::StationDefinition& station) const;
    [[nodiscard]] double station_price(const domain::StationState& state, const std::string& commodity_id) const;
    // Value of moving units into (+) or out of (-) a station, along its price curve.
    // Value of selling `units` at a station `days_ahead` from now, on its forecast stock.
    // The stock a delivery `days_ahead` from now lands on: today's run forward at the net
    // rate, with the same good other ships deliver before then.
    [[nodiscard]] double forecast_stock_on_arrival(const domain::StationState& state, const std::string& commodity_id,
        double days_ahead, const std::string& seller_ship_id) const;
    [[nodiscard]] double sale_value_on_arrival(const domain::StationState& state, const std::string& commodity_id,
        double units, double days_ahead, const std::string& seller_ship_id) const;
    // A sale's value and the part of it the station's faction pays (an emergency premium).
    struct SaleSplit {
        double total {0.0};
        double faction {0.0};
        double emergency_units {0.0};  // units sold at the emergency floor
    };
    // Selling `units` into a station whose stock is `stock`: along its curve, raised to an
    // open emergency's floor price for the units that land below its emergency cover.
    [[nodiscard]] SaleSplit sale_split(const domain::StationDefinition& station, const std::string& commodity_id,
        double stock, double units) const;
    [[nodiscard]] SaleSplit sale_split_on_arrival(const domain::StationState& state, const std::string& commodity_id,
        double units, double days_ahead, const std::string& seller_ship_id) const;
    [[nodiscard]] const domain::Emergency* find_emergency(const std::string& station_id, const std::string& commodity_id) const;
    // Opens, raises and closes emergency deliveries of life-support goods (step 14).
    void step_emergencies();
    [[nodiscard]] double trade_value(
        const domain::StationState& state, const std::string& commodity_id, double units_into_station) const;
    // Propellant the ship's current port will sell it (capped by free tank space).
    [[nodiscard]] double purchasable_propellant_kg(const domain::ShipState& ship) const;
    [[nodiscard]] double provisions_mass_kg(const domain::ShipState& ship) const;
    // Buys up to `kg` of propellant at the current port (missions fuel on departure).
    void buy_propellant(domain::ShipState& ship, double kg);
    // Operating economics (docs/plans/ship_operating_costs.md).
    [[nodiscard]] double daily_capital_cost(const domain::ShipClassDefinition& ship_class) const;
    [[nodiscard]] double daily_crew_cost(
        const domain::ShipClassDefinition& ship_class, const domain::StationState& market) const;
    [[nodiscard]] double credit_line(const domain::ShipClassDefinition& ship_class) const;
    // Days of life support the ship could carry from `market` (its own stock plus what the
    // station sells). Departing crews may buy from the whole stock (crew-scale amounts are
    // tiny); routine docked top-ups leave the station its own reserve.
    [[nodiscard]] double provisionable_days(const domain::ShipState& ship, const domain::StationState& market) const;
    void buy_provisions(domain::ShipState& ship, domain::StationState& market, double days, bool departing);
    void accrue_operating_costs(domain::ShipState& ship, double dt_s);
    void pay_home_station(domain::ShipState& ship, double amount);
    // Open economy: residents pay their station for the goods the economy step used up, at what
    // the station paid ships for them; local production is free (step 17). The other side of
    // every payment is the external account.
    void settle_local_economy(const std::vector<domain::Inventory>& stocks_before);
    // Dividends, the faction treasuries' subsidies, taxes and interest, and the money-supply controller.
    void step_treasuries(double dt_s);
    // Station money (step 17). Consumption per day of a station's goods at reference prices:
    // its upkeep goods only (for the core-crew subsidy), or every consumed good (its import bill).
    [[nodiscard]] double consumption_value_per_day(const domain::StationDefinition& station, bool upkeep_only) const;
    // What a faction may still spend on ships: its treasury plus what it may borrow.
    [[nodiscard]] double faction_credit_limit(const std::string& faction_id) const;
    [[nodiscard]] double faction_spendable(const std::string& faction_id) const;
    // Each station's cash plus credit line, less the contracts on their way to it; refreshed
    // every tick and lowered as contracts are made. A station that cannot pay offers less.
    void refresh_station_payable();
    // The share of a contract due at this time that reserves its station's money now.
    [[nodiscard]] double contract_reserve_share(double arrival_time_s) const;
    [[nodiscard]] double station_affordability(const domain::StationDefinition& station) const;
    void record_affordability();
    [[nodiscard]] double internal_money_supply() const;
    // Fleet investment (docs/plans/fleet_investment.md): owners sell ships laid up for long,
    // and every review the treasuries commission the ship with the best expected return.
    void step_fleet_investment();
    void sell_ship(std::size_t index);
    // One step of a fleet review: probes a batch of candidates (their cargo runs from the
    // yard, kept in review_probes_ for the rest of the review), or, once every candidate that
    // could still win has been probed, buys the best one.
    enum class CommissionStep { Probed, Bought, NothingToBuy };
    CommissionStep commission_step();
    void step_idle_ship(domain::ShipState& ship);
    // Dispatch's best mission for a ship flown as `ship_class` (its own class, or a tank
    // variant it could refit to). Planning only: changes nothing but the plan caches.
    struct MissionChoice {
        enum class Kind { None, Mission, Reposition };
        Kind kind {Kind::None};
        // Mission: two-leg profit per day (urgency-weighted for cargo runs);
        // Reposition: sourcing urgency, not credits.
        double score {0.0};
        const domain::StationDefinition* destination {nullptr};
        std::vector<domain::CargoLot> cargo;   // empty: an empty leg or repositioning
        // Cargo runs: sale value minus purchase and fuel, without urgency or time costs.
        double cargo_margin {0.0};
        domain::TrajectoryPlan plan;
        // What `plan` was planned with (without its path; step_idle_ship plans it again with it).
        trajectory::PlanningOptions plan_options {};
        double plan_departure_s {0.0};
        // Fuel-aware planning: return fuel loaded on top of the plan's load, for a port that
        // cannot refuel the ship.
        double carried_propellant_kg {0.0};
        // Missions: the follow-up load planned at the destination (see MissionAssignment).
        std::string pickup_commodity_id;
        double pickup_units {0.0};
        // Cargo-only probes (fleet investment): every feasible cargo run, which the
        // investment review values at its sustained flow and prices.
        struct CargoOption {
            const domain::StationDefinition* destination {nullptr};
            std::string commodity_id;
            double cargo_units {0.0};
            double travel_days {0.0};
            double wait_days {0.0};
            double fuel_cost {0.0};
            int run {-1};  // lots of one mixed hold share a run number (its fuel is on the first lot)
        };
        std::vector<CargoOption> cargo_options;
    };
    // `cargo_only`: only cargo runs from the ship's port (no empty legs, no repositioning).
    [[nodiscard]] MissionChoice choose_mission(const domain::ShipState& ship,
        const domain::ShipClassDefinition& ship_class, bool trace, double earliest_departure_s,
        bool cargo_only = false);
    // choose_mission for several ships at once (fleet investment probes, refit variants):
    // their plans are computed together, so the pool stays busy. Same results as one by one.
    struct MissionRequest {
        const domain::ShipState* ship {nullptr};
        const domain::ShipClassDefinition* ship_class {nullptr};
        double earliest_departure_s {0.0};
        bool cargo_only {false};
    };
    [[nodiscard]] std::vector<MissionChoice> choose_missions(const std::vector<MissionRequest>& requests);
    // One pass of choose_mission. Plans it has no result for yet are queued in
    // deferred_plans_ and read as infeasible; choose_mission computes the queue in parallel
    // and repeats the pass until nothing is missing, so the last pass is exactly the
    // sequential result.
    [[nodiscard]] MissionChoice choose_mission_pass(const domain::ShipState& ship,
        const domain::ShipClassDefinition& ship_class, bool trace, double earliest_departure_s,
        bool cargo_only, std::unordered_map<std::string, domain::TrajectoryPlan>& plans, std::string& trace_text);
    // A plan a pass asked for: computed on a worker thread, then stored on the simulation thread.
    struct DeferredPlan {
        std::function<domain::TrajectoryPlan()> compute;
        std::function<void(domain::TrajectoryPlan&&)> commit;
    };
    // Queues a plan once per key and pass.
    void defer_plan(const std::string& key, DeferredPlan request);
    // Part C: at its home base a ship may refit to another tank variant of its hull when
    // the missions that opens repay the yard bill. Returns true if the ship went into the yard.
    bool consider_refit(domain::ShipState& ship, const MissionChoice& current, bool trace);
    [[nodiscard]] double refit_bill(
        const domain::ShipClassDefinition& from, const domain::ShipClassDefinition& to) const;
    void step_refitting_ship(domain::ShipState& ship);
    // Two-leg mission scoring: a cheap, cached estimate of a follow-up leg.
    struct LegEstimate {
        bool feasible {false};
        double travel_days {0.0};      // from the caller's departure
        double propellant_kg {0.0};
        double arrival_time_s {0.0};   // cached; travel_days is derived per caller
    };
    [[nodiscard]] LegEstimate estimate_leg(
        const domain::ShipClassDefinition& ship_class,
        const domain::StationDefinition& origin,
        const domain::StationDefinition& destination,
        double departure_time_s,
        double available_propellant_kg,
        double payload_kg);
    void step_awaiting_departure_ship(domain::ShipState& ship);
    void step_in_transit_ship(domain::ShipState& ship, double dt_s);
    void add_event(std::string text, std::string category = "general");
    void record_trade(domain::TradeEntry trade);
    void record_trajectory_audit(
        const domain::ShipState& ship,
        const domain::StationDefinition& origin,
        const domain::StationDefinition& destination,
        const domain::TrajectoryPlan& plan,
        double planning_propellant_kg);

    domain::UniverseDefinition universe_;
    celestial::CelestialMechanics mechanics_;
    economy::EconomySystem economy_;
    variable_isp::VariableIspAtlas atlas_;
    bool atlas_loaded_ {false};
    std::unique_ptr<trajectory::KeplerTrajectoryPlanner> kepler_planner_;
    std::unique_ptr<trajectory::VariableIspTrajectoryPlanner> variable_isp_planner_;
    double game_time_s_ {0.0};
    double untimed_s_ {0.0};   // game time step() was given that no tick has used yet
    void tick();
    double outside_economy_credits_ {0.0};
    std::map<std::string, double> faction_treasuries_;
    double seeded_money_supply_ {0.0};   // grows by the working capital of new ships
    double next_investment_review_s_ {0.0};
    int investment_purchases_left_ {0};   // of the current review, one per tick
    // A review's probes, by "hull class|yard": the cargo runs a new ship could fly. Saved with
    // the game (the review spans several ticks).
    struct ProbedRun {
        std::string destination_id;
        std::string commodity_id;
        double cargo_units {0.0};
        double travel_days {0.0};
        double wait_days {0.0};
        double fuel_cost {0.0};
        int run {-1};
    };
    std::map<std::string, std::vector<ProbedRun>> review_probes_;
    domain::FleetInvestmentLedger investment_ledger_;
    // Step 17: taxes each faction collects per day (365-day average), interest paid on debt,
    // and what each station can still pay for (not saved: recomputed every tick).
    std::map<std::string, double> faction_tax_per_day_;
    double faction_interest_paid_ {0.0};
    std::unordered_map<std::string, double> station_payable_;
    // Emergency deliveries open now, and the totals since the start.
    std::vector<domain::Emergency> emergencies_;
    int emergencies_opened_ {0};
    double emergency_paid_ {0.0};   // by the faction treasuries, in premiums
    std::vector<domain::ShipState> sold_ships_;
    double timewarp_factor_ {3600.0};
    std::vector<domain::StationState> stations_;
    std::vector<domain::ShipState> ships_;
    std::vector<domain::EventEntry> recent_events_;
    std::vector<domain::TradeEntry> recent_trades_;
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
};

}  // namespace spacetrains::simulation
