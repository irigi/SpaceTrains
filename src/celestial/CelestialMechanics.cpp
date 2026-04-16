#include "celestial/CelestialMechanics.hpp"

#include <cmath>
#include <stdexcept>

namespace spacetrains::celestial {

namespace {
constexpr double TAU = 6.28318530717958647692;
}

CelestialMechanics::CelestialMechanics(const domain::UniverseDefinition& universe) : universe_(universe) {
    for (const auto& body : universe_.bodies) {
        bodies_by_id_[body.id] = &body;
    }
}

const domain::CelestialBodyDefinition& CelestialMechanics::get_body(const std::string& body_id) const {
    const auto it = bodies_by_id_.find(body_id);
    if (it == bodies_by_id_.end()) {
        throw std::runtime_error("Unknown body id: " + body_id);
    }
    return *it->second;
}

std::string CelestialMechanics::get_root_body_id() const {
    for (const auto& body : universe_.bodies) {
        if (body.orbit.parent_id.empty()) {
            return body.id;
        }
    }
    throw std::runtime_error("Universe has no root body");
}

math::Vec3d CelestialMechanics::get_body_position(const std::string& body_id, double time_s) const {
    const auto& body = get_body(body_id);
    if (body.orbit.parent_id.empty()) {
        return {};
    }

    const auto parent_position = get_body_position(body.orbit.parent_id, time_s);
    const double period = body.orbit.orbital_period_s;
    if (period <= 0.0) {
        return parent_position;
    }

    const double angle = body.orbit.phase_at_epoch_rad + (time_s / period) * TAU;
    const double radius = body.orbit.semi_major_axis_m;
    return parent_position + math::Vec3d {std::cos(angle) * radius, 0.0, std::sin(angle) * radius};
}

math::Vec3d CelestialMechanics::get_station_position(const domain::StationDefinition& station, double time_s) const {
    const auto& body = get_body(station.parent_body_id);
    const auto body_position = get_body_position(body.id, time_s);
    const double orbit_radius = body.radius_m + station.altitude_m;
    return body_position + math::Vec3d {std::cos(station.theta_rad) * orbit_radius, 0.0, std::sin(station.theta_rad) * orbit_radius};
}

double CelestialMechanics::get_heliocentric_radius(const std::string& body_id, double time_s) const {
    return get_body_position(body_id, time_s).length();
}

}  // namespace spacetrains::celestial
