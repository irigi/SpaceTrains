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
- Pacing: the simulation advances in fixed ticks (`Simulation::TICK_S`, 0.1 day). Each loop hands it the real time
  since the last loop x timewarp, one tick at a time and for at most 100 ms of work, and carries the rest over (up
  to 4 s): a slow stretch (a fleet review) delays snapshots by at most a tenth of a second and the UI slows down
  smoothly. The opening tick (the whole fleet's first dispatch, ~2-3 s) runs before the clock starts, and its
  result is kept (`--opening-cache DIR`, the UI passes `user://opening_cache`): a later start with the same data
  (fingerprint) and the same bridge build loads it in milliseconds.
- Files (all written aside and renamed, so a reader never sees half a file):
  - `<snapshot>`: the state, written when something changed, at most 10 times a second. Bodies carry their orbital
    elements and stations their altitude/angle (the UI places them itself); ships in flight carry a `path_id`.
  - `<snapshot>.seq`: the snapshot's sequence number; the UI polls it every frame and parses the snapshot only
    when it changes.
  - `<snapshot>.paths` / `.paths.seq`: `{"paths": {ship_id: {path_id, trajectory_path: [{t_s,x,y,z}...],
    destination_body_at_arrival}}}`, written only when some ship's plan changes (paths were 70% of a 600 KB
    snapshot at 90 ships), always before the snapshot that refers to them.
- Snapshot extras for the panels: ship ledger, crew, provisions, cargo lots, planned pickup, route commitment;
  station targets, demand/unmet totals, import/export flows, fuel factory, export market, ledger; an `economy`
  block (demand, unmet, exports, fleet purchases); `bridge.epoch` (changes with every load) and `bridge.status`
  (save/load result).
- Command JSON: `paused`, `timewarp_factor`, and save/load requests (`request` number with `save_path` or
  `load_path`).
- Debug: `SPACETRAINS_BRIDGE_LOG=1` reports loops over 0.3 s; `SPACETRAINS_PROFILE=1` enables the phase timers.

The exact transport may change later, but the architectural rule should not: Godot reads through the bridge and does not own simulation state directly.

## Data Flow

```
Godot input -> command file -> bridge -> Simulation
Simulation -> snapshot + paths files -> Godot rendering/UI
```

## Invariants

- The bridge mirrors the simulation; it does not duplicate gameplay logic.
- Polling the `.seq` files every frame is cheap; snapshots are parsed once each.
- Selected trajectory display comes from `trajectory_path`, not from Godot-derived station endpoints, and destination
  ghosts come from bridge arrival-body data.

## Deferred Work

- Optional GDExtension implementation
- Selection-detail APIs
- Orbit sample queries and event streaming
- Pause/resume and stepping semantics for the editor/debugger

## Tests

- Godot can initialize and observe a simulation through the bridge.
- Snapshot payloads are stable across frames.
- Timewarp and pause commands affect the simulation as expected.
