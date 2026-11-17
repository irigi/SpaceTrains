#include "simulation/query/Positions.hpp"

#include <algorithm>

#include "simulation/SimulationInternal.hpp"
#include "simulation/query/Lookups.hpp"

namespace spacetrains::simulation::query {

using namespace detail;

math::Vec3d get_ship_render_position(const World& w, const domain::ShipState& ship) {
    if ((ship.phase != domain::ShipMissionPhase::InTransit && ship.phase != domain::ShipMissionPhase::AwaitingDeparture)
        || ship.active_mission.total_travel_time_s <= 0.0) {
        const auto& station = get_station_definition(w, ship.current_station_id);
        return w.engines.mechanics_.get_station_position(station, w.state.game_time_s_);
    }

    if (ship.phase == domain::ShipMissionPhase::AwaitingDeparture
        && w.state.game_time_s_ < ship.active_mission.departure_time_s) {
        const auto& station = get_station_definition(w, ship.current_station_id);
        return w.engines.mechanics_.get_station_position(station, w.state.game_time_s_);
    }

    if (!ship.active_mission.sampled_path.empty() && !ship.active_mission.sampled_times_s.empty()) {
        return interpolate_timed_sampled_path(ship.active_mission.sampled_path, ship.active_mission.sampled_times_s, w.state.game_time_s_);
    }

    const double progress = std::clamp(
        (w.state.game_time_s_ - ship.active_mission.departure_time_s) / ship.active_mission.total_travel_time_s,
        0.0,
        1.0);
    if (!ship.active_mission.sampled_path.empty()) {
        return interpolate_sampled_path(ship.active_mission.sampled_path, progress);
    }

    const auto& origin = get_station_definition(w, ship.active_mission.origin_station_id);
    const auto& destination = get_station_definition(w, ship.active_mission.destination_station_id);
    const auto origin_position = w.engines.mechanics_.get_station_position(origin, ship.active_mission.departure_time_s);
    const auto destination_position = w.engines.mechanics_.get_station_position(destination, ship.active_mission.arrival_time_s);
    return origin_position * (1.0 - progress) + destination_position * progress;
}

double sun_mu(const World& w) {
    return w.engines.mechanics_.get_body(w.engines.mechanics_.get_root_body_id()).mu_m3_s2;
}

std::string primary_body_id(const World& w, const std::string& station_id) {
    const std::string root = w.engines.mechanics_.get_root_body_id();
    const auto* body = &w.engines.mechanics_.get_body(get_station_definition(w, station_id).parent_body_id);
    while (!body->orbit.parent_id.empty() && body->orbit.parent_id != root) {
        body = &w.engines.mechanics_.get_body(body->orbit.parent_id);
    }
    return body->id;
}

bool local_leg(const World& w, const domain::MissionAssignment& mission) {
    return primary_body_id(w, mission.origin_station_id) == primary_body_id(w, mission.destination_station_id);
}

}  // namespace spacetrains::simulation::query
