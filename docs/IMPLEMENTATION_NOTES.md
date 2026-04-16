# Implementation Notes

This file is a short status note. The primary design source of truth is now:

- [`docs/architecture_overview.md`](architecture_overview.md)
- [`docs/roadmap.md`](roadmap.md)
- module specs under [`docs/modules/`](modules)

## Current State

- `src/` contains the first C++20 core slice for loading data, querying celestial positions, stepping economy, dispatching simple trade missions, and exposing snapshots.
- `data/` contains seeded Solar System content for the first observer-sandbox scenario.
- `godot/` is still a preview shell, not the final live bridge-backed frontend.

## Still Missing

- live Godot <-> C++ bridge
- bridge-backed moving ships and UI
- save/load
- richer Kepler ship propagation
- pirates, police, rescue, and combat
- runtime VariableISP integration

## Architectural Commitments Already In Code

- `UniverseDefinition` is the loaded static scenario input.
- `SimulationSnapshot` is the read-only runtime query shape.
- `ITrajectoryPlanner` isolates high-level mission logic from specific planner implementations.
