# Clean-up review log (roadmap step 8, 2026-10-26)

Branch `feature/cleanup_review`, from `main` after PR #23. Rule (user): restructure with **identical results**.
A fix that changes results gets its own commit, a benchmark before and after, and a line here.

## Baseline

- Runs are deterministic: two 365-day runs give byte-identical saves, and save at day 60 → load → continue gives
  the same day-365 (and day-730) save as a straight run.
- Check for every refactor commit: build, 730-day run with `--econ-audit`, full save JSON at day 60 and 730, and
  the save/load continuation, all byte-compared with the baseline (plus ctest and the Python tests).
- Benchmark, 4 starts × 1,460 days: unmet 0.2 ± 0.2%, last fifth 0.0%, profitable ships 87 ± 4 of 131 ± 4,
  boarded 4,472 ± 335, 152 ± 8 s per run.
- Build: 10 `-Wmissing-field-initializers` warnings (StationSystems, Passengers, DataLoader, Simulation).

## Findings

Sources: `/code-review high` over each area (unverified candidates, checked here before acting).
Kinds: **bug** (a fix may change results), **latent** (cannot trigger with today's data, but a data change would
set it off; the fix changes nothing today), **robust** (crashes or corrupts on bad input or interruption; no
result change), **perf** (same results, faster), **refactor** (same results, cleaner).

### Simulation (`src/simulation`)

| # | Kind | Where | Finding |
|---|---|---|---|
| S1 | robust | `Simulation.cpp` `station_r_au` | Mutable radius cache written from worker threads during `--write-reference-prices/-fares` (data race). |
| S2 | bug (stats) | `Households.cpp` `station_lifetime_profit` | Starting stock is re-valued from today's (population-scaled) rates, so reported lifetime profit drifts; also slow (twice per station per snapshot). |
| S3 | bug | `Simulation.cpp` `follow_ups_from` | Follow-up legs cached by destination only, though they depend on the plan (arrival time, fuel left) and the carried mass; lighter loads reuse the full-load follow-ups. |
| S4 | bug | `Simulation.cpp` `contracted_arrivals`, `sustained_profit_per_day` | Inbound decay ignores the carrier's cold hold (arrival and contract pricing use it). |
| S5 | latent | `Simulation.cpp` `step_emergencies` | `order_stats.at()` on a good that became net-consumed after the start throws in `tick()`. |
| S6 | latent | `Simulation.cpp` `ship_design` / `register_design` | Damaged-radiator key rule differs between the two (non-variable-Isp hull with a radiator → "Unregistered ship design"). |
| S7 | latent | `update_market_prices` | Daily gate `fmod(t, day) < TICK/2` never fires for a start off the 0.1-day grid. |
| S8 | robust | `Statistics.cpp` | Wages etc. printed raw, so a NaN/inf makes the stats JSON invalid. |
| S9 | perf | `estimate_leg`, `plan_to` | Full planning options built before the cache lookup on every (mostly cached) call. |
| S10 | refactor | 5 files | JSON escaping written 5 times (only one escapes `\n\r\t`); shared constants duplicated (`FUEL_UNITS_TO_KG`, reserve fraction, `IMPORT_COST_DAYS`, ...). |

### Economy, trajectory, VariableISP

| # | Kind | Where | Finding |
|---|---|---|---|
| E1 | bug (chases only) | `GeneralTransfer.cpp` `solve_min_time` | Can return a transfer shorter than its lower bound `T_lo`. Not used by the game yet (step 7). |
| E2 | robust | `VariableIsp.cpp` `load_binary` | Atlas axis counts not checked (0/1 entries → out-of-range in `lower_grid_idx`; huge counts → bad_alloc). |
| E3 | latent | `EconomySystem.cpp` `step` | A legacy `export_markets.csv` row would zero surface-market goods unpaid. |
| E4 | bug (display) | `EconomySystem.cpp` `step` | `runs[output]` creates phantom `output_factor`/`limited_by` entries for goods the station does not make. |
| E5 | bug (save) | `EconomySystem.cpp` | `inventory["machinery"]` inserts a zero entry at every station. |
| E6 | perf | `get_target_stock` | Copies the net-rates map on every price query. |
| E7 | perf | `TrajectoryPlanner.cpp` planet-system phase scan | 601-wait scan repeated per call, not cached. |
| E8 | perf | `EconomySystem::step` | Linear definition lookup per station (O(N²) per tick). |
| E9 | refactor | `VariableIspTrajectoryPlanner.cpp` | Copies of `heliocentric_orbital_rate_rad_s`, `positive_mod`, `lower_grid_idx`, `kappa_scale_factor`. |
| E10 | refactor | `curve_centre` | Repeats `surface_supplied()` inline, two lookups. |

### Persistence, data loader, bridge, app, util

| # | Kind | Where | Finding |
|---|---|---|---|
| P1 | bug | `bridge/main.cpp` opening cache, `data_fingerprint()` | Fingerprint leaves out many data files (ships seeds, orbits, market, reference prices, ...), so a stale cached opening loads after editing them. |
| P2 | robust | `load_state_json` | Mutates the simulation before the whole file is read; a bad save leaves a half-loaded state. |
| P3 | bug | bridge command file | Godot writes the file in place; a half-written file uses up the request number and the save/load is silently dropped. |
| P4 | robust | bridge quicksave, `--save-at` | Written in place (no temp + rename); a kill mid-save loses the old save. |
| P5 | robust | `ThreadPool::parallel_for` | Exceptions: terminate on a worker; on the caller it returns while workers still run (use-after-free). |
| P6 | bug | bridge main loop | Never notices that Godot is gone; an orphaned bridge pegs the CPU (the KDE freeze). Also matters for the Windows deployment (step 9). |
| P7 | perf | `app/main.cpp` loop | Calls `snapshot()` every simulated day only for the game time. |
| P8 | robust | `DataLoader.cpp` station_radiation.csv | Station ids not checked, duplicates overwrite silently. |
| P9 | robust | `DataLoader.cpp` households.csv | Colony essentials / stock-day goods not checked. |
| P10 | robust | `ThreadPool.cpp` | Negative `SPACETRAINS_THREADS` wraps to a huge thread count. |

### Godot scripts

| # | Kind | Where | Finding |
|---|---|---|---|
| G1 | bug | `Main.gd` `_update_fine_paths` | An inactive ship whose fine-path node is null has its history trail hidden every frame. |
| G2 | bug | `Main.gd` `_reset_timeline` | Clears the trail history but leaves the trail meshes visible after a quickload. |
| G3 | bug | Statistics, EntityBrowser, Inspector controls | Default focus mode: after a click, Space (pause) and the letter shortcuts go to the control. TopBar/HelpPanel already use `FOCUS_NONE`. |
| G4 | robust | `Main.gd` `_exit_tree` | Leaves the `.stats`, `.stats.tmp`, `.stats.seq(.tmp)` side files behind; they pile up per session. |
| G5 | bug | `InspectorPanel.gd`, `StatisticsPanel.gd` | Rebuild all controls every refresh (0.5 s / 1 s), so clicks are lost (button freed between press and release) and hovers flicker. |
| G6 | bug | `Main.gd` paths vs snapshot | A snapshot naming a path id not yet in the read paths file loses the ship's path until the next snapshot. |
| G7 | perf | `ShipModels.gd` `_append_cargo` | `surface_get_arrays` per container (GPU readback), a hitch on every cargo change at close-up. |
| G8 | perf | `MarketPanel.gd` | Whole grid freed and rebuilt every 0.5 s. |
| G9 | bug | `Main.gd` `_update_faction_colors` | Runs after the first upsert, so stations keep the hardcoded colours, not the data's. |
| G10 | perf | `Main.gd` ship positions | `_path_sample` evaluated 2-3 times per ship per frame. |

## Architecture notes (by reading)

- `Simulation` is one class (~60 public functions, ~830-line header) whose members are spread over
  `Simulation.cpp` (6,549 lines) and five topic files (`Households`, `Passengers`, `Statistics`, `StationSystems`,
  `CrewEnvironment`). The split continues that pattern: one file per topic, same class. Moving topics into their own
  classes is a later, larger change and is not part of this step.
- `choose_mission_pass` is ~1,060 lines; it is split into named steps inside the same file.
- `domain/Types.hpp` (1,088 lines) is included by everything, so any change rebuilds the whole project.
- The bridge protocol is files on disk (snapshot, paths, designs, stats, command); the review's P3, P4, P6, G4 and
  G6 are all about that protocol. It is the same seam the deployment step (9) has to make robust on Windows.

## Status (2026-10-26, end of steps 1-4)

Every commit below kept results byte-identical (saves at day 60 and 730, save/load continuation, econ audit,
bridge snapshot, paths, designs and stats JSON, reference prices and fares), unless marked.

**Restructured:** `Simulation.cpp` split by topic (Market, ShipOperations, MissionPlanning, ShipPhases, Contracts,
FleetInvestment, Snapshot; shared helpers in `SimulationInternal.hpp`); dispatch as `Simulation::MissionPass`
and fleet investment as `Simulation::InvestmentReview` (named steps instead of 1,060- and 680-line lambdas); data
loader one function per file (`Csv.cpp`, `LoadShips/LoadEconomy/LoadPassengers.cpp`); headless reports in
`app/Reports.cpp` and `app/TrajectoryReports.cpp`; `domain/Types.hpp` split into Definitions, State and
TrajectoryPlan; shared orbit helpers (`trajectory/OrbitMath.hpp`); `Main.gd` lost its map icons (`MapIcons.gd`)
and economy history (`EconomyHistory.gd`).

**Fixed (no result change):** S1, S5, S6, S7, S8, S10, E2, E3, E6, E9, P1-P10, G1-G10. Plus: zero build warnings
on every target (warnings now also on the executables and tests), a 365-day run 14% faster (S9, P7).

**Not done, on purpose:** E8 (a linear search over ~20 stations per tick: too small to matter); E10 (cosmetic);
`Main.gd` kept its rails, trails and bridge I/O (tightly bound to its state; further splits would be risk for
little gain).

**Fixes that change results** (user: make all of them, 2026-10-26), 4 starts × 1,460 days:

| | Baseline | + S3 follow-ups | + S4 cold holds |
|---|---|---|---|
| Unmet | 0.2 ± 0.2% | 0.3 ± 0.4% | 0.3 ± 0.4% |
| Last fifth | 0.0% | 0.0% | 0.0% |
| Price ÷ reference | 1.8 | 1.8 | 1.9 |
| Profitable ships | 87 of 131 | 92 of 132 | 88.5 of 131 |
| Money % | 55 | 49 | 53 |
| Emergencies | 0.0 | 0.5 | 0.8 |
| Boarded | 4,472 | 4,679 | 4,793 |
| Run, s | 152 | 198 | 198 |

- **S3** keeps supply, and more ships pay (+5), but costs ~30% run time over 4 years (+4% over 2 years; the slowest
  20-day stretch over 730 days 6.9 → 6.2 days/s, still above 5). The extra follow-up plans are the price of
  scoring each load on its own arrival.
- **S4** is neutral within noise.
- **E1** (min-time lower bound), **E4/E5** (no phantom or zero entries) leave the game identical over 730 days;
  **S2** changes only the reported station profit and its saved samples.
- Not done: **E7** (the phase scan is ~0.1 ms of the Kepler plan; not worth a cache).
