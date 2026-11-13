# SpaceTrains Roadmap

Consolidated with the user 2026-11-02. Order: **To do** (top, in order) → **Architecture changes** (recommended, none started) → **Parked** (kept in mind, not
planned) → **Not doing** → standing decisions and checks → **Done**. The design as built is in `docs/modules/`
(start with `docs/architecture_overview.md`); plans being worked on are in `docs/plans/` (none open now); shelved concepts in
`docs/archive/`; studies in `docs/studies/`.

**Status.** `main` holds everything up to PR #35 (station designs, eclipses, moons, Earth Sunward rename), no open PRs.
Benchmark (4 starts × 1,460 days, 2026-10-26): unmet demand 0.2 ± 0.2%, 87 of 131 ships profitable,
~4,500 boardings, drift 0, ~150 s per run.

## To do

1. **Listen to the music** (user): judge the mix (2 quiet : 1 classical, 20-60 s gaps, 2 s fade-in, 60% volume,
   keys N / Shift+N / [ ]); settings in `Music.gd`. Credits are done (2026-11-02: help panel H → Credits,
   `CREDITS.txt` in the package; `deployment.md`, "Credits and licences").

2. **Engine-failure rescue** (user, 2026-11-02). A **plasma** ship's engine fails in flight (much more often
   than hull failures: nothing is lost but time); the ship coasts on its Kepler orbit. **One** rescue ship,
   newly spawned at the nearest larger station, same class as the police cutters, chases it (cheapest meeting,
   as police and salvage do); no trade ship or liner takes part. After the repair the ship re-plans from its
   current position and velocity. The **owner pays** the rescuer's propellant, crew days and the repair (so
   valuations should count the expected bill). Same building block: salvage-race losers that fly plasma turn
   back mid-flight instead of flying on to an empty meeting point (today they fly on, then home).
   Nuclear-thermal ships do not get engine failures. To settle in the plan: failure rate, repair time,
   where the rescuer goes afterwards, what passengers aboard do.

3. **Patched conics: planets' gravity wells** (user, 2026-11-02). Interplanetary plans run station to station
   around the Sun; the Kepler planner charges only the heliocentric burns (`TrajectoryPlanner.cpp` ~531), so
   leaving and entering a planet's gravity well appears to cost nothing. Measure first (Δv and propellant missing
   per route), then plan proper planet escape and capture arcs for both planners, with the economy effect
   benchmarked. Also the same-planet "local direct arc" approximation.
   **How to keep it fast** (outside review 2026-11-12): `docs/studies/soi_end_caps.md`. Wells as end caps with
   closed-form costs (a burn formula for nuclear-thermal ships; per-station spiral constants for plasma ships),
   the heliocentric planners unchanged for scoring, one exact solve with the true start and target states for
   the plan a ship flies, spirals drawn by the UI from parameters. Its numbers are estimates until measured.
   **Reconsider where Earth Sunward Terminal sits** (user, 2026-11-07; part of the same recalibration). Today it
   orbits at 1,500 km (inside the inner Van Allen belt) and does nothing Low Earth Logistics does not. Options:
   the Sun-Earth L1 point as originally advertised (outside the well, constant sunlight for its sun shield; needs
   a Lagrange-point station kind and non-circular endpoint velocities in the planners), or a high Earth orbit above
   the belts (data change only). The gain for plasma ships is mostly time and radiation (a low-thrust spiral out of
   LEO costs ~7 km/s and days in the belts), not propellant; for nuclear-thermal ships it is propellant. Advice:
   `docs/studies/sunward_terminal_position.md` (L1 preferred, only together with this item).

4. **Account ledger in Statistics** (instead of a full who-pays-whom matrix, which would be ~160 × 160 accounts):
   pick one account (station, its residents, a ship, a faction, Earth, a colony) and see whom it paid and who paid
   it, by category, over time. Needs per-account records in the money journal (today it keeps 6 sectors).

5. **Moon and outer-planet textures** (optional): Triton has no surface map (Europa, Ganymede, Titan and Callisto are ChatGPT-generated maps, Io a USGS mosaic; 2026-11-07);
   Uranus and Neptune use old small ones. Optionally make the package smaller (~230 MB, mostly the planet maps
   as imported textures).

6. **Is the depth visible?** (investigation, outside review 2026-11-12; no code). The simulation models crew
   radiation, boil-off, wear and overhauls, households and labour, colonies, order prices, contracts, failures and
   more. The question is how much of this a watcher can actually perceive, because depth nobody can see costs run
   time and calibration work without adding to the experience. Method:
   - List every simulated mechanism (from `docs/modules/economy.md` and `ships_and_fleet.md`), one row each.
   - For each row record: **where it shows** (map, ship or station model, inspector field, Statistics tab, comms
     log, nowhere); **whether a watcher would notice it switched off** (most mechanisms have a switch in
     `data/economy/*.csv`: run the game with it off and compare what is on screen, not the benchmark numbers);
     and **what it costs** (share of `--profile` time, number of settings to calibrate).
   - The user watches the game for 10-15 minutes at the usual speeds and writes down what they noticed and what
     they wondered about; compare with the table.
   - Result: a table in `docs/studies/depth_visibility.md` with a verdict per mechanism: *visible* (keep),
     *invisible but cheap to show* (propose the display: an event in the comms log, a model state, an overlay, a
     "why" line in the inspector), *invisible and costly* (candidate to simplify or switch off), *invisible but
     needed* (it shapes behaviour that is visible; say which).
   - The user then decides what to surface and what to simplify. Until this is done, prefer making existing
     mechanisms visible over adding new ones.

7. **Bring `docs/game_design.md` up to date** (outside review 2026-11-12). It still describes the first concept:
   "chemical" Hohmann ships and "electric ion" ships (today: nuclear-thermal with Lambert arcs, plasma/fusion
   variable-Isp), the inner Solar System only (stations now reach Ganymede and Titan), greedy dispatch by
   throughput (today: cost-based dispatch, orders, contracts, fleet investment), science stations as net consumers,
   and none of households, passengers, failures, salvage or police. Rewrite it as a short statement of what the
   game is and why (observer sandbox, physical and economic feasibility, 2D by design, technology era), pointing to
   `docs/modules/` for how it is built, so it cannot go stale again in the same way.

## Architecture changes (recommended by an outside review, 2026-11-12; none started)

The foundations (headless-first core, determinism, data-driven content, the benchmark discipline) are sound and
nothing needs a rewrite; in particular **no port to Rust** (the gains, threading and memory safety, are already
covered by the thread pool with pure caches and by the sanitizer runs, and a port could not be checked against
today's byte-identical saves). Four changes, in order of value. Each is a refactor: behaviour must not change, so
the check is the standing one for refactors (test suites, byte-identical saves and the 365-day economy audit,
`--profile` not slower).

1. **Split `Simulation` into state and systems.** Today `Simulation` (`src/simulation/Simulation.hpp`, 982 lines,
   about 240 members) is one class: all running state plus every rule as a member function, spread over 15 `.cpp`
   files by topic (`modules/simulation.md`). Every rule can read and write everything, every file includes the
   whole header, and no rule can be tested without building a whole game.
   - **Target:** a plain `World` struct (stations, ships, treasuries, journal, passengers, derelicts, patrols,
     caches, the random generators, the audits) and one system per topic (market and orders, households and
     labour, passengers, ship operations, dispatch, fleet investment, failures and salvage, patrols, snapshot and
     statistics), each a free function or small class that takes `World&` (or `const World&`) and declares in its
     own header what it needs. `Simulation` remains as the thin owner: it holds the `World`, runs `tick()` in
     today's order and keeps its public interface, so the bridge, the headless app and the tests do not change.
   - **How:** one topic per commit, starting with the ones that have the fewest ties (`StationSystems.cpp`,
     `Statistics.cpp`, `Snapshot.cpp`, which only read; then `Patrols.cpp`, `Failures.cpp`; `Market.cpp` and
     `MissionPlanning.cpp` last). Move code without changing it; keep the tick order and every iteration order.
     The nested helpers (`MissionPass`, `InvestmentReview`) become the systems' own types.
   - **Gain:** each system testable with a small hand-built `World`; `const World&` shows which code only reads
     (what may run in parallel); shorter rebuilds; a clear place for anything new.

2. **Split `godot/scripts/Main.gd`** (2,631 lines). It mixes five jobs. Suggested files, in this order:
   - `BridgeClient.gd`: starting and stopping the bridge, the command file, reading the snapshot, paths, designs,
     stats and porkchop files and their `.seq` files (`_start_bridge`, `_write_bridge_commands`, `_read_snapshot`,
     `_read_paths`, `_read_designs`, `_read_stats`, `_read_porkchops`). It emits signals; nothing else touches
     the files.
   - `PathMotion.gd` (display): cached ship paths and the position maths (`_cache_ship_path`, `_cache_helio_polar`,
     `_rail_point_m`, `_helio_point_m`, `_hermite`, `_station_blend_m`, the planet-frame handling).
   - `DisplayClock.gd`: `_advance_display_clock`, the visual phases and the spin clock.
   - `Berths.gd`: docked slots and easing into a berth (`_rebuild_docked_slots`, `_berth_pose`, `_ease_into_berth`).
   - `FineLines.gd`: fine orbit rings, fine paths and trails.
   `Main.gd` keeps the scene set-up, input, selection and focus, and the per-frame order of calls. Check: the
   shot tours give the same pictures.

3. **A GDExtension instead of the file bridge, only if the player ever gets commands.** For an observer the file
   transport works; its costs are known and paid (sequence files, write-aside-and-rename, rename retries on
   Windows, parent-death handling, the whole snapshot parsed from JSON in GDScript up to 10 times a second). A
   player who gives orders needs a reply to each command and state on demand, which files do badly. Then: build
   the core as a GDExtension library with the same `Simulation` interface, run it on its own thread, and hand
   Godot dictionaries instead of JSON files; keep `spacetrains_bridge` for debugging. The Long Burn already uses
   this pattern (Rust, godot-rust). Not worth doing for its own sake.

4. **Saves that survive data changes, only if long-running games matter** (to the user or to other players).
   Today `load_state_json` refuses a save unless its format version is 20-22 **and** its data fingerprint matches
   (`data_fingerprint()` hashes station ids, populations and storage, ship class masses and capacities, and many
   economy settings). So retuning one hashed number, or adding a station or a ship class, makes every existing
   save unloadable, and the user's long game ends with each such change. Loosening means:
   - split the fingerprint in two: **structure** (the ids of stations, bodies, commodities, ship classes, hulls and
     modules that a save refers to) and **tuning** (every number);
   - a tuning mismatch loads with a notice ("data changed since this save; the game continues under the new
     numbers"); a structure mismatch loads if everything the save refers to still exists (new stations start from
     their data, as in a new game) and is refused only when the save names something that is gone;
   - keep loading older format versions by filling new fields with defaults (as versions 20 and 21 do now), instead
     of dropping them.
   The exact-continuation guarantee ("saved, loaded and continued equals never stopped") then holds only when the
   data is unchanged, which is all it can mean anyway; its test stays as it is.

Looked at and left alone: string ids and `find_if` lookups are everywhere in the core, but nothing shows they
limit the fleet size (not profiled). Profile before touching them.

## Parked (kept in mind, not planned)

- **A thin player stake** (outside review 2026-11-12). The player becomes the treasurer of one faction and
  nothing more: they decide when that faction buys or sells a ship, of which class and tank size, and where it is
  based. Everything else stays autonomous: the ships still dispatch themselves, and the other factions keep
  today's rules (`FleetInvestment.cpp`), so the player competes with the same logic that runs now. The purchase
  valuations the factions already compute are shown to the player as advice (for example "a plasma freighter based at Ceres is
  expected to return 9% a year"). It gives the watching a stake (my ships, my balance sheet, was my purchase
  right?) at a small cost: a switch that takes one faction out of the automatic review, three or four commands
  (buy, sell, re-home, perhaps set a reserve), and a panel. It needs a reply to each command, so it is the case
  that would justify the GDExtension (Architecture 3); a first version can use the command file.
- **A world that changes** (outside review 2026-11-12). The economy now sits in a steady state (unmet demand
  0.2%): year 20 looks like year 2, which is dull to watch for long. Let the map itself change slowly: when a
  colony or a station's population grows past a threshold and its faction has the money, a new station is founded
  at a body that has none (Europa, Callisto, Io, Triton, Uranus, Neptune), starting small, supplied by ships and
  growing or failing on its own economics; stations that stay unprofitable for years could shrink or close.
  Stations are data already and population growth exists (`growth.csv`); what is new is creating a station during
  play (state, orders, reference prices and fares for the new pairs, a model, a save format with a variable station
  list). Related: "Scenarios" above (stations at the outer bodies as a starting condition instead).
- **Sound effects** (outside review 2026-11-12). The game has music (`Music.gd`) but no other sound. Candidates:
  quiet interface sounds (select, open a panel, pause, change speed), a soft chime with a comms-log event by kind
  (emergency, failure, salvage won, new ship), and ambience tied to the focus (a low engine hum near a thrusting
  ship, station ambience close up; silence far out, since space carries no sound, is fine as the default).
  Needs sound files with clear licences, as the music has, and a volume setting separate from the music's.
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
  failures (to do 2) give the on-screen events instead.

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
  never pop in, and planets and moons shading ships and stations (PR #34, 2026-11-03;
  the user's first notes done: Mars pods with flat decks, eclipses, finer Ceres craters);
  save/load (F5/F9); code review and clean-up (PR #24).
- **Stations and planets in motion** (2026-11-06, `BodySpin.gd`, display only): stations turn about their planets
  (true rate at 1×, capped at 4 s a turn), planets spin about axes tilted to their real obliquity, moons locked; the
  simulation stays planar and keeps `theta_rad`; ships ride docked stations and flights blend onto the drawn stations.
  Habitat gravity by design: 1 g for the torus, sphere and bola, 0.38 g for the compact industrial habitats (rock,
  drums, module wheel, carousel). Headless runs of the scene crash on worker-built meshes (dummy renderer only; the
  windowed game runs clean, see `modules/ui_observer.md`).
 and Windows zip built on the Linux PC, tested on Windows 11 (PR #30,
  `modules/deployment.md`).
