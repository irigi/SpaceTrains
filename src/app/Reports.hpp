#pragma once

// Text reports of the headless run (spacetrains_headless): summaries, audits and the trajectory sweep.

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "domain/ShipDesign.hpp"
#include "celestial/CelestialMechanics.hpp"
#include "economy/EconomySystem.hpp"
#include "simulation/Simulation.hpp"
#include "trajectory/TrajectoryAudit.hpp"
#include "trajectory/TrajectoryPlanner.hpp"
#include "trajectory/VariableIspTrajectoryPlanner.hpp"
#include "util/Profiling.hpp"
#include "variable_isp/VariableIsp.hpp"

namespace spacetrains::app {

inline constexpr double kAU = 1.495978707e11;
inline constexpr double kDayS = 86400.0;

void print_celestial_summary(const spacetrains::domain::UniverseDefinition& universe);
void print_ship_class_summary(const spacetrains::domain::UniverseDefinition& universe);
void print_route_diagnosis(
    const spacetrains::domain::UniverseDefinition& universe,
    const spacetrains::celestial::CelestialMechanics& mechanics);
void print_economy_summary(
    const spacetrains::domain::UniverseDefinition& universe,
    const spacetrains::economy::EconomySystem& economy);
void print_station_inventories(
    const spacetrains::domain::SimulationSnapshot& snap,
    const spacetrains::domain::UniverseDefinition& universe);
void print_passenger_audit(const spacetrains::domain::SimulationSnapshot& snap,
    const spacetrains::domain::UniverseDefinition& universe,
    const spacetrains::simulation::Simulation::PassengerAudit& audit,
    const std::map<std::string, spacetrains::simulation::Simulation::RotationTour>& tours);
void print_economy_audit(
    const spacetrains::domain::SimulationSnapshot& snap,
    const spacetrains::domain::UniverseDefinition& universe,
    const spacetrains::economy::EconomySystem& economy,
    double delivered_price_ratio,
    const spacetrains::simulation::Simulation::RadiationAudit& radiation,
    const std::function<double(const spacetrains::domain::StationState&)>& lifetime_profit);
void print_ship_phases(
    const spacetrains::domain::SimulationSnapshot& snap,
    const spacetrains::domain::UniverseDefinition& universe);
void print_patrol_audit(const spacetrains::simulation::Simulation& sim);
void print_failure_audit(const spacetrains::simulation::Simulation& sim);
void print_accounts_audit(const spacetrains::simulation::Simulation& sim);
void print_capacity_balance(const spacetrains::domain::UniverseDefinition& universe,
    const spacetrains::economy::EconomySystem& economy, const spacetrains::domain::SimulationSnapshot& snap);

void print_trajectory_record(const spacetrains::trajectory::TrajectoryAuditRecord& r);
void print_trajectory_audit_summary(const std::vector<spacetrains::trajectory::TrajectoryAuditRecord>& records);
void dump_flagged_trajectories(
    const std::vector<spacetrains::trajectory::TrajectoryAuditRecord>& records,
    const std::string& path);
std::vector<spacetrains::trajectory::TrajectoryAuditRecord> run_trajectory_sweep(
    const std::filesystem::path& data_root,
    const spacetrains::domain::UniverseDefinition& universe,
    const spacetrains::celestial::CelestialMechanics& mechanics,
    int sweep_days,
    int sweep_step_days);

}  // namespace spacetrains::app
