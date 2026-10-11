# SpaceTrains

SpaceTrains is a self-playing Solar System logistics simulation with a planned **C++ simulation core** and **Godot 4 presentation layer**. The current repository contains the first foundation slice:

- a data-driven seeded Solar System in `data/`
- an engine-independent C++ core in `src/`
- a headless simulation runner
- a bridge-backed Godot observer frontend in `godot/`

## Current Scope

The implemented gameplay slice is an observer-style sandbox:

- stations produce and consume commodities
- factions own stations and ship fleets
- ships autonomously pick cargo routes
- transfers are planned through a `ITrajectoryPlanner` interface
- the default planner is `KeplerTrajectoryPlanner`

The VariableISP research material in [`VariableISP/`](VariableISP) is **reference-only for now**. The architecture is shaped to support a later `VariableISPPlanner`, but runtime integration is not implemented yet.

## Repository Layout

- `src/` — C++20 domain core
- `data/` — CSV universe data for bodies, stations, ships, commodities, and recipes
- `tests/` — headless test executable
- `godot/` — observer frontend that launches the bridge process
- `VariableISP/` — research code, paper draft, and precomputed atlas

## Build and Run

Configure and build:

```bash
cmake -B build -G Ninja
cmake --build build
```

Run the headless sandbox:

```bash
./build/bin/spacetrains_headless .
```

Useful headless flags: `--days N`, `--verbose`, `--econ-audit` (with the transport-capacity balance),
`--profile` (time per phase and per step), `--step-days D` (call the simulation in steps of D days; the game
itself always ticks every 0.1 day), `--start-day D` (the same economy with the planets elsewhere),
`--save-at DAY FILE` / `--load FILE`, `--snapshot-json FILE` (the final UI snapshot), and the trajectory checks:

```bash
# Audit every mission plan accepted during a normal run
./build/bin/spacetrains_headless . --days 1095 --trajectory-audit 2>/dev/null
# Plan every station pair x fuel level x departure day (every 15 days over 730 days),
# bypassing the economy; dump flagged paths as CSV for plotting
./build/bin/spacetrains_headless . --days 730 --trajectory-sweep 15 --trajectory-dump build/audit/sweep.csv 2>/dev/null
```

Compare economy versions over several starting dates (one run is too noisy to judge a change):

```bash
tools/benchmark.py --binary build/bin/spacetrains_headless --days 730 --starts 0,90,180,270
```

Run the bridge directly for debugging:

```bash
./build/bin/spacetrains_bridge --data-root ./data --snapshot-file /tmp/spacetrains_snapshot.json --command-file /tmp/spacetrains_commands.json
```

Run tests:

```bash
cd build && ctest --output-on-failure
```

Open the Godot preview shell:

```bash
godot4 --editor godot/project.godot
```

Run the Godot observer frontend:

```bash
godot4 --path godot
```

The Godot scene starts `spacetrains_bridge` automatically, reads live JSON snapshots, and writes pause/timewarp and
save/load commands back through a command file. In the game: F5 quick save, F9 quick load, M economy and markets,
F focus, Space pause, `,` `.` timewarp. A scripted screenshot tour for checking the map without sitting at the
screen: `godot4 --path godot -- --shot-tour=/some/dir` (plus `--shot-day=N`, `--shot-saveload`, `--shot-smooth=SECONDS`).

## Near-Term Architecture

- `data_loader/` parses external world data
- `celestial/` provides body and station positions
- `economy/` advances station inventories
- `trajectory/` owns transfer planning interfaces
- `simulation/` coordinates time, missions, fleets, and event history

The full architecture pack now lives in:

- [`docs/architecture_overview.md`](docs/architecture_overview.md)
- [`docs/roadmap.md`](docs/roadmap.md)
- [`docs/modules/`](docs/modules)

See [`docs/IMPLEMENTATION_NOTES.md`](docs/IMPLEMENTATION_NOTES.md) for the short current-status note.
