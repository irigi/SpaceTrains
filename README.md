# SpaceTrains

SpaceTrains is a self-playing logistics simulation of the Solar System: a deterministic **C++20 simulation core**
and a **Godot 4** observer UI (built with double precision). Stations make and use goods, residents earn and spend,
autonomous ships plan physically real transfers and trade, factions buy and sell ships, and the player watches with
time controls, inspectors, statistics and save/load.

- Nuclear-thermal ships plan with `KeplerTrajectoryPlanner` (Lambert arcs with launch windows).
- Plasma (variable-Isp) ships plan with `VariableIspTrajectoryPlanner` from a precomputed atlas of optimal
  transfers, `tests/data/variable_isp/variable_isp_atlas.bin` (the master copy), refined by the C++ integrator
  (`src/variable_isp/`) and extended with `tools/atlas_fill`. `VariableISP/` holds the papers behind the method.
- The economy (orders, competitive asks, contracts, households, colonies, the money journal) and the ships
  (operating costs, dispatch, modules, radiation, wear, passengers) are described in `docs/modules/`.

## Repository Layout

- `src/` — C++20 core (simulation, economy, trajectory, variable_isp, celestial, data_loader, persistence, bridge,
  headless app)
- `data/` — CSV data: bodies, stations, goods, recipes, ship classes (built by `data/ship_classes/build_ship_classes.py`),
  economy settings
- `tests/` — test executables, Python checks and the VariableISP atlas
- `tools/` — benchmark, purchase check, production rebalance, atlas fill, solver bench
- `godot/` — observer frontend that launches the bridge process
- `VariableISP/` — papers on the variable-Isp method (LaTeX drafts)

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

Solve VariableISP atlas cells with the C++ solver (the game's atlas adds 14 low-κ levels; ~8 minutes):

```bash
./build/bin/spacetrains_atlas_fill --input <old atlas.bin> --extend 14 --below-kappa 0.1 --output new.bin
```

Check whether ships earn the return their purchase valuation promised (by ship group):

```bash
tools/purchase_returns.py --days 1460 --starts 0,90 [--ships]
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
save/load commands back through a command file. In the game, press H (or the KEYS button) for the list of key
bindings; the list lives in `godot/scripts/ui/KeyBindings.gd`, and `tests/test_key_bindings.py` fails when a script
handles a key or mouse button the list does not describe. A scripted screenshot tour for checking the map without sitting at the
screen: `godot4 --path godot -- --shot-tour=/some/dir` (plus `--shot-day=N`, `--shot-saveload`, `--shot-smooth=SECONDS`).

## Documentation

- [`docs/architecture_overview.md`](docs/architecture_overview.md): layers, modules, a tick
- [`docs/modules/`](docs/modules): each module's design as it stands
- [`docs/roadmap.md`](docs/roadmap.md): where it is going ("Current Plan" first)
- [`docs/game_design.md`](docs/game_design.md), [`docs/ship_technology.md`](docs/ship_technology.md)
