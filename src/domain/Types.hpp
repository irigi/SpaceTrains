#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "math/Vec3d.hpp"

namespace spacetrains::domain {

using Inventory = std::unordered_map<std::string, double>;

struct OrbitDefinition {
    std::string parent_id;
    double semi_major_axis_m {0.0};
    double eccentricity {0.0};
    double orbital_period_s {0.0};
    double phase_at_epoch_rad {0.0};
};

struct CelestialBodyDefinition {
    std::string id;
    std::string name;
    double radius_m {0.0};
    double mu_m3_s2 {0.0};
    OrbitDefinition orbit;
};

struct FactionDefinition {
    std::string id;
    std::string name;
};

struct CommodityDefinition {
    std::string id;
    std::string name;
    double mass_per_unit_kg {1.0};
};

struct ShipClassDefinition {
    std::string id;
    std::string name;
    std::string propulsion_type {"chemical"};   // "chemical" or "electric_ion"
    double dry_mass_kg {0.0};
    double propellant_capacity_kg {0.0};
    double cargo_capacity_units {0.0};
    // Chemical propulsion fields:
    double max_delta_v_mps {0.0};
    double cruise_accel_mps2 {0.0};
    // Electric ion propulsion fields:
    double specific_engine_power_w_per_kg {0.0};  // alpha [W/kg_dry]
};

struct StationDefinition {
    std::string id;
    std::string name;
    std::string faction_id;
    std::string parent_body_id;
    double altitude_m {0.0};
    double theta_rad {0.0};
    std::int64_t population {0};
    std::string economy_profile_id;
    Inventory initial_inventory;
};

struct RecipeDefinition {
    std::string profile_id;
    std::string commodity_id;
    double units_per_day {0.0};
};

struct ShipSeedDefinition {
    std::string id;
    std::string name;
    std::string faction_id;
    std::string class_id;
    std::string home_station_id;
    std::string start_station_id;
    double initial_propellant_kg {0.0};
};

struct UniverseDefinition {
    std::vector<CelestialBodyDefinition> bodies;
    std::vector<FactionDefinition> factions;
    std::vector<CommodityDefinition> commodities;
    std::vector<ShipClassDefinition> ship_classes;
    std::vector<StationDefinition> stations;
    std::vector<RecipeDefinition> recipes;
    std::vector<ShipSeedDefinition> ship_seeds;
};

enum class ShipMissionPhase {
    Idle,
    AwaitingDeparture,
    InTransit,
    Refueling,
    Stranded
};

struct EventEntry {
    double time_s {0.0};
    std::string text;
};

struct MissionAssignment {
    std::string origin_station_id;
    std::string destination_station_id;
    std::string commodity_id;
    double cargo_units {0.0};
    double departure_time_s {0.0};
    double arrival_time_s {0.0};
    double wait_time_s {0.0};
    double coast_time_s {0.0};
    double total_travel_time_s {0.0};
    double remaining_travel_time_s {0.0};
    double propellant_cost_kg {0.0};
    std::vector<math::Vec3d> sampled_path;
    std::vector<double> sampled_times_s;
    std::vector<double> sampled_propellant_kg;
};

struct ShipState {
    std::string id;
    std::string name;
    std::string faction_id;
    std::string class_id;
    std::string home_station_id;
    std::string current_station_id;
    ShipMissionPhase phase {ShipMissionPhase::Idle};
    double propellant_kg {0.0};
    MissionAssignment active_mission;
};

struct StationState {
    std::string station_id;
    Inventory inventory;
};

struct SimulationSnapshot {
    double game_time_s {0.0};
    std::vector<StationState> stations;
    std::vector<ShipState> ships;
    std::vector<EventEntry> recent_events;
};

struct TrajectoryPlan {
    bool feasible {false};
    double departure_time_s {0.0};
    double arrival_time_s {0.0};
    double wait_time_s {0.0};
    double coast_time_s {0.0};
    double travel_time_s {0.0};
    double propellant_required_kg {0.0};
    std::vector<math::Vec3d> sampled_path;
    std::vector<double> sampled_times_s;
    std::vector<double> sampled_propellant_kg;
    std::string summary;
};

}  // namespace spacetrains::domain
