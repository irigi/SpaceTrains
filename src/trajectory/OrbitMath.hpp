#pragma once

// Small orbit helpers shared by the Kepler and VariableISP planners.

#include <cmath>
#include <string>
#include <unordered_map>

#include "domain/Definitions.hpp"

namespace spacetrains::trajectory {

inline constexpr double TAU = 6.28318530717958647692;

// Modulo that always returns a non-negative value (0 for a non-positive modulus).
inline double positive_mod(double value, double modulus) {
    if (modulus <= 0.0) {
        return 0.0;
    }
    value = std::fmod(value, modulus);
    if (value < 0.0) {
        value += modulus;
    }
    return value;
}

inline double orbital_rate_rad_s(const domain::CelestialBodyDefinition& body) {
    if (body.orbit.orbital_period_s <= 0.0) {
        return 0.0;
    }
    return TAU / body.orbit.orbital_period_s;
}

// The rate at which a body's heliocentric angle changes, for Hohmann phasing: for a moon,
// its parent planet's rate, not the moon's own (shorter) rate around the planet.
inline double heliocentric_orbital_rate_rad_s(
    const domain::CelestialBodyDefinition& body,
    const std::string& root_id,
    const std::unordered_map<std::string, const domain::CelestialBodyDefinition*>& bodies_by_id) {
    const domain::CelestialBodyDefinition* current = &body;
    while (!current->orbit.parent_id.empty() && current->orbit.parent_id != root_id) {
        const auto it = bodies_by_id.find(current->orbit.parent_id);
        if (it == bodies_by_id.end()) {
            break;
        }
        current = it->second;
    }
    return orbital_rate_rad_s(*current);
}

}  // namespace spacetrains::trajectory
