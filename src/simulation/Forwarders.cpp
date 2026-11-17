// Simulation members whose code moved to systems/ or query/ (docs/plans/simulation_split.md,
// steps 5-11): each forwards to its free function on World. Callers move over topic by topic; this file
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

// Households (step 9), with the query forwarders that lived in its topic file.

double Simulation::resident_wages_per_day(const domain::StationState& station) const {
    return query::resident_wages_per_day(*this, station);
}

double Simulation::colony_wage_median(const domain::BackgroundEconomyDefinition& economy) const {
    return query::colony_wage_median(*this, economy);
}

bool Simulation::in_labour_market(const domain::StationState& station) const {
    return query::in_labour_market(*this, station);
}

double Simulation::station_wage_cr_per_day(const domain::StationState& station) const {
    return query::station_wage_cr_per_day(*this, station);
}

double Simulation::station_workers(const domain::StationState& station) const {
    return query::station_workers(*this, station);
}

double Simulation::station_lifetime_profit(const domain::StationState& station) const {
    return query::station_lifetime_profit(*this, station);
}

void Simulation::step_households(double dt_s) {
    systems::step_households(*this, dt_s);
}

double Simulation::colony_people_factor(const std::string& economy_id) const {
    return systems::colony_people_factor(*this, economy_id);
}

double Simulation::colony_affordability(double account, double credit_line, double buffer) {
    return systems::colony_affordability(account, credit_line, buffer);
}

double Simulation::colony_import_share(double affordability, bool essential) {
    return systems::colony_import_share(affordability, essential);
}

void Simulation::step_surface_markets(double dt_s) {
    systems::step_surface_markets(*this, dt_s);
}

double Simulation::labour_migrants_per_day(const domain::StationState& station) const {
    return systems::labour_migrants_per_day(*this, station);
}

double Simulation::labour_emigrants_per_day(const domain::StationState& station) const {
    return systems::labour_emigrants_per_day(*this, station);
}

double Simulation::settler_gain_tail(const domain::StationState& destination, double gain_cr) const {
    return systems::settler_gain_tail(*this, destination, gain_cr);
}

double Simulation::traveller_budget_tail(const std::string& home_id, double cost_cr) const {
    return systems::traveller_budget_tail(*this, home_id, cost_cr);
}

void Simulation::step_labour(double dt_s) {
    systems::step_labour(*this, dt_s);
}

void Simulation::update_market_responses() {
    systems::update_market_responses(*this);
}

std::string Simulation::station_economy_json(const domain::StationState& station) const {
    return systems::station_economy_json(*this, station);
}

void Simulation::step_colony_labour(double dt_s) {
    systems::step_colony_labour(*this, dt_s);
}

std::string Simulation::background_economies_json() const {
    return systems::background_economies_json(*this);
}

const domain::StationDefinition* Simulation::yard_station_for(const domain::StationDefinition& home) const {
    return systems::yard_station_for(*this, home);
}

// Passengers (step 9), with the query forwarders that lived in its topic file.

bool Simulation::employer_pays(domain::PassengerKind kind) const {
    return query::employer_pays(*this, kind);
}

bool Simulation::is_earth_gateway(const std::string& station_id) const {
    return query::is_earth_gateway(*this, station_id);
}

const domain::PassengerDefinition::ReferenceFare* Simulation::reference_fare(
    const std::string& origin_id, const std::string& destination_id) const {
    return query::reference_fare(*this, origin_id, destination_id);
}

double Simulation::port_fuel_price(const std::string& station_id) const {
    return systems::port_fuel_price(*this, station_id);
}

double Simulation::live_fare_cr(const std::string& origin_id, const std::string& destination_id,
    const domain::PassengerDefinition::ReferenceFare& reference) const {
    return systems::live_fare_cr(*this, origin_id, destination_id, reference);
}

const char* Simulation::passenger_kind_name(domain::PassengerKind kind) {
    return systems::passenger_kind_name(kind);
}

double Simulation::settler_share(double payback_years) const {
    return systems::settler_share(*this, payback_years);
}

void Simulation::post_travellers(domain::StationState& origin, domain::PassengerKind kind,
    const std::string& destination_id, const std::string& work_station_id, double count, bool fixed) {
    systems::post_travellers(*this, origin, kind, destination_id, work_station_id, count, fixed);
}

const std::string& Simulation::gateway_for(const std::string& station_id, bool to_station) const {
    return systems::gateway_for(*this, station_id, to_station);
}

double Simulation::travellers_pending(const std::string& station_id, domain::PassengerKind kind) const {
    return systems::travellers_pending(*this, station_id, kind);
}

void Simulation::compute_rotation_tours() {
    systems::compute_rotation_tours(*this);
}

const Simulation::RotationTour* Simulation::rotation_tour(const std::string& station_id) const {
    return systems::rotation_tour(*this, station_id);
}

double Simulation::rotation_positions(const domain::StationState& station) const {
    return systems::rotation_positions(*this, station);
}

double Simulation::passenger_flow_per_day(const std::string& origin_id, const std::string& destination_id) const {
    return systems::passenger_flow_per_day(*this, origin_id, destination_id);
}

double Simulation::carried_passengers_per_day(const std::string& origin_id, const std::string& destination_id) const {
    return systems::carried_passengers_per_day(*this, origin_id, destination_id);
}

Simulation::PassengerRevenue Simulation::expected_passenger_revenue(const std::string& origin_id,
    const std::string& destination_id, double days, double window_days, double berths, double open_share) const {
    return systems::expected_passenger_revenue(*this, origin_id, destination_id, days, window_days, berths, open_share);
}

double Simulation::expected_fare_cr(const std::string& origin_id, const std::string& destination_id, double days,
    double demand_per_trip, double berths) const {
    return systems::expected_fare_cr(*this, origin_id, destination_id, days, demand_per_trip, berths);
}

void Simulation::step_passengers(double dt_s) {
    systems::step_passengers(*this, dt_s);
}

void Simulation::board_passengers(domain::ShipState& ship, domain::StationState& origin,
    const std::string& destination_id, const std::vector<MissionChoice::PassengerPick>& picks, double days) {
    systems::board_passengers(*this, ship, origin, destination_id, picks, days);
}

void Simulation::land_passengers(domain::ShipState& ship, domain::StationState& destination) {
    systems::land_passengers(*this, ship, destination);
}

std::string Simulation::ship_passengers_json(const domain::ShipState& ship, const domain::ShipClassDefinition& design) const {
    return systems::ship_passengers_json(*this, ship, design);
}

std::string Simulation::station_passengers_json(const domain::StationState& station) const {
    return systems::station_passengers_json(*this, station);
}

std::vector<Simulation::ReferenceFareRow> Simulation::compute_reference_fares() const {
    return systems::compute_reference_fares(*this);
}

// Failures (step 10).

void Simulation::init_failure_clocks() {
    systems::init_failure_clocks(*this);
}

void Simulation::step_failures() {
    systems::step_failures(*this);
}

bool Simulation::consider_salvage(domain::ShipState& ship, double spot_per_day) {
    return systems::consider_salvage(*this, ship, spot_per_day);
}

void Simulation::reach_derelict(domain::ShipState& ship) {
    systems::reach_derelict(*this, ship);
}

// FleetInvestment (step 11), with the query forwarder that lived in its topic file.

double Simulation::internal_money_supply() const {
    return systems::internal_money_supply(*this);
}

void Simulation::step_treasuries(double dt_s) {
    systems::step_treasuries(*this, dt_s);
}

double Simulation::faction_credit_limit(const std::string& faction_id) const {
    return query::faction_credit_limit(*this, faction_id);
}

double Simulation::station_tax_ceiling(const domain::OpenEconomyDefinition& open, double treasury) {
    return systems::station_tax_ceiling(open, treasury);
}

void Simulation::step_fleet_investment() {
    systems::step_fleet_investment(*this);
}

}  // namespace spacetrains::simulation
