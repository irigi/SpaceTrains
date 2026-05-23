# SpaceTrains Roadmap

## Phase 1 — VariableISP Integration and Headless Verification (current)

- Add electric ion ship classes with tech-level parameters (α, ε).
- Implement `VariableIspTrajectoryPlanner` backed by the precomputed trajectory atlas.
- Wire both Kepler and VariableISP planners into the simulation; dispatch by ship propulsion type.
- Enhance the headless executable with rich debug output: celestial mechanics summary, ship class kappa values, economy rates, per-event logs, and periodic inventory reports.
- Validate end-to-end: electric ships plan and complete interplanetary missions with physically plausible transfer times; launch windows cause appropriate waiting; economy stays live.

## Phase 2 — Godot Observer Frontend

- Wire bridge snapshot to render both chemical and ion ship trajectories.
- Extend rendering: bodies, stations, ships, trajectory path lines, destination ghost at arrival.
- Harden orbit camera, time controls, selection panel, and event log.
- Add debug panels for inventories, fuel, and active mission details.

## Phase 3 — Persistence and Scenario Control

- Add save/load snapshots.
- Add seeded scenario setup and reproducible simulation runs.
- Add scenario-level data for alternative starting economies or factions.

## Phase 4 — Richer Orbital and Mission Fidelity

- Improve Kepler planner with patched-conic SOI handoffs.
- Replace local direct-transfer approximation with proper planet-SOI arc.
- Add stranded-ship handling, support/refuel behavior, clearer mission state transitions.
- Consider outer Solar System bodies (Jupiter and beyond).

## Phase 5 — Ships with Individual Variation

- Add per-ship power multiplier (±α variation) and size multiplier.
- Size scaling preserves κ; power multiplier changes κ slightly per ship.
- Reflect individual ship specs in UI selection panel.

## Phase 6 — Post-v1 Systems

- Add pirates, police, inspections, rescue, and later combat resolution.
- Expand content: outer-system stations, more factions, science mission chains.
- Support custom star systems through data-only content changes.
