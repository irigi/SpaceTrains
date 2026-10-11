# SpaceTrains: speed, GUI, save/load, unmet demand and station orders (plan, 2026-10-07)

## Context

The user ran the game with the GUI. The 30 s pause at start-up is tolerable, but **the game stalls briefly
about every 3 s (≈0.3 Hz) at base speed**, which makes it unplayable. The user's priorities, in order:

1. **Speed first**, so the game runs smoothly and every later experiment (730-day benchmarks take 11 min) is faster.
2. **GUI**: fix orbit rings that show as polygons, with planets off the ring, when zoomed in. A map that feels like
   Kerbal Space Program's: icons when far, textured spheres at true size when near. A brighter and more prominent
   Sun, brighter planets. Richer station and ship details: cargo, fuel, crew, mission in words.
3. **Save/load**.
4. **The unmet-demand investigation** (≈72% of consumption unmet since v26, although production is 8–10× consumption),
   then **station orders with mixed cargo** (the user's "clever contracts"), with the fleet held to about 100 ships
   or fewer.
5. Fix economy bugs found along the way.

Once approved, this plan is copied to `docs/plans/speed_gui_orders.md`, and the roadmap's "Current Plan" points to it
(steps 9–13). Every step keeps the usual checks: the three test suites, the 730-day `--econ-audit`, the
`--trajectory-audit`, money drift 0, a commit on `feature/opus5_5_return`, no push. A new benchmark baseline (v34)
starts at step 9b, because the simulation tick changes.

## What the code shows now (findings behind the plan)

- **All simulation runs in C++.** Godot only renders, but it is wasteful. `Main.gd:_read_snapshot()` opens and
  parses the whole snapshot (120–180 KB of JSON: every ship's full trajectory path, all definitions) on **every
  frame**, before it checks whether `snapshot_seq` changed. It also rebuilds the orbit rings and trails per snapshot.
- **The likely cause of the 0.3 Hz hitch.** Docked ships review missions every `IDLE_REVIEW_S` = 6 game hours
  (`Simulation.cpp:45`). They all started at t = 0, so they review **in the same tick**. The bridge steps 0.1 day per
  0.1 s, so 6 game hours is 2.5 real seconds, i.e. 0.4 Hz. Each review is `choose_mission()`, which runs many
  `plan_transfer` calls, and VariableISP ones refine by Newton shooting. The bridge is single-threaded
  (`src/bridge/main.cpp`), so a slow tick delays the snapshot. Godot's `snapshot_blend` then clamps at 1, and
  everything freezes and jumps.
- **The headless run and the GUI simulate different games.** Headless calls `step(1.0)` with timewarp 1 day
  (`app/main.cpp:865`); the bridge steps 0.1 day. Reviews, refits and stock forecasts therefore run at different
  rates. The benchmarks do not measure the game the player sees, and the GUI plans up to 4× more often per game day.
- **VariableISP integration uses `long double`** (`StateWide` in `VariableIsp.cpp`), i.e. x87 80-bit, which cannot
  be vectorised. Its Newton shooting builds a finite-difference Jacobian each iteration, and each column is one more
  integration.
- **Plans are cached only within one `choose_mission` call** (`plans` map, `Simulation.cpp:~777`). `estimate_leg()`
  caches follow-up legs across ships in 5-day buckets, but it computes each entry at
  `max(game_time, bucket start)`, so results depend on *when* the entry was first computed. That is a hidden
  dependence on history, and it matters for save/load.
- **No randomness anywhere** (no RNG in `src/`), so runs are deterministic, which save/load tests can use.
- **Orbits are circular and coplanar** (`CelestialMechanics::get_body_position`). Godot can compute every body's
  position exactly from `bodies.csv` and the game time, without interpolating snapshots.
- **Orbit rings are a fixed-segment `LINE_STRIP` unit circle** (`SpaceEnvironment.gd:_make_ring_mesh`) scaled to
  the snapshot radius, and planets are lerped linearly between snapshots (a chord inside the circle). Both explain the
  polygon and the mismatch at high zoom. Moon rings are centred on the parent's snapshot position, not its displayed
  one.
- **A ship carries one commodity per trip** (`MissionAssignment::commodity_id`, `cargo_units`). Prices already fall
  along a curve as units are delivered (`EconomySystem::get_trade_value`, the exact integral of
  `(target/stock)^e`), and inbound cargo of the same commodity is already counted (`sale_value_on_arrival`). So the
  falling price exists, but **a hold cannot be split across goods**. Once a big hold has pushed one good's price down,
  the next-best good cannot ride along. This is a prime suspect for unmet demand.
- **Hold capacity is in units for every commodity** (`cargo_capacity_units`), although unit masses range from 10 kg
  (platinum) to 500 kg (reactor fuel). Mass already enters the rocket equation.
- **`CMakeLists.txt` sets no default build type.** The local `build/` is Release, but a fresh `./run` builds without
  optimisation.

## Step 9: speed (measure, then remove the hitch, then cut planning cost)

**9a. Measure first.**
- Add `--profile` to `spacetrains_headless`: wall time per phase, summed over the run. Phases: economy step,
  `choose_mission`, `consider_refit`, fleet investment review, Kepler `plan_transfer`, VariableISP `plan_transfer`,
  split into atlas query, `refine_seed` (iterations and integrations) and path generation, and snapshot build.
  Also counts of plans and cache hits.
- In the bridge: log ticks over 50 ms with their phase breakdown (stderr, or env `SPACETRAINS_PROFILE=1`).
- Run `perf record` on a 120-day headless run to confirm the hot functions.
- In Godot: time `_read_snapshot` (parse), `_apply_snapshot`, `_refresh_ui` and the frame time (debug overlay with
  `SPACETRAINS_GODOT_PROFILE` or a key).
- Baselines: opening dispatch ≈36 s, 730-day run 11m10, bridge tick distribution, Godot frame time.
- Set `CMAKE_BUILD_TYPE` to Release by default in `CMakeLists.txt`.

**9b. One fixed simulation tick for headless and GUI.** Use a constant game tick (proposed 0.1 day, decided by the
9a timings). The bridge keeps a real-time accumulator and runs `timewarp × real_dt / tick` ticks per loop, so
timewarp changes how many ticks run, not their length. Headless runs the same ticks. Benchmark results then describe
the game the player sees. Re-baseline as v34 and record it in `ship_operating_costs.md`.

**9c. Remove the hitch (spread the work).**
- Stagger reviews: initial `next_review_s` offset by ship index (deterministic). The investment review does not
  all fire on one tick.
- Planning budget per tick: due reviews go to a deterministic queue (ordered by due time, then ship id), and a tick
  processes reviews until a time or plan budget is used. The rest wait for the next tick, a few game hours of delay
  at most. The opening dispatch then becomes a fast-forward of a few dozen ticks instead of one 36 s step.
- Split the investment review (up to 8 purchases × all candidates) across ticks the same way.

**9d. The bridge never stalls the picture.**
- Godot computes body and station positions analytically from the orbital elements (it already reads
  `godot/data/bodies.csv` and `stations.csv`) at a smooth display clock: game time extrapolated at the timewarp rate,
  gently corrected toward each snapshot's `game_time_s`.
- Ships in transit are placed by interpolating their `trajectory_path` samples (they carry `t_s`) at the display
  clock. Docked ships are placed at their station.
- A late snapshot then changes nothing visible. Only mission changes appear late.

**9e. Cheaper snapshots and IPC.**
- The bridge writes a tiny sequence file, or Godot checks the file's modification time and size. Godot parses only
  a new snapshot.
- Static data (factions, commodities, body and station definitions) are sent once. A ship's trajectory path is sent
  only when its mission changes (a `path_version` per ship). The UI refreshes on change.
- Keep the separate process: crash isolation, and the headless tools share code. A GDExtension is a later option,
  only if 9a shows IPC still matters.

**9f. Cut the planning cost**, ordered by expected gain for the risk; each change measured and checked with the
tests and the trajectory audit:
1. **`long double` → `double`** in the VariableISP integrator and shooting. Check endpoint accuracy against the
   existing tests and the 277-plan audit; keep `long double` only where a test proves it is needed.
2. **Cheaper Newton shooting**: warm-start from the costates of the last converged solve for the same route and
   class (cache keyed by origin, destination, departure bucket); Broyden rank-1 Jacobian updates between full
   finite-difference Jacobians; stop at the existing tolerance.
3. **Two-stage mission scoring**: rank all candidates with cheap estimates (cached leg estimates or an atlas lookup
   without refinement), fully plan only the best few (e.g. 3), and fall back to the next if a plan fails. Check that
   the chosen missions barely change (compare a 730-day run's mission log).
4. **A shared plan cache across ships and ticks**, keyed by (class, origin, destination, departure bucket, fuel
   bucket, payload bucket). Each entry is computed at the bucket's start time (not "now"), so results do not depend on
   history (also needed for 10c). Same fix for `estimate_leg()`.
5. **Parallel planning**: `plan_transfer` is read-only (an invariant in `docs/modules/trajectory.md`). The candidate
   plans of one `choose_mission`, and the reviews of one tick, run on a small thread pool and are combined in a fixed
   order, so results stay deterministic.
6. **A persistent route table** (the user's "pre-calculate and save" idea): orbits are circular and coplanar, so a
   heliocentric transfer depends on the departure time only through the bodies' relative phase. Tabulate per (class,
   origin, destination, fuel bucket, payload bucket) over a phase grid, lazily, and store it on disk. Invalidate it by
   a hash of the data files and the atlas. This also removes the start-up pause. Do it only if 1–5 leave the opening
   dispatch over ≈5 s.

**Targets:** bridge tick p99 under 30 ms at base speed (no visible hitch), opening dispatch under 5 s, 730-day
headless run under 2 min. Godot frame time under 8 ms with 60+ ships.

**Atlas.** The Python generator (`VariableISP/generate_atlas.py`, `rocketHamilton.py`) and the article
(`VariableISP/KeplerVariableISPRockets.tex`) are the references if the integrator or shooting needs theory. A
regenerated, time-optimal atlas (open item from step 8) is **not** part of step 9, unless profiling shows that poor
seeds cause most of the Newton iterations. Then it would move here, as a C++ port of the generator.

## Step 10: GUI

**10a. Orbit rings and positions (bug).**
- Bodies are positioned analytically (9d), so planets lie exactly on their circle at any zoom.
- Rings get level of detail: a coarse full circle plus an arc around the point nearest the camera, regenerated when
  the camera moves, with segments at about 2 px on screen. Alternative: a screen-space shader ring.
- Moon rings are centred on the parent's *displayed* position.
- Check with `DebugShot.gd`-style screenshots at Earth, Luna and Ganymede at maximum zoom.

**10b. A map like Kerbal Space Program's.**
- Bodies are drawn at true radius (drop the `BODY_MIN_MODEL_SCALE` inflation) as textured, lit spheres. When their
  projected size is under about 6–8 px, a flat round icon in the body's colour replaces them, with a short
  crossfade. The existing icon layer is reused.
- The Sun: emissive sphere plus bloom (WorldEnvironment glow) and a corona billboard, clearly the brightest object.
- Planets: stronger sunlight with no distance falloff (or a tuned falloff), so the outer planets are not dim.
- Ship icons stay bright but slightly smaller and less saturated than now.
- Labels fade by distance and priority.

**10c. Details panels** (`InspectorPanel.gd`; the bridge snapshot gains the fields):
- **Ship:**
  - The mission in words: "Carrying 120 u platinum, Ceres → Earth L1, sell to Earth's market; then return
    empty", or "Waiting 12 d for the window", "Refitting at …", "Laid up (no work)".
  - Cargo manifest, with value at departure and expected sale value.
  - Fuel in kg and as % of tank, with the reserve marked.
  - Crew size and provisions in days.
  - Home station, owner, class with tank variant.
  - ETA, waiting and coasting days.
  - Ledger: revenue, fuel, wages, capital, lifetime profit.
  - Route commitment, if any.
- **Station:**
  - Stock vs target and days of cover per good, with colour for shortage.
  - Production and consumption rates.
  - Price and trend.
  - Import and export flows.
  - Inbound ships with cargo and ETA.
  - Credits and treasury.
  - Fuel factory and export market badges.
  - Later: open orders (step 12).
- **New economy overview panel:**
  - Unmet demand (rolling 30 days and whole run).
  - Money supply and treasuries.
  - Exports.
  - Fleet size with ships bought and sold, laid-up count.
  - The worst-supplied station goods.
- **Event ticker** keeps ship purchases and sales and stranded alerts.
- **Verification:** screenshots via a debug autoload, plus the user's review. I cannot judge the live window myself;
  the user confirms the look.

## Step 11: save and load

- **Format:** versioned JSON, readable for debugging. Vendor `nlohmann/json` (single MIT header in `third_party/`);
  the code base has no JSON parser today.
- **Contents:** all mutable `Simulation` state:
  - game time and timewarp;
  - `stations_` and `ships_` (active missions with sampled paths), `sold_ships_`;
  - treasuries, outside-economy credits, seeded money supply;
  - investment ledger, next review times;
  - recent events and trades;
  - review queue (9c), and the plan caches or nothing (by 9f.4 caches are pure, so they can be rebuilt).
  - A hash of the data files and the atlas: load warns or refuses on mismatch.
- **Interfaces:**
  - `Simulation::save(path)` / `Simulation::load(data_root, path)`.
  - Headless `--save-at DAY FILE` and `--load FILE`.
  - Bridge commands `save` and `load` through the command file.
  - Godot: Save and Load buttons, quick-save F5 and F9, saves in `user://saves/`.
- **New game from a prepared start:** optionally a bundled save taken right after the opening dispatch, rebuilt by a
  script when the data change.
- **Determinism test (new):** run 100 days, save, load, run 100 more days; the report must be byte-identical to a
  straight 200-day run. This catches every piece of state the save misses.

## Step 12: the unmet-demand investigation (report before fixing)

**12a. Instrument** (headless `--demand-audit`). For every station × good, measure consumption wanted vs met, by base
value. For every day short, classify why:
- (a) no producer anywhere has surplus;
- (b) surplus exists, but no ship reviewed a mission from that producer (no ship docked there or headed there);
- (c) reviewed, but lost to a better-scoring mission (record the winner and both scores);
- (d) reviewed, but infeasible (fuel, time, port fuel);
- (e) scored below the minimum (price curve too flat, or distance too long for the margin);
- (f) delivered, but too little for the consumption rate (the hold or trip frequency is too small).

Also a **transport-capacity balance**: unit-days of transport the consumption needs (flow × trip time) vs what the
fleet supplies, per region (inner, belt, outer).

**12b. Report the findings to the user**, with the top causes by unmet value, before changing dispatch, pricing or
investment.

**12c. Known suspects to verify on the way:**
- Mercury metals after the v33 κ fix.
- Mars fuel running dry.
- Ships drifting off routes after their commitment ends.
- Ganymede and Titan at 94% unmet.
- Fuel's share of all cargo carried.
- The single-commodity hold.

## Step 13: station orders with mixed cargo (the user's "clever contracts")

**The idea, in the plan's words:**
- A station that will run short does not wait for a ship to find it profitable. It **orders ahead**: for each good
  it needs, it publishes a schedule, a price per unit that falls as more of that good is ordered.
- A supply ship fills its hold **unit by unit with whatever unit pays most next**. When fuel's marginal price has
  fallen below food's, the next unit is food.
- The order is a **contract** made at departure: units, agreed total price, delivery by the planned arrival plus a
  slack. Other ships see the contracted units as already coming.
- Hulls can be matched to orders, a limited fleet carries a useful mix, and stations get supplied on purpose.

**Design (to be refined after step 12):**
- **Manifest:** `MissionAssignment` gets `std::vector<CargoLot> {commodity_id, units, purchase_cost,
  contract_id}`. It replaces the single `commodity_id` and `cargo_units` (compatibility accessors during the
  migration). Its mass goes into the rocket equation (already done for one good).
- **Order book per station:** for each good, a demand schedule from the price curve at the forecast stock on
  arrival. The forecast counts stock, net production until arrival, and units already contracted. Wanted units cover
  the target stock plus consumption until the next likely delivery. The station also states what it **sells**: its
  surplus less contracted pickups (generalising v33a's `pickup_units`).
- **Filling the hold:** each unit's marginal profit is (destination's marginal price for it) − (origin's marginal
  price as the ship buys more). Both curves are monotone, so greedy filling in small chunks until the hold is full
  or the margin is ≤ 0 is optimal for a single capacity limit.
  - **Decision:** the limit stays in units, the current hold model; or move to mass or volume, since unit masses run
    from 10 to 500 kg.
- **Contract terms:** the price is fixed when the ship departs. The station pays on delivery. Delivery after the
  deadline is paid at the spot price. The station's credits for open contracts are reserved, so it cannot
  over-order.
- **Mission scoring:** the sum of the lots' margins − fuel, time and provisions, with the two-leg follow-up as now.
  Mission scoring and fleet investment both read the same order book, so investment can **size hulls to the order
  volume per round trip** (the user's "match cargo capacity to supply and demand").
- **Fleet size:** a soft cap per faction in `data/economy/fleet_investment.csv`, so the total stays near or under
  100 ships. Investment prefers bigger or better-matched hulls over more ships.
- **Measures of success:**
  - unmet demand down clearly from about 72%;
  - share of mixed-cargo trips;
  - fuel's share of carried value;
  - fleet profit not collapsing;
  - drift 0.
- **Rollout:** manifest and greedy fill first (no contracts); measure; then contracts and reservations; measure;
  then investment sizing.

## Critical files

- **Speed:**
  - `src/simulation/Simulation.{hpp,cpp}`: step, reviews, `choose_mission`, `estimate_leg`, investment.
  - `src/variable_isp/VariableIsp.{hpp,cpp}`: integrator, `refine_seed`.
  - `src/trajectory/VariableIspTrajectoryPlanner.cpp`.
  - `src/bridge/main.cpp`.
  - `src/app/main.cpp`.
  - `CMakeLists.txt`.
- **GUI:**
  - `godot/scripts/Main.gd`: `_read_snapshot`, `_apply_snapshot`, `_update_nodes`, `_display_position`.
  - `godot/scripts/SpaceEnvironment.gd`: rings, Sun.
  - `godot/scripts/EntityVisuals.gd`.
  - `godot/scripts/ui/InspectorPanel.gd`, `MarketPanel.gd`, `TopBar.gd`.
  - `Simulation::build_bridge_snapshot_json`.
- **Save/load:** `Simulation`, `src/domain/Types.hpp`, a new `src/persistence/` module (with a doc in
  `docs/modules/persistence.md`), `third_party/nlohmann/json.hpp`.
- **Economy:**
  - `src/simulation/Simulation.cpp` (`choose_mission`, `sale_value_on_arrival`, `open_surplus`,
    `commission_best_ship`).
  - `src/economy/EconomySystem.cpp` (price curve).
  - `src/domain/Types.hpp` (`MissionAssignment`).
  - `docs/modules/economy.md`.
  - New `docs/plans/station_orders.md`.

## Verification

- **Each step:**
  - the three test suites;
  - `spacetrains_headless --days 730 --report-interval 730 --econ-audit` and `--trajectory-audit` (0 flagged);
  - drift 0;
  - the calibration log in `docs/plans/ship_operating_costs.md` updated.
- **Speed:**
  - `--profile` numbers before and after each 9f change;
  - the bridge's slow-tick log is empty at base speed;
  - Godot frame-time overlay;
  - the user confirms smooth play.
- **Planner changes:** the regression tests in `tests/test_main.cpp` (κ at origin, Titan reach) still pass; a
  mission log diff on a 730-day run shows how many choices changed.
- **Save/load:** the 100+100 vs 200-day byte-identical report test, added to `tests/`.
- **GUI:** screenshots at maximum zoom on Earth, Luna and Ganymede (ring through the planet centre); the user's
  review of the look.
- **Economy:** the step-12 report delivered before step 13. Unmet demand, mixed-cargo share and fleet size tracked
  per version.

## Overnight run (2026-10-07 22:30 → 2026-10-08 06:45, user asleep)

- **Work without asking.** The user trusts my taste. Take the default decision and record it, with the reason, in
  `docs/plans/overnight_2026-10-08.md` (a running log: what was done, the numbers, the decisions to revisit). The
  checkpoints below become log entries, not stops.
- **Defaults chosen now:**
  - fixed tick 0.1 day;
  - `nlohmann/json` vendored;
  - hold capacity stays in units;
  - the GUI look judged from my own screenshots, and the user reviews it in the morning.
- **Commits:** commit each finished sub-step on `feature/opus5_5_return` (tests passing); never push. Kill bridge
  and Godot processes by PID or `pkill spacetrains_bri` after every GUI run.
- **Clock:** check `date` between sub-steps. From 06:15, start no new long benchmark. By 06:40, commit, finish the
  log with a morning summary (done / measured / decisions to review / next), and stop.
- **Rate limits:** if the 5-hour usage window runs out, a scheduled check (cron, every 30 min until 06:40) resumes the
  work from the log.
- If a step is blocked, log why and move to the next independent step. GUI work can run while long benchmarks run.

## Order of work and checkpoints with the user

1. 9a–9c, then report the measurements and the hitch fix.
2. 9d–9f, then report the targets reached.
3. 10a, then 10b–10c (the user reviews the look).
4. 11.
5. 12a–12b: report, then agree the step-13 design.
6. 13, in its three rollout stages.

No step starts beyond a checkpoint without the user's go-ahead.
