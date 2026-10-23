# SpaceTrains Roadmap

## Done

### Phase 1 — VariableISP Integration and Headless Verification (2026-05)

- Electric ion ship classes with tech-level parameters (α, ε).
- `VariableIspTrajectoryPlanner` backed by the precomputed trajectory atlas; per-sample propellant via the I-invariant scaling.
- Multi-planner dispatch by propulsion type; Lambert solver and NTR ships for the Kepler side.
- Headless executable with `--verbose`, `--econ-audit`, and periodic reports.

### Phase 2 — Godot Observer Frontend (2026-06)

- Bridge snapshots carry `trajectory_path`, destination ghost, and snapshot timing for smooth interpolation.
- Procedural sci-fi HUD (top bar, entity browser, inspector, market panel, event ticker).
- Starfield, Sun bloom/halo, planet textures, orbit rings, procedural ship/station meshes, trails, engine glow.
- Economy: dynamic prices, credits, trade settlement, storage caps, profit-based dispatch; fleet-collapse fix (730-day audit: 0 stranded). Fleet investment (v27-v28): treasuries buy ships for routes they commit to for 180 days, long-laid-up ships are sold. Fuel factories at five stations, bulk tankers, fuel-aware planning (v29).

## Current Plan (updated 2026-10-23) — start here

**Status.** `main` holds everything up to the economy overhaul (merged as PR #21, 2026-10-23). **Better 3D ship
models** (roadmap step 5) are on branch `feature/ship_models_3d` (2026-10-23, not merged; see "Ship and station
models" below). The economy overhaul (plan
`docs/plans/economy_overhaul.md`, decisions and results `docs/plans/economy_overhaul_log.md`, module doc
`docs/modules/economy.md` ("Economy overhaul")) brought:
- real prices: Earth surface price + $100/kg launch at the Earth stations;
- Earth and colony surface markets with their own accounts;
- residents paid wages, with travel and savings from their own accounts;
- ships paying their crews;
- a labour market with lognormal wage distributions, local hiring between colonies and their gateways,
  and colony recruiting on Earth;
- station incomes from Earth (need-based science and settlement grants, relay, tourism, yards in space,
  microgravity products, helium-3, low-orbit propellant from Luna);
- Venus Orbital Exchange.

Benchmark, 4 starts × 1,460 days (2026-10-23):
- unmet demand 0.2 ± 0.1%, last fifth 0.0%;
- 92 of 129 ships profitable (71%);
- no station in the red at the end (worst ~12 days in the red per 4 years);
- ~4,600 boardings (mostly cislunar);
- drift 0; 54.6 ms per simulated day, slowest 20-day stretch 6.0 days/s.

An 8-year run holds at 0.1% unmet.

**Ship and station models** (branch `feature/ship_models_3d`, `docs/modules/ui_observer.md`):
- Map ships are built from their design's components with the schematic's layout, in metres. A coarse model shows
  at mid zoom; a detailed one (built in the background) shows close up, with live radiators, plume and cargo.
- Generic stations with rings, berths and a flavour per economy profile; docked ships sit at the berths.
- Muted, non-emissive paint. Ships no longer glare near the Sun (0% clipped pixels, down from ~30%), and a camera
  fill light keeps night sides readable.
- Frame time at overview is unchanged against `main`; station close-ups run at 56-60 fps.
- The station snapshot gained `economy_profile_id`.
- **For the user to review:**
  - the look (gallery: `scripts/ModelGallery.gd`);
  - the size: ~4,500x real, stations compressed to 44-72 m ring radius;
  - the Low Earth and Lunar Gateway stations, drawn smaller than the ships' scale so they stay above their planet.
- Then merge.

**Open for the user:**
- **Review the economy overhaul** (merged; the log's decisions were all taken overnight without the user). The main
  calls:
  - goods repriced to Earth's real prices plus launch (bulk goods 3-7× cheaper, platinum 12× cheaper);
  - Earth buys only space-sourced goods;
  - station incomes sized from the stations' trade deficits;
  - purchase valuations capped at the landed cost; carrier margin 1.5;
  - the money-supply controller off, a 750k credit ceiling, factions covering stations in the red within
    2 days;
  - a 3% travel budget (passengers ~5,000 in 4 years, down from ~16,000);
  - Titan makes its own machinery.
- **Economy statistics** (built 2026-10-23 on the same branch): a money journal books every transfer by account
  and category, keeps a monthly history and is saved with the game (save version 19). It gives the
  `--econ-audit` Accounts tables, `--stats-json`, and the in-game Statistics screen (S): overview and who pays
  whom, factions, stations, residents, colonies & Earth, fleet. Docs: `docs/modules/economy.md` ("Money
  journal"), `docs/modules/ui_observer.md`.
- **Colonies fixed** (2026-10-23, same branch): the surface mines and the gateways trade. Luna surface and the
  belt sell metals, water, helium-3 and platinum at their gateways. Imports follow money and people; growth and
  wages follow welfare. Luna surface: all imports met, account +0.8M. Belt: 65-82% of imports met, off its line.
  Log: `economy_overhaul_log.md`, "Colonies fixed".
- **Mars fixed** (2026-10-23, same branch): gateways order their colony's imports at the colony's bid (Mars
  Transfer's reactor fuel was capped at the floor and never delivered), and a faction in debt taxes its stations
  above 375k. Mars surface 42% → ~70% of imports met; Mars Corporation and the Independent Consortium end 8 years
  near or above zero. Log: `economy_overhaul_log.md`, "Mars reactor fuel and faction debt". Still open: Luna
  surface's money piles up (later: imports that rise with income).
- **Light short-hop cabins** (optional): the remaining lever on cislunar cost per person (Luna Ferry study,
  `passenger_routes.md`). Its other open points were closed after the overhaul (same file, "Re-evaluated
  after the economy overhaul").
- **Supply that follows price** is built but off (`households.csv` `supply_price_elasticity`): production
  is 8-10× use, so it idles plants and cut jobs everywhere. It needs capacity sized to use first (see the
  population-composition item below).
- Fleet cap stays 130 (user, 2026-10-22). With the cap reached, faction treasuries pile up unspent money
  (Sol Federation ~9M by year 8).

### Decisions so far

- **Economy target:** physical and economic feasibility. Uneconomic trajectories (multi-revolution, long
  waits) must lose on cost (capital, wages, provisions, fuel), not on heuristics.
- **Operating costs (part A, done):** wages and capital paid to the home station; provisions are real
  food/water/oxygen; ships with no money or no work lay up. Two-leg mission scoring.
- **Fuelling (part B, done):** missions load burn + 10% reserve; cargo and provisions are in the rocket
  equation; ion ships choose their fuel load by fuel + time cost.
- **Perihelion limit** 0.1 AU for every planner.
- **Unviable ships are acceptable:** the starting fleet is not final. A class that cannot earn on a route
  should simply not be used there; the economy should deploy other ships (fleet investment, below).
- **Technology era:** variable-Isp ships stay at the level of the user's `~/VariableISPRocketTrajectories/linopt/`
  studies (exhaust 50-250 km/s, about 540 W/kg at ship level, radiators, 15% waste heat). No torchships;
  keep radiators and efficiency limits, but do not cut ship efficiency much.
- **Naming (confirmed by the user 2026-10-06):** variable-Isp classes are advanced plasma or fusion
  drives with radiators, not "ion"; the "chemical" Kepler classes are nuclear-thermal (solid core about
  900-1000 s, liquid/gas core up to about 1400 s). Rename in data and UI during the tech review.
- **Money:** keep exact accounting. Prefer an open economy with an explicit external account (Earth's
  economy, faction treasuries) over a strictly closed one; the audit must still reconcile to zero drift.
- **Outer-system exports:** no exotic matter. Candidates: platinum-group metals (Ceres belt), deuterium
  (Ganymede, Titan), nitrogen/ammonia (Titan), science samples.
- **Fuel supply (user, 2026-10-07):** fuel comes from **fuel factories** at a few particular places
  (not Earth: lifting from a planet surface costs extra) and very large freighters (or many of them) carry it
  to the stations that buy it; the planner sees that a destination will be short of fuel. Done in v29
  (`docs/modules/ships_and_fleet.md`): ships carry their return fuel and fly cheaper transfers where fuel is
  scarce, and reject a trip only when even that is impossible.

### Next steps, in order

1. ~~**Merge passengers**~~, ~~**passenger routes**~~, ~~**Mercury passengers**~~: done (PRs #18-20); the
   routes' open points were re-evaluated after the economy overhaul (`passenger_routes.md`).
2. ~~**Economy overhaul merge**~~ (PR #21); its review is still open (above).
3. (free)
4. **Tier 3 ship structures: stations** (`docs/plans/ship_internals.md`).
5. ~~**Better 3D ship models**~~ (user, 2026-10-20): built on `feature/ship_models_3d` (above), with generic
   station models and the colour fix; awaiting review and merge.
6. ~~**True scale for ships and stations**~~ (2026-10-23, on `feature/ship_models_3d`): ships and stations at real
   size (`Main.MODEL_SIZE_EXAGGERATION` = 1, `--model-scale=N` to change), Stanford-torus habitat rings by
   population, a ship-sized docking core, sticky berths with a blend into them, model-aware picking, near plane and
   zoom limits for metre scale. Also fixed: the bridge never ticked at the slowest timewarp (600x). Awaiting review
   with step 5.
7. **Space chases in the game:** wire `trajectory/ChaseSolver` (`plan_chase`, ~1-3 ms; replanning ~0.4 ms)
   into events, e.g. a rescue of a stranded ship, a courier racing a deadline, then piracy and police
   (below). Needs the game design: who chases whom, triggers, what happens at intercept.

8. **Code review and document cleanup** (user, 2026-10-22), once the steps above are done: review the code
   (`/code-review`, `/simplify`), delete obsolete plans, and rewrite the module docs and the plans that stay so they
   describe the current design, not the history of how we got there (decisions with their reasons may stay; the
   step-by-step narrative goes, git history keeps it).

Later (user, 2026-10-22):
- ~~**Earth's surface economy as a market**~~, ~~**planetary background economies**~~ and ~~**station economies**~~
  (`background_economies.md`, `economy_points_7_10.md`): built as the economy overhaul (2026-10-23, above),
  except industries as accounts of their own and supply following price (built, off). The passenger points were
  revisited (`passenger_routes.md`, "Re-evaluated after the economy overhaul").
- **Population composition in the style of Europa Universalis 2:** stations and ships hold people of different
  kinds (classes, jobs, purposes of travel, tuned to a space setting rather than copied from EU2). To be designed
  in detail together when we get there; the user wants help with the concept.

Deferred (user, 2026-10-08):
- **Piracy and police:** later. A first version needs no chase game: on a ballistic coast a ship's path is known,
  so an intercept is a rendezvous problem (Lambert), costly in delta-v and likeliest near departure and arrival.
  It could start as an expected-loss cost per route that police presence lowers.

### Done (one line each; details in the module docs, the logs named and git history)

- Tech plausibility review: classes rebuilt bottom-up, priced from hardware, renamed (`docs/ship_technology.md`).
- Part C, modular tanks and refits (v25); open economy (v26); fleet investment (v27-v31); fuel factories and
  bulk tankers (v29); outer-system exports (v32-v33); trajectory reach, κ at the origin's radius (v33)
  (`docs/modules/ships_and_fleet.md`, `trajectory.md`).
- Steps 9-13: speed (`--profile`, parallel planning), GUI, save/load (F5/F9), unmet-demand findings, orders
  and mixed cargo (`docs/plans/overnight_2026-10-08.md` in git history).
- Steps 14-17: production dependencies, landed-cost reference prices, science trade, station money
  (`docs/plans/steps_14_17_log.md`).
- Steps 18-22: start stocked, landed-cost depot fuel, scheduled liners to Titan and Ganymede
  (`docs/plans/steps_18_22_log.md`).
- Steps 23-28: cover from the round trip, input caps, faction credit, station growth, random events
  (`docs/plans/steps_23_28_log.md`).
- Step 29, station purchase orders (`docs/plans/station_orders.md`).
- Market redesign: competitive asks, multi-stop rotation contracts (PR #14, `docs/plans/market_redesign.md`,
  `overnight_2026-10-19.md`). Key bindings help (PR #15), close-zoom camera (PR #16).
- Ship structures: tier 1 parts and schematic (2026-10-11), tier 2 modules, radiation, boil-off and wear (PR #17;
  `docs/plans/ship_structures_log.md`, `tier2_log.md`). Wobbly outer-moon paths fixed (cubic Hermite, 2026-10-11).
- Passengers (PR #18), passenger routes (PR #19), atlas extension to low κ for Mercury (PR #20).
- Economy overhaul (branch `feature/economy_overhaul`, 2026-10-23): `docs/plans/economy_overhaul_log.md`.

Checks after each step: the test suites (`ctest`), `tools/benchmark.py` (four starting dates, mean and spread;
one run is too noisy to judge a change) and `--trajectory-audit`; for speed, `--profile` runs one after another
with all threads (the slowest 20-day stretch must stay above 5 days/s). "Unmet demand" is the share of the
stations' consumption (by base value, over the whole run) that found no stock. Debug idle ships with
`SPACETRAINS_TRACE_SHIP="<ship name>"`. After changing purchase valuations, fares, prices or ship classes, also
run `tools/purchase_returns.py` (expected against earned return by ship group; the estimate should stay within
about 2× of the result, and a group far below it is being overbought).

## Trajectory Correctness Over Long Runs (mostly done)

Unphysical trajectories appear after months of self-play:

- A VariableISP path ends with a sharp dent: the integrated trajectory misses the destination and the final sample is snapped onto the station.
- A path loops around the Sun many times with only ~10 samples per revolution.

Audit tooling is in place (`--trajectory-audit`, `--trajectory-sweep`; see `docs/modules/trajectory.md`).
Findings from the first 2-year sweep (192k plans, 2026-10-06):

- **Bug 2 = Kepler wait prefix — FIXED.** Paths for awaiting ships prepended only 11 samples of the origin orbit; a
  573-day wait at Venus drew 2.5 turns as a star polygon (83° per step). Now sampled at ≤ 5° of heliocentric sweep
  (11–360 segments); `coarse_wait` went from 11,982 plans to 0.
- **Bug 1 = VariableISP endpoint snap — FIXED.** 96% of nearest-cell seeds and 52% of interpolated seeds missed the
  target by > 0.01 AU (worst: Mars -> Titan overshoots to 22 AU); the snap drew a dent. The planner now refines a real
  atlas cell's seed by minimum-norm Newton shooting onto the destination station's actual position at arrival (and
  starts from the origin station's position at departure), trying up to 4 windows. Sweep: max miss 13.3 AU -> 0.0013 AU,
  `end_dent`/`endpoint_miss`/`start_miss` -> 0. Cost: ion plan p50 ~8 ms, p99 ~94 ms; 3-year headless run 1m36s -> 3m24s.
- **Integrator hang.** Some ion -> Titan plans got seeds that dive into the Sun and RK45 never finished (would
  freeze the simulation). Now guarded by a step budget; the planner treats failure as no window.
- **Theta wraparound.** Atlas theta spans ±1.1 rev; planner accepts retrograde/near-full-loop targets, and the
  integrated theta can land 2-4π away from the target.
- **Lambert path to Titan — FIXED.** A full-tank NTR Ceres -> Titan picks a hyperbolic Lambert arc; the path sampler
  only handled ellipses, so all samples sat at Ceres' radius with the departure time and the snap drew a 6.8 AU line.
  Now samples ellipses, hyperbolas, and near-parabolas (Barker). Sweep: Lambert max miss 6.8 AU -> 0.0001 AU.

- **VariableISP κ at the origin's radius — FIXED (v33).** κ was taken at 1 AU while the atlas path was scaled to
  the origin's orbit: no plasma ship could fly home from Titan, Ganymede -> Earth took 995 days against 241 out.
  Missed windows no longer wrap to a whole synodic period (Ganymede -> Ceres 3133 -> 432 days).

Next fixes:

- Decide which theta branches (retrograde, >1 rev) are acceptable; atlas θ labels are only valid mod 2π
  (~25% of cells actually fly θ ± 2πn).
- Atlas time-optimality: ~55% of solved cells end with > 2% fuel left (the generator only penalised overuse), so
  transfer times are not minimal for the ship's kappa. Consider enforcing m_end = m_dry in the shooting.
- Planning cost: 3-year headless run 1m36s (before) -> 4m22s now (~0.24 s per simulated day). Cache refined plans
  (path depends on (ρ, θ, T) only, not κ) if the live bridge stutters.
- **Hohmann fallback dents — FIXED.** The half-ellipse is now drawn through the actual stations with Kepler timing,
  and elliptic arcs (Hohmann and Lambert) are sampled evenly in eccentric anomaly so the sharply curved far end of
  eccentric ellipses has no kinks. Sweep: Hohmann flagged 1,647 -> 0; Lambert end dents 3 -> 0.
- **Perihelion limit — FIXED.** No planner bounded perihelion: 8 Lambert and 4 ion plans passed *through the Sun*.
  Both planners now enforce 0.1 AU (`kMinPerihelionM`); ion feasible plans -5%.
- **Path sampling — FIXED.** Rendered paths are thinned by curvature (≤ 6° per point, ≥ 40 segments) from a dense
  generation. Sweep: chemical plans 0 flagged; ion 1 `coarse_sampling` left (was 3,034).
- Regression tests for each fixed case.

## Economy Ideas (kept from finished plans)

Operating costs, tank refits, the open economy, fleet investment, fuel factories, outer exports and science are
done (`docs/modules/ships_and_fleet.md`, `economy.md`). Ideas from those plans not taken up yet:
- Nitrogen and ammonia from Titan for the nitrogen-poor Moon and Mars.
- Science samples that lose value with age (would favour fast couriers); research stations as a station type.
- Demand elasticity: residents cut consumption at high prices, not only the station ordering less.

## Phase 3 — Scenario Control

Save/load (exact continuation) and deterministic runs are done.
- Add scenario-level data for alternative starting economies or factions.

## Phase 4 — Richer Orbital and Mission Fidelity

- Improve Kepler planner with patched-conic SOI handoffs.
- Replace local direct-transfer approximation with proper planet-SOI arc (also fixes ~100% propellant use on Earth–Moon VariableISP-style hops).
- Add stranded-ship handling, support/refuel behavior, clearer mission state transitions.
- Consider outer Solar System bodies (Jupiter and beyond).

## Phase 5 — Ships with Individual Variation

- Per-ship fits and component condition: done (tier 2, PR #17).
- Add per-ship power multiplier (±α variation) and size multiplier.
- Size scaling preserves κ; power multiplier changes κ slightly per ship.
- Reflect individual ship specs in UI selection panel.

## Phase 6 — Post-v1 Systems

- Add pirates, police, inspections, rescue, and later combat resolution.
- Expand content: outer-system stations, more factions, science mission chains.
- Support custom star systems through data-only content changes.
