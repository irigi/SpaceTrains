# ui_observer

## Purpose

`ui_observer` is the Godot presentation layer for watching and inspecting the simulation. It is an observer/debug UI, not a game-rules module.

## Responsibilities

- Camera controls for map viewing.
- Rendering bodies, stations, ships, and route samples.
- Selection panel, event log, and time controls.
- Debug overlays for inventories, fuel, and mission state.

## Non-responsibilities

- Trajectory planning
- Economy stepping
- Persistent gameplay state ownership
- Static data loading as a long-term solution

## Planned UI Surface

- `MapView` for the main scene
- `OrbitCamera` or equivalent mouse-driven camera controller
- `SelectionPanel` for details on the chosen body/station/ship
- `TimeControls` for pause/play/timewarp
- `EventLog` for recent simulation events

## Data Flow

```
SimulationBridge snapshot/query -> UI widgets and 3D nodes -> player observation
```

## Invariants

- UI reads from the bridge and never mutates core data structures directly.
- Losing a UI node must not break the simulation.
- Debug displays may be incomplete, but they must not lie about authoritative state.

## Deferred Work

- Polished map rendering
- Rich orbit visualization
- Filtering and search
- Visual effects and camera modes beyond map-first viewing

## Tests

- Main scene loads without parsing errors.
- Camera control works with mouse input.
- Time controls update what the player sees.
- Selecting an entity shows bridge-backed details.
