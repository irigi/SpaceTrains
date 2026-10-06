#pragma once

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
    [[nodiscard]] bool try_refuel(domain::ShipState& ship);
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
    void step_idle_ship(domain::ShipState& ship);
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
    double timewarp_factor_ {3600.0};
    std::vector<domain::StationState> stations_;
    std::vector<domain::ShipState> ships_;
    std::vector<domain::EventEntry> recent_events_;
    std::vector<domain::TradeEntry> recent_trades_;
    bool trajectory_audit_enabled_ {false};
    std::vector<trajectory::TrajectoryAuditRecord> trajectory_audit_records_;
    std::unordered_map<std::string, const domain::StationDefinition*> station_defs_by_id_;
    std::unordered_map<std::string, const domain::ShipClassDefinition*> ship_classes_by_id_;
};

}  // namespace spacetrains::simulation
