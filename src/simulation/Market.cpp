// Prices, station orders, emergencies and the daily market (reference prices, quotes, bids).

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

domain::Inventory Simulation::starting_inventory(const domain::StationDefinition& station) const {
    // A station starts with what it would hold if it had been supplied all along: each good it
    // consumes at least at its seeded order-up-to level (step 29; until then its resupply cover).
    // With the few weeks of the data files, Ceres and Ganymede ran out of oxygen on day 20 and
    // the outposts' first emergencies stayed open for 450-660 days while the first freighters
    // were on their way. The raise is scaled down to fit 85% of the station's storage. Depot
    // fuel starts as the data has it: its use is what ships buy there, unknown until they do.
    domain::Inventory inventory = station.initial_inventory;
    domain::Inventory raises;
    const domain::StationState seeded {.station_id = station.id};
    for (const auto& [commodity_id, rate] : economy_.get_station_net_rates(station)) {
        if (rate >= 0.0 || !has_order(station, commodity_id)) {
            continue;
        }
        const double stock = inventory.contains(commodity_id) ? inventory.at(commodity_id) : 0.0;
        const double raise = order_levels(seeded, commodity_id, seed_order_stats(station, commodity_id)).order_up_to_units - stock;
        if (raise > 0.0) {
            raises[commodity_id] = raise;
        }
    }
    double raised_storage = 0.0;
    for (const auto& [commodity_id, raise] : raises) {
        raised_storage += economy_.storage_used_units({{commodity_id, raise}});
    }
    const double room = 0.85 * station.storage_capacity_units - economy_.storage_used_units(station, inventory);
    const double scale = station.storage_capacity_units > 0.0 && raised_storage > room
        ? std::max(0.0, room) / raised_storage : 1.0;
    for (const auto& [commodity_id, raise] : raises) {
        inventory[commodity_id] += raise * scale;
    }
    // A background economy's gateway starts stocked with what it sells in any amount.
    for (const auto* market : economy_.surface_markets_at(station.id)) {
        if (economy_.surface_supplied(station, market->commodity_id)) {
            inventory[market->commodity_id] = std::max(inventory[market->commodity_id], economy_.surface_depth_units());
        }
    }
    return inventory;
}

std::vector<Simulation::ReferencePrice> Simulation::compute_reference_prices() const {
    constexpr double MIN_HOLD_UNITS = 150.0;
    constexpr std::size_t PRODUCERS_PER_GOOD = 2;
    const auto& pricing = universe_.pricing;
    const auto departures = static_cast<int>(pricing.reference_departures);
    std::vector<const domain::ShipClassDefinition*> hulls;
    for (const auto& ship_class : universe_.ship_classes) {
        if (ship_class.id == ship_class.hull_id && ship_class.cargo_capacity_units >= MIN_HOLD_UNITS) {
            hulls.push_back(&ship_class);
        }
    }
    const double fuel_cr_per_kg = get_commodity(economy::FUEL_ID).base_price / FUEL_UNITS_TO_KG;
    const auto daily_cost = [&](const domain::ShipClassDefinition& ship_class) {
        double cost = daily_capital_cost(ship_class) + ship_class.crew_size * universe_.ship_operations.wage_cr_per_crew_day;
        for (const auto& [commodity_id, units_per_crew_day] : universe_.ship_operations.life_support_units_per_crew_day) {
            cost += ship_class.crew_size * units_per_crew_day * get_commodity(commodity_id).base_price;
        }
        return cost;
    };

    // What to price: every consumer's net-consumed goods (export markets keep their flat price),
    // with its nearest producers; and fuel at every depot without a factory (step 20: Mars, 250
    // days from the nearest factory, got almost no fuel while it was priced like a factory's).
    struct Route {
        const domain::StationDefinition* consumer {nullptr};
        const domain::CommodityDefinition* commodity {nullptr};
        std::vector<const domain::StationDefinition*> producers;
    };
    std::vector<Route> routes;
    for (const auto& consumer : universe_.stations) {
        auto goods = economy_.get_station_net_rates(consumer);
        const bool fuel_depot = economy_.fuel_factory_output(consumer) <= 0.0;
        if (fuel_depot) {
            goods[economy::FUEL_ID] = std::min(goods[economy::FUEL_ID], -1.0);
        } else {
            goods.erase(economy::FUEL_ID);
        }
        for (const auto& [commodity_id, rate] : goods) {
            // A background economy's gateway is priced at the surface's ask (no route).
            if (rate >= 0.0 || economy_.is_export_market(consumer, commodity_id) || economy_.surface_supplied(consumer, commodity_id)) {
                continue;
            }
            Route route {.consumer = &consumer, .commodity = &get_commodity(commodity_id)};
            for (const auto& producer : universe_.stations) {
                const auto& rates = economy_.get_station_net_rates(producer);
                const auto it = rates.find(commodity_id);
                if (producer.id != consumer.id
                    && ((it != rates.end() && it->second > 0.0) || economy_.surface_supplied(producer, commodity_id))) {
                    route.producers.push_back(&producer);
                }
            }
            std::stable_sort(route.producers.begin(), route.producers.end(), [&](const auto* a, const auto* b) {
                return economy_.transfer_days(*a, consumer) < economy_.transfer_days(*b, consumer);
            });
            if (route.producers.size() > PRODUCERS_PER_GOOD) {
                route.producers.resize(PRODUCERS_PER_GOOD);
            }
            if (!route.producers.empty()) {
                routes.push_back(std::move(route));
            }
        }
    }
    std::sort(routes.begin(), routes.end(), [](const Route& a, const Route& b) {
        return std::tie(a.consumer->id, a.commodity->id) < std::tie(b.consumer->id, b.commodity->id);
    });

    // Every leg to plan: (from, to, hull, departure, payload); the empty returns are shared.
    struct Leg {
        const domain::StationDefinition* from {nullptr};
        const domain::StationDefinition* to {nullptr};
        const domain::ShipClassDefinition* hull {nullptr};
        double departure_s {0.0};
        double payload_kg {0.0};
    };
    std::vector<Leg> legs;
    std::map<std::string, std::size_t> leg_index;
    const auto add_leg = [&](const Leg& leg) {
        const auto key = std::format("{}|{}|{}|{:.0f}|{:.0f}", leg.from->id, leg.to->id, leg.hull->id, leg.departure_s, leg.payload_kg);
        if (const auto it = leg_index.find(key); it != leg_index.end()) {
            return it->second;
        }
        legs.push_back(leg);
        return leg_index[key] = legs.size() - 1;
    };
    const auto departure_s = [&](int d) { return d * 365.25 * 86400.0 / departures; };
    // A full hold, or a part of it where a full one is too heavy for the transfer (Mercury).
    constexpr std::array<double, 3> LOADS {1.0, 0.5, 0.25};
    for (const auto& route : routes) {
        for (const auto* producer : route.producers) {
            for (const auto* hull : hulls) {
                for (const double load : LOADS) {
                    for (int d = 0; d < departures; ++d) {
                        add_leg({producer, route.consumer, hull, departure_s(d),
                            load * hull->cargo_capacity_units * route.commodity->mass_per_unit_kg});
                    }
                }
            }
        }
    }
    const auto plan_leg = [&](const Leg& leg) {
        domain::ShipState probe;
        probe.class_id = leg.hull->id;
        probe.current_station_id = leg.from->id;
        const trajectory::PlanningOptions options {
            .propellant_cr_per_kg = fuel_cr_per_kg,
            .time_cr_per_day = daily_cost(*leg.hull),
            .payload_kg = leg.payload_kg,
            .purchasable_propellant_kg = leg.hull->propellant_capacity_kg,
            .reserve_fraction = PROPELLANT_RESERVE_FRACTION,
            .boiloff_kg_per_day = leg_boiloff_kg_per_day(*leg.hull, *leg.from, *leg.to, leg.departure_s),
            .include_path = false,
        };
        const auto& planner = (leg.hull->propulsion_type == "variable_isp" && variable_isp_planner_)
            ? static_cast<const trajectory::ITrajectoryPlanner&>(*variable_isp_planner_)
            : static_cast<const trajectory::ITrajectoryPlanner&>(*kepler_planner_);
        return planner.plan_transfer(*leg.from, *leg.to, probe, *leg.hull, leg.departure_s, options);
    };
    std::vector<domain::TrajectoryPlan> outbound(legs.size());
    thread_pool_->parallel_for(legs.size(), [&](std::size_t i) { outbound[i] = plan_leg(legs[i]); });
    // The empty returns leave when the loaded legs arrive.
    std::vector<Leg> returns;
    std::map<std::string, std::size_t> return_index;
    std::vector<std::size_t> return_of(legs.size(), std::numeric_limits<std::size_t>::max());
    for (std::size_t i = 0; i < legs.size(); ++i) {
        if (!outbound[i].feasible) {
            continue;
        }
        const Leg back {legs[i].to, legs[i].from, legs[i].hull, outbound[i].arrival_time_s, 0.0};
        const auto key = std::format("{}|{}|{}|{:.0f}", back.from->id, back.to->id, back.hull->id, back.departure_s);
        auto it = return_index.find(key);
        if (it == return_index.end()) {
            returns.push_back(back);
            it = return_index.emplace(key, returns.size() - 1).first;
        }
        return_of[i] = it->second;
    }
    std::vector<domain::TrajectoryPlan> inbound(returns.size());
    thread_pool_->parallel_for(returns.size(), [&](std::size_t i) { inbound[i] = plan_leg(returns[i]); });

    std::vector<ReferencePrice> prices;
    for (const auto& route : routes) {
        ReferencePrice best {
            .station_id = route.consumer->id,
            .commodity_id = route.commodity->id,
            .base_price = route.commodity->base_price,
            .transport_per_unit = std::numeric_limits<double>::infinity(),
        };
        double best_total = std::numeric_limits<double>::infinity();
        for (const auto* producer : route.producers) {
            // What the good costs where it is loaded: a background economy's ask at its gateway
            // (a small colony sells below Earth's price), the base price elsewhere.
            double producer_price = route.commodity->base_price;
            if (const auto* market = economy_.surface_market(*producer, route.commodity->id); market != nullptr && market->ask_cr > 0.0) {
                producer_price = market->ask_cr;
            } else if (route.commodity->id == economy::FUEL_ID && economy_.fuel_factory_output(*producer) > 0.0) {
                producer_price = economy_.reference_price(*producer, economy::FUEL_ID);   // the factory's own price
            }
            for (const auto* hull : hulls) {
              for (const double load : LOADS) {
                const double units = load * hull->cargo_capacity_units;
                std::vector<std::pair<double, double>> trips;  // (cost per unit, round-trip days)
                for (int d = 0; d < departures; ++d) {
                    const auto key = std::format("{}|{}|{}|{:.0f}|{:.0f}", producer->id, route.consumer->id, hull->id,
                        departure_s(d), units * route.commodity->mass_per_unit_kg);
                    const auto i = leg_index.at(key);
                    if (return_of[i] == std::numeric_limits<std::size_t>::max() || !inbound[return_of[i]].feasible) {
                        continue;
                    }
                    const auto& out = outbound[i];
                    const auto& back = inbound[return_of[i]];
                    const double days = (back.arrival_time_s - departure_s(d)) / 86400.0;
                    const double cost = daily_cost(*hull) * days
                        + (consumed_propellant_kg(out) + consumed_propellant_kg(back)) * fuel_cr_per_kg;
                    trips.emplace_back(cost / units, days);
                }
                // A hull that cannot fly the route at most dates does not set its price.
                if (trips.size() * 2 <= static_cast<std::size_t>(departures)) {
                    continue;
                }
                std::sort(trips.begin(), trips.end());
                const auto& median = trips[trips.size() / 2];
                if (producer_price + pricing.carrier_margin * median.first < best_total) {
                    best_total = producer_price + pricing.carrier_margin * median.first;
                    best.base_price = producer_price;
                    best.transport_per_unit = median.first;
                    best.round_trip_days = median.second;
                    best.producer_id = producer->id;
                    best.class_id = load < 1.0 ? std::format("{}@{:.0f}%", hull->id, 100.0 * load) : hull->id;
                }
              }
            }
        }
        if (std::isfinite(best.transport_per_unit)) {
            best.reference_price = best.base_price + pricing.carrier_margin * best.transport_per_unit;
            prices.push_back(std::move(best));
        }
    }
    return prices;
}

void Simulation::set_station_stock(const std::string& station_id, const std::string& commodity_id, double units) {
    get_station_state(station_id).inventory[commodity_id] = units;
}

std::string Simulation::faction_name(const std::string& faction_id) const {
    for (const auto& faction : universe_.factions) {
        if (faction.id == faction_id) {
            return faction.name;
        }
    }
    return faction_id;
}

double Simulation::station_price(const domain::StationState& state, const std::string& commodity_id) const {
    // A good the station orders it sells to ships (provisions, depot fuel) at its own bid on its
    // stock now plus a margin (step 29). At its import cost plus the margin a hub resold dear
    // whatever it had once paid in a shortage, and onward routes from it looked poor (4.0% unmet
    // against 5.2%, 4 starts x 1460 days).
    if (const auto order = state.order_stats.find(commodity_id); priced_order(state, commodity_id)) {
        const auto stock_it = state.inventory.find(commodity_id);
        return order_bid(state, commodity_id, order_levels(state, commodity_id, order->second),
            stock_it == state.inventory.end() ? 0.0 : stock_it->second).price * (1.0 + universe_.orders.resale_margin);
    }
    const auto& definition = get_station_definition(state.station_id);
    const auto stock_it = state.inventory.find(commodity_id);
    const double stock = stock_it == state.inventory.end() ? 0.0 : stock_it->second;
    return economy_.get_price(definition, commodity_id, stock, get_commodity(commodity_id).base_price);
}

double Simulation::trade_value(
    const domain::StationState& state, const std::string& commodity_id, double units_into_station) const {
    // An ordered good: sold to ships at a flat price, bought under the order as if delivered now.
    if (priced_order(state, commodity_id)) {
        return units_into_station <= 0.0
            ? -units_into_station * station_price(state, commodity_id)
            : quote_split(order_quote(state, commodity_id, game_time_s_, {}), 0.0, units_into_station).total;
    }
    const auto& definition = get_station_definition(state.station_id);
    const auto stock_it = state.inventory.find(commodity_id);
    const double stock = stock_it == state.inventory.end() ? 0.0 : stock_it->second;
    return economy_.get_trade_value(
        definition, commodity_id, stock, units_into_station, get_commodity(commodity_id).base_price);
}

double Simulation::sale_value_on_arrival(const domain::StationState& state, const std::string& commodity_id,
    double units, double days_ahead, const std::string& seller_ship_id) const {
    return sale_split_on_arrival(state, commodity_id, units, days_ahead, seller_ship_id).total;
}

Simulation::SaleSplit Simulation::sale_split_on_arrival(const domain::StationState& state, const std::string& commodity_id,
    double units, double days_ahead, const std::string& seller_ship_id) const {
    if (priced_order(state, commodity_id)) {
        return quote_split(order_quote(state, commodity_id, game_time_s_ + std::max(0.0, days_ahead) * 86400.0, seller_ship_id),
            0.0, units);
    }
    return sale_split(get_station_definition(state.station_id), commodity_id,
        forecast_stock_on_arrival(state, commodity_id, days_ahead, seller_ship_id), units);
}

const domain::Emergency* Simulation::find_emergency(const std::string& station_id, const std::string& commodity_id) const {
    for (const auto& emergency : emergencies_) {
        if (emergency.station_id == station_id && emergency.commodity_id == commodity_id) {
            return &emergency;
        }
    }
    return nullptr;
}

Simulation::SaleSplit Simulation::sale_split(const domain::StationDefinition& station, const std::string& commodity_id,
    double stock, double units) const {
    // A good the station posts no order for (producers, non-traders): along its curve.
    return {.total = economy_.get_trade_value(station, commodity_id, stock, units, get_commodity(commodity_id).base_price)};
}

void Simulation::step_emergencies() {
    for (const auto& state : stations_) {
        const auto& definition = get_station_definition(state.station_id);
        const auto& rates = economy_.get_station_net_rates(definition);
        for (const auto& commodity_id : universe_.emergency_goods) {
            const auto rate = rates.find(commodity_id);
            if (rate == rates.end() || rate->second >= 0.0 || !economy_.is_upkeep(definition, commodity_id)
                || economy_.surface_supplied(definition, commodity_id)) {
                continue;
            }
            const double use_per_day = -rate->second;
            const double forecast = forecast_stock_on_arrival(state, commodity_id, EMERGENCY_HORIZON_DAYS, {});
            const auto open = std::find_if(emergencies_.begin(), emergencies_.end(), [&](const domain::Emergency& e) {
                return e.station_id == state.station_id && e.commodity_id == commodity_id;
            });
            if (open == emergencies_.end()) {
                if (forecast <= 0.0) {
                    // The faction pays above the station's own cap (its order's bid on an empty
                    // stock), so the premium starts there (step 29). A faction offer that rose with
                    // the projected stock instead (24x at once on an empty stock) opened emergencies
                    // three to four times as often and paid 2M cr in four years.
                    const auto& stats = state.order_stats.at(commodity_id);
                    const double premium = order_bid(state, commodity_id, order_levels(state, commodity_id, stats), 0.0).cap
                        / economy_.reference_price(definition, commodity_id);
                    emergencies_.push_back({
                        .station_id = state.station_id,
                        .commodity_id = commodity_id,
                        .premium = premium,
                        .units_open = use_per_day * EMERGENCY_COVER_DAYS,
                        .opened_s = game_time_s_,
                        .last_raise_s = game_time_s_,
                    });
                    ++emergencies_opened_;
                    add_event(std::format("EMERGENCY: {} will run out of {} — {} pays {:.0f}x for deliveries",
                        definition.name, get_commodity(commodity_id).name, faction_name(definition.faction_id),
                        premium), "alert");
                }
                continue;
            }
            if (forecast >= use_per_day * EMERGENCY_CLOSE_DAYS) {
                add_event(std::format("Emergency over: {} has enough {} coming ({:.0f} days after it began)",
                    definition.name, get_commodity(commodity_id).name, (game_time_s_ - open->opened_s) / 86400.0), "alert");
                emergencies_.erase(open);
            } else if (game_time_s_ - open->last_raise_s >= EMERGENCY_RAISE_DAYS * 86400.0
                && open->premium < universe_.orders.life_value && open->units_open > 0.0) {
                open->premium = std::min(universe_.orders.life_value, open->premium * EMERGENCY_RAISE_FACTOR);
                open->last_raise_s = game_time_s_;
                add_event(std::format("EMERGENCY: no ship for {} at {} — {} raises its offer to {:.1f}x",
                    get_commodity(commodity_id).name, definition.name, faction_name(definition.faction_id), open->premium),
                    "alert");
            }
        }
    }
}

double Simulation::forecast_stock_on_arrival(const domain::StationState& state, const std::string& commodity_id,
    double days_ahead, const std::string& seller_ship_id) const {
    // Forecast the stock the sale lands on: today's stock run forward to this ship's
    // arrival at the station's net rate (never below empty), plus the cargo of the same
    // kind other ships deliver before then. Without the inbound cargo, every ship sent to a
    // starving port expects its scarcity price and the late arrivals sell at a loss. Cargo
    // that arrives after this ship does not lower its price: before v36 all inbound cargo
    // counted, so a hop of hours to a starving station looked worthless whenever a slow
    // ship was months out with the same good (Low Earth Logistics waited for Venus food
    // with 10,000 units at Earth L1).
    const auto& definition = get_station_definition(state.station_id);
    const auto stock_it = state.inventory.find(commodity_id);
    double stock = stock_it == state.inventory.end() ? 0.0 : stock_it->second;
    const double arrival_s = game_time_s_ + std::max(0.0, days_ahead) * 86400.0;
    std::vector<std::pair<double, double>> deliveries;
    for (const auto& other : ships_) {
        if (other.id != seller_ship_id
            && (other.phase == domain::ShipMissionPhase::InTransit
                || other.phase == domain::ShipMissionPhase::AwaitingDeparture)
            && other.active_mission.destination_station_id == state.station_id
            && other.active_mission.arrival_time_s <= arrival_s) {
            if (const double units_aboard = domain::units_of(other.active_mission.cargo, commodity_id); units_aboard > 0.0) {
                deliveries.emplace_back(other.active_mission.arrival_time_s, units_aboard);
            }
        }
    }
    std::sort(deliveries.begin(), deliveries.end());
    const auto& rates = economy_.get_station_net_rates(definition);
    const double rate = rates.contains(commodity_id) ? rates.at(commodity_id) : 0.0;
    double time_s = game_time_s_;
    for (const auto& [delivery_s, delivered] : deliveries) {
        stock = std::max(0.0, stock + rate * std::max(0.0, delivery_s - time_s) / 86400.0) + delivered;
        time_s = std::max(time_s, delivery_s);
    }
    return std::max(0.0, stock + rate * std::max(0.0, arrival_s - time_s) / 86400.0);
}

bool Simulation::priced_order(const domain::StationState& state, const std::string& commodity_id) const {
    return state.order_stats.contains(commodity_id);
}

bool Simulation::has_order(const domain::StationDefinition& station, const std::string& commodity_id) const {
    if (economy_.is_export_market(station, commodity_id) || economy_.surface_supplied(station, commodity_id)) {
        return false;
    }
    // Depot fuel: every station without a fuel factory orders the fuel ships buy there.
    if (commodity_id == economy::FUEL_ID) {
        return economy_.fuel_factory_output(station) <= 0.0;
    }
    const auto& rates = economy_.get_station_net_rates(station);
    const auto rate = rates.find(commodity_id);
    return rate != rates.end() && rate->second < 0.0;
}

domain::OrderStats Simulation::seed_order_stats(const domain::StationDefinition& station, const std::string& commodity_id) const {
    // The nearest producer, by the map-level transfer estimate (fuel: the nearest factory).
    double nearest = std::numeric_limits<double>::infinity();
    for (const auto& producer : universe_.stations) {
        bool makes = (commodity_id == economy::FUEL_ID && economy_.fuel_factory_output(producer) > 0.0)
            || economy_.surface_supplied(producer, commodity_id);
        for (const auto* recipe : economy_.recipes_of(producer)) {
            makes = makes || (recipe->commodity_id == commodity_id && recipe->units_per_day > 0.0);
        }
        if (makes && producer.id != station.id) {
            nearest = std::min(nearest, economy_.transfer_days(producer, station));
        }
    }
    double round_trip = std::isfinite(nearest) ? 2.0 * nearest : 42.0;
    const auto& trips = universe_.pricing.round_trip_days;
    if (const auto station_it = trips.find(station.id); station_it != trips.end()) {
        if (const auto it = station_it->second.find(commodity_id); it != station_it->second.end()) {
            round_trip = it->second;
        }
    }
    // Within one planet's system the reference plasma ship's spirals overstate a local
    // shuttle's trip (as in step 23's cover rule): plan for three weeks between deliveries.
    const bool local = std::isfinite(nearest) && nearest <= 5.0;
    domain::OrderStats stats;
    stats.lead_days = local ? std::max(1.0, nearest) : std::max(1.0, 0.5 * round_trip);
    stats.interval_days = local ? 21.0 : std::max(21.0, round_trip);
    stats.spread_days = 0.5 * stats.lead_days;
    stats.going_rate = economy_.reference_price(station, commodity_id);
    stats.order_price = stats.going_rate;
    return stats;
}

void Simulation::seed_order_stats() {
    for (auto& state : stations_) {
        const auto& definition = get_station_definition(state.station_id);
        for (const auto& commodity : universe_.commodities) {
            if (has_order(definition, commodity.id) && !state.order_stats.contains(commodity.id)) {
                state.order_stats[commodity.id] = seed_order_stats(definition, commodity.id);
            }
        }
    }
}

void Simulation::observe_contract(domain::StationState& state, const std::string& commodity_id, double units, double unit_price) {
    const auto it = state.order_stats.find(commodity_id);
    if (it == state.order_stats.end() || units <= 0.0) {
        return;
    }
    // Weighted by units: a lot of one interval's use moves the going rate by 1/ema_deliveries.
    auto& stats = it->second;
    const double use = order_levels(state, commodity_id, stats).use_per_day;
    const double history = use * stats.interval_days * (universe_.orders.ema_deliveries - 1.0);
    stats.going_rate += (unit_price - stats.going_rate) * units / (units + history);
}

void Simulation::observe_delivery(domain::StationState& state, const std::string& commodity_id, double contracted_s) {
    const auto it = state.order_stats.find(commodity_id);
    if (it == state.order_stats.end()) {
        return;
    }
    auto& stats = it->second;
    const double alpha = 1.0 / universe_.orders.ema_deliveries;
    const auto fold = [&](double& mean, double sample) {
        stats.spread_days += alpha * (std::abs(sample - mean) - stats.spread_days);
        mean += alpha * (sample - mean);
    };
    fold(stats.lead_days, std::max(0.0, game_time_s_ - contracted_s) / 86400.0);
    // Ships landing within a day of each other make one delivery.
    if (stats.last_delivery_s >= 0.0 && game_time_s_ - stats.last_delivery_s >= 86400.0) {
        fold(stats.interval_days, (game_time_s_ - stats.last_delivery_s) / 86400.0);
    }
    stats.last_delivery_s = game_time_s_;
    ++stats.deliveries;
}

double Simulation::order_ceiling(const domain::StationState& state, const std::string& commodity_id,
    std::string& kind, double& station_ceiling) const {
    const auto& definition = get_station_definition(state.station_id);
    const auto& orders = universe_.orders;
    const double reference = economy_.reference_price(definition, commodity_id);
    if (commodity_id == economy::FUEL_ID && economy_.fuel_factory_output(definition) <= 0.0) {
        kind = "depot_fuel";
        station_ceiling = orders.depot_fuel_value * reference;
        return station_ceiling;
    }
    const auto& recipes = economy_.recipes_of(definition);
    const auto going_rate = [&](const std::string& id) {
        const auto it = state.order_stats.find(id);
        return it == state.order_stats.end() ? economy_.reference_price(definition, id) : it->second.going_rate;
    };
    // Output value per day at reference prices (population scaling cancels out below).
    const auto output_value = [&](const std::string& output_id) {
        double value = 0.0;
        for (const auto* recipe : recipes) {
            if (recipe->role == domain::RecipeRole::Output && recipe->units_per_day > 0.0
                && (output_id.empty() || recipe->commodity_id == output_id)) {
                value += recipe->units_per_day * economy_.reference_price(definition, recipe->commodity_id);
            }
        }
        return value;
    };
    // The most a unit is worth over the recipes that use it.
    double best = 0.0;
    bool found = false;
    kind = "consume";
    for (const auto* recipe : recipes) {
        if (recipe->commodity_id != commodity_id || recipe->units_per_day >= 0.0) {
            continue;
        }
        const double use = -recipe->units_per_day;
        double value = 0.0;
        std::string role;
        switch (recipe->role) {
            case domain::RecipeRole::Input: {
                // The outputs it feeds, less the going cost of the other inputs they need.
                double margin = 0.0;
                for (const auto& output_id : recipe->feeds) {
                    double others = 0.0;
                    for (const auto* other : recipes) {
                        if (other->role == domain::RecipeRole::Input && other->commodity_id != commodity_id
                            && other->units_per_day < 0.0 && !other->feeds.empty()
                            && std::find(other->feeds.begin(), other->feeds.end(), output_id) != other->feeds.end()) {
                            others += -other->units_per_day / static_cast<double>(other->feeds.size())
                                * going_rate(other->commodity_id);
                        }
                    }
                    margin += std::max(0.0, output_value(output_id) - others);
                }
                value = (1.0 - orders.keep_share) * margin / use;
                role = "input";
                break;
            }
            case domain::RecipeRole::Upkeep: {
                // The output a full shortage would cost, per unit of use; at least what a good
                // used up without output is worth, since residents need it whatever the station
                // makes (Mars food came to 20 cr against a landed cost of 64; user, 2026-10-16).
                const auto penalty = universe_.upkeep_penalties.find(commodity_id);
                const double lost = penalty == universe_.upkeep_penalties.end() ? 0.0 : 1.0 - penalty->second;
                value = std::max(lost * output_value({}) / use, orders.consume_value * reference);
                role = "upkeep";
                break;
            }
            default:
                value = orders.consume_value * reference;
                role = "consume";
                break;
        }
        if (!found || value > best) {
            best = value;
            kind = role;
            found = true;
        }
    }
    // A gateway's colony import is worth what the colony pays for it (Mars Transfer uses no reactor
    // fuel itself, so its order sat at the floor and no ship ever came).
    for (const auto* market : economy_.surface_markets_at(definition.id)) {
        if (market->commodity_id == commodity_id && market->buy_units_per_day > 0.0) {
            const double bid = economy_.surface_bid(definition, commodity_id);
            if (!found || bid > best) {
                best = bid;
                kind = "colony";
                found = true;
            }
        }
    }
    station_ceiling = best;
    if (universe_.emergency_goods.contains(commodity_id) && economy_.is_upkeep(definition, commodity_id)) {
        kind = "life";
        return std::max(best, orders.life_value * reference);
    }
    return best;
}

Simulation::OrderLevels Simulation::order_levels(
    const domain::StationState& state, const std::string& commodity_id, const domain::OrderStats& stats) const {
    const auto& definition = get_station_definition(state.station_id);
    const auto& orders = universe_.orders;
    const bool depot_fuel = commodity_id == economy::FUEL_ID && economy_.fuel_factory_output(definition) <= 0.0;
    const auto& rates = economy_.get_station_net_rates(definition);
    const auto rate = rates.find(commodity_id);
    const double recipe_use = rate == rates.end() ? 0.0 : std::max(0.0, -rate->second);
    OrderLevels levels;
    levels.use_per_day = recipe_use + (depot_fuel ? state.ship_fuel_units_per_day : stats.ship_sales_per_day);
    const double u = levels.use_per_day;
    levels.safety_units = u * orders.safety_factor * stats.spread_days;
    // Storage is shared by every good (depot fuel stands outside it).
    double cap = u * universe_.pricing.max_cover_days;
    if (definition.storage_capacity_units > 0.0 && !depot_fuel) {
        cap = std::min(cap, definition.storage_capacity_units);
    }
    levels.order_up_to_units = std::min(cap, levels.safety_units + u * (stats.lead_days + stats.interval_days));
    // Extra days the discount pays for: holding a unit costs interest plus its spoilage. For a
    // good that does not spoil that is two years, so it is capped at one interval and at
    // max_extra_days (user, 2026-10-16): a little extra, not a stockpile.
    const double holding_per_day = universe_.ship_operations.interest_rate_per_year / 365.0
        + get_commodity(commodity_id).decay_fraction_per_day;
    const double paid_days = holding_per_day > 0.0 ? orders.extra_discount / holding_per_day : orders.max_extra_days;
    levels.extra_days = std::min({paid_days, stats.interval_days, orders.max_extra_days});
    levels.max_units = std::min(cap, levels.order_up_to_units + u * levels.extra_days);
    return levels;
}

std::vector<std::pair<double, double>> Simulation::contracted_arrivals(
    const domain::StationState& state, const std::string& commodity_id, const std::string& except_ship_id) const {
    const double decay = get_commodity(commodity_id).decay_fraction_per_day;
    std::vector<std::pair<double, double>> arrivals;
    for (const auto& ship : ships_) {
        if (ship.id != except_ship_id
            && (ship.phase == domain::ShipMissionPhase::InTransit || ship.phase == domain::ShipMissionPhase::AwaitingDeparture)
            && ship.active_mission.destination_station_id == state.station_id) {
            if (const double units = domain::units_of(ship.active_mission.cargo, commodity_id); units > 0.0) {
                arrivals.emplace_back(ship.active_mission.arrival_time_s,
                    units * std::pow(1.0 - decay, ship.active_mission.total_travel_time_s / 86400.0));
            }
        }
    }
    // Rotation contracts: the deliveries still to come on each loop, at their estimated dates
    // (the leg under way is in the cargo above).
    for (const auto& ship : ships_) {
        if (!under_contract(ship)) {
            continue;
        }
        const auto& contract = ship.contract;
        const auto count = contract.stops.size();
        std::size_t at = count;
        double t = game_time_s_;
        const bool moving = ship.phase == domain::ShipMissionPhase::InTransit
            || ship.phase == domain::ShipMissionPhase::AwaitingDeparture;
        const auto& here = moving ? ship.active_mission.destination_station_id : ship.current_station_id;
        for (std::size_t k = 0; k < count; ++k) {
            if (contract.stops[k] == here) {
                at = k;
            }
        }
        if (at == count) {
            continue;
        }
        if (moving) {
            t = std::max(game_time_s_, ship.active_mission.arrival_time_s);
        }
        const double dwell_s = contract_dwell_days(contract) * 86400.0;
        for (std::size_t step = 0; step < 2 * count; ++step) {
            const auto next = (at + 1) % count;
            if (at == 0) {
                // The wait in port at the first stop: a docked ship has served part of it.
                t = step == 0 && !moving ? std::max(t, ship.idle_since_s + dwell_s) : t + dwell_s;
            }
            t += contract.leg_days[at] * 86400.0;
            if (t > contract.until_s) {
                break;
            }
            if (contract.stops[next] == state.station_id) {
                for (const auto& delivery : contract.deliveries) {
                    if (delivery.to_station_id == state.station_id && delivery.commodity_id == commodity_id) {
                        arrivals.emplace_back(t, delivery.units);
                    }
                }
            }
            at = next;
        }
    }
    std::sort(arrivals.begin(), arrivals.end());
    return arrivals;
}

namespace {

// The projected stock of a good (today's, falling at `use` per day, rising at each contracted
// arrival) seen by a delivery landing at `t`: the open quantity N(t) = max over [t, end] of
// (safety - p), and the highest the stock gets over that window. Before the delivery the stock
// can go no lower than empty; after it, the delivery's own units fill the dip, so it runs on
// below zero.
struct ProfileGap {
    double open_units {0.0};
    double peak {0.0};
    double at_arrival {0.0};
};

ProfileGap profile_gap(const std::vector<std::pair<double, double>>& arrivals, double stock, double use,
    double safety, double now, double t, double end) {
    constexpr double day_s = 86400.0;
    double p = stock;
    double time = now;
    std::size_t next = 0;
    for (; next < arrivals.size() && arrivals[next].first <= t; ++next) {
        p = std::max(0.0, p - use * std::max(0.0, arrivals[next].first - time) / day_s) + arrivals[next].second;
        time = std::max(time, arrivals[next].first);
    }
    p = std::max(0.0, p - use * std::max(0.0, t - time) / day_s);
    time = std::max(time, t);
    const double at_arrival = p;
    double low = p;
    double peak = p;
    for (; next < arrivals.size() && arrivals[next].first <= end; ++next) {
        p -= use * (arrivals[next].first - time) / day_s;
        low = std::min(low, p);
        p += arrivals[next].second;
        peak = std::max(peak, p);
        time = arrivals[next].first;
    }
    low = std::min(low, p - use * std::max(0.0, end - time) / day_s);
    return {.open_units = std::max(0.0, safety - low), .peak = peak, .at_arrival = at_arrival};
}

// Days from now until the projected stock (every contract counted) first falls below `safety`,
// within `horizon_s`; -1 if it does not.
double days_to_safety(const std::vector<std::pair<double, double>>& arrivals, double stock, double use,
    double safety, double now, double horizon_s) {
    constexpr double day_s = 86400.0;
    if (use <= 0.0) {
        return -1.0;
    }
    double p = stock;
    double time = now;
    const auto crossing = [&](double until) {
        if (p < safety) {
            return time;
        }
        const double below_s = time + (p - safety) / use * day_s;
        return below_s < until ? below_s : -1.0;
    };
    for (const auto& [arrival_s, units] : arrivals) {
        if (arrival_s > horizon_s) {
            break;
        }
        if (const double when = crossing(arrival_s); when >= 0.0) {
            return (when - now) / day_s;
        }
        p = std::max(0.0, p - use * std::max(0.0, arrival_s - time) / day_s) + units;
        time = std::max(time, arrival_s);
    }
    const double when = crossing(horizon_s);
    return when < 0.0 ? -1.0 : (when - now) / day_s;
}

}  // namespace

domain::OrderView Simulation::order_view(const domain::StationState& state, const std::string& commodity_id) const {
    const auto& definition = get_station_definition(state.station_id);
    const auto stats_it = state.order_stats.find(commodity_id);
    const auto stats = stats_it == state.order_stats.end() ? seed_order_stats(definition, commodity_id) : stats_it->second;
    const auto levels = order_levels(state, commodity_id, stats);

    domain::OrderView view;
    view.use_per_day = levels.use_per_day;
    view.lead_days = stats.lead_days;
    view.interval_days = stats.interval_days;
    view.spread_days = stats.spread_days;
    view.going_rate = stats.going_rate;
    view.order_price = stats.order_price;
    view.reference_price = economy_.reference_price(definition, commodity_id);
    view.safety_units = levels.safety_units;
    view.reorder_units = levels.use_per_day * (stats.lead_days + universe_.orders.safety_factor * stats.spread_days);
    view.order_up_to_units = levels.order_up_to_units;
    view.extra_days = levels.extra_days;
    view.max_units = levels.max_units;
    const auto stock_it = state.inventory.find(commodity_id);
    view.stock = stock_it == state.inventory.end() ? 0.0 : std::max(0.0, stock_it->second);
    const auto arrivals = contracted_arrivals(state, commodity_id, {});
    for (const auto& [arrival_s, units] : arrivals) {
        view.contracted_units += units;
    }
    // N for a delivery one lead time from now.
    const double day_s = 86400.0;
    const double t = game_time_s_ + stats.lead_days * day_s;
    view.open_units = profile_gap(arrivals, view.stock, levels.use_per_day, levels.safety_units, game_time_s_, t,
        t + (stats.interval_days + stats.lead_days) * day_s).open_units;
    // While the projected stock falls below B within the lead time (a delivery ordered now
    // comes too late) and nobody takes the order, its price rises.
    view.days_to_safety = days_to_safety(arrivals, view.stock, levels.use_per_day, levels.safety_units, game_time_s_,
        game_time_s_ + stats.lead_days * day_s);
    view.ceiling = order_ceiling(state, commodity_id, view.ceiling_kind, view.station_ceiling);
    return view;
}

Simulation::OrderQuote Simulation::order_quote(const domain::StationState& state, const std::string& commodity_id,
    double arrival_s, const std::string& except_ship_id) const {
    const auto& stats = state.order_stats.at(commodity_id);
    const auto levels = order_levels(state, commodity_id, stats);
    const auto stock_it = state.inventory.find(commodity_id);
    const double stock = stock_it == state.inventory.end() ? 0.0 : std::max(0.0, stock_it->second);
    const double t = std::max(game_time_s_, arrival_s);
    const auto gap = profile_gap(contracted_arrivals(state, commodity_id, except_ship_id), stock, levels.use_per_day,
        levels.safety_units, game_time_s_, t, t + (stats.interval_days + stats.lead_days) * 86400.0);
    OrderQuote quote;
    quote.open_units = gap.open_units;
    quote.extra_units = std::max(0.0, levels.max_units - std::max(gap.peak + gap.open_units, levels.order_up_to_units));
    const auto bid = order_bid(state, commodity_id, levels, gap.at_arrival);
    quote.price = bid.price;
    quote.station_cap = bid.cap;
    // Competitive pricing: the order pays no more than the ships near a source ask.
    if (const auto& market = universe_.market; market.competitive > 0.0 && stats.market_price > 0.0) {
        double urgency = 1.0;
        if (market.urgency_elasticity > 0.0 && levels.order_up_to_units > 0.0) {
            urgency = std::clamp(std::pow(levels.order_up_to_units / std::max(gap.at_arrival, 0.5), market.urgency_elasticity),
                1.0, market.urgency_max);
        }
        quote.price = std::min(quote.price, stats.market_price * urgency);
    }
    // The extra only when it is cheap: below both the going rate and what the order bids now. The
    // floor follows the reference, not the going rate: a going rate raised by a shortage made
    // dumping at a floor of a quarter of it pay (47,000 u of food at Low Earth Logistics).
    const double reference = economy_.reference_price(get_station_definition(state.station_id), commodity_id);
    quote.extra_price = std::min(quote.price, stats.going_rate) * (1.0 - universe_.orders.extra_discount);
    quote.floor_price = std::min(quote.extra_price, economy::PRICE_FLOOR_MULTIPLIER * reference);
    if (const auto* emergency = find_emergency(state.station_id, commodity_id); emergency != nullptr) {
        quote.emergency_price = emergency->premium * reference;
        quote.emergency_units = emergency->units_open;
    }
    return quote;
}

Simulation::SaleSplit Simulation::quote_split(const OrderQuote& quote, double from, double units) const {
    SaleSplit split;
    if (units <= 0.0) {
        return split;
    }
    const auto overlap = [&](double lo, double hi) {
        return std::max(0.0, std::min(hi, from + units) - std::max(lo, from));
    };
    const double ordered = overlap(0.0, quote.open_units);
    const double extra = overlap(quote.open_units, quote.open_units + quote.extra_units);
    const double rest = units - ordered - extra;
    split.order_units = ordered;
    split.total = ordered * quote.price + extra * quote.extra_price + rest * quote.floor_price;
    split.order_value = ordered * quote.price;
    // An emergency's price for the first of the ordered units, paid above the order price by the faction.
    if (quote.emergency_price > quote.price) {
        const double emergency = overlap(0.0, std::min(quote.open_units, quote.emergency_units));
        split.emergency_units = emergency;
        split.faction = emergency * (quote.emergency_price - quote.price);
        split.total += split.faction;
    }
    return split;
}

Simulation::OrderBid Simulation::order_bid(const domain::StationState& state, const std::string& commodity_id,
    const OrderLevels& levels, double stock_at_arrival) const {
    // State, not time: a bid that rose while nobody came saturated at its cap almost everywhere
    // with the fleet short of ships, so ships chose by cap, not by need (8.5% unmet against 4.3%).
    // The projected stock at arrival ranks stations by how short and how soon; the price is flat
    // over the slice, so a big delivery does not cut its own price.
    // The cap is the lower of price_cap (4x; inputs input_cap, 12x) and what the good is worth to the
    // station, V (step 3), so an input that does not pay is not bought dear. V alone as the cap
    // let any dip in stock pay up to 75x for an input, and residents pay what their station paid:
    // money grew 2-5x in four years and unmet rose to 7.8%. Oxygen and water above the station's
    // cap are its faction's (emergencies, step_emergencies).
    const auto& definition = get_station_definition(state.station_id);
    const double reference = economy_.reference_price(definition, commodity_id);
    std::string kind;
    double station_ceiling = 0.0;
    static_cast<void>(order_ceiling(state, commodity_id, kind, station_ceiling));
    OrderBid bid;
    // Depot fuel: its own ceiling (depot_fuel_value x the reference). An input: its V within
    // [price_cap, input_cap], which with keep_share 0.75 is step 24's cap (a quarter of the value
    // of what a unit makes, 4-12x). V at 80% of the margin up to 12x: 3.9% unmet against 3.3%.
    const double limit = kind == "depot_fuel" ? station_ceiling / reference
        : kind == "input" ? universe_.orders.input_cap : universe_.pricing.price_cap;
    const double lowest = kind == "input" ? universe_.pricing.price_cap : economy::PRICE_FLOOR_MULTIPLIER;
    bid.cap = reference * std::clamp(station_ceiling / reference, std::min(lowest, limit), std::max(lowest, limit));
    if (levels.order_up_to_units <= 0.0) {
        bid.price = std::min(reference, bid.cap);
        return bid;
    }
    const double ratio = levels.order_up_to_units / std::max(stock_at_arrival, 0.5);
    bid.price = std::min(bid.cap, reference * std::max(economy::PRICE_FLOOR_MULTIPLIER, std::pow(ratio, economy::PRICE_ELASTICITY)));
    return bid;
}

void Simulation::update_market_prices() {
    const auto& market = universe_.market;
    if (market.competitive <= 0.0 || std::fmod(game_time_s_, 86400.0) >= TICK_S * 0.5) {
        return;
    }
    const profiling::Scope profile_scope(profiling::Phase::EstimateLeg);
    constexpr double day_s = 86400.0;
    // Where each ship is free next, and from when: docked ships now, ships under way on arrival.
    struct Free {
        const domain::ShipState* ship;
        const domain::StationDefinition* at;
        double from_s;
    };
    std::vector<Free> free;
    for (const auto& ship : ships_) {
        switch (ship.phase) {
            case domain::ShipMissionPhase::Idle:
            case domain::ShipMissionPhase::LaidUp:
            case domain::ShipMissionPhase::Refueling:
                free.push_back({&ship, &get_station_definition(ship.current_station_id), game_time_s_});
                break;
            case domain::ShipMissionPhase::InTransit:
            case domain::ShipMissionPhase::AwaitingDeparture:
                if (!ship.active_mission.destination_station_id.empty()
                    && ship.active_mission.arrival_time_s <= game_time_s_ + market.available_days * day_s) {
                    free.push_back({&ship, &get_station_definition(ship.active_mission.destination_station_id),
                        std::max(game_time_s_, ship.active_mission.arrival_time_s)});
                }
                break;
            default:
                break;
        }
    }
    const double fuel_base = get_commodity(economy::FUEL_ID).base_price;
    // A source: a station that makes the good (fuel: a factory) and has some to spare.
    const auto spare = [&](const domain::StationDefinition& station, const std::string& commodity_id) {
        const auto& state = get_station_state(station.id);
        const auto& rates = economy_.get_station_net_rates(station);
        const auto rate = rates.find(commodity_id);
        const double r = rate == rates.end() ? 0.0 : rate->second;
        const bool makes = commodity_id == economy::FUEL_ID ? economy_.fuel_factory_output(station) > 0.0 : r > 0.0;
        if (!makes) {
            return 0.0;
        }
        const auto stock = state.inventory.find(commodity_id);
        return std::max(0.0, (stock == state.inventory.end() ? 0.0 : stock->second) - (8.0 + std::max(0.0, r) * 7.0));
    };
    std::unordered_map<std::string, std::vector<double>> asks;  // station|good -> asks
    // What each order still wants (open and extra units), so a big hold's trip is spread only
    // over what a leg can carry to that station from that source.
    std::unordered_map<std::string, double> wanted;  // station|good -> units
    if (market.ask_open_units > 0.0) {
        for (const auto& state : stations_) {
            for (const auto& [commodity_id, stats] : state.order_stats) {
                if (priced_order(state, commodity_id)) {
                    const auto quote = order_quote(state, commodity_id, game_time_s_ + stats.lead_days * day_s, {});
                    wanted[state.station_id + "|" + commodity_id] = quote.open_units + quote.extra_units;
                }
            }
        }
    }
    const auto joint_load = [&](const domain::StationDefinition& source, const domain::StationState& destination) {
        double units = 0.0;
        for (const auto& [commodity_id, stats] : destination.order_stats) {
            const auto it = wanted.find(destination.station_id + "|" + commodity_id);
            if (it != wanted.end()) {
                const double available = spare(source, commodity_id);
                if (available >= market.min_source_surplus) {
                    units += std::min(it->second, available);
                }
            }
        }
        return units;
    };
    // Plans are estimated in parallel passes, like dispatch: a pass that misses plans queues them.
    with_parallel_plans("market#", [&] {
        asks.clear();
        for (const auto& state : stations_) {
            const auto& destination = get_station_definition(state.station_id);
            for (const auto& [commodity_id, stats] : state.order_stats) {
                if (!priced_order(state, commodity_id)) {
                    continue;
                }
                const auto& commodity = get_commodity(commodity_id);
                auto& list = asks[state.station_id + "|" + commodity_id];
                for (const auto& f : free) {
                    if (f.at->id == destination.id || spare(*f.at, commodity_id) < market.min_source_surplus) {
                        continue;
                    }
                    const auto& ship_class = ship_design(*f.ship);
                    if (ship_class.cargo_capacity_units <= 0.0) {
                        continue;
                    }
                    const double hold_kg = ship_class.cargo_capacity_units * commodity.mass_per_unit_kg;
                    const auto leg = estimate_leg(ship_class, *f.at, destination, f.from_s, ship_class.propellant_capacity_kg,
                        market.fill_ref * hold_kg);
                    if (!leg.feasible) {
                        continue;
                    }
                    double crew = ship_class.crew_size * universe_.ship_operations.wage_cr_per_crew_day;
                    for (const auto& [good, units_per_crew_day] : universe_.ship_operations.life_support_units_per_crew_day) {
                        crew += ship_class.crew_size * units_per_crew_day * get_commodity(good).base_price;
                    }
                    const double fuel_price = std::max(fuel_base, station_price(get_station_state(f.at->id), economy::FUEL_ID));
                    const double trip = leg.travel_days * (daily_capital_cost(ship_class) + crew
                            + hazard_cr_per_day(ship_class, *f.at, destination, f.from_s) + wear_cr_per_flight_day(ship_class))
                        + leg.propellant_kg / FUEL_UNITS_TO_KG * fuel_price
                        + leg.propellant_kg * wear_cr_per_propellant_kg(ship_class);
                    double units = std::max(1.0, market.fill_ref * ship_class.cargo_capacity_units);
                    if (market.ask_open_units > 0.0) {
                        units = std::clamp(joint_load(*f.at, state), 1.0, units);
                    }
                    const double buy = station_price(get_station_state(f.at->id), commodity_id);
                    list.push_back(buy + trip * (1.0 + market.return_share) / units * (1.0 + market.ask_markup));
                }
            }
        }
    });
    for (auto& state : stations_) {
        for (auto& [commodity_id, stats] : state.order_stats) {
            auto it = asks.find(state.station_id + "|" + commodity_id);
            if (it == asks.end() || it->second.empty()) {
                stats.market_price = 0.0;
                stats.market_bidders = 0;
                continue;
            }
            auto& list = it->second;
            std::sort(list.begin(), list.end());
            stats.market_bidders = static_cast<int>(list.size());
            stats.market_price = list.size() >= 2 ? list[1] : list[0] * (1.0 + market.solo_premium);
        }
    }
}

void Simulation::step_orders() {
    for (auto& state : stations_) {
        for (auto& [commodity_id, stats] : state.order_stats) {
            const auto quote = order_quote(state, commodity_id, game_time_s_ + stats.lead_days * 86400.0, {});
            stats.order_price = quote.price;
            // Debug aid: SPACETRAINS_TRACE_ORDER="<station id>:<good>" logs that order once a day (stderr).
            if (static const char* const trace_order = std::getenv("SPACETRAINS_TRACE_ORDER");
                trace_order != nullptr && state.station_id + ":" + commodity_id == trace_order
                && std::fmod(game_time_s_, 86400.0) < Simulation::TICK_S * 0.5) {
                const auto view = order_view(state, commodity_id);
                std::cerr << std::format("[ORDER] day {:.0f} {} stock {:.0f} coming {:.0f} S {:.0f} N {:.0f} short {:.0f} "
                    "P {:.1f} G {:.1f} L {:.0f} I {:.0f} sigma {:.0f}\n", game_time_s_ / 86400.0, trace_order, view.stock,
                    view.contracted_units, view.order_up_to_units, view.open_units, view.days_to_safety, stats.order_price,
                    stats.going_rate, stats.lead_days, stats.interval_days, stats.spread_days);
            }
        }
    }
}

double Simulation::consumer_bid(const domain::StationState& state, const std::string& commodity_id) const {
    const auto it = state.order_stats.find(commodity_id);
    // The going rate, not today's bid: a calm order probes down toward the floor, and a route
    // is bought for years (valued at the bid, local routes looked worthless).
    return priced_order(state, commodity_id) ? std::max(it->second.order_price, it->second.going_rate) : station_price(state, commodity_id);
}

}  // namespace spacetrains::simulation
