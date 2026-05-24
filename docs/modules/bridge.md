# bridge

## Purpose

`bridge` is the file-backed boundary between the authoritative C++ simulation and the Godot frontend. Godot launches the bridge executable, reads JSON snapshots, and writes a narrow JSON command file for pause/timewarp controls.

## Responsibilities

- Own or reference a running `Simulation`.
- Expose read-only snapshot data for Godot.
- Convert core state into Godot-friendly shapes.
- Accept a narrow set of player/debug commands and route them into the simulation.
- Include active mission `trajectory_path` arrays and destination-body-at-arrival data for awaiting/in-transit ships so Godot renders selected trajectories from authoritative simulation samples.

## Non-responsibilities

- Owning game rules
- Loading static scenario files directly in GDScript
- Presentation layout and widget logic

## Current Public Surface

- `spacetrains_bridge --data-root ... --snapshot-file ... --command-file ... --step-seconds ...`
- Snapshot JSON includes simulation timing, bodies, stations, ships, recent events, and per-ship active mission fields.
- Awaiting/in-transit ships include `trajectory_path: [{t_s,x,y,z}, ...]` and `destination_body_at_arrival`.
- Command JSON currently accepts `paused` and `timewarp_factor`.

The exact transport may change later, but the architectural rule should not: Godot reads through the bridge and does not own simulation state directly.

## Data Flow

```
Godot input -> SimulationBridge commands -> Simulation
Simulation -> bridge snapshot JSON -> Godot rendering/UI
```

## Invariants

- The bridge mirrors the simulation; it does not duplicate gameplay logic.
- Snapshot reads are safe to perform every frame.
- Commands are intentionally narrow during early stages.
- Selected trajectory display comes from `trajectory_path`, not from Godot-derived station endpoints, and destination ghosts come from bridge arrival-body data.

## Deferred Work

- Optional GDExtension implementation
- Selection-detail APIs
- Orbit sample queries and event streaming
- Pause/resume and stepping semantics for the editor/debugger

## Tests

- Godot can initialize and observe a simulation through the bridge.
- Snapshot payloads are stable across frames.
- Timewarp and pause commands affect the simulation as expected.
