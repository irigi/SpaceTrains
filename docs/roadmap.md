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

### Study — banded-exhaust transfers (2026-10-31, branch `feature/banded_solver`)

- A standalone optimal solver for the 50-250 km/s band (`src/variable_isp/BandedTransfer.*`, its own library, not linked by the game), and a study against "plan ideal, fly throttled": `docs/studies/banded_transfer/banded_transfer.pdf`.
- Result: cargo ships lose about 1% with the throttled plan, liners 3-7%. Recommendation: keep the throttled plan as the planner; banded solves at most as an optional, time-boxed refinement. Open: engine tiers, tanks and coasting (to discuss with the user).

## Current Plan (updated 2026-10-27) — start here

**Status.** `main` holds the game up to the economy overhaul (PR #21), the 3D models at true scale (PR #22), the
station systems view (PR #23), the clean-up review (PR #24, merged 2026-10-26) and the statistics views (2026-10-27,
below). The design as it stands is in `docs/modules/` (start with `docs/architecture_overview.md`); the economy in
`economy.md`, ships, dispatch and passengers in `ships_and_fleet.md`, the statistics screen and inspector in
`ui_observer.md`, porkchop plots in `trajectory.md`.

Benchmark, 4 starts × 1,460 days (before the review's result-changing fixes, 2026-10-26): unmet demand
0.2 ± 0.2%, last fifth 0.0%; 87 of 131 ships profitable; ~4,500 boardings; drift 0; ~150 s per run.

**Statistics views (2026-10-27, the "Later" block below, reviewed by the user in three rounds):**
- inspector redesign: ships and stations share one layout (key numbers, folding sections, tables with headers),
  rebuilt one part a frame;
- People tab (where people live, what they do, residents against housing, jobs by kind, life needs met, people over
  time) and the residents' donut in the inspector;
- flow record and Flows tab (from × to goods, goods balance by station, passengers by route, lane costs) and the flow
  overlay on the map (G);
- porkchop plots (Routes tab: from/to planets, fastest trip by departure per class, Δv and effort maps with each
  class's reach; launch windows in the station inspector), computed in the background per planet pair and kept on
  disk, keyed by a hash of the trajectory code.
All display only: the 365-day economy audit is byte-identical to before; saves are version 21 (20 still loads).
Departures from the plan: no origin on cargo lots (every lot is sold at the end of its mission); "made net of use" from
stock snapshots. Known: sharp steps in plasma maps where the cheapest path would pass inside 0.1 AU (a filter, not a
solver constraint); a few blocky cells at flights of a Hohmann time or more; while the porkchop worker fills a fresh
cache, 5 d/s shows ~10 more frames over 50 ms a minute (ui_observer.md).

**Open for the user:**
- **Light short-hop cabins** (optional): the remaining lever on cislunar cost per person
  (`docs/plans/passenger_routes.md`).
- **Supply that follows price** is built but off: production is 8-10× use, so it needs capacity sized to use first
  (with the population composition item below).
- Luna surface's money piles up (later: imports that rise with income); with the fleet cap reached, faction
  treasuries pile up unspent money (Sol Federation ~9M by year 8). Fleet cap stays 130 (user, 2026-10-22).

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

1. **Space chases in the game** — built 2026-10-28 overnight, merged (PR #26), review accepted by the user
   2026-10-30 (decisions in `docs/plans/overnight_2026-10-28.md`; design in `docs/modules/ships_and_fleet.md`, "Failures and
   salvage" and "Police inspections"):
   - **Failures:** Weibull hull lives (shape 3, mean 30 y × 1.10, exact in expectation against the capital charge);
     a ship that fails in flight becomes a derelict coasting on its orbit; its owner may replace it first.
   - **Salvage races:** ships weigh a wreck's cargo and scrap against their missions; the first to reach it wins.
   - **Police inspections:** Earth-paid cutters meet ships of every faction now and then, at the cheapest point of their path
     and ride alongside for 3 days; the inspected ship keeps its course (legal assumption).
   - Later: insurance (maybe; it only smooths owners' losses), customs and smuggling (on the inspection seam), a timed
     crew rescue, losers of a race turning back mid-flight.
   - **Piracy** as hidden raiders does not fit (burns are visible across the inner system and raiders need an openly
     complicit port): if it comes back, as open conflict between factions (blockades, commerce raiding), later.

2. **Deployment on Linux and Windows 11** (user, 2026-10-26): one robust way to install and run SpaceTrains on
   both systems, as an installer or a documented process. Today it needs a Godot built from source with double
   precision (`precision=double`) plus our CMake build of the bridge, and compiling during installation is not
   practical. Likely shape: build once per platform and ship the binaries (a double-precision Godot export
   template, the exported game, `spacetrains_bridge` and the data with the atlas), e.g. from CI or a build machine;
   then an installer or archive per platform (Windows: Inno Setup / NSIS or a zip; Linux: AppImage or tarball).
   Points to check: how the game finds the bridge and data when installed, writable paths for saves and the
   snapshot/command files, MSVC or MinGW for the C++ core, and the bridge being cleaned up when the game exits.
   The user can run Claude on a Windows machine to build and test there.

Later (user, 2026-10-26): more statistics views and porkchop plots, done (above). Left from it: the "who pays whom" matrix drilled down to accounts (the journal keeps sectors only).

Later (user, 2026-10-27), visuals:
- **Planet textures:** done 2026-10-30 (8k/4k maps with mipmaps, albedo scaled against the Sun light, Mars and
  Venus desaturated, Earth's clouds, Saturn's ring map; `docs/modules/ui_observer.md`, "Planet surfaces").
- **Stations orbiting their planets:** at 5 days/s real orbital speeds are impractical (a low Earth orbit would
  be ~80 revolutions a second), but a station hovering at a fixed point over the planet looks wrong too. Needs
  a display convention, e.g. a visual orbital rate capped to a few seconds per revolution (or slowed by a
  fixed factor), with the true rate at low time speeds; display only, the simulation stays as is.

Later (user, 2026-10-22):
- **Population composition in the style of Victoria 2:** stations and ships hold people of different
  kinds (classes, jobs, purposes of travel, tuned to a space setting rather than copied from Victoria 2). To be designed
  in detail together when we get there; the user wants help with the concept.

Deferred (user, 2026-10-08; revised 2026-10-27):
- **Piracy:** not as hidden raiders (see step 1); perhaps later as open faction conflict.

### Done (details in the module docs and git history)

- Physics and planners: Kepler and VariableISP planners, the atlas with low-κ levels, κ at the origin's radius,
  STM refinement, the general-boundary and chase solver, perihelion limit, trajectory audit
  (`modules/trajectory.md`); tech review of the ship classes (`ship_technology.md`).
- Ships: operating costs, mission-sized fuelling, tank refits, fleet investment, fuel factories and tankers,
  outer exports; parts, modules, crew radiation, boil-off and wear; passengers and liners
  (`modules/ships_and_fleet.md`, PRs #17-#20).
- Economy: production roles, landed-cost reference prices, station money and faction credit, start stocked,
  growth, events, station purchase orders, competitive asks and rotation contracts (PR #14), the economy overhaul
  with real prices, households, labour, colonies, incomes and the money journal (PR #21) (`modules/economy.md`).
- UI: the observer with display clock, close zoom, statistics screen, ship schematic, 3D models at true scale,
  station systems view (PRs #15, #16, #22, #23; `modules/ui_observer.md`); save/load (F5/F9).
- Step 8, code review and clean-up (2026-10-26, `feature/cleanup_review`): `docs/plans/cleanup_review_log.md`.

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
