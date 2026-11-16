#include "simulation/query/Labour.hpp"

#include <algorithm>

namespace spacetrains::simulation::query {

bool is_earth_gateway(const World& w, const std::string& station_id) {
    const auto& gateways = w.universe_.passengers.earth_gateways;
    return std::find(gateways.begin(), gateways.end(), station_id) != gateways.end();
}

bool in_labour_market(const World& w, const domain::StationState& station) {
    return labour_on(w) && !is_earth_gateway(w, station.station_id);
}

double station_wage_cr_per_day(const World& w, const domain::StationState& station) {
    if (in_labour_market(w, station) && station.wage_cr_per_day > 0.0) {
        return station.wage_cr_per_day;
    }
    const auto& households = w.universe_.households;
    const auto multiple = households.wage_multiple.find(station.station_id);
    return w.universe_.labour.reference_wage_cr_per_day * (multiple == households.wage_multiple.end() ? 1.0 : multiple->second);
}

double station_workers(const World& w, const domain::StationState& station) {
    return station.population * w.universe_.households.participation;
}

}  // namespace spacetrains::simulation::query
