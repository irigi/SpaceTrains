# SpaceTrains Roadmap

## Phase 0 — Architecture Baseline

- Write the architecture overview and per-module design docs.
- Align repo-facing docs with the real implementation.
- Treat `docs/` as the primary design source of truth.

## Phase 1 — Core Cleanup and Headless Stability

- Harden `UniverseDefinition`, `SimulationSnapshot`, and planner-facing types.
- Improve data validation and error reporting in `data_loader`.
- Expand tests around economy, mission dispatch, and trajectory feasibility.
- Keep the headless executable as the truth-checking environment.

## Phase 2 — Bridge and Observer Frontend

- Implement `SimulationBridge` between the C++ core and Godot.
- Replace CSV preview rendering with bridge-backed body, station, and ship rendering.
- Add orbit camera controls, time controls, selection, and event log.
- Add debug panels for inventories, fuel, and active missions.

## Phase 3 — Better Orbital and Mission Fidelity

- Move from coarse transfer timing toward richer Kepler state propagation for ships.
- Expose sampled paths and arrival windows for rendering and debugging.
- Add stranded-ship handling, support/refuel behavior, and clearer mission state transitions.

## Phase 4 — Persistence and Scenario Control

- Add save/load snapshots.
- Add seeded scenario setup and reproducible simulation runs.
- Add scenario-level data for alternative starting economies or factions.

## Phase 5 — Advanced Trajectories

- Add a second planner implementation, `VariableISPPlanner`.
- Use `/VariableISP` paper/code/atlas as correctness and regression reference, not as the final runtime dependency.
- Keep higher-level mission logic unchanged while swapping planner implementations.

## Phase 6 — Post-v1 Systems

- Add pirates, police, inspections, rescue, and later combat resolution.
- Expand the content set beyond the initial inner-system logistics sandbox.
- Support custom star systems without changing core gameplay code.
