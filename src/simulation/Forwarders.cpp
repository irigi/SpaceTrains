// Simulation members whose code moved to systems/ (docs/plans/simulation_split.md, step 5):
// each forwards to its free function on World. Callers move over topic by topic; this file
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

}  // namespace spacetrains::simulation
