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

## Current UI Surface (2026-10-08)

- **Display clock** (`Main.gd:_advance_display_clock`): game time for the picture, advanced every frame at the
  simulation's measured pace (at most the requested timewarp), at most 3 s of real time ahead of the last snapshot,
  braking smoothly near that limit, never stepping back; held while paused; reset when a game is loaded (`bridge.epoch`).
- **Positions per frame:** bodies and stations from their orbital elements (same formula as `CelestialMechanics`),
  ships in flight by interpolating their planned path at the display time, docked ships at their station. All in
  double precision relative to the focused entity (`entity_root`), then cast, so close-ups do not jitter;
  `world_root` holds the absolute-frame lines (orbit rings, planned paths, trails).
- **Orbit rings:** a 360-segment ring per body, and, for rings passing near the focus, a ring redrawn each frame with
  points concentrated at the camera (sinh spacing) in focus-relative coordinates: exact at any zoom.
- **Map look (KSP-like):** bodies at true radius (smooth spheres, faint self-lit texture for the night side), the
  camera stops outside the focused body; ship and station models a few tens of km, a map icon stands in when a model
  is a few pixels; screen-space labels beside icons (bodies, then stations, then ships in flight; overlap culling);
  Sun halo and bloom; sunlight with a gentle falloff.
- **Panels:** top bar (date, pause, timewarp, fleet summary, unmet demand over 30 days, money); registry (ships,
  stations, bodies); inspector: ship (mission in words, propellant, cargo lots, provisions, transit, expected trip
  earnings, crew, home, route commitment, ledger), station (badges, market rows with stock vs target, cover and share
  short, orders, inbound and docked ships, money since start), body (orbit, moons, stations); economy and markets
  overview (M); comms log.
- **Keys:** F5 quick save, F9 quick load, M economy, F focus, Space pause, `,` `.` timewarp, F11 map debug.
- **Dev:** `-- --shot-tour=<dir>` scripted screenshots (`--shot-day=N`, `--shot-saveload`, `--shot-pause`,
  `--shot-smooth=SECONDS` counts stalled frames); `SPACETRAINS_GODOT_PROFILE=1` prints frame-time shares.

## Data Flow

```
SimulationBridge snapshot/query -> UI widgets and 3D nodes -> player observation
```

## Invariants

- UI reads from the bridge and never mutates core data structures directly.
- Losing a UI node must not break the simulation.
- Debug displays may be incomplete, but they must not lie about authoritative state.

## Deferred Work

- Moon orbit rings fading with zoom; trajectory progress as a shader instead of rebuilt lines
- Filtering and search
- Visual effects and camera modes beyond map-first viewing

## Tests

- Main scene loads without parsing errors.
- Camera control works with mouse input.
- Time controls update what the player sees.
- Selecting an entity shows bridge-backed details.
