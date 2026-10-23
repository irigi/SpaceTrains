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
  camera stops outside the focused body; ship and station models (below) drawn ~4,500x real size, a map icon
  stands in when a model is a few pixels; screen-space labels beside icons (bodies, then stations, then ships in
  flight; overlap culling); Sun halo and bloom; sunlight with a gentle falloff, and a weak fill light from the
  camera that reaches only ships and stations (render layer 2), so their night sides stay readable.
- **Ship models** (`ShipModels.gd`, 2026-10-23): built from the design's components with the schematic's layout
  (`ShipLayout.gd`, shared with `ui/ShipSchematic.gd`), so the map shows the same parts in the same order and
  proportions, in metres, at true scale (roadmap step 6): `Main.MODEL_SIZE_EXAGGERATION` (1.0) times real size, so a
  49 m light freighter is 6e-9 world units long next to a 12,742 km Earth. Ships and stations share the factor,
  so their size ratios are real at any setting; the user arg `--model-scale=N` overrides it (testing, or a
  fallback if true scale ever proves awkward). The camera's near plane goes down to 1e-14 world units (~8 cm)
  and the focus zoom limit follows the model's size. The 3D view uses 4x MSAA (`project.godot`): without it the
  stations' sub-pixel windows sparkled as the rings turned.
  - Picking: near its icon the nearest entity wins; up close a ship or station is also hit anywhere within its
    projected radius, and then the smallest model wins (a docked ship before its station).
  - Two levels of detail, each one cached mesh per design (`MeshBuilder.gd` merges the parts into one surface per
    material slot, so a ship is a few draw calls):
    - coarse (mid zoom, ~1,000 triangles);
    - detailed (on-screen radius above 6 px, back below 4 px, so a docked ship stays detailed until its station is
      well away; ~8,500 triangles): truss lattice, domed habitat
      with windows, docking port, antenna and RCS, open cargo bay, tank clusters, the tier 2 modules, shadow
      shield, reactor and bell nozzle, or plasma drive, field coils and radiator wings on booms.
  - Detailed meshes take ~25 ms each, so they are built on worker threads; the coarse model stays until the
    detailed one is ready.
  - Live state per ship:
    - radiators: dark grey panels with a silver root manifold; while thrusting the heat pipes and manifold glow a
      faint dull orange (594 K panels would not glow to the eye, so this is only a hint); worn tips are dark;
    - the thermal view (T, label at the bottom) shows the radiators in false colour (`ThermalPalette.gd`, as in
      the schematic) at their temperature while running, the scale's cold end otherwise;
    - the nozzle glows and a plume shows (close up) while the engine runs;
    - cargo (close up): standard containers on a grid of about 2.4 m cells, up to 4 x 4 across, in rows of about
      6 m. Rows fill from the bow, each from the floor and the middle out, so a part load is a solid block. The
      share of slots per lot is its share of the capacity. Shape by commodity: ISO tanks for water, oxygen, fuel,
      deuterium and helium-3, ore bins for metals and platinum, ribbed boxes for the rest. Light grey, with the
      commodity colour on the doors, stripe, tank shell or bin rim. A cold-hold liner hides them;
    - the empty cargo bay still reads as one: a floor plate with ribs and a latch pair per slot, low sills and
      corner gussets at the ends (no solid bulkheads, which hid the cargo when looking along the ship); the
      truss stops at the bay;
    - the faction band, red when stranded.
  - Far out, the billboard engine glow marks a running engine (`EntityVisuals.update_engine_glow`: thermal ships
    for `engine_burn_s` after departure and before arrival, plasma ships the whole transit, sized by jet power).
  - Until the designs file is read, a stand-in design is drawn.
- **Station models** (`StationModels.gd`): generic, in metres at real size, at the ships' scale.
  - The habitat ring is anchored on the Stanford torus (900 m radius, 65 m tube, 10,000 people): radius grows with
    the square root of the people per ring, at least 250 m (1 g at 2 rpm), so 640-990 m for the stations today.
    Rings turn at the rate that gives 1 g (about 0.1 rad/s, a turn a minute), in real time.
  - Everything else is sized for the ships (40-140 m long; the large liner's radiators span ~200 m) and is the same
    at every station: a spine with a docking hub and four 110 m berthing arms at each end; docked ships sit at the
    arm ends, bow to the hub, queuing outwards 160 m apart when there are more ships than berths.
  - A docked ship keeps its berth while docked; an arriving ship takes the lowest free one. Near a station a ship
    in flight blends between its path (which ends at the station's centre) and its berth, by distance (full within
    1.3 station radii, none beyond 4), so it docks without a jump and passes over the ring, not through it. In
    practice ships arrive at speed (burns are impulsive), ~30 km per frame even at 600x, so this is rarely seen.
  - One habitat ring, two counter-rotating ones from 15,000 people, a tube diameter apart (the Stanford ratio of
    tube to ring, 130 m across a 1.8 km ring, is kept; two rings that nearly touched read as one fat drum).
  - Two levels of detail (station radius above 90 px on screen, back below 60 px):
    - coarse: a plain torus with two window strips, a plain spine, beams for arms and masts;
    - detailed (~0.3 M triangles a ring, ~50 k the core; built on a worker thread in ~0.4 s, coarse until then):
      the ring is cut into sectors (~100 m of rim, 6-10 between spokes) in four variants (spoke junction, two
      home variants, greenhouse), each built once and drawn as a MultiMesh round the ring. Round the tube: the
      faction band at the rim, rows of windows (some lit) or greenhouse glazing either side, shield panels with
      seams top and bottom, Stanford-style sunlight windows on the inner shoulder, service lines inside, radiator
      fins underneath, frames round the tube, airlocks, antennas, equipment and beacons. Spokes with frames and
      elevator rails, a spinning hub collar with windows. The core: lattice spine, pressurized modules with
      frames, windows and cupolas, hubs with windows, antennas and beacons, lattice berthing arms with propellant
      lines and cradles (clamps, floodlights), banded tanks with feed lines, paneled solar arrays and radiators.
  - The real sizes need no altitude cap (stations orbit at least 200 km up); `_station_display_scale` keeps one
    (60% of the altitude) only for large `--model-scale` values.
  - Storage tanks by capacity, solar arrays, radiators.
  - A flavour by economy profile (`economy_profile_id` in the snapshot): greenhouses, fabrication blocks, sphere
    tank farm (also at fuel factories), solar furnace, container stacks, ore bins, shuttle pads, extraction rigs.
- **Paint** (`ModelMaterials.gd`): muted and matte, nothing self-lit except windows, hot nozzles, plumes and
  radiators while a drive runs:
  - white and gold insulation blankets;
  - grey truss and steel;
  - dark engine metal;
  - the faction colour as a band with capped saturation.
  The old models' hull self-emission, which clipped about 30% of their pixels and made them glare near the Sun, is
  gone. A close-up near Mercury and one at Titan measure the same.
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
  the panel area, with their roots clear of the widest part beside them (a wide hold or tank cluster).
  - Live state: tank fill (LH2 frost white, water blue), hold lots by commodity colour, crew dots, and the plume
    while the engine burns or thrusts.
  - Temperatures in false colour (inferno scale, `ThermalPalette.gd`, 200-6000 K, with a legend and a tick at
    the current value): the radiators at `radiator_temperature_k`, and the reactor at its core temperature while
    burning.
  - Below the drawing: a scale bar and a bar of the current mass by part (dry parts, propellant, cargo,
    provisions). Hovering a part or a segment highlights both and shows the spec line. The inspector keeps one
    instance across refreshes, so a hover survives them.
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
- **Keys:** F5 quick save, F9 quick load, M economy, S statistics, F focus, T thermal view, Space pause, `,` `.`
  timewarp, F11 map debug.
- **Dev:** `-- --shot-tour=<dir>` scripted screenshots (`--shot-day=N`, `--shot-saveload`, `--shot-pause`,
  `--shot-smooth=SECONDS` counts stalled frames; the tour shoots the ship schematic for each drive, hovered, and a thermal ship drawn as
  burning, and checks that the hover holds through refreshes and the wheel over the inspector leaves
  the map alone; it shoots each flying hull at mid zoom and close up (plasma ships also in the thermal view,
  pressing T), and station close-ups at multiples of the model size; `--shot-fps` prints the frame rate at each stop;
  `--shot-close` adds a docked ship close up with a picking check and prints how far paths end from their stations;
  `--shot-arrival` watches the next ship dock at 600x); `SPACETRAINS_GODOT_PROFILE=1` prints frame-time shares.
  - Model gallery: `godot4 --path godot -s res://scripts/ModelGallery.gd -- --designs=<snapshot>.designs --out=<dir>
    [--only=id,prefix*] [--stations=<snapshot.json>] [--thermal]`. It renders every base class and one design per
    module, coarse and detailed, from several views (plus the cargo bay close and from the side), with the engine
    running and cargo aboard; plasma ships also in the thermal view; the bulk carriers and the light freighter
    half-full and empty; every station (detailed) whole, a stretch of ring, the core and a docking hub with
    docked ships; and a contact sheet. `spacetrains_headless --snapshot-json <file>` writes both inputs.

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
