#include "simulation/systems/Passengers.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "simulation/SimulationInternal.hpp"
#include "simulation/query/Finance.hpp"
#include "simulation/query/Labour.hpp"
#include "simulation/query/Lookups.hpp"
#include "simulation/query/Passengers.hpp"
#include "simulation/query/ShipRates.hpp"
#include "simulation/systems/Market.hpp"

namespace spacetrains::simulation::systems {

using namespace detail;

double port_fuel_price(const World& w, const std::string& station_id) {
    const double base = query::get_commodity(w, economy::FUEL_ID).base_price;
    return std::max(base, systems::station_price(w, query::get_station_state(w, station_id), economy::FUEL_ID));
}

// The reference fare at today's fuel prices: its propellant per seat is bought at the origin
// (and the return share at the destination) at those ports' prices instead of the base price.
// Earth makes no fuel, so trips from Earth cost several times the base-price fare's fuel.
double live_fare_cr(const World& w, const std::string& origin_id, const std::string& destination_id,
    const domain::PassengerDefinition::ReferenceFare& reference) {
    const double base = query::get_commodity(w, economy::FUEL_ID).base_price;
    return reference.fare_cr
        + (reference.fuel_out_kg * (systems::port_fuel_price(w, origin_id) - base)
              + reference.fuel_back_kg * (systems::port_fuel_price(w, destination_id) - base))
        / FUEL_UNITS_TO_KG;
}

PassengerOffer passenger_offer(const World& w, const domain::ShipState& ship,
    const domain::ShipClassDefinition& design, const domain::StationState& origin,
    const domain::StationDefinition& destination, double days, double propellant_cr_per_kg, double berths) {
    (void)ship;
    PassengerOffer offer;
    if (berths < 1.0 || origin.waiting.empty()) {
        return offer;
    }
    const auto* reference = query::reference_fare(w, origin.station_id, destination.id);
    if (reference == nullptr) {
        return offer;
    }
    const auto& env = w.universe_.ship_environment;
    const auto& origin_def = query::get_station_definition(w, origin.station_id);
    const double dose_rate = env.radiation ? query::leg_dose_msv_per_day(w, design, origin_def, destination, w.game_time_s_) : 0.0;
    const double reference_dose_rate = env.radiation ? env.gcr_msv_per_day : 0.0;
    double provisions_per_day = 0.0;
    for (const auto& [commodity_id, units_per_day] : w.universe_.ship_operations.life_support_units_per_crew_day) {
        provisions_per_day += units_per_day * systems::station_price(w, origin, commodity_id);
    }
    const double marginal = provisions_per_day * days + query::passenger_kg(w, days) * propellant_cr_per_kg;
    // Each group pays what this trip is worth to it against the reference trip (hedonic), or,
    // where more people wait than there are berths, the fare that clears the berths: what the
    // first person left behind would have paid (a uniform-price auction). Never more than its
    // own budget allows, never less than what carrying it costs.
    struct Candidate {
        std::size_t group;
        double whole;
        double willing;   // highest fare the group pays for this trip
        double hedonic;
    };
    std::vector<Candidate> candidates;
    for (std::size_t g = 0; g < origin.waiting.size(); ++g) {
        const auto& group = origin.waiting[g];
        const double whole = std::floor(group.count + 1e-9);
        if (group.destination_id != destination.id || whole < 1.0) {
            continue;
        }
        if (env.radiation && group.dose_msv + dose_rate * days > env.career_limit_msv) {
            continue;
        }
        const double vot = group.value_of_time_cr_per_day;
        const double nonfare = query::passenger_nonfare_cr(w, vot, days, dose_rate);
        const double willing = group.max_cost_cr - nonfare;
        if (willing <= marginal) {
            continue;
        }
        candidates.push_back({g, whole, willing,
            systems::live_fare_cr(w, origin.station_id, destination.id, *reference)
                + query::passenger_nonfare_cr(w, vot, reference->days, reference_dose_rate) - nonfare});
    }
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const Candidate& a, const Candidate& b) { return a.willing > b.willing; });
    double left = std::floor(berths + 1e-9);
    double clearing = 0.0;
    std::size_t boarding = 0;
    for (const auto& candidate : candidates) {
        if (left < 1.0) {
            clearing = candidate.willing;
            break;
        }
        const double take = std::min(candidate.whole, left);
        left -= take;
        ++boarding;
        if (take < candidate.whole) {
            clearing = candidate.willing;  // the rest of this group stays behind
            break;
        }
    }
    left = std::floor(berths + 1e-9);
    // An employer pays only what it has above its credit floor.
    std::map<std::string, double> budgets;
    for (std::size_t c = 0; c < boarding; ++c) {
        const auto& candidate = candidates[c];
        const double fare = std::min(candidate.willing, std::max({candidate.hedonic, clearing, marginal}));
        if (fare <= marginal) {
            continue;
        }
        double count = std::min(candidate.whole, left);
        const auto& group = origin.waiting[candidate.group];
        if (query::employer_pays(w, group.kind) && !group.work_station_id.empty()) {
            auto budget = budgets.find(group.work_station_id);
            if (budget == budgets.end()) {
                budget = budgets.emplace(group.work_station_id, query::get_station_state(w, group.work_station_id).credits
                    - w.universe_.open_economy.station_credit_floor).first;
            }
            count = std::min(count, std::floor(std::max(0.0, budget->second) / fare + 1e-9));
            if (count < 1.0) {
                continue;
            }
            budget->second -= count * fare;
        } else if (query::colony_employer(group.work_station_id)) {
            // A colony pays its recruits' passage while its account allows.
            const auto& economy_id = group.work_station_id.substr(std::string("colony:").size());
            auto budget = budgets.find(group.work_station_id);
            if (budget == budgets.end()) {
                budget = budgets.emplace(group.work_station_id, query::economy_balance(w, economy_id)).first;   // out of savings, not credit
            }
            count = std::min(count, std::floor(std::max(0.0, budget->second) / fare + 1e-9));
            if (count < 1.0) {
                continue;
            }
            budget->second -= count * fare;
        } else if (query::households_on(w)) {
            // Residents travel on their savings: a station's households pay only what they have.
            const std::string& home = group.kind == domain::PassengerKind::VisitorHome ? group.destination_id : origin.station_id;
            if (!query::is_earth_gateway(w, home)) {
                const auto key = "households:" + home;
                auto budget = budgets.find(key);
                if (budget == budgets.end()) {
                    // Their travel budget (or, without one, their savings beyond half their buffer).
                    budget = budgets.emplace(key, query::household_travel_budget(w, home, 0.0)).first;
                }
                count = std::min(count, std::floor(std::max(0.0, budget->second) / fare + 1e-9));
                if (count < 1.0) {
                    continue;
                }
                budget->second -= count * fare;
            }
        }
        left -= count;
        offer.picks.push_back({.group = candidate.group, .count = count, .fare_cr = fare});
        offer.people += count;
        offer.revenue_cr += count * fare;
        offer.marginal_cr += count * provisions_per_day * days;
        offer.time_cr_per_day += count * query::passenger_nonfare_cr(w, origin.waiting[candidate.group].value_of_time_cr_per_day, 1.0, dose_rate);
    }
    offer.mass_kg = offer.people * query::passenger_kg(w, days);
    return offer;
}

}  // namespace spacetrains::simulation::systems
