#include "simulation/Simulation.hpp"

#include <algorithm>
#include <format>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace spacetrains::simulation {

namespace {
constexpr double FUEL_UNITS_TO_KG = 100.0;
constexpr std::size_t MAX_EVENT_HISTORY = 24;

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += ch;
                break;
        }
    }
    return out;
}
}

Simulation::Simulation(domain::UniverseDefinition universe)
    : universe_(std::move(universe)),
      mechanics_(universe_),
      economy_(universe_) {
    planner_ = std::make_unique<trajectory::KeplerTrajectoryPlanner>(universe_, mechanics_);
    for (const auto& station : universe_.stations) {
        station_defs_by_id_[station.id] = &station;
        stations_.push_back({.station_id = station.id, .inventory = station.initial_inventory});
    }
    for (const auto& ship_class : universe_.ship_classes) {
        ship_classes_by_id_[ship_class.id] = &ship_class;
    }
    for (const auto& seed : universe_.ship_seeds) {
        ships_.push_back({
            .id = seed.id,
            .name = seed.name,
            .faction_id = seed.faction_id,
            .class_id = seed.class_id,
            .home_station_id = seed.home_station_id,
            .current_station_id = seed.start_station_id,
            .phase = domain::ShipMissionPhase::Idle,
            .propellant_kg = seed.initial_propellant_kg,
            .active_mission = {},
        });
    }
}

Simulation Simulation::from_data_root(const std::string& data_root) {
    data_loader::DataLoader loader;
    auto universe = loader.load_universe(data_root);
    return Simulation(std::move(universe));
}

void Simulation::set_timewarp(double timewarp_factor) {
    timewarp_factor_ = std::max(1.0, timewarp_factor);
}

double Simulation::timewarp_factor() const {
    return timewarp_factor_;
}

const domain::UniverseDefinition& Simulation::universe() const {
    return universe_;
}

const domain::ShipClassDefinition& Simulation::get_ship_class(const std::string& class_id) const {
    const auto it = ship_classes_by_id_.find(class_id);
    if (it == ship_classes_by_id_.end()) {
        throw std::runtime_error("Unknown ship class: " + class_id);
    }
    return *it->second;
}

const domain::StationDefinition& Simulation::get_station_definition(const std::string& station_id) const {
    const auto it = station_defs_by_id_.find(station_id);
    if (it == station_defs_by_id_.end()) {
        throw std::runtime_error("Unknown station: " + station_id);
    }
    return *it->second;
}

domain::StationState& Simulation::get_station_state(const std::string& station_id) {
    const auto it = std::find_if(
        stations_.begin(),
        stations_.end(),
        [&](const domain::StationState& station) { return station.station_id == station_id; });
    if (it == stations_.end()) {
        throw std::runtime_error("Unknown station state: " + station_id);
    }
    return *it;
}

const domain::StationState& Simulation::get_station_state(const std::string& station_id) const {
    const auto it = std::find_if(
        stations_.begin(),
        stations_.end(),
        [&](const domain::StationState& station) { return station.station_id == station_id; });
    if (it == stations_.end()) {
        throw std::runtime_error("Unknown station state: " + station_id);
    }
    return *it;
}

void Simulation::add_event(std::string text) {
    recent_events_.push_back({.time_s = game_time_s_, .text = std::move(text)});
    if (recent_events_.size() > MAX_EVENT_HISTORY) {
        recent_events_.erase(recent_events_.begin(), recent_events_.begin() + static_cast<long>(recent_events_.size() - MAX_EVENT_HISTORY));
    }
}

bool Simulation::try_refuel(domain::ShipState& ship) {
    auto& station = get_station_state(ship.current_station_id);
    const auto& ship_class = get_ship_class(ship.class_id);
    const double missing_kg = std::max(0.0, ship_class.propellant_capacity_kg - ship.propellant_kg);
    if (missing_kg <= 0.0) {
        return true;
    }

    const double available_fuel_units = station.inventory["fuel"];
    const double transferable_kg = std::min(missing_kg, available_fuel_units * FUEL_UNITS_TO_KG);
    station.inventory["fuel"] -= transferable_kg / FUEL_UNITS_TO_KG;
    ship.propellant_kg += transferable_kg;

    if (transferable_kg > 0.0) {
        add_event(std::format("{} refueled {:.0f} kg at {}", ship.name, transferable_kg, get_station_definition(ship.current_station_id).name));
    }
    return ship.propellant_kg >= ship_class.propellant_capacity_kg * 0.4;
}

void Simulation::step_idle_ship(domain::ShipState& ship) {
    if (!try_refuel(ship)) {
        ship.phase = domain::ShipMissionPhase::Stranded;
        add_event(std::format("{} is stranded at {} due to fuel shortage", ship.name, get_station_definition(ship.current_station_id).name));
        return;
    }

    auto& origin_state = get_station_state(ship.current_station_id);
    const auto& origin_def = get_station_definition(ship.current_station_id);
    const auto& ship_class = get_ship_class(ship.class_id);

    double best_score = 0.0;
    const domain::StationDefinition* best_destination = nullptr;
    std::string best_commodity;
    double best_cargo_units = 0.0;

    const auto origin_rates = economy_.get_profile_net_rates(origin_def.economy_profile_id);
    for (const auto& [commodity_id, stock] : origin_state.inventory) {
        if (stock < 8.0) {
            continue;
        }
        const double origin_rate = origin_rates.contains(commodity_id) ? origin_rates.at(commodity_id) : 0.0;
        if (origin_rate <= 0.0) {
            continue;
        }

        for (const auto& destination : universe_.stations) {
            if (destination.id == origin_def.id || destination.faction_id != ship.faction_id) {
                continue;
            }
            const auto destination_rates = economy_.get_profile_net_rates(destination.economy_profile_id);
            const double destination_rate = destination_rates.contains(commodity_id) ? destination_rates.at(commodity_id) : 0.0;
            if (destination_rate >= 0.0) {
                continue;
            }
            const auto& destination_state = get_station_state(destination.id);
            const double need = std::max(0.0, 18.0 - destination_state.inventory.at(commodity_id));
            if (need <= 1.0) {
                continue;
            }

            const double cargo_units = std::min({ship_class.cargo_capacity_units, stock * 0.5, need});
            if (cargo_units <= 0.0) {
                continue;
            }

            const auto plan = planner_->plan_transfer(origin_def, destination, ship, ship_class, game_time_s_);
            if (!plan.feasible) {
                continue;
            }

            const double score = cargo_units / std::max(1.0, plan.travel_time_s / 86400.0);
            if (score > best_score) {
                best_score = score;
                best_destination = &destination;
                best_commodity = commodity_id;
                best_cargo_units = cargo_units;
            }
        }
    }

    if (best_destination == nullptr) {
        return;
    }

    auto plan = planner_->plan_transfer(origin_def, *best_destination, ship, ship_class, game_time_s_);
    if (!plan.feasible) {
        return;
    }

    origin_state.inventory[best_commodity] -= best_cargo_units;
    ship.propellant_kg -= plan.propellant_required_kg;
    ship.phase = domain::ShipMissionPhase::InTransit;
    ship.active_mission = {
        .origin_station_id = origin_def.id,
        .destination_station_id = best_destination->id,
        .commodity_id = best_commodity,
        .cargo_units = best_cargo_units,
        .departure_time_s = game_time_s_,
        .arrival_time_s = plan.arrival_time_s,
        .total_travel_time_s = plan.travel_time_s,
        .remaining_travel_time_s = plan.travel_time_s,
        .propellant_cost_kg = plan.propellant_required_kg,
    };
    add_event(std::format(
        "{} departed {} for {} carrying {:.1f} units of {}",
        ship.name,
        origin_def.name,
        best_destination->name,
        best_cargo_units,
        best_commodity));
}

void Simulation::step_in_transit_ship(domain::ShipState& ship, double dt_s) {
    ship.active_mission.remaining_travel_time_s -= dt_s;
    if (ship.active_mission.remaining_travel_time_s > 0.0) {
        return;
    }

    ship.current_station_id = ship.active_mission.destination_station_id;
    ship.phase = domain::ShipMissionPhase::Idle;
    auto& destination = get_station_state(ship.current_station_id);
    destination.inventory[ship.active_mission.commodity_id] += ship.active_mission.cargo_units;
    add_event(std::format(
        "{} arrived at {} and unloaded {:.1f} units of {}",
        ship.name,
        get_station_definition(ship.current_station_id).name,
        ship.active_mission.cargo_units,
        ship.active_mission.commodity_id));
    ship.active_mission = {};
}

void Simulation::step(double real_dt_s) {
    const double dt_s = real_dt_s * timewarp_factor_;
    game_time_s_ += dt_s;
    economy_.step(stations_, dt_s);

    for (auto& ship : ships_) {
        switch (ship.phase) {
            case domain::ShipMissionPhase::Idle:
                step_idle_ship(ship);
                break;
            case domain::ShipMissionPhase::InTransit:
                step_in_transit_ship(ship, dt_s);
                break;
            case domain::ShipMissionPhase::Refueling:
                ship.phase = domain::ShipMissionPhase::Idle;
                break;
            case domain::ShipMissionPhase::Stranded:
                if (try_refuel(ship)) {
                    ship.phase = domain::ShipMissionPhase::Idle;
                    add_event(std::format("{} recovered from stranded state at {}", ship.name, get_station_definition(ship.current_station_id).name));
                }
                break;
        }
    }
}

domain::SimulationSnapshot Simulation::snapshot() const {
    return {
        .game_time_s = game_time_s_,
        .stations = stations_,
        .ships = ships_,
        .recent_events = recent_events_,
    };
}

std::string Simulation::mission_phase_name(domain::ShipMissionPhase phase) const {
    switch (phase) {
        case domain::ShipMissionPhase::Idle:
            return "idle";
        case domain::ShipMissionPhase::InTransit:
            return "in_transit";
        case domain::ShipMissionPhase::Refueling:
            return "refueling";
        case domain::ShipMissionPhase::Stranded:
            return "stranded";
    }
    return "unknown";
}

math::Vec3d Simulation::get_ship_render_position(const domain::ShipState& ship) const {
    if (ship.phase != domain::ShipMissionPhase::InTransit || ship.active_mission.total_travel_time_s <= 0.0) {
        const auto& station = get_station_definition(ship.current_station_id);
        return mechanics_.get_station_position(station, game_time_s_);
    }

    const auto& origin = get_station_definition(ship.active_mission.origin_station_id);
    const auto& destination = get_station_definition(ship.active_mission.destination_station_id);
    const auto origin_position = mechanics_.get_station_position(origin, ship.active_mission.departure_time_s);
    const auto destination_position = mechanics_.get_station_position(destination, ship.active_mission.arrival_time_s);
    const double progress = std::clamp(
        (game_time_s_ - ship.active_mission.departure_time_s) / ship.active_mission.total_travel_time_s,
        0.0,
        1.0);
    return origin_position * (1.0 - progress) + destination_position * progress;
}

std::string Simulation::build_bridge_snapshot_json(bool paused, std::uint64_t snapshot_seq, double snapshot_real_time_s) const {
    auto inventory_value = [](const domain::Inventory& inventory, const std::string& commodity_id) {
        const auto it = inventory.find(commodity_id);
        return it == inventory.end() ? 0.0 : it->second;
    };

    auto station_inventory = [&](const std::string& station_id) -> const domain::Inventory& {
        return get_station_state(station_id).inventory;
    };

    std::ostringstream output;
    output << std::fixed << std::setprecision(6);
    output << "{";
    output << "\"snapshot_seq\":" << snapshot_seq << ",";
    output << "\"snapshot_real_time_s\":" << snapshot_real_time_s << ",";
    output << "\"game_time_s\":" << game_time_s_ << ",";
    output << "\"game_time_days\":" << (game_time_s_ / 86400.0) << ",";
    output << "\"timewarp_factor\":" << timewarp_factor_ << ",";
    output << "\"paused\":" << (paused ? "true" : "false") << ",";

    output << "\"bodies\":[";
    for (std::size_t i = 0; i < universe_.bodies.size(); ++i) {
        const auto& body = universe_.bodies[i];
        const auto position = mechanics_.get_body_position(body.id, game_time_s_);
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"id\":\"" << json_escape(body.id) << "\","
               << "\"name\":\"" << json_escape(body.name) << "\","
               << "\"radius_m\":" << body.radius_m << ","
               << "\"x\":" << position.x << ","
               << "\"y\":" << position.y << ","
               << "\"z\":" << position.z
               << "}";
    }
    output << "],";

    output << "\"stations\":[";
    for (std::size_t i = 0; i < universe_.stations.size(); ++i) {
        const auto& station = universe_.stations[i];
        const auto position = mechanics_.get_station_position(station, game_time_s_);
        if (i > 0) {
            output << ",";
        }
        const auto& inventory = station_inventory(station.id);
        output << "{"
               << "\"id\":\"" << json_escape(station.id) << "\","
               << "\"name\":\"" << json_escape(station.name) << "\","
               << "\"faction_id\":\"" << json_escape(station.faction_id) << "\","
               << "\"parent_body_id\":\"" << json_escape(station.parent_body_id) << "\","
               << "\"population\":" << station.population << ","
               << "\"x\":" << position.x << ","
               << "\"y\":" << position.y << ","
               << "\"z\":" << position.z << ","
               << "\"food\":" << inventory_value(inventory, "food") << ","
               << "\"fuel\":" << inventory_value(inventory, "fuel") << ","
               << "\"metals\":" << inventory_value(inventory, "metals")
               << "}";
    }
    output << "],";

    output << "\"ships\":[";
    for (std::size_t i = 0; i < ships_.size(); ++i) {
        const auto& ship = ships_[i];
        const auto position = get_ship_render_position(ship);
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"id\":\"" << json_escape(ship.id) << "\","
               << "\"name\":\"" << json_escape(ship.name) << "\","
               << "\"faction_id\":\"" << json_escape(ship.faction_id) << "\","
               << "\"phase\":\"" << json_escape(mission_phase_name(ship.phase)) << "\","
               << "\"current_station_id\":\"" << json_escape(ship.current_station_id) << "\","
               << "\"propellant_kg\":" << ship.propellant_kg << ","
               << "\"origin_station_id\":\"" << json_escape(ship.active_mission.origin_station_id) << "\","
               << "\"destination_station_id\":\"" << json_escape(ship.active_mission.destination_station_id) << "\","
               << "\"cargo_units\":" << ship.active_mission.cargo_units << ","
               << "\"total_travel_time_s\":" << ship.active_mission.total_travel_time_s << ","
               << "\"remaining_travel_time_s\":" << ship.active_mission.remaining_travel_time_s << ","
               << "\"x\":" << position.x << ","
               << "\"y\":" << position.y << ","
               << "\"z\":" << position.z
               << "}";
    }
    output << "],";

    output << "\"recent_events\":[";
    for (std::size_t i = 0; i < recent_events_.size(); ++i) {
        const auto& event = recent_events_[i];
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"time_s\":" << event.time_s << ","
               << "\"text\":\"" << json_escape(event.text) << "\""
               << "}";
    }
    output << "]";
    output << "}";
    return output.str();
}

std::string Simulation::build_report() const {
    auto inventory_value = [](const domain::Inventory& inventory, const std::string& commodity_id) {
        const auto it = inventory.find(commodity_id);
        return it == inventory.end() ? 0.0 : it->second;
    };

    std::ostringstream output;
    output << "SpaceTrains snapshot at day " << (game_time_s_ / 86400.0) << "\n";
    output << "Stations:\n";
    for (const auto& station : stations_) {
        output << "  - " << get_station_definition(station.station_id).name
               << " food=" << inventory_value(station.inventory, "food")
               << " fuel=" << inventory_value(station.inventory, "fuel")
               << " metals=" << inventory_value(station.inventory, "metals")
               << "\n";
    }
    output << "Ships:\n";
    for (const auto& ship : ships_) {
        output << "  - " << ship.name << " at " << get_station_definition(ship.current_station_id).name
               << " propellant=" << ship.propellant_kg
               << " phase=" << static_cast<int>(ship.phase) << "\n";
    }
    output << "Recent events:\n";
    for (const auto& event : recent_events_) {
        output << "  - [day " << (event.time_s / 86400.0) << "] " << event.text << "\n";
    }
    return output.str();
}

}  // namespace spacetrains::simulation
