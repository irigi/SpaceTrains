// Simulation members whose code moved to systems/ or query/ (docs/plans/simulation_split.md,
// steps 5-8): each forwards to its free function on World. Callers move over topic by topic; this file
// is deleted in step 15.

#include "simulation/Simulation.hpp"

namespace spacetrains::simulation {

double Simulation::fleet_hold_units() const {
    return systems::fleet_hold_units(*this);
}

std::size_t Simulation::fleet_ship_count() const {
    return systems::fleet_ship_count(*this);
}

const std::vector<std::string>* Simulation::liner_stops(const domain::ShipState& ship) const {
    return systems::liner_stops(*this, ship);
}

bool Simulation::under_contract(const domain::ShipState& ship) const {
    return systems::under_contract(*this, ship);
}

const std::vector<std::string>* Simulation::scheduled_stops(const domain::ShipState& ship) const {
    return systems::scheduled_stops(*this, ship);
}

double Simulation::contract_dwell_days(const domain::ShipContract& contract) {
    return systems::contract_dwell_days(contract);
}

std::pair<domain::ShipContract, double> Simulation::find_contract(
    const domain::ShipState& ship, const domain::ShipClassDefinition& ship_class) {
    return systems::find_contract(*this, ship, ship_class);
}

domain::Inventory Simulation::starting_inventory(const domain::StationDefinition& station) const {
    return systems::starting_inventory(*this, station);
}

std::vector<Simulation::ReferencePrice> Simulation::compute_reference_prices() const {
    return systems::compute_reference_prices(*this);
}

void Simulation::set_station_stock(const std::string& station_id, const std::string& commodity_id, double units) {
    systems::set_station_stock(*this, station_id, commodity_id, units);
}

std::string Simulation::faction_name(const std::string& faction_id) const {
    return systems::faction_name(*this, faction_id);
}

double Simulation::station_price(const domain::StationState& state, const std::string& commodity_id) const {
    return systems::station_price(*this, state, commodity_id);
}

double Simulation::trade_value(
    const domain::StationState& state, const std::string& commodity_id, double units_into_station) const {
    return systems::trade_value(*this, state, commodity_id, units_into_station);
}

Simulation::SaleSplit Simulation::sale_split_on_arrival(const domain::StationState& state, const std::string& commodity_id,
    double units, double days_ahead, const std::string& seller_ship_id) const {
    return systems::sale_split_on_arrival(*this, state, commodity_id, units, days_ahead, seller_ship_id);
}

void Simulation::step_emergencies() {
    systems::step_emergencies(*this);
}

bool Simulation::priced_order(const domain::StationState& state, const std::string& commodity_id) const {
    return systems::priced_order(state, commodity_id);
}

domain::OrderStats Simulation::seed_order_stats(const domain::StationDefinition& station, const std::string& commodity_id) const {
    return systems::seed_order_stats(*this, station, commodity_id);
}

void Simulation::seed_order_stats() {
    systems::seed_order_stats(*this);
}

void Simulation::observe_contract(domain::StationState& state, const std::string& commodity_id, double units, double unit_price) {
    systems::observe_contract(*this, state, commodity_id, units, unit_price);
}

void Simulation::observe_delivery(domain::StationState& state, const std::string& commodity_id, double contracted_s) {
    systems::observe_delivery(*this, state, commodity_id, contracted_s);
}

Simulation::OrderLevels Simulation::order_levels(
    const domain::StationState& state, const std::string& commodity_id, const domain::OrderStats& stats) const {
    return systems::order_levels(*this, state, commodity_id, stats);
}

domain::OrderView Simulation::order_view(const domain::StationState& state, const std::string& commodity_id) const {
    return systems::order_view(*this, state, commodity_id);
}

Simulation::SaleSplit Simulation::quote_split(const OrderQuote& quote, double from, double units) const {
    return systems::quote_split(quote, from, units);
}

Simulation::OrderBid Simulation::order_bid(const domain::StationState& state, const std::string& commodity_id,
    const OrderLevels& levels, double stock_at_arrival) const {
    return systems::order_bid(*this, state, commodity_id, levels, stock_at_arrival);
}

void Simulation::update_market_prices() {
    systems::update_market_prices(*this);
}

void Simulation::step_orders() {
    systems::step_orders(*this);
}

double Simulation::consumer_bid(const domain::StationState& state, const std::string& commodity_id) const {
    return systems::consumer_bid(*this, state, commodity_id);
}

Simulation::LegEstimate Simulation::estimate_leg(
    const domain::ShipClassDefinition& ship_class,
    const domain::StationDefinition& origin,
    const domain::StationDefinition& destination,
    double departure_time_s,
    double available_propellant_kg,
    double payload_kg) {
    return systems::estimate_leg(*this, ship_class, origin, destination, departure_time_s, available_propellant_kg, payload_kg);
}

Simulation::MissionChoice Simulation::choose_mission(
    const domain::ShipState& ship, const domain::ShipClassDefinition& ship_class, bool trace,
    double earliest_departure_s, bool cargo_only) {
    return systems::choose_mission(*this, ship, ship_class, trace, earliest_departure_s, cargo_only);
}

std::vector<Simulation::MissionChoice> Simulation::choose_missions(const std::vector<MissionRequest>& requests) {
    return systems::choose_missions(*this, requests);
}

// ShipOperations (step 6), with the query forwarders that lived in its topic file.

double Simulation::daily_capital_cost(const domain::ShipClassDefinition& ship_class) const {
    return query::daily_capital_cost(*this, ship_class);
}

double Simulation::daily_crew_cost(
    const domain::ShipClassDefinition& ship_class, const domain::StationState& market) const {
    return systems::daily_crew_cost(*this, ship_class, market);
}

double Simulation::credit_line(const domain::ShipClassDefinition& ship_class) const {
    return query::credit_line(ship_class);
}

double Simulation::persons_aboard(const domain::ShipState& ship) const {
    return query::persons_aboard(*this, ship);
}

double Simulation::provisionable_days(const domain::ShipState& ship, const domain::StationState& market) const {
    return query::provisionable_days(*this, ship, market);
}

double Simulation::station_r_au(const domain::StationDefinition& station, double time_s) const {
    return query::station_r_au(*this, station, time_s);
}

double Simulation::docked_dose_msv_per_day(const std::string& station_id, double time_s) const {
    return query::docked_dose_msv_per_day(*this, station_id, time_s);
}

double Simulation::cabin_shield_g_cm2(const domain::ShipClassDefinition& design, double propellant_kg) const {
    return query::cabin_shield_g_cm2(*this, design, propellant_kg);
}

double Simulation::leg_dose_msv_per_day(const domain::ShipClassDefinition& design,
    const domain::StationDefinition& from, const domain::StationDefinition& to, double time_s) const {
    return query::leg_dose_msv_per_day(*this, design, from, to, time_s);
}

double Simulation::hazard_cr_per_day(const domain::ShipClassDefinition& design,
    const domain::StationDefinition& from, const domain::StationDefinition& to, double time_s) const {
    return query::hazard_cr_per_day(*this, design, from, to, time_s);
}

bool Simulation::leg_dose_allowed(const domain::ShipClassDefinition& design,
    const domain::StationDefinition& from, const domain::StationDefinition& to, double time_s, double days) const {
    return query::leg_dose_allowed(*this, design, from, to, time_s, days);
}

double Simulation::cargo_decay_per_day(const domain::ShipClassDefinition& design, const std::string& commodity_id) const {
    return query::cargo_decay_per_day(*this, design, commodity_id);
}

double Simulation::drive_life_h(const domain::ShipClassDefinition& design) const {
    return query::drive_life_h(*this, design);
}

double Simulation::drive_hours(const domain::ShipClassDefinition& design, const domain::TrajectoryPlan& plan) const {
    return query::drive_hours(design, plan);
}

bool Simulation::drive_life_allows(const domain::ShipState& ship, const domain::ShipClassDefinition& design,
    const domain::TrajectoryPlan& plan) const {
    return query::drive_life_allows(*this, ship, design, plan);
}

Simulation::OverhaulBill Simulation::overhaul_bill(const domain::ShipClassDefinition& design, double drive_share,
    double radiator_share) const {
    return query::overhaul_bill(*this, design, drive_share, radiator_share);
}

double Simulation::wear_cr_per_drive_hour(const domain::ShipClassDefinition& design) const {
    return query::wear_cr_per_drive_hour(*this, design);
}

double Simulation::wear_cr_per_flight_day(const domain::ShipClassDefinition& design) const {
    return query::wear_cr_per_flight_day(*this, design);
}

double Simulation::wear_cr_per_propellant_kg(const domain::ShipClassDefinition& design) const {
    return query::wear_cr_per_propellant_kg(*this, design);
}

double Simulation::ship_r_au(const domain::ShipState& ship) const {
    return query::ship_r_au(*this, ship);
}

double Simulation::boiloff_kg_per_day(const domain::ShipClassDefinition& design, double r_au) const {
    return query::boiloff_kg_per_day(*this, design, r_au);
}

double Simulation::leg_boiloff_kg_per_day(const domain::ShipClassDefinition& design, double r1_au, double r2_au) const {
    return query::leg_boiloff_kg_per_day(*this, design, r1_au, r2_au);
}

double Simulation::leg_boiloff_kg_per_day(const domain::ShipClassDefinition& design,
    const domain::StationDefinition& from, const domain::StationDefinition& to, double time_s) const {
    return query::leg_boiloff_kg_per_day(*this, design, from, to, time_s);
}

double Simulation::purchasable_propellant_kg(const domain::ShipState& ship) const {
    return query::purchasable_propellant_kg(*this, ship);
}

double Simulation::provisions_mass_kg(const domain::ShipState& ship) const {
    return query::provisions_mass_kg(*this, ship);
}

void Simulation::buy_provisions(domain::ShipState& ship, domain::StationState& market, double days, bool departing) {
    systems::buy_provisions(*this, ship, market, days, departing);
}

bool Simulation::consider_repair(domain::ShipState& ship) {
    return systems::consider_repair(*this, ship);
}

void Simulation::step_particle_events(double dt_s) {
    systems::step_particle_events(*this, dt_s);
}

void Simulation::accrue_operating_costs(domain::ShipState& ship, double dt_s) {
    systems::accrue_operating_costs(*this, ship, dt_s);
}

void Simulation::buy_propellant(domain::ShipState& ship, double kg) {
    systems::buy_propellant(*this, ship, kg);
}

// Statistics (step 7), with the query forwarders that lived in its topic file.

int Simulation::account_economy(const std::string& economy_id) const {
    return query::account_economy(*this, economy_id);
}

int Simulation::account_faction(const std::string& faction_id) const {
    return query::account_faction(*this, faction_id);
}

int Simulation::account_fleet(const std::string& faction_id) const {
    return query::account_fleet(*this, faction_id);
}

int Simulation::account_station(const domain::StationState& station) const {
    return query::account_station(*this, station);
}

int Simulation::account_households(const domain::StationState& station) const {
    return query::account_households(*this, station);
}

int Simulation::account_station(const std::string& station_id) const {
    return query::account_station(*this, station_id);
}

int Simulation::account_households(const std::string& station_id) const {
    return query::account_households(*this, station_id);
}

double Simulation::station_profit_last_year(const domain::StationState& station) const {
    return query::station_profit_last_year(*this, station);
}

void Simulation::setup_money_journal() {
    systems::setup_money_journal(*this);
}

std::vector<std::vector<double>> Simulation::flow_stocks() const {
    return systems::flow_stocks(*this);
}

double Simulation::account_balance(int account) const {
    return systems::account_balance(*this, account);
}

void Simulation::close_journal_period() {
    systems::close_journal_period(*this);
}

std::string Simulation::build_stats_json() const {
    return systems::build_stats_json(*this);
}

// Patrols (step 8).

void Simulation::init_patrols() {
    systems::init_patrols(*this);
}

void Simulation::step_patrols(double dt_s) {
    systems::step_patrols(*this, dt_s);
}

math::Vec3d Simulation::patrol_position(const domain::PatrolState& patrol) const {
    return systems::patrol_position(*this, patrol);
}

}  // namespace spacetrains::simulation
