#include "simulation/query/Accounts.hpp"

#include <stdexcept>

#include "simulation/query/Lookups.hpp"

namespace spacetrains::simulation::query {

int account_economy(const World& w, const std::string& economy_id) {
    if (economy_id == "earth") {
        return account_earth();
    }
    const auto it = w.data.colony_accounts.find(economy_id);
    if (it == w.data.colony_accounts.end()) {
        throw std::logic_error("money journal: no account for economy " + economy_id);
    }
    return it->second;
}

int account_faction(const World& w, const std::string& faction_id) {
    const auto it = w.data.faction_accounts.find(faction_id);
    if (it == w.data.faction_accounts.end()) {
        throw std::logic_error("money journal: no account for faction " + faction_id);
    }
    return it->second;
}

int account_fleet(const World& w, const std::string& faction_id) {
    const auto it = w.data.fleet_accounts.find(faction_id);
    if (it == w.data.fleet_accounts.end()) {
        throw std::logic_error("money journal: no fleet account for faction " + faction_id);
    }
    return it->second;
}

int account_station(const World& w, const domain::StationState& station) {
    // state.stations is in definitions order (the constructor) and never resized.
    return w.data.first_station_account + static_cast<int>(&station - w.state.stations.data());
}

int account_households(const World& w, const domain::StationState& station) {
    return w.data.first_household_account + static_cast<int>(&station - w.state.stations.data());
}

int account_station(const World& w, const std::string& station_id) {
    return account_station(w, get_station_state(w, station_id));
}

int account_households(const World& w, const std::string& station_id) {
    return account_households(w, get_station_state(w, station_id));
}

}  // namespace spacetrains::simulation::query
