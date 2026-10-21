#pragma once

#include <functional>
#include <iosfwd>
#include <limits>
#include <map>
#include <memory>
#include <random>
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
    // The same, with the loaded universe adjusted first (tests: switches without data copies).
    [[nodiscard]] static Simulation from_data_root(const std::string& data_root,
        const std::function<void(domain::UniverseDefinition&)>& adjust);

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
    // Random events (step 28) are on unless switched off, e.g. to compare economies.
    void set_events_enabled(bool enabled) { events_enabled_ = enabled; }

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
    // Ship designs: every class with its parts (ship_components.csv), for the ship inspector.
    // Static today, so the bridge writes it once as <snapshot>.designs; with refittable
    // modules (tier 2) it changes when a ship's fit does, keyed by bridge_designs_signature().
    [[nodiscard]] std::string build_bridge_designs_json() const;
    // Designs are only ever added, so their count tells the bridge when to rewrite the file.
    [[nodiscard]] std::string bridge_designs_signature() const { return "classes+" + std::to_string(designs_.size()); }
    [[nodiscard]] std::string bridge_paths_signature() const;
    [[nodiscard]] double timewarp_factor() const;
    [[nodiscard]] math::Vec3d get_ship_render_position(const domain::ShipState& ship) const;
    [[nodiscard]] const std::vector<domain::TradeEntry>& recent_trades() const { return recent_trades_; }
    // Audit (tier 2, crew radiation): since start or load, not saved.
    struct RadiationAudit {
        double crew_dose_msv {0.0};      // person-mSv over the fleet
        double hazard_pay_cr {0.0};
        double highest_leg_dose_msv {0.0};
        int particle_events {0};
        double largest_event_magnitude {0.0};
        double hydrogen_boiled_kg {0.0};   // step F
        int overhauls {0};                 // step H
        double repair_bills_cr {0.0};
        std::map<std::string, double> spares_units;
        int repairs_waiting_for_spares {0};
    };
    [[nodiscard]] const RadiationAudit& radiation_audit() const { return radiation_audit_; }
    // Audit: deliveries with a reference price since start (or load), paid vs reference.
    [[nodiscard]] double delivered_price_ratio() const {
        return delivered_reference_cr_ > 0.0 ? delivered_paid_cr_ / delivered_reference_cr_ : 0.0;
    }

    // When enabled, every accepted mission plan is checked by audit_trajectory()
    // and recorded (headless --trajectory-audit).
    void set_trajectory_audit_enabled(bool enabled) { trajectory_audit_enabled_ = enabled; }
    [[nodiscard]] const std::vector<trajectory::TrajectoryAuditRecord>& trajectory_audit_records() const {
        return trajectory_audit_records_;
    }

private:
    [[nodiscard]] const domain::ShipClassDefinition& get_ship_class(const std::string& class_id) const;
    // The design a ship flies with: its class, or with modules fitted (tier 2,
    // docs/plans/ship_structures_tier2.md) the design derived from the class and the fit.
    // Ship-level code asks this, never get_ship_class(ship.class_id). Fitted designs must be
    // registered first (serial code only: planning passes read the cache in parallel).
    [[nodiscard]] const domain::ShipClassDefinition& ship_design(const domain::ShipState& ship) const;
    // Derives and caches the design class_id + fit (serial code only); throws on a fit the
    // hull cannot take (unknown module, slots overfilled, a module twice).
    const domain::ShipClassDefinition& register_design(const std::string& class_id, const std::vector<std::string>& fit,
        double radiator_bucket = 1.0);
    // Radiator condition in the 10% steps designs are derived at (1 = intact, or wear off).
    [[nodiscard]] double radiator_bucket(const domain::ShipState& ship) const;
    // Wear and repairs (tier 2, step H).
    // Hours of drive life a plan uses: burn time (nuclear-thermal) or thrust time (plasma).
    [[nodiscard]] double drive_hours(const domain::ShipClassDefinition& design, const domain::TrajectoryPlan& plan) const;
    [[nodiscard]] double drive_life_h(const domain::ShipClassDefinition& design) const;
    [[nodiscard]] bool drive_life_allows(const domain::ShipState& ship, const domain::ShipClassDefinition& design,
        const domain::TrajectoryPlan& plan) const;
    void accrue_wear(domain::ShipState& ship, const domain::ShipClassDefinition& design, double days);
    // Overhauls a worn drive or holed radiators where the port has the spares; true if the
    // ship went into the yard.
    bool consider_repair(domain::ShipState& ship);
    [[nodiscard]] const domain::ShipModuleDefinition* find_module(const std::string& hull_id, const std::string& module_id) const;
    // A module is offered at refits and to new ships only if one of its effects is switched on
    // (data/economy/ship_environment.csv): an inert module only adds mass.
    [[nodiscard]] bool module_has_active_effect(const domain::ShipModuleDefinition& module) const;
    // A refit or build option: a class (tank variant) and the modules fitted with it.
    struct FitOption {
        const domain::ShipClassDefinition* design {nullptr};
        std::string class_id;
        std::vector<std::string> fit;
    };
    // The fits one yard visit can reach from class_id + fit: the next tank size either way, and
    // one module added (into a free slot) or removed. Registers the designs (serial code only).
    std::vector<FitOption> neighbouring_fits(const std::string& class_id, const std::vector<std::string>& fit,
        double radiator_bucket = 1.0);
    [[nodiscard]] const domain::CelestialBodyDefinition& get_body_definition(const std::string& body_id) const;
    [[nodiscard]] const domain::StationDefinition& get_station_definition(const std::string& station_id) const;
    [[nodiscard]] domain::StationState& get_station_state(const std::string& station_id);
    [[nodiscard]] const domain::StationState& get_station_state(const std::string& station_id) const;
    [[nodiscard]] std::string mission_phase_name(domain::ShipMissionPhase phase) const;
    [[nodiscard]] std::string bridge_path_id(const domain::ShipState& ship) const;
    [[nodiscard]] std::string ship_systems_json(
        const domain::ShipState& ship, const domain::ShipClassDefinition& ship_class, const math::Vec3d& position) const;
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
        double order_units {0.0};      // units that fill an open order (step 29), and what they earn
        double order_value {0.0};
    };
    // Selling `units` into a station whose stock is `stock`: along its curve, raised to an
    // open emergency's floor price for the units that land below its emergency cover.
    [[nodiscard]] SaleSplit sale_split(const domain::StationDefinition& station, const std::string& commodity_id,
        double stock, double units) const;
    [[nodiscard]] SaleSplit sale_split_on_arrival(const domain::StationState& state, const std::string& commodity_id,
        double units, double days_ahead, const std::string& seller_ship_id) const;
    // Station purchase orders (step 29, docs/plans/station_orders.md). A station posts an order
    // for a good it is a net consumer of, and for depot fuel where it has no factory, never at
    // an export market; it then has OrderStats for the good.
    [[nodiscard]] bool has_order(const domain::StationDefinition& station, const std::string& commodity_id) const;
    // Ships are paid under the station's order for the good, not along its curve: true wherever
    // the station has OrderStats for it (step 29).
    [[nodiscard]] bool priced_order(const domain::StationState& state, const std::string& commodity_id) const;
    // What a station expects before its first delivery: the reference trip's lead and interval
    // (21 days within one planet's system), half the lead as spread, the reference price.
    [[nodiscard]] domain::OrderStats seed_order_stats(const domain::StationDefinition& station, const std::string& commodity_id) const;
    // Seeds every order a station has no observations for yet.
    void seed_order_stats();
    // Folds a contract (its unit price) or a delivery (its lead and the interval since the last
    // one) into a station's observations; nothing if the station posts no order for the good.
    void observe_contract(domain::StationState& state, const std::string& commodity_id, double units, double unit_price);
    void observe_delivery(domain::StationState& state, const std::string& commodity_id, double contracted_s);
    // An order's levels before contracts: use, safety stock, order-up-to level, the most with the extra.
    struct OrderLevels {
        double use_per_day {0.0};
        double safety_units {0.0};
        double order_up_to_units {0.0};
        double max_units {0.0};
        double extra_days {0.0};
    };
    [[nodiscard]] OrderLevels order_levels(
        const domain::StationState& state, const std::string& commodity_id, const domain::OrderStats& stats) const;
    // The good's contracted arrivals at the station (time, units that survive the trip), sorted,
    // without one ship's.
    [[nodiscard]] std::vector<std::pair<double, double>> contracted_arrivals(
        const domain::StationState& state, const std::string& commodity_id, const std::string& except_ship_id) const;
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
    [[nodiscard]] OrderQuote order_quote(const domain::StationState& state, const std::string& commodity_id,
        double arrival_s, const std::string& except_ship_id) const;
    // The value of the units [from, from + units) of a delivery under a quote.
    [[nodiscard]] SaleSplit quote_split(const OrderQuote& quote, double from, double units) const;
    // The order's bid for a delivery that finds `stock_at_arrival` (every earlier contract
    // counted): the curve's shape around the reference, on the order-up-to level, up to the
    // lower of today's cap and the ceiling V (its cap: `cap`).
    struct OrderBid {
        double price {0.0};
        double cap {0.0};
    };
    [[nodiscard]] OrderBid order_bid(const domain::StationState& state, const std::string& commodity_id,
        const OrderLevels& levels, double stock_at_arrival) const;
    // Sets each order's posted price to its bid for a delivery one lead time from now (shown in
    // the inspector and the audit; quotes compute their own).
    void step_orders();
    // Competitive pricing: each order's market price from the asks of ships near a source (daily).
    void update_market_prices();
    // What a consumer offers for a good now: its order price where it posts an order, else its price.
    [[nodiscard]] double consumer_bid(const domain::StationState& state, const std::string& commodity_id) const;
    // The order derived from the observations, the stock and the contracts on their way.
    [[nodiscard]] domain::OrderView order_view(const domain::StationState& state, const std::string& commodity_id) const;
    // V: what a unit is worth to the station (see the plan's section 4); `station_ceiling` is
    // the part from its own output, before the faction tops up a life-critical good.
    [[nodiscard]] double order_ceiling(const domain::StationState& state, const std::string& commodity_id,
        std::string& kind, double& station_ceiling) const;
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
    // Operating economics (docs/modules/ships_and_fleet.md).
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
    void step_population(double dt_s);
    void step_events(double dt_s);
    // Station money (step 17). Consumption per day of a station's goods at reference prices:
    // its upkeep goods only (for the core-crew subsidy), or every consumed good (its import bill).
    [[nodiscard]] double consumption_value_per_day(const domain::StationDefinition& station, bool upkeep_only) const;
    // What a faction may still spend on ships: its treasury plus what it may borrow.
    [[nodiscard]] double faction_credit_limit(const std::string& faction_id) const;
    [[nodiscard]] double faction_spendable(const std::string& faction_id) const;
    [[nodiscard]] double internal_money_supply() const;
    // The fleet's total hold, against max_fleet_hold_units (step 21).
    [[nodiscard]] double fleet_hold_units() const;
    // Ships counted against max_fleet_size (liners are not).
    [[nodiscard]] std::size_t fleet_ship_count() const;
    // Scheduled liners (step 22): a liner's loop (null for other ships), and its stop after `from_id`.
    [[nodiscard]] const std::vector<std::string>* liner_stops(const domain::ShipState& ship) const;
    [[nodiscard]] std::string next_liner_stop(const domain::ShipState& ship, const std::string& from_id) const;
    // A liner's stops, or the loop of a ship under a rotation contract (nullptr: neither).
    [[nodiscard]] const std::vector<std::string>* scheduled_stops(const domain::ShipState& ship) const;
    [[nodiscard]] std::string next_scheduled_stop(const domain::ShipState& ship, const std::string& from_id) const;
    [[nodiscard]] bool under_contract(const domain::ShipState& ship) const;
    // Rotation contracts: units per day of a good contracted to a station, over every contract.
    [[nodiscard]] double contracted_flow(const std::string& station_id, const std::string& commodity_id) const;
    // The best loop for a ship from its port, with its profit per day (0: none).
    std::pair<domain::ShipContract, double> find_contract(const domain::ShipState& ship,
        const domain::ShipClassDefinition& ship_class);
    // Runs `pass` until it queues no plans, computing the queued plans in parallel in between.
    void with_parallel_plans(const std::string& prefix, const std::function<void()>& pass);
    // Fleet investment (docs/modules/ships_and_fleet.md): owners sell ships laid up for long,
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
    // Crew radiation (tier 2, step D; src/simulation/CrewEnvironment.hpp).
    [[nodiscard]] double station_r_au(const domain::StationDefinition& station, double time_s) const;
    [[nodiscard]] double docked_dose_msv_per_day(const std::string& station_id, double time_s) const;
    // Expected dose rate of a crew flying from -> to (departing at time_s).
    [[nodiscard]] double leg_dose_msv_per_day(const domain::ShipClassDefinition& design,
        const domain::StationDefinition& from, const domain::StationDefinition& to, double time_s) const;
    // Hazard pay per day of that leg (0 with radiation off).
    [[nodiscard]] double hazard_cr_per_day(const domain::ShipClassDefinition& design,
        const domain::StationDefinition& from, const domain::StationDefinition& to, double time_s) const;
    // Whether a leg of this length keeps its crew under the career limit.
    [[nodiscard]] bool leg_dose_allowed(const domain::ShipClassDefinition& design,
        const domain::StationDefinition& from, const domain::StationDefinition& to, double time_s, double days) const;
    void accrue_radiation(domain::ShipState& ship, const domain::ShipClassDefinition& design, double days);
    // Where a ship is from the Sun now: its station, or interpolated over its leg.
    [[nodiscard]] double ship_r_au(const domain::ShipState& ship) const;
    // Spoilage per day of a good in this design's hold (a cold hold slows food and medicine).
    [[nodiscard]] double cargo_decay_per_day(const domain::ShipClassDefinition& design, const std::string& commodity_id) const;
    // Liquid hydrogen a design loses per day at r (AU): the tanks' heat leak less what its
    // cryocooler lifts (tier 2, step F; 0 for water-propellant plasma ships or boil-off off).
    [[nodiscard]] double boiloff_kg_per_day(const domain::ShipClassDefinition& design, double r_au) const;
    void step_particle_events(double dt_s);
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
    double timewarp_factor_ {3600.0};
    std::vector<domain::StationState> stations_;
    std::vector<domain::ShipState> ships_;
    std::vector<domain::EventEntry> recent_events_;
    std::vector<domain::TradeEntry> recent_trades_;
    double delivered_paid_cr_ {0.0};       // audit only, not saved
    RadiationAudit radiation_audit_;       // audit only, not saved
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
