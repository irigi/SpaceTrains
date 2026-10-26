# SpaceTrains Architecture Overview

## What it is

A self-playing logistics simulation of the Solar System: stations make and use goods, residents earn and spend,
autonomous ships plan physically real transfers (Kepler arcs for nuclear-thermal ships, constant-power variable-Isp
trajectories for plasma ships), fuel and money matter, and factions buy and sell ships. The player watches, with
time controls, inspectors, statistics and save/load. The game is planar by design.

- a **C++20 simulation core**, deterministic and authoritative;
- **Godot 4** (built with double precision) as the observer UI;
- **data files** (`data/`, CSV) for bodies, stations, goods, recipes, ships and every economy setting.

## Layers

```
Godot (godot/)            map, models, panels, statistics, time controls      reads files, writes commands
        │  snapshot + paths + designs + stats files  /  command file
Bridge (src/bridge)       runs the Simulation in real time × timewarp, writes the files, applies commands
        │
Core (src/)               simulation · economy · trajectory · variable_isp · celestial · data_loader ·
                          persistence · domain · util
```

Rules:
- The core is authoritative; Godot never owns gameplay state and the bridge adds no rules.
- Content is data: the Solar System, stations and ships are seeded content, not code.
- Planners sit behind `ITrajectoryPlanner`, chosen by a ship class's `propulsion_type`.
- Determinism: the same data and start give byte-identical results, whatever the thread count, and a saved,
  loaded and continued game equals one that never stopped.

## Modules

| Module | Docs | What |
|---|---|---|
| `domain` | — | types: `Definitions.hpp` (the loaded universe), `State.hpp` (the running game, snapshots), `TrajectoryPlan.hpp`; `ShipDesign` (designs from parts) |
| `data_loader` | `modules/data_loader.md` | CSV → `UniverseDefinition`, with checks |
| `celestial` | `modules/celestial.md` | body and station positions over time |
| `trajectory` | `modules/trajectory.md` | Kepler and VariableISP planners, path sampling, audit, chase solver |
| `variable_isp` | `modules/trajectory.md` | the atlas, the integrator and the general-boundary solver |
| `economy` | `modules/economy.md` | production, price curves, surface markets |
| `simulation` | `modules/simulation.md` | the running game; ships (`modules/ships_and_fleet.md`) and economy rules (`modules/economy.md`) |
| `persistence` | `modules/persistence.md` | save games (JSON) |
| `bridge` | `modules/bridge.md` | the file-backed link to Godot |
| `app` | README | `spacetrains_headless`: runs, reports, audits, benchmarks' engine |
| `util` | — | thread pool, profiling |
| Godot | `modules/ui_observer.md` | the observer UI |

Background: `game_design.md` (the game), `ship_technology.md` (the ships' physics and sources), `roadmap.md`
(where it is going), `plans/` (open concepts and the current work's log).

## A tick

```
Simulation::step(real_dt)                     whole ticks of 0.1 day
  └─ tick()
       ├─ economy: production and consumption (EconomySystem::step); residents pay for what they used
       ├─ households, surface markets, labour, colonies, population, passengers
       ├─ events, particle events
       ├─ treasuries; emergencies; the daily market among ships; station orders
       ├─ fleet investment (sell laid-up ships, review purchases)
       ├─ each ship: operating costs, then by phase: idle (dispatch, refit, repair) · awaiting departure ·
       │  in transit (arrival) · refitting
       └─ the money journal closes a month every 30 days
```

Dispatch plans in parallel (a thread pool computes queued plans; results are combined in a fixed order), and every
cache it uses is a pure function of its key, so a loaded game, which starts with empty caches, plays the same.

## Binaries and tools

- `spacetrains_bridge` (started by Godot), `spacetrains_headless` (runs and reports), tests
  (`spacetrains_tests`, `spacetrains_economy_tests`, `spacetrains_variable_isp_tests`, Python checks),
  `spacetrains_visp_bench`, `spacetrains_atlas_fill`.
- `tools/benchmark.py` (economy over 4 starts), `tools/purchase_returns.py`, `tools/rebalance_production.py`.
- The VariableISP atlas lives in `tests/data/variable_isp/` (the `.bin` is the master).

## Known gaps

- No pirates, police or rescue gameplay yet (the chase solver is ready for it).
- Per-ship variation beyond tanks and modules.
- Deployment: Godot must be built with double precision, and there is no installer yet (roadmap).
