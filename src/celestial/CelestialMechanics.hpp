#pragma once

#include <string>
#include <unordered_map>

#include "domain/Types.hpp"

namespace spacetrains::celestial {

class CelestialMechanics {
public:
    explicit CelestialMechanics(const domain::UniverseDefinition& universe);

    [[nodiscard]] const domain::CelestialBodyDefinition& get_body(const std::string& body_id) const;
    [[nodiscard]] math::Vec3d get_body_position(const std::string& body_id, double time_s) const;
    [[nodiscard]] math::Vec3d get_station_position(const domain::StationDefinition& station, double time_s) const;
    [[nodiscard]] double get_heliocentric_radius(const std::string& body_id, double time_s) const;
    [[nodiscard]] std::string get_root_body_id() const;

private:
    const domain::UniverseDefinition& universe_;
    std::unordered_map<std::string, const domain::CelestialBodyDefinition*> bodies_by_id_;
};

}  // namespace spacetrains::celestial
