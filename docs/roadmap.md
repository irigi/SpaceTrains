# SpaceTrains Roadmap

Consolidated with the user 2026-11-02. Order: **To do** (top, in order) → **Parked** (kept in mind, not
planned) → **Not doing** → standing decisions and checks → **Done**. The design as built is in `docs/modules/`
(start with `docs/architecture_overview.md`); plans being worked on are in `docs/plans/` (none open now); shelved concepts in
`docs/archive/`; studies in `docs/studies/`.

**Status.** `main` holds everything up to PR #32 (planet-frame paths, music player), no open PRs.
Benchmark (4 starts × 1,460 days, 2026-10-26): unmet demand 0.2 ± 0.2%, 87 of 131 ships profitable,
~4,500 boardings, drift 0, ~150 s per run.

## To do

1. **Listen to the music** (user): judge the mix (2 quiet : 1 classical, 20-60 s gaps, 2 s fade-in, 60% volume,
   keys N / Shift+N / [ ]); settings in `Music.gd`. Credits are done (2026-11-02: help panel H → Credits,
   `CREDITS.txt` in the package; `deployment.md`, "Credits and licences").

2. **Stations orbiting their planets** (user, 2026-11-02: wanted, to be designed). Stations sit at a fixed angle
   from their planet in the simulation itself (`CelestialMechanics.cpp:56`, `theta_rad`). At 5 days/s a real
   low Earth orbit is ~80 turns a second. To decide: a display-only convention (visual rate capped to a few
   seconds per turn, true rate at low time speeds; ship paths near the station then disagree with it) or real
   orbits in the simulation (planning and economy change).

3. **Engine-failure rescue** (user, 2026-11-02). A **plasma** ship's engine fails in flight (much more often
   than hull failures: nothing is lost but time); the ship coasts on its Kepler orbit. **One** rescue ship,
   newly spawned at the nearest larger station, same class as the police cutters, chases it (cheapest meeting,
   as police and salvage do); no trade ship or liner takes part. After the repair the ship re-plans from its
   current position and velocity. The **owner pays** the rescuer's propellant, crew days and the repair (so
   valuations should count the expected bill). Same building block: salvage-race losers that fly plasma turn
   back mid-flight instead of flying on to an empty meeting point (today they fly on, then home).
   Nuclear-thermal ships do not get engine failures. To settle in the plan: failure rate, repair time,
   where the rescuer goes afterwards, what passengers aboard do.

4. **Patched conics: planets' gravity wells** (user, 2026-11-02). Interplanetary plans run station to station
   around the Sun; the Kepler planner charges only the heliocentric burns (`TrajectoryPlanner.cpp` ~531), so
   leaving and entering a planet's gravity well appears to cost nothing. Measure first (Δv and propellant missing
   per route), then plan proper planet escape and capture arcs for both planners, with the economy effect
   benchmarked. Also the same-planet "local direct arc" approximation.

5. **Account ledger in Statistics** (instead of a full who-pays-whom matrix, which would be ~160 × 160 accounts):
   pick one account (station, its residents, a ship, a faction, Earth, a colony) and see whom it paid and who paid
   it, by category, over time. Needs per-account records in the money journal (today it keeps 6 sectors).

6. **Moon and outer-planet textures** (optional): Ganymede, Titan, Europa and Triton have no surface maps;
   Uranus and Neptune use old small ones. Optionally make the package smaller (~230 MB, mostly the planet maps
   as imported textures).

## Parked (kept in mind, not planned)

- **Per-ship variation:** ±power and size within a class (fits, modules, tank variants and wear per ship exist).
- **Scenarios:** alternative starting economies, more factions, stations at the outer bodies (Uranus, Neptune,
  Triton, Europa have none), data-only star systems. All content is data already.
- **Faction conflict** (blockades, commerce raiding) as the only form piracy could take; the user asks for it if
  wanted.
- **Trajectory quirks, invisible in play:** ~25% of atlas cells fly an extra turn (θ only valid mod 2π); ~55% end
  with > 2% fuel left, so flights are not as fast as the ship could fly. A refill by `tools/atlas_fill` would fix
  both (+12% run time measured for filling holes).
- **Supply following price:** built but off (`households.csv` `supply_price_elasticity` 0); needs production
  capacity sized to use first (production is 8-10× use; cutting it to 3-5× caused shortages around Earth).
- **Science samples that lose value with age** (would favour fast couriers); research stations as a type.
- **More hull failures on screen:** the honest lever is shorter ship lifetimes (`lifetime_years`); engine
  failures (to do 3) give the on-screen events instead.

## Not doing (decided 2026-11-02 unless dated)

- **Population kinds (Victoria 2 style):** a layer that keeps the economy unchanged would be statistics only (it
  must add up to today's totals); one with feedback is a rebalance. Off the roadmap; the user brings it back if
  wanted. Seam: the station systems view; concept and tensions in `docs/archive/station_structures.md`.
- **Insurance; customs and smuggling** (no clear concept; inspections stay without economic effect).
- **Titan nitrogen/ammonia:** Earth goods at $100/kg launch beat a multi-year haul from Saturn; Mars has its
  own atmospheric nitrogen.
- **Light short-hop cabins:** the Luna Ferry trial changed nothing measurable (`docs/archive/passenger_routes.md`).
- **Demand elasticity:** already built (consume goods follow price^−1 and wage, upkeep goods −0.1; `economy.md`).
- **Idle faction money:** closed by rule: a treasury's surplus counts as profit spent in the planetary economies
  (paid to Earth, the open account). To build as a small change when convenient: a treasury cap above which
  money goes to Earth, journal category "dividends to Earth".
- **Packaging:** zip/tar.gz is enough; no AppImage, installer, CI or code signing.
- **Hidden pirates** (2026-10-27): burns are visible, raiders would need an openly complicit port.
- **Exhaust-band engines, engine tiers, bigger tanks, coasting** (2026-11-01, banded solver study);
  `feature/exhaust_band` stays unmerged.
- **Tier 3 station structures** (2026-10-26): would recalibrate most of the economy.

## Standing decisions

- **Economy target:** physical and economic feasibility. Uneconomic trajectories must lose on cost (capital,
  wages, provisions, fuel), not on heuristics. Unviable ships are acceptable: a class that cannot earn on a route
  is simply not used there.
- **Technology era:** variable-Isp ships at the level of `~/VariableISPRocketTrajectories/linopt/` (exhaust
  50-250 km/s, ~540 W/kg at ship level, radiators, 15% waste heat). No torchships. Variable-Isp drives are
  "plasma"/fusion with radiators; the "chemical" classes are nuclear-thermal (900-1400 s).
- **Perihelion limit** 0.1 AU for every planner. The game is planar on purpose.
- **Money:** exact accounting, drift 0; an open economy with explicit external accounts (Earth, colonies,
  treasuries).
- **Fuel** comes from fuel factories at a few places (not Earth) and is hauled by tankers.
- **Outer exports:** no exotic matter.
- **Fleet cap** 130 (2026-10-22).

## Checks after each step

The test suites (`ctest`), `tools/benchmark.py` (four starting dates, mean and spread; one run is too noisy to
judge a change) and `--trajectory-audit`; for speed, `--profile` runs one after another with all threads (the
slowest 20-day stretch must stay above 5 days/s). "Unmet demand" is the share of the stations' consumption (by
base value, over the whole run) that found no stock. Debug idle ships with `SPACETRAINS_TRACE_SHIP="<ship name>"`.
After changing purchase valuations, fares, prices or ship classes, also run `tools/purchase_returns.py` (the
estimate should stay within about 2× of the result). Display-only changes: the 365-day economy audit must stay
byte-identical.

## Done (details in the module docs and git history)

- **Physics and planners:** Kepler and VariableISP planners, the atlas with low-κ levels, κ at the origin's radius,
  STM refinement, the general-boundary and chase solver (rotation-invariant, PR #31), Lambert short-way and
  multi-revolution arcs, perihelion limit, the trajectory audit and its long-run fixes (endpoint snaps, wait
  prefixes, hyperbolic Lambert sampling, Hohmann dents, path thinning) (`modules/trajectory.md`); tech review of
  the ship classes (`ship_technology.md`); banded-exhaust study (`docs/studies/banded_transfer/`).
- **Ships:** operating costs, mission-sized fuelling, tank refits, fleet investment, fuel factories and tankers,
  outer exports; parts, modules, crew radiation, boil-off and wear; passengers and liners; Weibull failures,
  derelict salvage races and Earth-paid police inspections (PR #26) (`modules/ships_and_fleet.md`).
- **Economy:** production roles, landed-cost prices, station orders, competitive asks and rotation contracts
  (PR #14), the economy overhaul with real prices, households, labour, colonies, incomes and the money journal
  (PR #21) (`modules/economy.md`).
- **UI:** the observer with display clock, close zoom, statistics screen (People, Flows, Routes with porkchops),
  ship schematic, 3D models at true scale, station systems view, planet textures, planet-frame paths for hops
  within one planet's system, music player (PRs #15-#16, #22-#23, #25, #32; `modules/ui_observer.md`); a design
  of its own for every station (torus pairs with mirrors or dry docks, Bernal sphere, sun-shaded and ice-armoured
  drums, bolas, hollowed asteroid, module wheel, carousel) with detailed models built in the background so they
  never pop in (`feature/station_models_diverse`, 2026-11-03, awaiting the user's look);
  save/load (F5/F9); code review and clean-up (PR #24).
- **Deployment:** Linux tar.gz and Windows zip built on the Linux PC, tested on Windows 11 (PR #30,
  `modules/deployment.md`).
