#pragma once

#include <map>
#include <memory>
#include <string>
#include <unordered_map>

#include "celestial/CelestialMechanics.hpp"
#include "data_loader/DataLoader.hpp"
#include "domain/Types.hpp"
#include "economy/EconomySystem.hpp"
#include "trajectory/TrajectoryAudit.hpp"
#include "trajectory/TrajectoryPlanner.hpp"
#include "trajectory/VariableIspTrajectoryPlanner.hpp"
#include "variable_isp/VariableIsp.hpp"

namespace spacetrains::simulation {

class Simulation {
public:
    explicit Simulation(domain::UniverseDefinition universe);

    [[nodiscard]] static Simulation from_data_root(const std::string& data_root);

    void step(double real_dt_s);
    void set_timewarp(double timewarp_factor);

    [[nodiscard]] const domain::UniverseDefinition& universe() const;
    [[nodiscard]] const economy::EconomySystem& economy_system() const;
    [[nodiscard]] domain::SimulationSnapshot snapshot() const;
    [[nodiscard]] std::string build_report() const;
    [[nodiscard]] std::string build_bridge_snapshot_json(bool paused, std::uint64_t snapshot_seq, double snapshot_real_time_s) const;
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
    [[nodiscard]] const domain::CommodityDefinition& get_commodity(const std::string& commodity_id) const;
    [[nodiscard]] double station_price(const domain::StationState& state, const std::string& commodity_id) const;
    // Value of moving units into (+) or out of (-) a station, along its price curve.
    // Value of selling `units` at a station `days_ahead` from now, on its forecast stock.
    [[nodiscard]] double sale_value_on_arrival(const domain::StationState& state, const std::string& commodity_id,
        double units, double days_ahead, const std::string& seller_ship_id) const;
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
    // Open economy: residents pay their station for the goods the economy step used up, and
    // the station pays local producers (and its fuel depot) for new output, along the same
    // price curve as ship trades. The other side of every payment is the external account.
    void settle_local_economy(const std::vector<domain::Inventory>& stocks_before);
    // Dividends, the faction treasuries' subsidies and taxes, and the money-supply controller.
    void step_treasuries(double dt_s);
    [[nodiscard]] double internal_money_supply() const;
    // Fleet investment (docs/plans/fleet_investment.md): owners sell ships laid up for long,
    // and every review the treasuries commission the ship with the best expected return.
    void step_fleet_investment();
    void sell_ship(std::size_t index);
    void commission_best_ship();
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
        std::string commodity_id;
        double cargo_units {0.0};
        // Cargo runs: sale value minus purchase and fuel, without urgency or time costs.
        double cargo_margin {0.0};
        domain::TrajectoryPlan plan;
        // Fuel-aware planning: return fuel loaded on top of the plan's load, for a port that
        // cannot refuel the ship.
        double carried_propellant_kg {0.0};
        // Cargo-only probes (fleet investment): every feasible cargo run, which the
        // investment review values at its sustained flow and prices.
        struct CargoOption {
            const domain::StationDefinition* destination {nullptr};
            std::string commodity_id;
            double cargo_units {0.0};
            double travel_days {0.0};
            double wait_days {0.0};
            double fuel_cost {0.0};
        };
        std::vector<CargoOption> cargo_options;
    };
    // `cargo_only`: only cargo runs from the ship's port (no empty legs, no repositioning).
    [[nodiscard]] MissionChoice choose_mission(const domain::ShipState& ship,
        const domain::ShipClassDefinition& ship_class, bool trace, double earliest_departure_s,
        bool cargo_only = false);
    // Part C: at its home base a ship may refit to another tank variant of its hull when
    // the missions that opens repay the yard bill. Returns true if the ship went into the yard.
    bool consider_refit(domain::ShipState& ship, const MissionChoice& current, bool trace);
    [[nodiscard]] double refit_bill(
        const domain::ShipClassDefinition& from, const domain::ShipClassDefinition& to) const;
    void step_refitting_ship(domain::ShipState& ship);
    // Two-leg mission scoring: a cheap, cached estimate of a follow-up leg.
    struct LegEstimate {
        bool feasible {false};
        double travel_days {0.0};
        double propellant_kg {0.0};
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
    double outside_economy_credits_ {0.0};
    std::unordered_map<std::string, double> faction_treasuries_;
    double seeded_money_supply_ {0.0};   // grows by the working capital of new ships
    double next_investment_review_s_ {0.0};
    domain::FleetInvestmentLedger investment_ledger_;
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
    std::unordered_map<std::string, const domain::StationDefinition*> station_defs_by_id_;
    std::unordered_map<std::string, const domain::ShipClassDefinition*> ship_classes_by_id_;
};

}  // namespace spacetrains::simulation
