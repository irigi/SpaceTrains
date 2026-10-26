// Liners, scheduled stops and route contracts.

#include "simulation/Simulation.hpp"
#include "domain/ShipDesign.hpp"
#include "simulation/CrewEnvironment.hpp"

#include "util/Profiling.hpp"

#include <algorithm>
#include <chrono>
#include <array>
#include <tuple>
#include <cmath>
#include <numbers>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "simulation/SimulationInternal.hpp"

namespace spacetrains::simulation {

using namespace detail;

double Simulation::fleet_hold_units() const {
    double units = 0.0;
    for (const auto& ship : ships_) {
        if (liner_stops(ship) == nullptr) {  // liners are a public service, outside the cap
            units += ship_design(ship).cargo_capacity_units;
        }
    }
    return units;
}

std::size_t Simulation::fleet_ship_count() const {
    return static_cast<std::size_t>(std::count_if(ships_.begin(), ships_.end(),
        [&](const domain::ShipState& ship) { return liner_stops(ship) == nullptr; }));
}

const std::vector<std::string>* Simulation::liner_stops(const domain::ShipState& ship) const {
    const auto it = universe_.liners.find(ship.id);
    return it == universe_.liners.end() ? nullptr : &it->second;
}

std::string Simulation::next_liner_stop(const domain::ShipState& ship, const std::string& from_id) const {
    const auto* stops = liner_stops(ship);
    if (stops == nullptr) {
        return {};
    }
    const auto at = std::find(stops->begin(), stops->end(), from_id);
    return at == stops->end() ? stops->front() : stops->at(static_cast<std::size_t>(at - stops->begin() + 1) % stops->size());
}

bool Simulation::under_contract(const domain::ShipState& ship) const {
    return !ship.contract.stops.empty() && game_time_s_ < ship.contract.until_s;
}

const std::vector<std::string>* Simulation::scheduled_stops(const domain::ShipState& ship) const {
    if (const auto* stops = liner_stops(ship); stops != nullptr) {
        return stops;
    }
    return under_contract(ship) ? &ship.contract.stops : nullptr;
}

std::string Simulation::next_scheduled_stop(const domain::ShipState& ship, const std::string& from_id) const {
    if (liner_stops(ship) != nullptr) {
        return next_liner_stop(ship, from_id);
    }
    const auto& stops = ship.contract.stops;
    if (stops.empty()) {
        return {};
    }
    const auto at = std::find(stops.begin(), stops.end(), from_id);
    return at == stops.end() ? stops.front() : stops.at(static_cast<std::size_t>(at - stops.begin() + 1) % stops.size());
}

double Simulation::contract_dwell_days(const domain::ShipContract& contract) {
    double legs = 0.0;
    for (const double days : contract.leg_days) {
        legs += days;
    }
    return std::max(0.0, contract.cycle_days - legs);
}

double Simulation::contracted_flow(const std::string& station_id, const std::string& commodity_id) const {
    double flow = 0.0;
    for (const auto& ship : ships_) {
        if (!under_contract(ship) || ship.contract.cycle_days <= 0.0) {
            continue;
        }
        for (const auto& delivery : ship.contract.deliveries) {
            if (delivery.to_station_id == station_id && delivery.commodity_id == commodity_id) {
                flow += delivery.units / ship.contract.cycle_days;
            }
        }
    }
    return flow;
}

void Simulation::with_parallel_plans(const std::string& prefix, const std::function<void()>& pass) {
    for (;;) {
        std::vector<DeferredPlan> deferred;
        auto* const outer = deferred_plans_;
        deferred_plans_ = &deferred;
        deferred_keys_.clear();
        deferred_prefix_ = prefix;
        pass();
        deferred_plans_ = outer;
        deferred_prefix_.clear();
        if (deferred.empty()) {
            return;
        }
        std::vector<domain::TrajectoryPlan> results(deferred.size());
        thread_pool_->parallel_for(deferred.size(), [&](std::size_t i) { results[i] = deferred[i].compute(); });
        for (std::size_t i = 0; i < deferred.size(); ++i) {
            deferred[i].commit(std::move(results[i]));
        }
    }
}

std::pair<domain::ShipContract, double> Simulation::find_contract(
    const domain::ShipState& ship, const domain::ShipClassDefinition& ship_class) {
    const auto& market = universe_.market;
    constexpr double day_s = 86400.0;
    const auto& origin = get_station_definition(ship.current_station_id);
    const double hold = ship_class.cargo_capacity_units;
    if (hold <= 0.0) {
        return {{}, 0.0};
    }
    double crew = ship_class.crew_size * universe_.ship_operations.wage_cr_per_crew_day;
    for (const auto& [good, units_per_crew_day] : universe_.ship_operations.life_support_units_per_crew_day) {
        crew += ship_class.crew_size * units_per_crew_day * get_commodity(good).base_price;
    }
    const double time_cost = daily_capital_cost(ship_class) + crew + hazard_cr_per_day(ship_class, origin, origin, game_time_s_)
        + wear_cr_per_flight_day(ship_class);
    const double fuel_base = get_commodity(economy::FUEL_ID).base_price;
    const auto n = universe_.stations.size();
    // Every leg once, from today's bucket (later legs' windows are not dated: an estimate).
    std::vector<LegEstimate> legs(n * n);
    with_parallel_plans("contract#", [&] {
        for (std::size_t a = 0; a < n; ++a) {
            for (std::size_t b = 0; b < n; ++b) {
                if (a != b) {
                    legs[a * n + b] = estimate_leg(ship_class, universe_.stations[a], universe_.stations[b], game_time_s_,
                        ship_class.propellant_capacity_kg, market.fill_ref * hold * 100.0);
                }
            }
        }
    });
    // What each station can spare of each good, and what each still needs under no contract.
    struct Need {
        std::string commodity_id;
        double free_use {0.0};   // units per day not yet contracted
        double price {0.0};
        double cap {0.0};        // cost-plus contracts: never above what the station has been paying
    };
    std::vector<std::vector<Need>> needs(n);
    std::vector<std::unordered_map<std::string, std::pair<double, double>>> spares(n);  // good -> (units now, per day)
    for (std::size_t i = 0; i < n; ++i) {
        const auto& station = universe_.stations[i];
        const auto& state = get_station_state(station.id);
        const auto& rates = economy_.get_station_net_rates(station);
        for (const auto& [commodity_id, rate] : rates) {
            const bool makes = commodity_id == economy::FUEL_ID ? economy_.fuel_factory_output(station) > 0.0 : rate > 0.0;
            if (makes) {
                const auto stock = state.inventory.find(commodity_id);
                const double spare = std::max(0.0, (stock == state.inventory.end() ? 0.0 : stock->second) - (8.0 + std::max(0.0, rate) * 7.0));
                const double per_day = commodity_id == economy::FUEL_ID ? economy_.fuel_factory_output(station) : rate;
                spares[i][commodity_id] = {spare, std::max(0.0, per_day)};
            }
        }
        for (const auto& [commodity_id, stats] : state.order_stats) {
            if (!priced_order(state, commodity_id)) {
                continue;
            }
            const double use = order_levels(state, commodity_id, stats).use_per_day;
            const double free_use = use - contracted_flow(station.id, commodity_id);
            if (free_use <= 0.0) {
                continue;
            }
            // The price a loop is signed at: the order's market price where ships compete,
            // else the going rate, never above today's bid.
            double price = market.competitive > 0.0 && stats.market_price > 0.0 ? stats.market_price : stats.going_rate;
            price = std::min(price, std::max(stats.order_price, economy_.reference_price(station, commodity_id)));
            needs[i].push_back({commodity_id, free_use, price, std::max(stats.going_rate, price)});
        }
    }
    std::size_t origin_index = n;
    for (std::size_t i = 0; i < n; ++i) {
        if (universe_.stations[i].id == origin.id) {
            origin_index = i;
        }
    }
    if (origin_index == n) {
        return {{}, 0.0};
    }
    const auto max_stops = static_cast<std::size_t>(std::clamp(market.contract_max_stops, 2.0, 4.0));
    // A loop's value with the given legs: profit per day after time and fuel, its hold filled
    // on each leg by margin per unit with what the next stop still needs.
    // A loop may wait in port at its first stop before each lap (`dwell_days`), so each lap
    // carries more of the stations' needs on the same fuel; the wait costs the ship's time.
    // (Short hops such as Earth <-> Moon would otherwise fly half-empty loops.)
    const auto value = [&](const std::vector<std::size_t>& loop, const std::vector<LegEstimate>& loop_legs,
                           domain::ShipContract* contract, double* contract_profit_per_day = nullptr,
                           double dwell_days = 0.0) {
        std::vector<double> buys;
        std::vector<double> caps;
        double cycle_days = dwell_days;
        for (const auto& leg : loop_legs) {
            if (!leg.feasible) {
                return 0.0;
            }
            cycle_days += leg.travel_days;
        }
        if (cycle_days <= 0.5 || cycle_days > market.contract_term_days) {
            return 0.0;
        }
        double profit = -time_cost * cycle_days;
        std::vector<domain::ShipContract::Delivery> deliveries;
        for (std::size_t k = 0; k < loop.size(); ++k) {
            const auto from = loop[k];
            const auto to = loop[(k + 1) % loop.size()];
            const auto& from_state = get_station_state(universe_.stations[from].id);
            const double fuel_price = std::max(fuel_base, station_price(from_state, economy::FUEL_ID));
            profit -= loop_legs[k].propellant_kg / FUEL_UNITS_TO_KG * fuel_price
                + loop_legs[k].propellant_kg * wear_cr_per_propellant_kg(ship_class);
            struct Pick {
                const Need* need;
                double units;
                double margin;
            };
            std::vector<Pick> picks;
            for (const auto& need : needs[to]) {
                const auto spare = spares[from].find(need.commodity_id);
                if (spare == spares[from].end()) {
                    continue;
                }
                const double available = std::max(spare->second.first, spare->second.second * cycle_days);
                const double units = std::min(market.contract_cover * need.free_use * cycle_days, available);
                const double buy = station_price(from_state, need.commodity_id);
                if (units >= market.contract_min_units && need.price > buy) {
                    picks.push_back({&need, units, need.price - buy});
                }
            }
            std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.margin > b.margin; });
            double room = hold;
            for (const auto& pick : picks) {
                const double units = std::min(pick.units, room);
                if (units < market.contract_min_units) {
                    continue;
                }
                room -= units;
                profit += units * pick.margin;
                deliveries.push_back({.to_station_id = universe_.stations[to].id,
                    .commodity_id = pick.need->commodity_id, .units = units, .unit_price = pick.need->price});
                buys.push_back(pick.need->price - pick.margin);
                caps.push_back(pick.need->cap);
            }
        }
        if (deliveries.empty()) {
            return 0.0;
        }
        // Cost-plus: each unit pays its purchase plus its share of the loop's time and fuel, plus
        // the contract markup, at most what the station has been paying. The loop is still chosen
        // by the value it creates at market prices.
        if (market.contract_cost_plus > 0.0 && contract != nullptr) {
            double units_total = 0.0;
            for (const auto& delivery : deliveries) {
                units_total += delivery.units;
            }
            const double cost = -(profit - [&] {
                double margin = 0.0;
                for (std::size_t d = 0; d < deliveries.size(); ++d) {
                    margin += deliveries[d].units * (deliveries[d].unit_price - buys[d]);
                }
                return margin;
            }());
            const double share = cost / std::max(1.0, units_total);
            double contract_profit = -cost;
            for (std::size_t d = 0; d < deliveries.size(); ++d) {
                deliveries[d].unit_price = std::min(caps[d], buys[d] + share * (1.0 + market.contract_cost_plus));
                contract_profit += deliveries[d].units * (deliveries[d].unit_price - buys[d]);
            }
            *contract_profit_per_day = contract_profit / cycle_days;
        } else if (contract_profit_per_day != nullptr) {
            *contract_profit_per_day = profit / cycle_days;
        }
        if (contract != nullptr) {
            *contract = {};
            for (std::size_t k = 0; k < loop.size(); ++k) {
                contract->stops.push_back(universe_.stations[loop[k]].id);
                contract->leg_days.push_back(loop_legs[k].travel_days);
            }
            contract->deliveries = std::move(deliveries);
            contract->cycle_days = cycle_days;
        }
        return profit / cycle_days;
    };
    static constexpr std::array<double, 5> kDwellDays {0.0, 7.0, 14.0, 28.0, 56.0};
    const auto best_dwell = [&](const std::vector<std::size_t>& candidate, const std::vector<LegEstimate>& loop_legs) {
        double best_value = 0.0;
        double best_wait = 0.0;
        for (const double dwell : kDwellDays) {
            if (const double v = value(candidate, loop_legs, nullptr, nullptr, dwell); v > best_value) {
                best_value = v;
                best_wait = dwell;
            }
        }
        return std::make_pair(best_value, best_wait);
    };
    // Every loop from this port, valued with today's legs; the best few again with each leg
    // dated from when the one before it arrives (launch windows).
    std::vector<std::pair<double, std::vector<std::size_t>>> ranked;
    std::vector<std::size_t> loop {origin_index};
    const std::function<void()> extend = [&] {
        if (loop.size() >= 2) {
            std::vector<LegEstimate> loop_legs;
            for (std::size_t k = 0; k < loop.size(); ++k) {
                loop_legs.push_back(legs[loop[k] * n + loop[(k + 1) % loop.size()]]);
            }
            if (const double score = best_dwell(loop, loop_legs).first; score > 0.0) {
                ranked.emplace_back(score, loop);
            }
        }
        if (loop.size() >= max_stops) {
            return;
        }
        for (std::size_t next = 0; next < n; ++next) {
            if (std::find(loop.begin(), loop.end(), next) != loop.end() || !legs[loop.back() * n + next].feasible) {
                continue;
            }
            loop.push_back(next);
            extend();
            loop.pop_back();
        }
    };
    extend();
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    ranked.resize(std::min(ranked.size(), static_cast<std::size_t>(std::max(1.0, market.contract_refine))));
    std::vector<std::vector<LegEstimate>> dated(ranked.size());
    with_parallel_plans("contract#", [&] {
        for (std::size_t r = 0; r < ranked.size(); ++r) {
            const auto& candidate = ranked[r].second;
            dated[r].clear();
            double t = game_time_s_;
            for (std::size_t k = 0; k < candidate.size(); ++k) {
                const auto leg = estimate_leg(ship_class, universe_.stations[candidate[k]],
                    universe_.stations[candidate[(k + 1) % candidate.size()]], t, ship_class.propellant_capacity_kg,
                    market.fill_ref * hold * 100.0);
                dated[r].push_back(leg);
                t += leg.travel_days * day_s;
            }
        }
    });
    domain::ShipContract best;
    double best_score = 0.0;
    double best_profit = 0.0;
    for (std::size_t r = 0; r < ranked.size(); ++r) {
        domain::ShipContract contract;
        double profit = 0.0;
        const double dwell = best_dwell(ranked[r].second, dated[r]).second;
        if (const double score = value(ranked[r].second, dated[r], &contract, &profit, dwell); score > best_score) {
            best_score = score;
            best_profit = profit;
            best = std::move(contract);
        }
    }
    // What the ship earns on it (cost-plus: its markup on the loop's cost).
    best_score = best_profit;
    if (!best.stops.empty()) {
        best.signed_s = game_time_s_;
        best.until_s = game_time_s_ + market.contract_term_days * day_s;
    }
    return {best, best_score};
}

}  // namespace spacetrains::simulation
