// The UI snapshot and the bridge JSON (snapshot, paths, designs) and the text report.

#include "simulation/systems/Snapshot.hpp"

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
#include "simulation/query/Finance.hpp"
#include "simulation/query/Lookups.hpp"
#include "simulation/query/Positions.hpp"
#include "simulation/query/ShipRates.hpp"
#include "simulation/systems/Contracts.hpp"
#include "simulation/systems/Households.hpp"
#include "simulation/systems/Market.hpp"
#include "simulation/systems/Passengers.hpp"
#include "simulation/systems/Patrols.hpp"
#include "simulation/systems/StationSystems.hpp"

namespace spacetrains::simulation::systems {

using namespace detail;

namespace {

// Defined below, file-local.
[[nodiscard]] std::string bridge_path_id(const domain::ShipState& ship);
void write_ship_path_json(const World& w, std::ostream& output, const domain::ShipState& ship);
[[nodiscard]] std::string ship_systems_json(const World& w, const domain::ShipState& ship,
    const domain::ShipClassDefinition& ship_class, const math::Vec3d& position);

}  // namespace

domain::SimulationSnapshot snapshot(const World& w) {
    domain::SimulationSnapshot snapshot {
        .game_time_s = w.state.game_time_s,
        .outside_economy_credits = w.state.outside_economy_credits,
        .background_credits = w.state.background_credits,
        .background_population = w.state.background_population,
        .faction_treasuries = w.state.faction_treasuries,
        .faction_credit_limits = [&] {
            std::map<std::string, double> limits;
            for (const auto& faction : w.data.universe.factions) {
                limits[faction.id] = query::faction_credit_limit(w, faction.id);
            }
            return limits;
        }(),
        .faction_interest_paid = w.state.faction_interest_paid,
        .money_supply_target = w.state.seeded_money_supply,
        .fleet_investment = w.state.investment_ledger,
        .stations = w.state.stations,
        .ships = w.state.ships,
        .sold_ships = w.state.sold_ships,
        .recent_events = w.state.recent_events,
        .emergencies = w.state.emergencies,
        .emergencies_opened = w.state.emergencies_opened,
        .events_started = w.state.events_started,
        .emergency_paid = w.state.emergency_paid,
    };
    for (auto& station : snapshot.stations) {
        for (const auto& [commodity_id, stats] : station.order_stats) {
            station.orders[commodity_id] = systems::order_view(w, station, commodity_id);
        }
    }
    return snapshot;
}

namespace {

// Wrecks and police cutters draw their paths like ships in flight: a wreck its coast, a cutter
// the leg it flies now (none while it rides alongside its target, whose path is drawn).
std::string derelict_path_id(const domain::DerelictState& derelict) {
    return std::format("derelict|{:.3f}|{}", derelict.failed_at_s, derelict.sampled_path.size());
}

bool patrol_has_path(const domain::PatrolState& patrol) {
    return (patrol.phase == domain::PatrolPhase::Outbound || patrol.phase == domain::PatrolPhase::Returning)
        && patrol.path.size() >= 2 && patrol.times.size() == patrol.path.size();
}

std::string patrol_path_id(const domain::PatrolState& patrol) {
    return std::format("patrol|{}|{:.3f}|{}", static_cast<int>(patrol.phase), patrol.times.front(), patrol.path.size());
}

void write_timed_path_json(std::ostream& output, const std::vector<math::Vec3d>& path, const std::vector<double>& times) {
    output << "\"trajectory_path\":[";
    for (std::size_t i = 0; i < path.size(); ++i) {
        output << (i > 0 ? "," : "") << "{\"t_s\":" << (i < times.size() ? times[i] : 0.0) << ",\"x\":" << path[i].x
               << ",\"y\":" << path[i].y << ",\"z\":" << path[i].z << "}";
    }
    output << "]";
}

std::string bridge_path_id(const domain::ShipState& ship) {
    const auto& mission = ship.active_mission;
    return std::format("{}|{}|{:.3f}|{}", mission.destination_station_id, mission.trajectory_type, mission.departure_time_s,
        mission.sampled_path.size());
}

void write_ship_path_json(const World& w, std::ostream& output, const domain::ShipState& ship) {
    output << "\"trajectory_path\":[";
    for (std::size_t path_index = 0; path_index < ship.active_mission.sampled_path.size(); ++path_index) {
        const auto& point = ship.active_mission.sampled_path[path_index];
        if (path_index > 0) {
            output << ",";
        }
        output << "{"
               << "\"t_s\":" << (path_index < ship.active_mission.sampled_times_s.size() ? ship.active_mission.sampled_times_s[path_index] : 0.0) << ","
               << "\"x\":" << point.x << ","
               << "\"y\":" << point.y << ","
               << "\"z\":" << point.z
               << "}";
    }
    output << "]";
    if (!ship.active_mission.destination_station_id.empty()) {
        const auto& destination_station = query::get_station_definition(w, ship.active_mission.destination_station_id);
        const auto& destination_body = query::get_body_definition(w, destination_station.parent_body_id);
        const auto destination_body_position = w.engines.mechanics.get_body_position(destination_body.id, ship.active_mission.arrival_time_s);
        output << ",\"destination_body_at_arrival\":{"
               << "\"id\":\"" << json_escape(destination_body.id) << "\","
               << "\"name\":\"" << json_escape(destination_body.name) << "\","
               << "\"radius_m\":" << destination_body.radius_m << ","
               << "\"x\":" << destination_body_position.x << ","
               << "\"y\":" << destination_body_position.y << ","
               << "\"z\":" << destination_body_position.z
               << "}";
    }
}

}  // namespace

std::string bridge_paths_signature(const World& w) {
    std::string signature;
    for (const auto& ship : w.state.ships) {
        if (ship.phase == domain::ShipMissionPhase::InTransit || ship.phase == domain::ShipMissionPhase::AwaitingDeparture) {
            signature += ship.id + "=" + bridge_path_id(ship) + ";";
        }
    }
    for (const auto& derelict : w.state.derelicts) {
        signature += derelict.id + "=" + derelict_path_id(derelict) + ";";
    }
    for (const auto& patrol : w.state.patrols) {
        if (patrol_has_path(patrol)) {
            signature += patrol.id + "=" + patrol_path_id(patrol) + ";";
        }
    }
    return signature;
}

std::string build_bridge_paths_json(const World& w) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(6);
    output << "{\"paths\":{";
    bool first = true;
    for (const auto& ship : w.state.ships) {
        if (ship.phase != domain::ShipMissionPhase::InTransit && ship.phase != domain::ShipMissionPhase::AwaitingDeparture) {
            continue;
        }
        output << (first ? "" : ",") << "\"" << json_escape(ship.id) << "\":{\"path_id\":\"" << json_escape(bridge_path_id(ship)) << "\",";
        write_ship_path_json(w, output, ship);
        output << "}";
        first = false;
    }
    for (const auto& derelict : w.state.derelicts) {
        if (derelict.sampled_path.size() < 2) {
            continue;
        }
        output << (first ? "" : ",") << "\"" << json_escape(derelict.id) << "\":{\"path_id\":\"" << derelict_path_id(derelict) << "\",";
        write_timed_path_json(output, derelict.sampled_path, derelict.sampled_times_s);
        output << "}";
        first = false;
    }
    for (const auto& patrol : w.state.patrols) {
        if (!patrol_has_path(patrol)) {
            continue;
        }
        output << (first ? "" : ",") << "\"" << json_escape(patrol.id) << "\":{\"path_id\":\"" << patrol_path_id(patrol) << "\",";
        write_timed_path_json(output, patrol.path, patrol.times);
        output << "}";
        first = false;
    }
    output << "}}";
    return output.str();
}

// Live state of a ship's systems for the inspector (docs/modules/ships_and_fleet.md):
// whether the engine runs, and how hot the drive radiators are. Nuclear-thermal burns are
// impulsive in the planners; for display each burn lasts its share of the mission's
// propellant (half each) at the engine's mass flow. Variable-Isp ships thrust the whole way.
// Idle radiators sit near the sunlit equilibrium of a grey body, 278.6 K / sqrt(r in AU).
namespace {

std::string ship_systems_json(const World& w, 
    const domain::ShipState& ship, const domain::ShipClassDefinition& ship_class, const math::Vec3d& position) {
    std::string engine_state = "off";
    double burn_s = 0.0;
    const domain::ShipComponentDefinition* engine = nullptr;
    const domain::ShipComponentDefinition* radiator = nullptr;
    for (const auto& c : ship_class.components) {
        engine = c.kind == "engine" ? &c : engine;
        radiator = c.kind == "radiator" ? &c : radiator;
    }
    if (ship.phase == domain::ShipMissionPhase::InTransit) {
        const auto& mission = ship.active_mission;
        if (ship_class.propulsion_type == "variable_isp") {
            engine_state = "thrusting";
        } else if (engine != nullptr && engine->thrust_n > 0.0 && engine->isp_s > 0.0) {
            const double mass_flow_kg_s = engine->thrust_n / (engine->isp_s * 9.80665);
            burn_s = 0.5 * std::max(0.0, mission.propellant_cost_kg) / mass_flow_kg_s;
            const bool burning = w.state.game_time_s - mission.departure_time_s < burn_s || mission.arrival_time_s - w.state.game_time_s < burn_s;
            engine_state = burning ? "burning" : "coasting";
        }
    }
    double radiator_temperature_k = 0.0;
    if (radiator != nullptr) {
        const double r_au = std::max(0.05, std::hypot(position.x, position.z) / 1.495978707e11);
        radiator_temperature_k = engine_state == "thrusting" ? radiator->temperature_k : 278.6 / std::sqrt(r_au);
    }
    // Tier 2: the crew's dose this leg against the limit, today's dose rate, boil-off and wear.
    const auto& env = w.data.universe.ship_environment;
    std::string tier2;
    if (env.radiation) {
        const double rate = ship.phase == domain::ShipMissionPhase::InTransit
            ? crew_environment::gcr_msv_per_day(env, query::ship_r_au(w, ship), w.state.game_time_s / 86400.0)
                * crew_environment::transit_gcr_factor(env, ship_class, query::cabin_shield_g_cm2(w, ship_class, ship.propellant_kg))
            : query::docked_dose_msv_per_day(w, ship.current_station_id, w.state.game_time_s);
        tier2 += std::format("\"leg_dose_msv\":{:.1f},\"dose_limit_msv\":{:.0f},\"dose_rate_msv_day\":{:.3f},",
            ship.leg_dose_msv, env.career_limit_msv, rate);
    }
    if (const double boil = query::boiloff_kg_per_day(w, ship_class, query::ship_r_au(w, ship)); boil > 0.0) {
        tier2 += std::format("\"boiloff_kg_day\":{:.1f},", ship.phase == domain::ShipMissionPhase::InTransit ? boil : 0.0);
    }
    if (env.wear) {
        tier2 += std::format("\"engine_wear\":{:.4f},\"drive_life_h\":{:.0f},\"radiator_condition\":{:.4f},",
            ship.engine_wear, query::drive_life_h(w, ship_class), ship.radiator_condition);
    }
    return std::format("\"engine_state\":\"{}\",\"engine_burn_s\":{:.1f},\"radiator_temperature_k\":{:.1f},{}",
        engine_state, burn_s, radiator_temperature_k, tier2);
}

}  // namespace

std::string build_bridge_designs_json(const World& w) {
    std::ostringstream output;
    output << std::setprecision(10);
    output << "{\"designs\":{";
    bool first_class = true;
    std::vector<const domain::ShipClassDefinition*> all;
    for (const auto& ship_class : w.data.universe.ship_classes) {
        all.push_back(&ship_class);
    }
    for (const auto& [id, design] : w.caches.designs) {
        all.push_back(&design);
    }
    for (const auto* class_ptr : all) {
        const auto& ship_class = *class_ptr;
        output << (first_class ? "" : ",") << "\"" << json_escape(ship_class.id) << "\":{"
               << "\"name\":\"" << json_escape(ship_class.name) << "\","
               << "\"hull_id\":\"" << json_escape(ship_class.hull_id) << "\","
               << "\"propulsion_type\":\"" << json_escape(ship_class.propulsion_type) << "\","
               << "\"dry_mass_kg\":" << ship_class.dry_mass_kg << ","
               << "\"propellant_capacity_kg\":" << ship_class.propellant_capacity_kg << ","
               << "\"cargo_capacity_units\":" << ship_class.cargo_capacity_units << ","
               << "\"crew_size\":" << ship_class.crew_size << ","
               << (ship_class.berths > 0.0 ? std::format("\"berths\":{},", ship_class.berths) : std::string())
               << "\"ship_value_cr\":" << ship_class.ship_value_cr << ","
               << "\"max_delta_v_mps\":" << ship_class.max_delta_v_mps << ","
               << "\"cruise_accel_mps2\":" << ship_class.cruise_accel_mps2 << ","
               << "\"specific_engine_power_w_per_kg\":" << ship_class.specific_engine_power_w_per_kg << ","
               << "\"components\":[";
        bool first = true;
        for (const auto& c : ship_class.components) {
            output << (first ? "" : ",") << "{"
                   << "\"id\":\"" << json_escape(c.id) << "\","
                   << "\"parent_id\":\"" << json_escape(c.parent_id) << "\","
                   << "\"kind\":\"" << json_escape(c.kind) << "\","
                   << "\"name\":\"" << json_escape(c.name) << "\","
                   << "\"mass_kg\":" << c.mass_kg << ","
                   << "\"price_cr\":" << c.price_cr << ","
                   << "\"power_w\":" << c.power_w << ","
                   << "\"heat_w\":" << c.heat_w << ","
                   << "\"temperature_k\":" << c.temperature_k << ","
                   << "\"volume_m3\":" << c.volume_m3 << ","
                   << "\"area_m2\":" << c.area_m2 << ","
                   << "\"capacity_kg\":" << c.capacity_kg << ","
                   << "\"crew_capacity\":" << c.crew_capacity << ","
                   << "\"isp_s\":" << c.isp_s << ","
                   << "\"thrust_n\":" << c.thrust_n << "}";
            first = false;
        }
        output << "]}";
        first_class = false;
    }
    output << "}}";
    return output.str();
}

std::string build_bridge_snapshot_json(const World& w, bool paused, std::uint64_t snapshot_seq, double snapshot_real_time_s, bool with_paths) {
    const profiling::Scope profile_scope(profiling::Phase::Snapshot);
    auto inventory_value = [](const domain::Inventory& inventory, const std::string& commodity_id) {
        const auto it = inventory.find(commodity_id);
        return it == inventory.end() ? 0.0 : it->second;
    };

    auto station_inventory = [&](const std::string& station_id) -> const domain::Inventory& {
        return query::get_station_state(w, station_id).inventory;
    };

    std::ostringstream output;
    output << std::fixed << std::setprecision(6);
    // {"commodity": units, ...} for the non-zero entries, in the commodity table's order.
    const auto write_goods = [&](const domain::Inventory& goods) {
        output << "{";
        bool first = true;
        for (const auto& commodity : w.data.universe.commodities) {
            const auto it = goods.find(commodity.id);
            if (it == goods.end() || std::abs(it->second) < 1e-9) {
                continue;
            }
            output << (first ? "" : ",") << "\"" << json_escape(commodity.id) << "\":" << it->second;
            first = false;
        }
        output << "}";
    };
    output << "{";
    output << "\"snapshot_seq\":" << snapshot_seq << ",";
    output << "\"snapshot_real_time_s\":" << snapshot_real_time_s << ",";
    output << "\"game_time_s\":" << w.state.game_time_s << ",";
    output << "\"game_time_days\":" << (w.state.game_time_s / 86400.0) << ",";
    output << "\"timewarp_factor\":" << w.state.timewarp_factor << ",";
    output << "\"paused\":" << (paused ? "true" : "false") << ",";

    output << "\"factions\":[";
    for (std::size_t i = 0; i < w.data.universe.factions.size(); ++i) {
        const auto& faction = w.data.universe.factions[i];
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"id\":\"" << json_escape(faction.id) << "\","
               << "\"name\":\"" << json_escape(faction.name) << "\","
               << "\"color\":\"" << json_escape(faction.color_hex) << "\""
               << "}";
    }
    output << "],";

    output << "\"commodities\":[";
    for (std::size_t i = 0; i < w.data.universe.commodities.size(); ++i) {
        const auto& commodity = w.data.universe.commodities[i];
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"id\":\"" << json_escape(commodity.id) << "\","
               << "\"name\":\"" << json_escape(commodity.name) << "\","
               << "\"base_price\":" << commodity.base_price
               << "}";
    }
    output << "],";

    double total_credits = 0.0;
    for (const auto& station : w.state.stations) {
        total_credits += station.credits;
    }
    for (const auto& ship : w.state.ships) {
        total_credits += ship.credits;
    }
    output << "\"total_credits\":" << total_credits << ",";
    output << "\"outside_economy_credits\":" << w.state.outside_economy_credits << ",";
    output << "\"faction_treasuries\":{";
    for (std::size_t i = 0; i < w.data.universe.factions.size(); ++i) {
        const auto& faction_id = w.data.universe.factions[i].id;
        const auto it = w.state.faction_treasuries.find(faction_id);
        output << (i > 0 ? "," : "") << "\"" << json_escape(faction_id) << "\":"
               << (it == w.state.faction_treasuries.end() ? 0.0 : it->second);
    }
    output << "},\"faction_credit_limits\":{";
    for (std::size_t i = 0; i < w.data.universe.factions.size(); ++i) {
        const auto& faction_id = w.data.universe.factions[i].id;
        output << (i > 0 ? "," : "") << "\"" << json_escape(faction_id) << "\":" << query::faction_credit_limit(w, faction_id);
    }
    output << "},";

    output << "\"recent_trades\":[";
    for (std::size_t i = 0; i < w.state.recent_trades.size(); ++i) {
        const auto& trade = w.state.recent_trades[i];
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"time_s\":" << trade.time_s << ","
               << "\"ship_id\":\"" << json_escape(trade.ship_id) << "\","
               << "\"station_id\":\"" << json_escape(trade.station_id) << "\","
               << "\"commodity_id\":\"" << json_escape(trade.commodity_id) << "\","
               << "\"kind\":\"" << json_escape(trade.kind) << "\","
               << "\"units\":" << trade.units << ","
               << "\"unit_price\":" << trade.unit_price << ","
               << "\"total\":" << trade.total
               << "}";
    }
    output << "],";

    output << "\"bodies\":[";
    for (std::size_t i = 0; i < w.data.universe.bodies.size(); ++i) {
        const auto& body = w.data.universe.bodies[i];
        const auto position = w.engines.mechanics.get_body_position(body.id, w.state.game_time_s);
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"id\":\"" << json_escape(body.id) << "\","
               << "\"name\":\"" << json_escape(body.name) << "\","
               << "\"parent_id\":\"" << json_escape(body.orbit.parent_id) << "\","
               << "\"radius_m\":" << body.radius_m << ","
               // Circular orbits: the UI places bodies at any time from these.
               << "\"semi_major_axis_m\":" << body.orbit.semi_major_axis_m << ","
               << "\"orbital_period_s\":" << body.orbit.orbital_period_s << ","
               << "\"phase_at_epoch_rad\":" << body.orbit.phase_at_epoch_rad << ","
               << "\"x\":" << position.x << ","
               << "\"y\":" << position.y << ","
               << "\"z\":" << position.z
               << "}";
    }
    output << "],";

    output << "\"stations\":[";
    for (std::size_t i = 0; i < w.data.universe.stations.size(); ++i) {
        const auto& station = w.data.universe.stations[i];
        const auto position = w.engines.mechanics.get_station_position(station, w.state.game_time_s);
        if (i > 0) {
            output << ",";
        }
        const auto& inventory = station_inventory(station.id);
        output << "{"
               << "\"id\":\"" << json_escape(station.id) << "\","
               << "\"name\":\"" << json_escape(station.name) << "\","
               << "\"faction_id\":\"" << json_escape(station.faction_id) << "\","
               << "\"parent_body_id\":\"" << json_escape(station.parent_body_id) << "\","
               << "\"economy_profile_id\":\"" << json_escape(station.economy_profile_id) << "\","
               << "\"altitude_m\":" << station.altitude_m << ","
               << "\"theta_rad\":" << station.theta_rad << ","
               << "\"population\":" << std::llround(query::get_station_state(w, station.id).population) << ","
               << "\"seeded_population\":" << station.population << ","
               << "\"supply_index\":" << query::get_station_state(w, station.id).supply_index << ","
               << systems::station_economy_json(w, query::get_station_state(w, station.id))
               << systems::station_passengers_json(w, query::get_station_state(w, station.id))
               << systems::station_systems_json(w, station, query::get_station_state(w, station.id))
               << "\"events\":" << [&] {
                      std::string list = "[";
                      for (const auto& active : query::get_station_state(w, station.id).events) {
                          const auto it = std::find_if(w.data.universe.events.begin(), w.data.universe.events.end(),
                              [&](const auto& event) { return event.id == active.event_id; });
                          if (it == w.data.universe.events.end()) {
                              continue;
                          }
                          list += std::format("{}{{\"headline\":\"{}\",\"days_left\":{:.1f}}}", list.size() > 1 ? "," : "",
                              json_escape(it->headline), (active.end_s - w.state.game_time_s) / 86400.0);
                      }
                      return list + "]";
                  }() << ","
               << "\"x\":" << position.x << ","
               << "\"y\":" << position.y << ","
               << "\"z\":" << position.z << ","
               // Legacy fields kept for Godot frontend compatibility
               << "\"food\":" << inventory_value(inventory, "food") << ","
               << "\"fuel\":" << inventory_value(inventory, "fuel") << ","
               << "\"metals\":" << inventory_value(inventory, "metals") << ","
               // Full inventory dict for all commodities
               << "\"inventory\":{";
        for (std::size_t ci = 0; ci < w.data.universe.commodities.size(); ++ci) {
            if (ci > 0) output << ",";
            output << "\"" << json_escape(w.data.universe.commodities[ci].id) << "\":"
                   << inventory_value(inventory, w.data.universe.commodities[ci].id);
        }
        output << "},";

        const auto& station_state = query::get_station_state(w, station.id);
        double storage_used = 0.0;
        for (const auto& [_, units] : inventory) {
            storage_used += std::max(0.0, units);
        }
        output << "\"credits\":" << station_state.credits << ","
               << "\"storage_capacity\":" << station.storage_capacity_units << ","
               << "\"storage_used\":" << storage_used << ","
               << "\"prices\":{";
        for (std::size_t ci = 0; ci < w.data.universe.commodities.size(); ++ci) {
            if (ci > 0) output << ",";
            output << "\"" << json_escape(w.data.universe.commodities[ci].id) << "\":"
                   << systems::station_price(w, station_state, w.data.universe.commodities[ci].id);
        }
        output << "},"
               << "\"net_rates\":{";
        const auto& net_rates = w.engines.economy.get_station_net_rates(station);
        bool first_rate = true;
        for (const auto& commodity : w.data.universe.commodities) {
            const auto rate_it = net_rates.find(commodity.id);
            if (rate_it == net_rates.end()) {
                continue;
            }
            if (!first_rate) output << ",";
            first_rate = false;
            output << "\"" << json_escape(commodity.id) << "\":" << rate_it->second;
        }
        output << "},\"target_stock\":{";
        bool first_target = true;
        for (const auto& commodity : w.data.universe.commodities) {
            if (!net_rates.contains(commodity.id) && !(commodity.id == economy::FUEL_ID)) {
                continue;
            }
            // Ordered goods: the order-up-to level S (step 29).
            const auto order = station_state.order_stats.find(commodity.id);
            output << (first_target ? "" : ",") << "\"" << json_escape(commodity.id) << "\":"
                   << (order == station_state.order_stats.end() ? w.engines.economy.get_target_stock(station, commodity.id)
                       : systems::order_levels(w, station_state, commodity.id, order->second).order_up_to_units);
            first_target = false;
        }
        output << "},\"demand_units\":";
        write_goods(station_state.demand_units);
        output << ",\"unmet_units\":";
        write_goods(station_state.unmet_units);
        output << ",\"imports_per_day\":";
        write_goods(station_state.import_units_per_day);
        output << ",\"exports_per_day\":";
        write_goods(station_state.export_units_per_day);
        output << ",\"ship_fuel_per_day\":" << station_state.ship_fuel_units_per_day
               << ",\"fuel_factory_per_day\":" << w.engines.economy.fuel_factory_output(station)
               << ",\"export_market\":[";
        bool first_export = true;
        for (const auto& commodity : w.data.universe.commodities) {
            if (w.engines.economy.is_export_market(station, commodity.id)) {
                output << (first_export ? "" : ",") << "\"" << json_escape(commodity.id) << "\"";
                first_export = false;
            }
        }
        // Step 14-17 state: reference prices of what it consumes, what limits its outputs, its
        // upkeep, its import costs, what it can pay and its open emergencies.
        output << "],\"reference_prices\":{";
        bool first_reference = true;
        for (const auto& commodity : w.data.universe.commodities) {
            const auto rate_it = net_rates.find(commodity.id);
            if (rate_it == net_rates.end() || rate_it->second >= 0.0) {
                continue;
            }
            output << (first_reference ? "" : ",") << "\"" << json_escape(commodity.id) << "\":"
                   << w.engines.economy.reference_price(station, commodity.id);
            first_reference = false;
        }
        output << "},\"output_factor\":";
        write_goods(station_state.output_factor);
        output << ",\"output_limited_by\":{";
        bool first_limit = true;
        for (const auto& [output_id, input_id] : station_state.output_limited_by) {
            output << (first_limit ? "" : ",") << "\"" << json_escape(output_id) << "\":\"" << json_escape(input_id) << "\"";
            first_limit = false;
        }
        output << "},\"upkeep_multiplier\":" << station_state.upkeep_multiplier
               << ",\"upkeep_availability\":{";
        bool first_upkeep = true;  // zeros included: those are the shortages
        for (const auto& commodity : w.data.universe.commodities) {
            if (const auto it = station_state.upkeep_availability.find(commodity.id); it != station_state.upkeep_availability.end()) {
                output << (first_upkeep ? "" : ",") << "\"" << json_escape(commodity.id) << "\":" << it->second;
                first_upkeep = false;
            }
        }
        output << "},\"import_unit_cost\":";
        write_goods(station_state.import_unit_cost);
        // Step 29, observed only: the purchase order the station would post for each good.
        output << ",\"orders\":{";
        bool first_order = true;
        for (const auto& [commodity_id, stats] : station_state.order_stats) {
            const auto view = systems::order_view(w, station_state, commodity_id);
            output << (first_order ? "" : ",") << "\"" << json_escape(commodity_id) << "\":{"
                   << "\"kind\":\"" << view.ceiling_kind << "\""
                   << ",\"use_per_day\":" << view.use_per_day
                   << ",\"lead_days\":" << view.lead_days
                   << ",\"interval_days\":" << view.interval_days
                   << ",\"spread_days\":" << view.spread_days
                   << ",\"going_rate\":" << view.going_rate
                   << ",\"order_price\":" << view.order_price
                   << ",\"reference_price\":" << view.reference_price
                   << ",\"safety_units\":" << view.safety_units
                   << ",\"reorder_units\":" << view.reorder_units
                   << ",\"order_up_to_units\":" << view.order_up_to_units
                   << ",\"max_units\":" << view.max_units
                   << ",\"contracted_units\":" << view.contracted_units
                   << ",\"open_units\":" << view.open_units
                   << ",\"days_to_safety\":" << view.days_to_safety
                   << ",\"ceiling\":" << view.ceiling
                   << ",\"station_ceiling\":" << view.station_ceiling
                   << ",\"market_price\":" << stats.market_price
                   << ",\"market_bidders\":" << stats.market_bidders
                   << ",\"deliveries\":" << stats.deliveries << "}";
            first_order = false;
        }
        output << "}";
        output << ",\"emergencies\":[";
        bool first_emergency = true;
        for (const auto& emergency : w.state.emergencies) {
            if (emergency.station_id != station.id) {
                continue;
            }
            output << (first_emergency ? "" : ",") << "{\"commodity_id\":\"" << json_escape(emergency.commodity_id)
                   << "\",\"premium\":" << emergency.premium << ",\"units_open\":" << emergency.units_open
                   << ",\"days\":" << (w.state.game_time_s - emergency.opened_s) / 86400.0
                   << ",\"faction_paid\":" << emergency.faction_paid << "}";
            first_emergency = false;
        }
        output << "],\"ledger\":{"
               << "\"household_sales\":" << station_state.ledger.household_sales << ","
               << "\"producer_purchases\":" << station_state.ledger.producer_purchases << ","
               << "\"dividends\":" << station_state.ledger.dividends << ","
               << "\"subsidies\":" << station_state.ledger.subsidies << ","
               << "\"taxes\":" << station_state.ledger.taxes
               << "}"
               << "}";
    }
    output << "],";

    output << "\"ships\":[";
    for (std::size_t i = 0; i < w.state.ships.size(); ++i) {
        const auto& ship = w.state.ships[i];
        const auto& ship_class = query::ship_design(w, ship);
        const auto position = query::get_ship_render_position(w, ship);
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"id\":\"" << json_escape(ship.id) << "\","
               << "\"name\":\"" << json_escape(ship.name) << "\","
               << "\"faction_id\":\"" << json_escape(ship.faction_id) << "\","
               << "\"class_id\":\"" << json_escape(ship.class_id) << "\","
               << "\"design_id\":\"" << json_escape(ship_class.id) << "\","
               << "\"cargo_capacity_units\":" << ship_class.cargo_capacity_units << ","
               << "\"credits\":" << ship.credits << ","
               << "\"lifetime_profit\":" << ship.lifetime_profit << ","
               << "\"mission_value\":" << (ship.active_mission.expected_revenue - ship.active_mission.purchase_cost
                   - ship.active_mission.fuel_cost - ship.active_mission.operating_cost) << ","
               << "\"propulsion_type\":\"" << json_escape(ship_class.propulsion_type) << "\","
               << "\"trajectory_type\":\"" << json_escape(ship.active_mission.trajectory_type) << "\","
               << "\"phase\":\"" << json_escape(query::mission_phase_name(ship.phase)) << "\","
               << "\"current_station_id\":\"" << json_escape(ship.current_station_id) << "\","
               << "\"propellant_kg\":" << ship.propellant_kg << ","
               << "\"dry_mass_kg\":" << ship_class.dry_mass_kg << ","
               << "\"propellant_capacity_kg\":" << ship_class.propellant_capacity_kg << ","
               << "\"initial_mass_kg\":" << (ship_class.dry_mass_kg + ship_class.propellant_capacity_kg) << ","
               << "\"current_mass_kg\":" << (ship_class.dry_mass_kg + std::max(0.0, ship.propellant_kg)) << ","
               << "\"origin_station_id\":\"" << json_escape(ship.active_mission.origin_station_id) << "\","
               << "\"destination_station_id\":\"" << json_escape(ship.active_mission.destination_station_id) << "\","
               << "\"commodity_id\":\"" << json_escape(largest_lot(ship.active_mission.cargo)) << "\","
               << "\"cargo_units\":" << domain::total_units(ship.active_mission.cargo) << ","
               << "\"cargo\":" << cargo_json(ship.active_mission.cargo) << ","
               << "\"departure_time_s\":" << ship.active_mission.departure_time_s << ","
               << "\"arrival_time_s\":" << ship.active_mission.arrival_time_s << ","
               << "\"wait_time_s\":" << ship.active_mission.wait_time_s << ","
               << "\"coast_time_s\":" << ship.active_mission.coast_time_s << ","
               << "\"total_travel_time_s\":" << ship.active_mission.total_travel_time_s << ","
               << "\"remaining_travel_time_s\":" << ship.active_mission.remaining_travel_time_s << ","
               << "\"x\":" << position.x << ","
               << "\"y\":" << position.y << ","
               << "\"z\":" << position.z << ","
               << "\"class_name\":\"" << json_escape(ship_class.name) << "\","
               << "\"hull_id\":\"" << json_escape(ship_class.hull_id.empty() ? ship_class.id : ship_class.hull_id) << "\","
               << "\"crew_size\":" << ship_class.crew_size << ","
               << "\"cargo_mass_kg\":" << [&] {
                      double kg = 0.0;
                      for (const auto& lot : ship.active_mission.cargo) {
                          kg += lot.units * query::get_commodity(w, lot.commodity_id).mass_per_unit_kg;
                      }
                      return kg;
                  }() << ","
               << "\"provisions_mass_kg\":" << query::provisions_mass_kg(w, ship) << ","
               << systems::ship_passengers_json(w, ship, ship_class)
               << ship_systems_json(w, ship, ship_class, position)
               << "\"provision_days\":" << [&] {
                      double days = std::numeric_limits<double>::infinity();
                      for (const auto& [commodity_id, per_crew_day] : w.data.universe.ship_operations.life_support_units_per_crew_day) {
                          const double need = ship_class.crew_size * per_crew_day;
                          if (need > 0.0) {
                              const auto it = ship.provisions.find(commodity_id);
                              days = std::min(days, (it == ship.provisions.end() ? 0.0 : it->second) / need);
                          }
                      }
                      return std::isfinite(days) ? days : 0.0;
                  }() << ","
               << "\"home_station_id\":\"" << json_escape(ship.home_station_id) << "\","
               << "\"pickup_commodity_id\":\"" << json_escape(ship.active_mission.pickup_commodity_id) << "\","
               << "\"pickup_units\":" << ship.active_mission.pickup_units << ","
               << "\"expected_revenue\":" << ship.active_mission.expected_revenue << ","
               << "\"purchase_cost\":" << ship.active_mission.purchase_cost << ","
               << "\"liner_stops\":[" << [&] {
                      std::string text;
                      if (const auto* stops = systems::liner_stops(w, ship)) {
                          for (const auto& stop : *stops) {
                              text += (text.empty() ? "\"" : ",\"") + json_escape(stop) + "\"";
                          }
                      }
                      return text;
                  }() << "],"
               << "\"contract_stops\":[" << [&] {
                      std::string text;
                      if (systems::under_contract(w, ship)) {
                          for (const auto& stop : ship.contract.stops) {
                              text += (text.empty() ? "\"" : ",\"") + json_escape(stop) + "\"";
                          }
                      }
                      return text;
                  }() << "],"
               << "\"contract_until_s\":" << (systems::under_contract(w, ship) ? ship.contract.until_s : 0.0) << ","
               << "\"contract_deliveries\":[" << [&] {
                      std::string text;
                      if (systems::under_contract(w, ship)) {
                          for (const auto& delivery : ship.contract.deliveries) {
                              text += std::format("{}{{\"to\":\"{}\",\"commodity_id\":\"{}\",\"units\":{:.1f},\"unit_price\":{:.2f}}}",
                                  text.empty() ? "" : ",", json_escape(delivery.to_station_id), json_escape(delivery.commodity_id),
                                  delivery.units, delivery.unit_price);
                          }
                      }
                      return text;
                  }() << "],"
               << "\"route_destination_id\":\"" << json_escape(ship.route_destination_id) << "\","
               << "\"route_commodity_id\":\"" << json_escape(ship.route_commodity_id) << "\","
               << "\"route_until_s\":" << ship.route_until_s << ","
               << "\"refit_class_id\":\"" << json_escape(ship.refit_class_id) << "\","
               << "\"refit_done_s\":" << ship.refit_done_s << ","
               << "\"commissioned_s\":" << ship.commissioned_s << ","
               << "\"built_s\":" << ship.built_s << ","
               << "\"salvage_derelict_id\":\"" << json_escape(ship.active_mission.salvage_done ? std::string() : ship.active_mission.salvage_derelict_id) << "\","
               << "\"salvage_target_name\":\"" << [&] {
                      if (ship.active_mission.salvage_derelict_id.empty() || ship.active_mission.salvage_done) {
                          return std::string();
                      }
                      for (const auto& derelict : w.state.derelicts) {
                          if (derelict.id == ship.active_mission.salvage_derelict_id) {
                              return json_escape(derelict.name);
                          }
                      }
                      return std::string();
                  }() << "\","
               << "\"rendezvous_time_s\":" << ship.active_mission.rendezvous_time_s << ","
               << "\"ledger\":{"
               << "\"cargo_revenue\":" << ship.ledger.cargo_revenue << ","
               << "\"cargo_purchases\":" << ship.ledger.cargo_purchases << ","
               << "\"fuel\":" << ship.ledger.fuel << ","
               << "\"wages\":" << ship.ledger.wages << ","
               << "\"capital\":" << ship.ledger.capital << ","
               << "\"provisions\":" << ship.ledger.provisions << ","
               << "\"refits\":" << ship.ledger.refits << ","
               << "\"dividends\":" << ship.ledger.dividends
               << "}";
        if (ship.phase == domain::ShipMissionPhase::InTransit || ship.phase == domain::ShipMissionPhase::AwaitingDeparture) {
            output << ",\"path_id\":\"" << json_escape(bridge_path_id(ship)) << "\"";
            if (with_paths) {
                output << ",";
                write_ship_path_json(w, output, ship);
            }
        }
        output << "}";
    }
    // Derelicts (failures and salvage) ride with the ships, phase "derelict", at their coast's
    // position now; the racers name them in salvage_target_name.
    for (const auto& derelict : w.state.derelicts) {
        const auto position = interpolate_timed_sampled_path(derelict.sampled_path, derelict.sampled_times_s, w.state.game_time_s);
        const auto& design = query::get_ship_class(w, derelict.class_id);
        output << (w.state.ships.empty() && &derelict == &w.state.derelicts.front() ? "" : ",") << "{"
               << "\"id\":\"" << json_escape(derelict.id) << "\","
               << "\"name\":\"" << json_escape("Wreck of " + derelict.name) << "\","
               << "\"faction_id\":\"" << json_escape(derelict.faction_id) << "\","
               << "\"class_id\":\"" << json_escape(derelict.class_id) << "\","
               << "\"design_id\":\"" << json_escape(derelict.class_id) << "\","
               << "\"class_name\":\"" << json_escape(design.name) << "\","
               << "\"hull_id\":\"" << json_escape(design.hull_id.empty() ? design.id : design.hull_id) << "\","
               << "\"propulsion_type\":\"" << json_escape(design.propulsion_type) << "\","
               << "\"phase\":\"derelict\","
               << "\"current_station_id\":\"\","
               << "\"origin_station_id\":\"" << json_escape(derelict.origin_station_id) << "\","
               << "\"destination_station_id\":\"" << json_escape(derelict.destination_station_id) << "\","
               << "\"home_station_id\":\"" << json_escape(derelict.home_station_id) << "\","
               << "\"commodity_id\":\"" << json_escape(largest_lot(derelict.cargo)) << "\","
               << "\"cargo_units\":" << domain::total_units(derelict.cargo) << ","
               << "\"cargo\":" << cargo_json(derelict.cargo) << ","
               << "\"cargo_capacity_units\":" << design.cargo_capacity_units << ","
               << "\"dry_mass_kg\":" << design.dry_mass_kg << ","
               << "\"crew_size\":0,"
               << "\"failed_at_s\":" << derelict.failed_at_s << ","
               << "\"expires_s\":" << derelict.expires_s << ","
               << "\"claimant_ship_id\":\"" << json_escape(derelict.claimant_ship_id) << "\","
               << "\"claim_eta_s\":" << derelict.claim_eta_s << ","
               << "\"racers\":" << derelict.racers << ","
               << "\"scrap_value_cr\":" << w.data.universe.failures.scrap_fraction * derelict.ship_value_cr << ","
               << "\"trajectory_type\":\"derelict\",";
        if (derelict.sampled_path.size() >= 2) {
            output << "\"path_id\":\"" << derelict_path_id(derelict) << "\","
                   << "\"departure_time_s\":" << derelict.sampled_times_s.front() << ","
                   << "\"arrival_time_s\":" << derelict.sampled_times_s.back() << ",";
            if (with_paths) {
                write_timed_path_json(output, derelict.sampled_path, derelict.sampled_times_s);
                output << ",";
            }
        }
        output
               << "\"x\":" << position.x << ","
               << "\"y\":" << position.y << ","
               << "\"z\":" << position.z
               << "}";
    }
    // Police cutters: phase "patrol" when out, "patrol_docked" at their home port.
    bool any_ship = !w.state.ships.empty() || !w.state.derelicts.empty();
    for (const auto& patrol : w.state.patrols) {
        const auto position = systems::patrol_position(w, patrol);
        const auto& design = query::get_ship_class(w, patrol.class_id);
        const bool docked = patrol.phase == domain::PatrolPhase::Docked;
        std::string status;
        switch (patrol.phase) {
            case domain::PatrolPhase::Docked:
                status = "in port";
                break;
            case domain::PatrolPhase::Outbound:
                status = std::format("on its way to inspect {}", patrol.target_name);
                break;
            case domain::PatrolPhase::Inspecting:
                status = std::format("alongside {}, inspecting", patrol.target_name);
                break;
            case domain::PatrolPhase::Returning:
                status = "on its way home";
                break;
        }
        output << (any_ship ? "," : "") << "{"
               << "\"id\":\"" << json_escape(patrol.id) << "\","
               << "\"name\":\"" << json_escape(patrol.name) << "\","
               << "\"faction_id\":\"" << json_escape(patrol.faction_id) << "\","
               << "\"class_id\":\"" << json_escape(patrol.class_id) << "\","
               << "\"design_id\":\"" << json_escape(patrol.class_id) << "\","
               << "\"class_name\":\"Patrol Cutter\","
               << "\"hull_id\":\"" << json_escape(design.hull_id.empty() ? design.id : design.hull_id) << "\","
               << "\"propulsion_type\":\"" << json_escape(design.propulsion_type) << "\","
               << "\"phase\":\"" << (docked ? "patrol_docked" : "patrol") << "\","
               << "\"patrol_status\":\"" << json_escape(status) << "\","
               << "\"patrol_target_id\":\"" << json_escape(patrol.target_ship_id) << "\","
               << "\"inspections\":" << patrol.inspections << ",";
        if (patrol_has_path(patrol)) {
            output << "\"trajectory_type\":\"patrol\",\"path_id\":\"" << patrol_path_id(patrol) << "\","
                   << "\"departure_time_s\":" << patrol.times.front() << ","
                   << "\"arrival_time_s\":" << patrol.times.back() << ",";
            if (with_paths) {
                write_timed_path_json(output, patrol.path, patrol.times);
                output << ",";
            }
        }
        output
               << "\"current_station_id\":\"" << json_escape(docked ? patrol.home_station_id : std::string()) << "\","
               << "\"home_station_id\":\"" << json_escape(patrol.home_station_id) << "\","
               << "\"propellant_kg\":" << patrol.propellant_kg << ","
               << "\"propellant_capacity_kg\":" << design.propellant_capacity_kg << ","
               << "\"dry_mass_kg\":" << design.dry_mass_kg << ","
               << "\"crew_size\":" << design.crew_size << ","
               << "\"cargo\":[],\"cargo_units\":0,\"cargo_capacity_units\":0,"
               << "\"x\":" << position.x << ","
               << "\"y\":" << position.y << ","
               << "\"z\":" << position.z
               << "}";
        any_ship = true;
    }
    output << "],";

    {
        // Economy overview: consumption that found no stock (cumulative, at base prices),
        // exports to the outside economy, the fleet's trade in ships.
        double demand_value = 0.0;
        double unmet_value = 0.0;
        double exports_value = 0.0;
        for (const auto& station : w.state.stations) {
            for (const auto& [commodity_id, units] : station.demand_units) {
                demand_value += units * query::get_commodity(w, commodity_id).base_price;
            }
            for (const auto& [commodity_id, units] : station.unmet_units) {
                unmet_value += units * query::get_commodity(w, commodity_id).base_price;
            }
            for (const auto& [commodity_id, units] : station.market_sold_units) {
                exports_value += units * query::get_commodity(w, commodity_id).base_price;
            }
        }
        output << "\"economy\":{"
               << "\"demand_value\":" << demand_value << ","
               << "\"unmet_value\":" << unmet_value << ","
               << "\"exports_value\":" << exports_value << ","
               << "\"money_supply_target\":" << w.state.seeded_money_supply << ","
               << "\"ships_commissioned\":" << w.state.investment_ledger.ships_commissioned << ","
               << "\"ships_sold\":" << w.state.investment_ledger.ships_sold << ","
               << "\"emergencies_opened\":" << w.state.emergencies_opened << ","
               << "\"emergency_paid\":" << w.state.emergency_paid << ","
               << "\"faction_interest_paid\":" << w.state.faction_interest_paid << ","
               << systems::background_economies_json(w)
               << "},";
    }
    output << "\"recent_events\":[";
    for (std::size_t i = 0; i < w.state.recent_events.size(); ++i) {
        const auto& event = w.state.recent_events[i];
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"time_s\":" << event.time_s << ","
               << "\"category\":\"" << json_escape(event.category) << "\","
               << "\"text\":\"" << json_escape(event.text) << "\""
               << "}";
    }
    output << "]";
    output << "}";
    return output.str();
}

std::string build_report(const World& w) {
    auto inventory_value = [](const domain::Inventory& inventory, const std::string& commodity_id) {
        const auto it = inventory.find(commodity_id);
        return it == inventory.end() ? 0.0 : it->second;
    };

    std::ostringstream output;
    output << "SpaceTrains snapshot at day " << (w.state.game_time_s / 86400.0) << "\n";
    output << "Stations:\n";
    for (const auto& station : w.state.stations) {
        output << "  - " << query::get_station_definition(w, station.station_id).name;
        for (const auto& commodity : w.data.universe.commodities) {
            const double val = inventory_value(station.inventory, commodity.id);
            if (val > 0.1) {
                output << " " << commodity.id << "=" << static_cast<int>(val);
            }
        }
        output << "\n";
    }
    output << "Ships:\n";
    for (const auto& ship : w.state.ships) {
        output << "  - " << ship.name << " at " << query::get_station_definition(w, ship.current_station_id).name
               << " propellant=" << ship.propellant_kg
               << " phase=" << static_cast<int>(ship.phase) << "\n";
    }
    output << "Recent events:\n";
    for (const auto& event : w.state.recent_events) {
        output << "  - [day " << (event.time_s / 86400.0) << "] " << event.text << "\n";
    }
    return output.str();
}

}  // namespace spacetrains::simulation::systems
