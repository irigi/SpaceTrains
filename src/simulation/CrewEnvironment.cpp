#include "simulation/CrewEnvironment.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace spacetrains::simulation::crew_environment {

double solar_activity(const domain::ShipEnvironmentDefinition& env, double day) {
    const double phase = 2.0 * std::numbers::pi * (day - env.solar_minimum_day) / env.solar_cycle_days;
    return 0.5 * (1.0 - std::cos(phase));
}

double gcr_msv_per_day(const domain::ShipEnvironmentDefinition& env, double r_au, double day) {
    // Highest at solar minimum: the cycle mean times (1 + A) at minimum, (1 - A) at maximum.
    const double cycle = 1.0 + env.gcr_cycle_amplitude * (1.0 - 2.0 * solar_activity(env, day));
    const double radial = std::max(0.0, 1.0 + env.gcr_gradient_per_au * (r_au - 1.0));
    return env.gcr_msv_per_day * cycle * radial;
}

double shelter_g_cm2(const domain::ShipClassDefinition& design) {
    const auto it = design.effects.find("spe_shield_g_cm2");
    return it == design.effects.end() ? 0.0 : it->second;
}

double gcr_factor(const domain::ShipEnvironmentDefinition& env, double shelter) {
    if (shelter <= 0.0) {
        return 1.0;
    }
    return 1.0 - env.shelter_occupancy * (1.0 - std::exp(-shelter / env.gcr_attenuation_g_cm2));
}

double spe_reference_msv(const domain::ShipEnvironmentDefinition& env, double extra_g_cm2) {
    if (extra_g_cm2 <= 0.0) {
        return env.spe_hull_msv;
    }
    return env.spe_hull_msv * std::pow(env.spe_shelter_msv / env.spe_hull_msv, extra_g_cm2 / env.spe_shelter_g_cm2);
}

double spe_magnitude_per_day(const domain::ShipEnvironmentDefinition& env, double day) {
    // Events per day at this activity (none at minimum, twice the mean at maximum) times the
    // mean magnitude of the truncated power law N(>M) ~ M^-k on [m0, m1].
    const double k = env.spe_index;
    const double m0 = env.spe_min_magnitude;
    const double m1 = env.spe_max_magnitude;
    const double tail = 1.0 - std::pow(m1 / m0, -k);  // share of events below m1
    const double mean = k == 1.0
        ? m0 * std::log(m1 / m0) / tail
        : k * std::pow(m0, k) * (std::pow(m1, 1.0 - k) - std::pow(m0, 1.0 - k)) / ((1.0 - k) * tail);
    return env.spe_rate_per_year / 365.25 * 2.0 * solar_activity(env, day) * mean;
}

double expected_transit_msv_per_day(const domain::ShipEnvironmentDefinition& env,
    const domain::ShipClassDefinition& design, double r1_au, double r2_au, double day) {
    const double shelter = shelter_g_cm2(design);
    const double gcr = gcr_msv_per_day(env, 0.5 * (r1_au + r2_au), day) * gcr_factor(env, shelter);
    const double spe = spe_magnitude_per_day(env, day) * spe_reference_msv(env, shelter)
        / std::max(0.01, r1_au * r2_au);
    return gcr + spe;
}

double expected_docked_msv_per_day(const domain::ShipEnvironmentDefinition& env,
    const domain::StationRadiationDefinition& station, double r_au, double day) {
    // Stations have shelters: a particle event finds the crew behind the reference shelter.
    const double shield = std::exp(-station.shield_g_cm2 / env.gcr_attenuation_g_cm2);
    const double gcr = gcr_msv_per_day(env, r_au, day) * shield;
    const double spe = spe_magnitude_per_day(env, day) * env.spe_shelter_msv / std::max(0.01, r_au * r_au);
    return station.location_factor * (gcr + spe);
}

double draw_spe_magnitude(const domain::ShipEnvironmentDefinition& env, double uniform) {
    // Inverse of the truncated power law's distribution.
    const double k = env.spe_index;
    const double low = 1.0;
    const double high = std::pow(env.spe_max_magnitude / env.spe_min_magnitude, -k);
    const double u = low - uniform * (low - high);
    return env.spe_min_magnitude * std::pow(u, -1.0 / k);
}

}  // namespace spacetrains::simulation::crew_environment
