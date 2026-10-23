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
  points concentrated at the camera (sinh spacing) in focus-relative coordinates: exact at any zoom. A moon's ring
  fades out when it is only a few pixels across.
- **Map look (KSP-like):** bodies at true radius (smooth spheres, faint self-lit texture for the night side), the
  camera stops outside the focused body; ship and station models a few tens of km, a map icon stands in when a model
  is a few pixels; screen-space labels beside icons (bodies, then stations, then ships in flight; overlap culling);
  Sun halo and bloom; sunlight with a gentle falloff.
- **Panels:** top bar (date, pause, timewarp, fleet summary, unmet demand over 30 days, money); registry (ships,
  stations, bodies); inspector: ship (mission in words, propellant, cargo lots, provisions, transit, expected trip
  earnings, crew, home, route commitment, ledger, and a collapsible ship systems section: engine state and drive
  radiator temperature, a to-scale top-view schematic (`ui/ShipSchematic.gd`, below), and every part with its mass
  and key numbers in a folded parts list), station (badges, market rows with stock vs target, cover and share
  short, orders, inbound and docked ships, money since start), body (orbit, moons, stations); economy and markets
  overview (M); comms log.
- **Ship schematic** (`ui/ShipSchematic.gd`): drawn from the design's components (the bridge's `.designs` file), bow
  on the left, sizes in metres. The habitat is a 4.5 m cylinder of its pressurized volume, with its rooms and systems
  as bands by mass. The hold is a box of its volume, the tanks a cylinder of the propellant volume. Thermal ships
  get a shadow shield, reactor and nozzle; plasma ships a drive, magnetic nozzle and two radiator wings (2.5:1) of
  the panel area.
  - Live state: tank fill (LH2 frost white, water blue), hold lots by commodity colour, crew dots, and the plume
    while the engine burns or thrusts.
  - Temperatures in false colour (inferno scale, `ThermalPalette.gd`, 200-6000 K, with a legend and a tick at
    the current value): the radiators at `radiator_temperature_k`, and the reactor at its core temperature while
    burning.
  - Below the drawing: a scale bar and a bar of the current mass by part (dry parts, propellant, cargo,
    provisions). Hovering a part or a segment highlights both and shows the spec line. The inspector keeps one
    instance across refreshes, so a hover survives them.
- **Map ship hints** (`EntityVisuals.update_engine_glow`):
  - Nuclear-thermal ships glow while burning, for the bridge's `engine_burn_s` after departure and before
    arrival.
  - The glow's size follows the engine's jet power, log-scaled.
  - Plasma ships' radiator wings glow in false colour while thrusting.
- **Wheel:** zooms the map, except over the inspector, browser or market panel, which take it even at the
  end of their scroll.
- **Statistics screen** (S, `ui/StatisticsPanel.gd`, charts `ui/LineChart.gd`): it reads the bridge's
  `.stats` side file (the money journal, docs/modules/economy.md), which the bridge writes only while the screen
  is open (`stats_open` in the command file).
  - **Period switch:** since the start, last year, last 30 days.
  - **Chart switch:** balances, or net flow per month.
  - **Tabs:**
    - Overview: money by sector, who pays whom, sector balances;
    - Factions: treasury, credit, operating and investment results, their stations' and fleets' profit, flows by
      category;
    - Stations: cash, profit before the faction, top income and expense, red days; a station's and its
      residents' flows;
    - Residents: people, workers, open jobs, wage, savings, savings rate;
    - Colonies & Earth: accounts against credit lines, wage p10/p50/p90, hiring up and down, Earth's flows;
    - Fleet: by faction and class, the best and worst ships (click to select one).
  - The station inspector shows its profit before the faction, since the start and over the last year.
- **Keys:** F5 quick save, F9 quick load, M economy, S statistics, F focus, Space pause, `,` `.` timewarp, F11 map debug.
- **Dev:** `-- --shot-tour=<dir>` scripted screenshots (`--shot-day=N`, `--shot-saveload`, `--shot-pause`,
  `--shot-smooth=SECONDS` counts stalled frames; the tour shoots the ship schematic for each drive, hovered, and a thermal ship drawn as
  burning, and checks that the hover holds through refreshes and the wheel over the inspector leaves
  the map alone); `SPACETRAINS_GODOT_PROFILE=1` prints frame-time shares.

## Data Flow

```
SimulationBridge snapshot/query -> UI widgets and 3D nodes -> player observation
```

## Invariants

- UI reads from the bridge and never mutates core data structures directly.
- Losing a UI node must not break the simulation.
- Debug displays may be incomplete, but they must not lie about authoritative state.

## Deferred Work

- Trajectory progress as a shader instead of rebuilt lines
- Filtering and search
- Visual effects and camera modes beyond map-first viewing

## Tests

- Main scene loads without parsing errors.
- Camera control works with mouse input.
- Time controls update what the player sees.
- Selecting an entity shows bridge-backed details.
