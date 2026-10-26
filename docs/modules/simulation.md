# simulation

## Purpose

`Simulation` (`src/simulation/Simulation.hpp`) owns the running game: time, stations, ships and their missions,
residents, colonies, treasuries, the money journal and events. It advances in fixed ticks and exposes read-only
snapshots for the bridge, the headless reports and tests. Godot never owns gameplay state.

## Files

One class, its members spread over files by topic:

| File | What |
|---|---|
| `Simulation.cpp` | construction, accessors, `step`/`tick`, local economy, population, events |
| `Market.cpp` | reference prices, station prices and orders, emergencies, the daily market among ships |
| `ShipOperations.cpp` | capital and crew costs, provisions, radiation, wear and repair, boil-off, propellant |
| `MissionPlanning.cpp` | leg estimates and dispatch (`Simulation::MissionPass`) |
| `ShipPhases.cpp` | a ship's phases (idle, refitting, awaiting departure, in transit, arrival) and refits |
| `Contracts.cpp` | liners, scheduled stops, rotation contracts |
| `FleetInvestment.cpp` | treasuries, credit, ship sales and purchases (`Simulation::InvestmentReview`) |
| `Households.cpp` | residents, colonies, labour, station incomes |
| `Passengers.cpp` | travellers, boarding, fares, rotation tours |
| `CrewEnvironment.cpp` | crew radiation and the particle-event model |
| `StationSystems.cpp` | the station systems view (display only) |
| `MoneyJournal.cpp`, `Statistics.cpp` | the money journal, its history and the statistics JSON |
| `Snapshot.cpp` | the snapshot, the bridge's JSON (snapshot, paths, designs) and the text report |
| `SimulationInternal.hpp` | constants and helpers shared by these files (namespace `detail`) |

What each topic does: `economy.md` (prices, orders, money) and `ships_and_fleet.md` (ships, dispatch, passengers).

## Time

- `step(real_dt_s)` adds real time × timewarp and runs the whole ticks due; a tick is `TICK_S` (0.1 day) of game
  time whatever the caller's step, so headless runs and the UI simulate the same game.
- `start_at(time_s)` starts a fresh game at another date (planets elsewhere along their orbits): the same economy in
  another geometry, for benchmarks over several starts.
- A tick: economy step, station orders and emergencies, the daily market (on the tick that crosses midnight),
  residents and colonies, treasuries, fleet investment, then each ship by phase.

## Invariants

- **Deterministic:** the same data and start give byte-identical saves, whatever the thread count. Parallel work
  (plans, reference prices) only computes; results are combined in a fixed order on the simulation thread.
- **Save and load are exact:** a game saved, loaded and continued equals one that never stopped (`persistence.md`).
  Caches must therefore be pure functions of their keys (leg estimates, Lambert rows, VariableISP refinements,
  designs), because a loaded game starts with them empty.
- Iterated maps are ordered (`Inventory`, treasuries), so a loaded game iterates, and chooses, as the original did.
- Snapshot queries do not change the state.
- Money is exact: every transfer has a payer and a payee, and the audit's drift is 0.

## Snapshots

`snapshot()` (C++ structs, for the headless reports) and `build_bridge_snapshot_json()` (the bridge's JSON).
Ships in flight carry a `path_id`; their planned paths are in `build_bridge_paths_json()`, written only when some
ship's plan changes (`bridge.md`).

## Tests

`tests/test_main.cpp` (dispatch, planners, save/load, ship designs), `tests/economy_test.cpp` (economy and money),
`tests/variable_isp_test.cpp` (the VariableISP solver).
