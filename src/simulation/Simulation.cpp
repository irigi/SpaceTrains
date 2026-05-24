#include "simulation/Simulation.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace spacetrains::simulation {

namespace {
constexpr double FUEL_UNITS_TO_KG = 100.0;
constexpr std::size_t MAX_EVENT_HISTORY = 24;

math::Vec3d interpolate_sampled_path(const std::vector<math::Vec3d>& path, double progress) {
    if (path.empty()) {
        return {};
    }
    if (path.size() == 1) {
        return path.front();
    }

    const double clamped_progress = std::clamp(progress, 0.0, 1.0);
    const double scaled_index = clamped_progress * static_cast<double>(path.size() - 1);
    const auto lower_index = static_cast<std::size_t>(std::floor(scaled_index));
    const auto upper_index = std::min(lower_index + 1, path.size() - 1);
    const double segment_alpha = scaled_index - static_cast<double>(lower_index);
    return path[lower_index] * (1.0 - segment_alpha) + path[upper_index] * segment_alpha;
}

double interpolate_timed_scalar(
    const std::vector<double>& values,
    const std::vector<double>& sample_times_s,
    double time_s) {
    if (values.empty()) {
        return 0.0;
    }
    if (values.size() == 1 || sample_times_s.size() != values.size()) {
        return values.front();
    }
    if (time_s <= sample_times_s.front()) {
        return values.front();
    }
    if (time_s >= sample_times_s.back()) {
        return values.back();
    }
    const auto upper = std::upper_bound(sample_times_s.begin(), sample_times_s.end(), time_s);
    const auto upper_index = static_cast<std::size_t>(std::distance(sample_times_s.begin(), upper));
    const auto lower_index = upper_index - 1;
    const double span_s = std::max(1.0e-6, sample_times_s[upper_index] - sample_times_s[lower_index]);
    const double alpha = (time_s - sample_times_s[lower_index]) / span_s;
    return values[lower_index] * (1.0 - alpha) + values[upper_index] * alpha;
}

math::Vec3d interpolate_timed_sampled_path(
    const std::vector<math::Vec3d>& path,
    const std::vector<double>& sample_times_s,
    double time_s) {
    if (path.empty()) {
        return {};
    }
    if (path.size() == 1 || sample_times_s.size() != path.size()) {
        return path.front();
    }
    if (time_s <= sample_times_s.front()) {
        return path.front();
    }
    if (time_s >= sample_times_s.back()) {
        return path.back();
    }
    const auto upper = std::upper_bound(sample_times_s.begin(), sample_times_s.end(), time_s);
    const auto upper_index = static_cast<std::size_t>(std::distance(sample_times_s.begin(), upper));
    const auto lower_index = upper_index - 1;
    const double span_s = std::max(1.0e-6, sample_times_s[upper_index] - sample_times_s[lower_index]);
    const double alpha = (time_s - sample_times_s[lower_index]) / span_s;
    return path[lower_index] * (1.0 - alpha) + path[upper_index] * alpha;
}

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
    kepler_planner_ = std::make_unique<trajectory::KeplerTrajectoryPlanner>(universe_, mechanics_);
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
    Simulation sim(std::move(universe));

    // Try to load the VariableISP atlas. It lives next to the data root.
    const std::filesystem::path data_path(data_root);
    const std::filesystem::path atlas_path =
        data_path.parent_path() / "tests" / "data" / "variable_isp" / "variable_isp_atlas.bin";
    if (std::filesystem::exists(atlas_path)) {
        try {
            sim.atlas_.load_binary(atlas_path.string());
            sim.atlas_loaded_ = true;
            sim.variable_isp_planner_ = std::make_unique<trajectory::VariableIspTrajectoryPlanner>(
                sim.universe_, sim.mechanics_, sim.atlas_);
        } catch (const std::exception& ex) {
            std::cerr << "[VariableISP] Atlas load failed: " << ex.what() << " — electric ships will be stranded.\n";
        }
    } else {
        std::cerr << "[VariableISP] Atlas not found at " << atlas_path.string()
                  << " — electric ships will be stranded.\n";
    }

    return sim;
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

const economy::EconomySystem& Simulation::economy_system() const {
    return economy_;
}

const domain::ShipClassDefinition& Simulation::get_ship_class(const std::string& class_id) const {
    const auto it = ship_classes_by_id_.find(class_id);
    if (it == ship_classes_by_id_.end()) {
        throw std::runtime_error("Unknown ship class: " + class_id);
    }
    return *it->second;
}

const domain::CelestialBodyDefinition& Simulation::get_body_definition(const std::string& body_id) const {
    const auto it = std::find_if(
        universe_.bodies.begin(),
        universe_.bodies.end(),
        [&](const domain::CelestialBodyDefinition& body) { return body.id == body_id; });
    if (it == universe_.bodies.end()) {
        throw std::runtime_error("Unknown body: " + body_id);
    }
    return *it;
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
    const bool has_refuel_reserve = try_refuel(ship);

    auto& origin_state = get_station_state(ship.current_station_id);
    const auto& origin_def = get_station_definition(ship.current_station_id);
    const auto& ship_class = get_ship_class(ship.class_id);

    double best_score = 0.0;
    const domain::StationDefinition* best_destination = nullptr;
    std::string best_commodity;
    double best_cargo_units = 0.0;

    const auto origin_rates = economy_.get_profile_net_rates(origin_def.economy_profile_id);
    for (const auto& [commodity_id, stock] : origin_state.inventory) {
        const double origin_rate = origin_rates.contains(commodity_id) ? origin_rates.at(commodity_id) : 0.0;
        if (origin_rate <= 0.0) {
            continue;
        }
        const double origin_reserve = 8.0 + origin_rate * 7.0;
        const double surplus = std::max(0.0, stock - origin_reserve);
        if (surplus <= 1.0) {
            continue;
        }

        for (const auto& destination : universe_.stations) {
            if (destination.id == origin_def.id) {
                continue;
            }
            const auto destination_rates = economy_.get_profile_net_rates(destination.economy_profile_id);
            const double destination_rate = destination_rates.contains(commodity_id) ? destination_rates.at(commodity_id) : 0.0;
            if (destination_rate >= 0.0) {
                continue;
            }
            const auto& destination_state = get_station_state(destination.id);
            const auto destination_stock_it = destination_state.inventory.find(commodity_id);
            const double destination_stock = destination_stock_it == destination_state.inventory.end() ? 0.0 : destination_stock_it->second;
            const double target_stock = 8.0 + std::abs(destination_rate) * 14.0;
            const double need = std::max(0.0, target_stock - destination_stock);
            if (need <= 1.0) {
                continue;
            }

            const double cargo_units = std::min({ship_class.cargo_capacity_units, surplus, need});
            if (cargo_units <= 0.0) {
                continue;
            }

            const auto& planner = (ship_class.propulsion_type == "electric_ion" && variable_isp_planner_)
                ? static_cast<trajectory::ITrajectoryPlanner&>(*variable_isp_planner_)
                : static_cast<trajectory::ITrajectoryPlanner&>(*kepler_planner_);
            const auto plan = planner.plan_transfer(origin_def, destination, ship, ship_class, game_time_s_);
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
        for (const auto& destination : universe_.stations) {
            if (destination.id == origin_def.id) {
                continue;
            }
            const auto destination_rates = economy_.get_profile_net_rates(destination.economy_profile_id);
            double destination_score = 0.0;
            for (const auto& [commodity_id, rate] : destination_rates) {
                if (rate <= 0.0) {
                    destination_score += std::abs(rate);
                }
            }
            if (destination_score <= 0.0) {
                continue;
            }
            const auto& planner = (ship_class.propulsion_type == "electric_ion" && variable_isp_planner_)
                ? static_cast<trajectory::ITrajectoryPlanner&>(*variable_isp_planner_)
                : static_cast<trajectory::ITrajectoryPlanner&>(*kepler_planner_);
            const auto plan = planner.plan_transfer(origin_def, destination, ship, ship_class, game_time_s_);
            if (!plan.feasible) {
                continue;
            }
            const double score = destination_score / std::max(1.0, plan.travel_time_s / 86400.0);
            if (score > best_score) {
                best_score = score;
                best_destination = &destination;
                best_commodity.clear();
                best_cargo_units = 0.0;
            }
        }
    }

    if (best_destination == nullptr) {
        if (!has_refuel_reserve && ship.propellant_kg <= 0.0) {
            ship.phase = domain::ShipMissionPhase::Stranded;
            add_event(std::format("{} is stranded at {} due to fuel shortage", ship.name, origin_def.name));
        }
        return;
    }

    const auto& final_planner = (ship_class.propulsion_type == "electric_ion" && variable_isp_planner_)
        ? static_cast<trajectory::ITrajectoryPlanner&>(*variable_isp_planner_)
        : static_cast<trajectory::ITrajectoryPlanner&>(*kepler_planner_);
    auto plan = final_planner.plan_transfer(origin_def, *best_destination, ship, ship_class, game_time_s_);
    if (!plan.feasible) {
        return;
    }

    if (best_cargo_units > 0.0 && !best_commodity.empty()) {
        origin_state.inventory[best_commodity] -= best_cargo_units;
    }
    // Chemical ships: deduct propellant at mission start (instantaneous burns).
    // Electric ion ships: propellant is consumed continuously during transit and
    // tracked per-sample, so we don't deduct here — ship.propellant_kg is updated
    // in step_in_transit_ship from sampled_propellant_kg.
    if (ship_class.propulsion_type != "electric_ion") {
        ship.propellant_kg -= plan.propellant_required_kg;
    }
    ship.phase = plan.wait_time_s > 0.0 ? domain::ShipMissionPhase::AwaitingDeparture : domain::ShipMissionPhase::InTransit;
    ship.active_mission = {
        .origin_station_id = origin_def.id,
        .destination_station_id = best_destination->id,
        .commodity_id = best_commodity,
        .cargo_units = best_cargo_units,
        .departure_time_s = plan.departure_time_s,
        .arrival_time_s = plan.arrival_time_s,
        .wait_time_s = plan.wait_time_s,
        .coast_time_s = plan.coast_time_s,
        .total_travel_time_s = plan.travel_time_s,
        .remaining_travel_time_s = plan.travel_time_s,
        .propellant_cost_kg = plan.propellant_required_kg,
        .sampled_path = plan.sampled_path,
        .sampled_times_s = plan.sampled_times_s,
        .sampled_propellant_kg = plan.sampled_propellant_kg,
    };
    if (plan.wait_time_s > 0.0) {
        add_event(std::format(
            "{} scheduled {} for {} in {:.1f} days",
            ship.name,
            origin_def.name,
            best_destination->name,
            plan.wait_time_s / 86400.0));
    } else if (best_cargo_units > 0.0) {
        add_event(std::format(
            "{} departed {} for {} carrying {:.1f} units of {}",
            ship.name,
            origin_def.name,
            best_destination->name,
            best_cargo_units,
            best_commodity));
    } else {
        add_event(std::format("{} repositioned from {} to {}", ship.name, origin_def.name, best_destination->name));
    }
}

void Simulation::step_awaiting_departure_ship(domain::ShipState& ship) {
    ship.active_mission.remaining_travel_time_s = std::max(0.0, ship.active_mission.arrival_time_s - game_time_s_);
    if (game_time_s_ < ship.active_mission.departure_time_s) {
        return;
    }
    ship.phase = domain::ShipMissionPhase::InTransit;
    const auto& origin = get_station_definition(ship.active_mission.origin_station_id);
    const auto& destination = get_station_definition(ship.active_mission.destination_station_id);
    if (ship.active_mission.cargo_units > 0.0) {
        add_event(std::format(
            "{} departed {} for {} carrying {:.1f} units of {}",
            ship.name,
            origin.name,
            destination.name,
            ship.active_mission.cargo_units,
            ship.active_mission.commodity_id));
    } else {
        add_event(std::format("{} departed {} for {}", ship.name, origin.name, destination.name));
    }
}

void Simulation::step_in_transit_ship(domain::ShipState& ship, double dt_s) {
    (void)dt_s;
    ship.active_mission.remaining_travel_time_s = std::max(0.0, ship.active_mission.arrival_time_s - game_time_s_);

    // Update propellant continuously from the pre-computed mass samples (ion drives only).
    // The samples run from initial propellant at departure to final propellant at arrival,
    // giving a smooth display rather than a step-change at mission assignment.
    if (!ship.active_mission.sampled_propellant_kg.empty()
        && ship.active_mission.sampled_times_s.size() == ship.active_mission.sampled_propellant_kg.size()) {
        ship.propellant_kg = interpolate_timed_scalar(
            ship.active_mission.sampled_propellant_kg,
            ship.active_mission.sampled_times_s,
            game_time_s_);
    }

    if (ship.active_mission.remaining_travel_time_s > 0.0) {
        return;
    }

    ship.current_station_id = ship.active_mission.destination_station_id;
    ship.phase = domain::ShipMissionPhase::Idle;
    auto& destination = get_station_state(ship.current_station_id);
    if (ship.active_mission.cargo_units > 0.0 && !ship.active_mission.commodity_id.empty()) {
        destination.inventory[ship.active_mission.commodity_id] += ship.active_mission.cargo_units;
        add_event(std::format(
            "{} arrived at {} and unloaded {:.1f} units of {}",
            ship.name,
            get_station_definition(ship.current_station_id).name,
            ship.active_mission.cargo_units,
            ship.active_mission.commodity_id));
    } else {
        add_event(std::format("{} arrived at {}", ship.name, get_station_definition(ship.current_station_id).name));
    }
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
            case domain::ShipMissionPhase::AwaitingDeparture:
                step_awaiting_departure_ship(ship);
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
        case domain::ShipMissionPhase::AwaitingDeparture:
            return "awaiting_departure";
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
    if ((ship.phase != domain::ShipMissionPhase::InTransit && ship.phase != domain::ShipMissionPhase::AwaitingDeparture)
        || ship.active_mission.total_travel_time_s <= 0.0) {
        const auto& station = get_station_definition(ship.current_station_id);
        return mechanics_.get_station_position(station, game_time_s_);
    }

    if (ship.phase == domain::ShipMissionPhase::AwaitingDeparture
        && game_time_s_ < ship.active_mission.departure_time_s) {
        const auto& station = get_station_definition(ship.current_station_id);
        return mechanics_.get_station_position(station, game_time_s_);
    }

    if (!ship.active_mission.sampled_path.empty() && !ship.active_mission.sampled_times_s.empty()) {
        return interpolate_timed_sampled_path(ship.active_mission.sampled_path, ship.active_mission.sampled_times_s, game_time_s_);
    }

    const double progress = std::clamp(
        (game_time_s_ - ship.active_mission.departure_time_s) / ship.active_mission.total_travel_time_s,
        0.0,
        1.0);
    if (!ship.active_mission.sampled_path.empty()) {
        return interpolate_sampled_path(ship.active_mission.sampled_path, progress);
    }

    const auto& origin = get_station_definition(ship.active_mission.origin_station_id);
    const auto& destination = get_station_definition(ship.active_mission.destination_station_id);
    const auto origin_position = mechanics_.get_station_position(origin, ship.active_mission.departure_time_s);
    const auto destination_position = mechanics_.get_station_position(destination, ship.active_mission.arrival_time_s);
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
        const auto& ship_class = get_ship_class(ship.class_id);
        const auto position = get_ship_render_position(ship);
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"id\":\"" << json_escape(ship.id) << "\","
               << "\"name\":\"" << json_escape(ship.name) << "\","
               << "\"faction_id\":\"" << json_escape(ship.faction_id) << "\","
               << "\"propulsion_type\":\"" << json_escape(ship_class.propulsion_type) << "\","
               << "\"phase\":\"" << json_escape(mission_phase_name(ship.phase)) << "\","
               << "\"current_station_id\":\"" << json_escape(ship.current_station_id) << "\","
               << "\"propellant_kg\":" << ship.propellant_kg << ","
               << "\"dry_mass_kg\":" << ship_class.dry_mass_kg << ","
               << "\"propellant_capacity_kg\":" << ship_class.propellant_capacity_kg << ","
               << "\"initial_mass_kg\":" << (ship_class.dry_mass_kg + ship_class.propellant_capacity_kg) << ","
               << "\"current_mass_kg\":" << (ship_class.dry_mass_kg + std::max(0.0, ship.propellant_kg)) << ","
               << "\"origin_station_id\":\"" << json_escape(ship.active_mission.origin_station_id) << "\","
               << "\"destination_station_id\":\"" << json_escape(ship.active_mission.destination_station_id) << "\","
               << "\"commodity_id\":\"" << json_escape(ship.active_mission.commodity_id) << "\","
               << "\"cargo_units\":" << ship.active_mission.cargo_units << ","
               << "\"departure_time_s\":" << ship.active_mission.departure_time_s << ","
               << "\"arrival_time_s\":" << ship.active_mission.arrival_time_s << ","
               << "\"wait_time_s\":" << ship.active_mission.wait_time_s << ","
               << "\"coast_time_s\":" << ship.active_mission.coast_time_s << ","
               << "\"total_travel_time_s\":" << ship.active_mission.total_travel_time_s << ","
               << "\"remaining_travel_time_s\":" << ship.active_mission.remaining_travel_time_s << ","
               << "\"x\":" << position.x << ","
               << "\"y\":" << position.y << ","
               << "\"z\":" << position.z;
        if (ship.phase == domain::ShipMissionPhase::InTransit || ship.phase == domain::ShipMissionPhase::AwaitingDeparture) {
            output << ",\"trajectory_path\":[";
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
                const auto& destination_station = get_station_definition(ship.active_mission.destination_station_id);
                const auto& destination_body = get_body_definition(destination_station.parent_body_id);
                const auto destination_body_position = mechanics_.get_body_position(destination_body.id, ship.active_mission.arrival_time_s);
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
        output << "}";
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
