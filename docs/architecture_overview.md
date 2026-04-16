# SpaceTrains Architecture Overview

## Project Intent

SpaceTrains is a self-playing logistics simulation set in the Solar System, with a planned path toward custom star systems, richer faction behavior, realistic fuel-constrained ship operations, patched-conics travel, and later VariableISP transfer planning. The project uses:

- a **C++20 simulation core** for deterministic gameplay state and orbital/economic logic
- **Godot 4** as the presentation, input, and observer UI layer
- external **data files** for Solar System content, stations, factions, ship classes, and economy definitions

The first playable target is an **observer sandbox**: the player watches stations produce and consume goods, ships autonomously plan missions, fuel levels matter, and the UI exposes time controls, selection, and debugging tools.

---

## Layered Architecture

```
┌────────────────────────────────────────────────────────────────────┐
│                          GODOT LAYER                              │
│  map view │ camera │ selection UI │ event log │ time controls     │
│  preview shell now, authoritative bridge later                    │
└──────────────────────────────┬─────────────────────────────────────┘
                               │ reads snapshots, sends commands
                               ▼
┌────────────────────────────────────────────────────────────────────┐
│                        BRIDGE / QUERY LAYER                       │
│  SimulationBridge │ read-only queries │ command surface           │
│  converts C++ state to Godot-friendly data                        │
└──────────────────────────────┬─────────────────────────────────────┘
                               │ owns / drives
                               ▼
┌────────────────────────────────────────────────────────────────────┐
│                        SIMULATION CORE (C++)                      │
│  simulation │ trajectory │ celestial │ economy │ data_loader      │
│  deterministic state, ticking, missions, fuel, inventories        │
└────────────────────────────────────────────────────────────────────┘
```

### Design rules

- The simulation core is authoritative. Godot does not own gameplay state.
- Content is data-driven from the start. The Solar System is seeded content, not hardcoded behavior.
- Trajectory planning is abstracted behind interfaces so Kepler and VariableISP planners can coexist later.
- The initial game is mission-level and economy-level, not local rigid-body ship control.

---

## Core Modules

### `data_loader`
- Loads bodies, factions, commodities, ship classes, stations, recipes, and ship seeds from external files.
- Produces `UniverseDefinition`, the static input for a simulation run.

### `celestial`
- Stores body lookup and simple orbital propagation for bodies and station anchors.
- Provides spatial queries required by planners and renderers.

### `trajectory`
- Defines the stable planner interface.
- Version 1 uses `KeplerTrajectoryPlanner`.
- Future versions add `VariableISPPlanner` and correctness comparison against `/VariableISP`.

### `economy`
- Applies production and consumption rules to station inventories over time.
- Supplies the simulation with shortage/surplus information used for mission generation.

### `simulation`
- Owns the runtime world state: current time, station inventories, ship state, active missions, and event history.
- Advances the simulation, dispatches missions, applies refueling, and exposes read-only snapshots.

### `bridge`
- Not fully implemented yet.
- Will expose body/station/ship/event/time queries and accept a narrow command set from Godot.

### `ui_observer`
- Godot-side presentation layer for camera, map rendering, selection, time controls, and debug overlays.
- Must remain presentation-only.

---

## Key Runtime Types

### `UniverseDefinition`
- Static loaded content for a scenario.
- Contains bodies, stations, factions, ship classes, commodities, recipes, and initial ship seeds.

### `SimulationSnapshot`
- Read-only runtime state for presentation and debugging.
- Contains sim time, station states, ship states, and recent events.

### `ITrajectoryPlanner`
- Stable interface for ship transfer planning.
- Must return enough information for feasibility checks, fuel budgeting, ETA, and rendering samples.

### `MissionAssignment`
- Runtime mission contract between the simulation and a ship.
- Contains origin, destination, cargo, remaining travel time, and fuel cost.

---

## Data Flow

### Startup

```
external data files
    ↓
data_loader.load_universe()
    ↓
UniverseDefinition
    ↓
Simulation.initialize(...)
```

### Simulation tick

```
Simulation.step(dt)
    ├── economy.step(stations, dt)
    ├── refuel idle ships when possible
    ├── generate/scored trade opportunities
    ├── trajectory.plan_transfer(...)
    ├── start or advance missions
    └── append event history
```

### Presentation flow

```
Godot UI
    ↓
SimulationBridge queries snapshot / derived render data
    ↓
map rendering, labels, selection panel, event log, time controls
```

---

## Planned Evolution

### Near term
- Replace the Godot CSV preview shell with a real bridge-backed scene.
- Add time progression, camera controls, moving ships, selection, and event log.
- Expand the simulation from station-level mission timing to rendered ship motion.

### Medium term
- Improve the Kepler planner to use richer orbital state and sampled paths.
- Add save/load, deterministic scenario seeding, and stronger debugging surfaces.
- Introduce rescue and support mission states before combat.

### Long term
- Add `VariableISPPlanner` using the `/VariableISP` paper/code/atlas as reference material and verification targets.
- Potentially rewrite more of the numerical integration layer in C++ while keeping the planner interface stable.
- Support custom star systems through data-only content changes.

---

## Current Gaps

- No live Godot-to-C++ bridge yet
- No save/load yet
- No rendered moving ships in Godot yet
- No pirates, police, combat, or rescue gameplay yet
- No runtime VariableISP integration yet

These are expected gaps, not architectural contradictions.
