#pragma once

#include <array>
#include <cmath>
#include <functional>
#include <iosfwd>
#include <limits>
#include <map>
#include <optional>
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
#include "simulation/FlowRecord.hpp"
#include "simulation/MoneyJournal.hpp"
#include "simulation/World.hpp"
#include "simulation/query/Accounts.hpp"
#include "simulation/query/Clock.hpp"
#include "simulation/query/Finance.hpp"
#include "simulation/query/Labour.hpp"
#include "simulation/query/Passengers.hpp"
#include "simulation/query/ShipRates.hpp"
#include "simulation/systems/Contracts.hpp"
#include "simulation/systems/Failures.hpp"
#include "simulation/systems/FleetInvestment.hpp"
#include "simulation/systems/Households.hpp"
#include "simulation/systems/Market.hpp"
#include "simulation/systems/MissionPlanning.hpp"
#include "simulation/systems/Passengers.hpp"
#include "simulation/systems/Patrols.hpp"
#include "simulation/systems/ShipPhases.hpp"
#include "simulation/systems/Snapshot.hpp"
#include "simulation/systems/ShipOperations.hpp"
#include "simulation/systems/Statistics.hpp"
#include "trajectory/ChaseSolver.hpp"
#include "trajectory/TrajectoryAudit.hpp"
#include "trajectory/TrajectoryPlanner.hpp"
#include "trajectory/VariableIspTrajectoryPlanner.hpp"
#include "util/ThreadPool.hpp"
#include "variable_isp/VariableIsp.hpp"

namespace spacetrains::simulation {

// The tick on a World (Simulation.cpp; Simulation forwards to these, tests call them directly).
// step() adds real_dt_s x the timewarp and runs the whole ticks due; tick() runs one, in the
// order listed in docs/plans/simulation_split.md ("Tick order"); start_at() starts a fresh
// game at another date (call before the first step).
void start_at(World& w, double time_s);
void step(World& w, double real_dt_s);
void tick(World& w);

// The game: owns the World (all state, World.hpp) and runs its tick; everything else is in
// systems/ and query/ (docs/plans/simulation_split.md). The World lives on the heap and never
// moves, so the engines' references into it stay valid when a Simulation is moved.
class Simulation {
public:
    explicit Simulation(domain::UniverseDefinition universe);

    [[nodiscard]] static Simulation from_data_root(const std::string& data_root);
    // The same, with the loaded universe adjusted first (tests: switches without data copies).
    [[nodiscard]] static Simulation from_data_root(const std::string& data_root,
        const std::function<void(domain::UniverseDefinition&)>& adjust);
    // With the VariableISP atlas at the given path (an installed game keeps it outside the
    // source tree); empty means default_atlas_path(data_root).
    [[nodiscard]] static Simulation from_data_root(const std::string& data_root, const std::string& atlas_path,
        const std::function<void(domain::UniverseDefinition&)>& adjust);
    // The atlas in a source checkout: tests/data/variable_isp next to the data root.
    [[nodiscard]] static std::string default_atlas_path(const std::string& data_root);

    // The simulation advances in fixed ticks of TICK_S game seconds, whatever the timewarp
    // or the caller's step: step() adds real_dt_s x timewarp and runs the whole ticks due.
    // Headless runs and the UI therefore simulate the same game.
    static constexpr double TICK_S = simulation::TICK_S;
    void step(double real_dt_s);
    void set_timewarp(double timewarp_factor);

    [[nodiscard]] const domain::UniverseDefinition& universe() const;
    [[nodiscard]] const economy::EconomySystem& economy_system() const;
    [[nodiscard]] domain::SimulationSnapshot snapshot() const;
    [[nodiscard]] double game_time_s() const { return world_->state.game_time_s; }
    // Starts a fresh game at another date (planets elsewhere along their orbits): the same
    // economy in a different geometry, for benchmarks over several starts. Call before the
    // first step.
    void start_at(double time_s);
    // Random events (step 28) are on unless switched off, e.g. to compare economies.
    void set_events_enabled(bool enabled) { world_->data.events_enabled = enabled; }

    // Save games (persistence/SimulationPersistence.{hpp,cpp}): the mutable state as JSON.
    // load_state_json() expects a Simulation built from the same data files (checked by
    // fingerprint) and throws, leaving the simulation unchanged, on a bad or foreign save.
    [[nodiscard]] std::string save_state_json() const;
    void load_state_json(const std::string& text);
    // Landed-cost reference prices (step 15): for each consumer and good it buys, the base price
    // plus carrier_margin x the cheapest cost per unit of carrying it from one of its two nearest
    // producers, in a full hold of a standard-tank hull of at least 150 u, out loaded and back
    // empty (median over departure dates in a year). Written to data by
    // `spacetrains_headless --write-reference-prices`.
    using ReferencePrice = systems::ReferencePrice;
    [[nodiscard]] std::vector<ReferencePrice> compute_reference_prices() const;
    // Passengers (docs/modules/ships_and_fleet.md): for every station pair, the cheapest cabin carrier's
    // cost per berth one way (carrier_margin x (its days out + return_share x the way back, at its
    // daily cost, plus propellant) over berths x fill_ref, plus the passenger's provisions); median
    // over departure dates. Written by `spacetrains_headless --write-reference-fares`.
    using ReferenceFareRow = systems::ReferenceFareRow;
    [[nodiscard]] std::vector<ReferenceFareRow> compute_reference_fares() const;
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
    [[nodiscard]] std::string bridge_designs_signature() const { return "classes+" + std::to_string(world_->caches.designs.size()); }
    [[nodiscard]] std::string bridge_paths_signature() const;
    [[nodiscard]] double timewarp_factor() const;
    [[nodiscard]] math::Vec3d get_ship_render_position(const domain::ShipState& ship) const;
    [[nodiscard]] const std::vector<domain::TradeEntry>& recent_trades() const { return world_->state.recent_trades; }
    using RadiationAudit = simulation::RadiationAudit;
    [[nodiscard]] const RadiationAudit& radiation_audit() const { return world_->audits.radiation_audit; }
    using FailureAudit = simulation::FailureAudit;
    [[nodiscard]] const FailureAudit& failure_audit() const { return world_->audits.failure_audit; }
    using PatrolAudit = simulation::PatrolAudit;
    [[nodiscard]] const PatrolAudit& patrol_audit() const { return world_->audits.patrol_audit; }
    [[nodiscard]] const std::vector<domain::PatrolState>& patrols() const { return world_->state.patrols; }
    [[nodiscard]] const std::vector<domain::DerelictState>& derelicts() const { return world_->state.derelicts; }
    using PassengerAudit = simulation::PassengerAudit;
    [[nodiscard]] const PassengerAudit& passenger_audit() const { return world_->audits.passenger_audit; }
    [[nodiscard]] static const char* passenger_kind_name(domain::PassengerKind kind);
    // A station's profit since the start, before the faction's taxes and subsidies: its cash gained
    // + taxes paid - subsidies received + the value its stock gained (at its price curves).
    [[nodiscard]] double station_lifetime_profit(const domain::StationState& station) const;
    // The same over the last 12 monthly samples of the money journal (since the start until there are 12).
    [[nodiscard]] double station_profit_last_year(const domain::StationState& station) const;
    // The money journal (MoneyJournal.hpp): every transfer by account and category, and its history.
    [[nodiscard]] const MoneyJournal& money_journal() const { return world_->state.journal; }
    // The running state, for calling systems:: and query:: on it directly (tests, tools).
    [[nodiscard]] World& world() { return *world_; }
    [[nodiscard]] const World& world() const { return *world_; }
    // What moved where (FlowRecord.hpp): goods, passengers, lanes, with a monthly history.
    [[nodiscard]] const FlowRecord& flow_record() const { return world_->state.flows; }
    // The real balance of a journal account (Earth, a colony, a faction, a station, its residents,
    // or a faction's fleet: the sum of its ships' cash).
    [[nodiscard]] double account_balance(int account) const;
    // Statistics (bridge side file and headless --stats-json): accounts, flows, history, people, fleet.
    [[nodiscard]] std::string build_stats_json() const;
    // Audit: deliveries with a reference price since start (or load), paid vs reference.
    [[nodiscard]] double delivered_price_ratio() const {
        return world_->audits.delivered_reference_cr > 0.0 ? world_->audits.delivered_paid_cr / world_->audits.delivered_reference_cr : 0.0;
    }

    // When enabled, every accepted mission plan is checked by audit_trajectory()
    // and recorded (headless --trajectory-audit).
    void set_trajectory_audit_enabled(bool enabled) { world_->audits.trajectory_audit_enabled = enabled; }
    [[nodiscard]] const std::vector<trajectory::TrajectoryAuditRecord>& trajectory_audit_records() const {
        return world_->audits.trajectory_audit_records;
    }
    using OverhaulBill = query::OverhaulBill;
    [[nodiscard]] OverhaulBill overhaul_bill(const domain::ShipClassDefinition& design, double drive_share,
        double radiator_share) const;
    // Wear priced into planning: a drive life's overhaul (base prices) per hour of thrust, and
    // that per day in flight (plasma) or per kg of propellant burned (nuclear-thermal); 0 with wear off.
    [[nodiscard]] double wear_cr_per_drive_hour(const domain::ShipClassDefinition& design) const;
    [[nodiscard]] double wear_cr_per_flight_day(const domain::ShipClassDefinition& design) const;
    [[nodiscard]] double wear_cr_per_propellant_kg(const domain::ShipClassDefinition& design) const;

    // Passengers (docs/modules/ships_and_fleet.md). Fleet investment's probes and route commitments name passenger runs by this pseudo-good.
    static constexpr const char* PASSENGER_GOOD = simulation::PASSENGER_GOOD;
    // People per day who would take the trip origin -> destination at about its reference cost:
    // the demand a new carrier on that pair competes for (settlers posted, emigrants, and the
    // queue spread over the route commitment).
    [[nodiscard]] double passenger_flow_per_day(const std::string& origin_id, const std::string& destination_id) const;
    // Who pays a passenger's fare: the work station (rotation, sponsored settlers) or the traveller.
    [[nodiscard]] bool employer_pays(domain::PassengerKind kind) const;
    // Share of a station's would-be settlers whose job repays a passage of this many years' wages.
    [[nodiscard]] double settler_share(double payback_years) const;
    // Travellers by willingness band (what post_travellers would post over window_days of the
    // steady flows, plus those waiting spread over a route commitment), and the fares that
    // berths filled from them would take.
    using PassengerBand = systems::PassengerBand;
    using PassengerRevenue = systems::PassengerRevenue;
    [[nodiscard]] double carried_passengers_per_day(const std::string& origin_id, const std::string& destination_id) const;
    [[nodiscard]] PassengerRevenue expected_passenger_revenue(const std::string& origin_id,
        const std::string& destination_id, double days, double window_days, double berths, double open_share) const;
    // Fuel at a port at today's price (never below base), and a reference fare at those prices.
    [[nodiscard]] double port_fuel_price(const std::string& station_id) const;
    [[nodiscard]] double live_fare_cr(const std::string& origin_id, const std::string& destination_id,
        const domain::PassengerDefinition::ReferenceFare& reference) const;
    // The fare a carrier can expect per passenger on that pair on a leg of `days`, filling
    // `berths` from `demand_per_trip` people: the reference fare adjusted for its days, or the
    // fare that clears the berths when demand exceeds them.
    [[nodiscard]] double expected_fare_cr(const std::string& origin_id, const std::string& destination_id, double days,
        double demand_per_trip, double berths) const;
    using RotationTour = simulation::RotationTour;
    [[nodiscard]] const std::map<std::string, RotationTour>& rotation_tours() const { return world_->data.rotation_tours; }
    // Economy overhaul (systems/Households.cpp): residents' wages and spending, station incomes, trade
    // with the background economies at their gateways.
    [[nodiscard]] bool households_on() const { return query::households_on(*world_); }
    [[nodiscard]] double resident_wages_per_day(const domain::StationState& station) const;
    [[nodiscard]] const domain::StationDefinition* yard_station_for(const domain::StationDefinition& home) const;
    // Labour market (phase B, systems/Households.cpp).
    [[nodiscard]] bool labour_on() const { return query::labour_on(*world_); }
    [[nodiscard]] bool in_labour_market(const domain::StationState& station) const;
    [[nodiscard]] double station_wage_cr_per_day(const domain::StationState& station) const;
    [[nodiscard]] double station_workers(const domain::StationState& station) const;
    [[nodiscard]] double labour_migrants_per_day(const domain::StationState& station) const;
    [[nodiscard]] double labour_emigrants_per_day(const domain::StationState& station) const;
    [[nodiscard]] double settler_gain_tail(const domain::StationState& destination, double gain_cr) const;
    // The order derived from the observations, the stock and the contracts on their way.
    [[nodiscard]] domain::OrderView order_view(const domain::StationState& state, const std::string& commodity_id) const;
    // The cash above which a station pays its faction tax: lower while the faction is in debt.
    [[nodiscard]] static double station_tax_ceiling(const domain::OpenEconomyDefinition& open, double treasury);
    [[nodiscard]] double traveller_budget_tail(const std::string& home_id, double cost_cr) const;

    using ColonyState = simulation::ColonyState;
    // Colonies: their people against the data population (sales and imports scale with it when
    // colony_people_scaling is on), their wage median now, and their welfare state.
    [[nodiscard]] double colony_people_factor(const std::string& economy_id) const;
    [[nodiscard]] double colony_wage_median(const domain::BackgroundEconomyDefinition& economy) const;
    [[nodiscard]] const ColonyState* colony_state(const std::string& economy_id) const {
        return query::colony_state(*world_, economy_id);
    }
    // Affordability: 1 above the buffer, falling to 0 at minus the credit line (or the old hard stop).
    [[nodiscard]] static double colony_affordability(double account, double credit_line, double buffer);
    // The share of a good's base imports a colony buys at affordability f (essentials first).
    [[nodiscard]] static double colony_import_share(double affordability, bool essential);

private:
    explicit Simulation(std::unique_ptr<World> world);

    std::unique_ptr<World> world_;
};

}  // namespace spacetrains::simulation
