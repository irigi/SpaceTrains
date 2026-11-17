# simulation

## Purpose

`Simulation` (`src/simulation/Simulation.hpp`) owns the running game: time, stations, ships and their missions,
residents, colonies, treasuries, the money journal and events. It advances in fixed ticks and exposes read-only
snapshots for the bridge, the headless reports and tests. Godot never owns gameplay state.

## Files

State and systems (`docs/plans/simulation_split.md`): `World` holds all of the running state, `query/` the
lookups every system uses, `systems/` the rules as free functions on `World`. `Simulation` owns the `World` and runs
its tick; its public members forward to the systems for the bridge, the apps and the tests.

| File | What |
|---|---|
| `Simulation.{hpp,cpp}` | the owner: `std::unique_ptr<World>`, the public interface (each member forwards to a system or query); the tick on a `World` (`step`, `tick`, `start_at`) with the local economy, population and events |
| `WorldSetup.{hpp,cpp}` | building a `World`: `init_world` (a new game's set-up), `load_universe`, `attach_engines` (thread pool, atlas), `make_world` |
| `CrewEnvironment.cpp` | crew radiation and the particle-event model |
| `MoneyJournal.cpp` | the money journal and its history |
| `SimulationInternal.hpp` | constants and helpers shared by these files (namespace `detail`) |
| `World.hpp` | all of the running state, as plain data; never copied or moved (the engines hold references into it) |
| `query/` | free functions on `World`: lookups and designs, account indices, positions, events and trades, labour leaves (split step 2); ship cost and environment rates (`ShipRates`), money leaves (`Finance`: economy balances, colony state, station profits, faction credit limits), passenger leaves (`Passengers`: the travel switches, fares, value of time) and the clock (`Clock`: `first_tick_of_day`) (step 4) |
| `systems/` | the rules as free functions on `World`: `StationSystems` (the station systems view, display only; step 3); `Market` (reference prices, station prices and orders, emergencies, the daily market among ships), `Contracts` (liners, scheduled stops, rotation contracts), `MissionPlanning` (leg estimates and dispatch, `systems::MissionPass`) (step 5); `ShipOperations` (crew costs, provisions, radiation and particle events, wear and repair, boil-off, propellant; step 6); `Statistics` (the journal's accounts and monthly close, the flow record's stocks, the statistics JSON; step 7); `Patrols` (police cutters: sorties, inspections, the way home; step 8); `Households` (residents, colonies, labour, station incomes) and `Passengers` (travellers, fares and who boards, boarding and landing, rotation tours, reference fares) (step 9); `Failures` (failure clocks, derelicts, salvage races, replacement claims; step 10); `FleetInvestment` (treasuries, credit, ship sales and purchases, the file-local `InvestmentReview`; step 11); `ShipPhases` (a ship's phases: idle, refitting, awaiting departure, in transit, arrival; refits and the trajectory audit records; step 12); `Snapshot` (the snapshot, the bridge's JSON: snapshot, paths, designs; the text report; read only; step 13) |

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
`tests/variable_isp_test.cpp` (the VariableISP solver), `tests/test_world.cpp` (systems called directly on the `World`).
