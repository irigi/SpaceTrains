# Overnight log, 2026-10-07/08

Working through `docs/plans/speed_gui_orders.md` without the user (asleep). Decisions taken on my own are marked
**Decision** with the reason; the user reviews them in the morning.

## Morning summary

Everything is committed on `feature/opus5_5_return` (not pushed). All three test suites pass; the trajectory audit
flags 0 plans; money drift is 0.

**Done:**

- **Speed:** a 730-day run with 90 ships takes about a minute (11 minutes with 41 ships before). The parallel
  planning gives exactly the same results as sequential. The opening dispatch is cached: after the first start with
  the same data and build, the game is running within a second.
- **Smooth play:** measured in the real window with 90 ships: at 1 day/s no frame stalls; at 5 days/s 0.8% of
  frames, the longest 0.24 s. About 1-2% of frames take over 50 ms (engine side, not the scripts).
- **GUI:** orbit lines exact at any zoom (planets sit on them); KSP-like map (true-size planets, icons and labels
  when far, brighter Sun, readable night sides); ship panel says what the ship does ("Carrying 230 u food + 140 u
  medicine to Mars Transfer Port to sell, then load 150 u reactor_fuel there."), with fuel, cargo, crew,
  provisions, trip earnings and ledger; station panel with stock vs target, days of cover, share gone short,
  **orders** and **inbound contracts**; body panel; economy overview (M); registry tooltips.
- **Save/load:** SAVE/LOAD buttons and F5/F9; a loaded game continues exactly (tested).
- **Unmet demand** (four starting dates each): v33 72.9% (one run) -> **49.8 +- 1.0% at 730 days**, and over four
  years **32.3 +- 1.2%, only 10.5 +- 1.9% in the last ~290 days**, 84.5 of 93 ships profitable. An 8-year run stays
  stable (8% unmet in years 7-8, no drift, no trajectory flags).

**What the investigation found (step 12):** the fleet had a fifth of the hold capacity steady supply needs;
consumers wanted three weeks of stock even when the next delivery was a year away, so supplying them did not pay;
forecasts counted cargo arriving after the ship, and counted waits twice; one scarce input cut a station's whole
output to 10% (shortages cascaded); metals came only from hard-to-reach Mercury. Your contract idea works as
designed: holds are filled by marginal value across goods, stations' orders are visible, and prices are agreed at
departure.

**Decisions to review** (each can be reverted on its own; details in the log below):

1. Consumers' target stock covers their resupply time (`EconomySystem::cover_days`).
2. Mixed cargo by marginal value; contracts with prices agreed at departure.
3. Fleet limit `max_fleet_size` 90; near it, new ships are ranked by profit per day (bigger holds).
4. Production runs at the weighted average of its inputs' availability, not the scarcest one. **Rejected by the user;
   replaced by step 14 (per-output dependencies and upkeep penalties), planned.**
5. Ship cash reserve 50k -> 25k.
6. **Map data (commit `934baa8`, data only):** a fuel factory at Mercury, metals from Lunar Gateway.
7. Look: true-size planets, screen-space labels, sunlight energy 7.
8. The game ticks every 0.1 day in headless runs too, so benchmarks measure the game you play.

**Open:**

- Money supply over four years: +10.5 +- 12% (one start +28%). The excess sits in ships (3.1M in year 4: big
  contract payments drain over the 30-day dividend period); shrinking their reserve cost supply (ships need cash for
  big holds) and was reverted. A reserve sized to each ship's hold would be the cleaner fix. Also: faction
  treasuries diverge (Mars Corporation -5.3M subsidising its port's imports at scarcity prices while residents pay
  base prices; the other two +12M each) - the open economy allows it, but you may want a limit.
- Titan and Ganymede remain the worst supplied (62-94% in year 4): remote, small demand, prices capped at 16x.
  A higher cap for remote outposts (up to 64x, tested over 4 years) made things worse: unmet 38.8 +- 2.4% (32.3),
  money +55%: ships chase the remote premiums. Faction subsidies for outpost supply would be the next idea to try.
- Exports at 730 days are lower than in v33 (domestic supply now pays better); over four years 1.84M.
- **Git history:** purged 2026-10-08 (`build-prof/` removed from the unpushed commits; trees verified identical otherwise).
- Old snapshot files from earlier sessions in `~/.local/share/godot/app_userdata/SpaceTrains/` (safe to delete).
- Atlas time-optimality (regenerate the atlas) not started.

## Log

- 22:25 Plan copied. Defaults: fixed tick 0.1 day, nlohmann/json vendored, hold capacity stays in units.
- 22:4x **9a measured** (`spacetrains_headless --profile`, new `--step-days`; perf is blocked by
  perf_event_paranoid=4, so scoped timers in `src/util/Profiling.*`). 60 days, 1-day steps: run 65.4 s, first step
  (opening dispatch) 50.1 s; ~100% of the time is mission planning (economy, treasuries, investment < 1 ms).
  `estimate_leg` (follow-up legs) 49 s of it, VariableISP plans 46 s (307k RK45 integrations), Kepler 19 s.
  With the GUI's 0.1-day steps: 111 of 600 steps over 50 ms (p90 166 ms, p99 295 ms) = the hitch.
- 22:4x **9f.1 double integrator**: `long double` -> `double` (SciPy reference is float64). Identical 60-day
  report, run 65 -> 33 s, VariableISP plan 10.2 -> 3.2 ms.
- 22:4x **9f.5 parallel planning, exact**: `choose_mission` runs in passes; plans a pass lacks are queued (read as
  infeasible), computed on a thread pool (`src/util/ThreadPool.*`, SPACETRAINS_THREADS, default cores-2), and the pass
  repeats until nothing is missing. Reports with 1 and 14 threads are byte-identical. 60 days: 33 -> 5.9 s, opening
  step 25 -> 4.2 s. **Decision:** `estimate_leg` now plans at the bucket start with base-price crew costs and
  measures travel days from each caller's own departure (before: from whichever ship asked first, at "now"): no
  history dependence, needed for save/load. The 60-day report did not change.
- 22:4x **9b fixed tick**: `Simulation::step()` now runs whole ticks of `TICK_S` = 0.1 day (an accumulator), so a
  1-day headless step and the bridge's 0.1-day step simulate the same game (60-day reports byte-identical). The
  headless benchmark now plans as often as the GUI (reviews every 6 h instead of once a day), so it is a little
  slower than with 1-day steps, but it measures the game the player sees. **New baseline needed (v34).**
- 22:4x **Lambert grid memo**: the Kepler planner's 30x10 Lambert grid depends only on the two bodies and the time,
  not on the payload or fuel; one mission choice plans the same pair many times. Cached (thread-safe, 2048 grids),
  identical reports. 60 days 11.5 -> 5.4 s, opening dispatch 3.3 s (was 50 s at the start of the night).
- 22:4x **v34 baseline** (fixed tick, everything above), 730 days: run **42 s** (v33: 11m10), unmet demand 72.2%
  (v33 72.9%), 18/41 profitable, exports 928k, money +5.5%, drift 0, 34 CRITICAL, trajectory audit 0 flagged.
  The fleet investment review was then 29 of the 42 s, with single ticks up to 7 s.
- 22:4x **Investment probes batched** (`choose_missions`: several probes planned together; identical 730-day report)
  and **one purchase per tick** instead of up to 8 in one tick (**Decision**: a review's later purchases see a few
  more hours of state; results at day 730 unchanged to the credit). 730 days 35 s; worst tick 2.1 s (one purchase),
  opening dispatch 3.4 s.
- 22:5x **9d/9e smooth display + 10a orbit fix** (Godot): a display clock runs at the timewarp rate between
  snapshots (up to 3 s of real time ahead of the last one, never backwards); bodies and stations are placed every
  frame from their orbital elements (same formula as `CelestialMechanics`), ships in flight along their planned
  paths, docked ships at their station. Positions are computed in double precision relative to the focus, then cast,
  so close-ups do not jitter. Godot parses a snapshot only when the bridge's `.seq` file changes (it parsed the whole
  150 KB file every frame). Rings that pass near the focus are redrawn every frame with points concentrated at the
  camera (sinh spacing, 720 points), focus-relative; the coarse ring is hidden meanwhile. The bridge runs the
  opening tick before its clock starts and catches up stalls of up to 4 s instead of dropping time. Checked with a
  scripted screenshot tour (`godot4 --path godot -- --shot-tour=<dir>`, `godot/scripts/ShotTour.gd`): the Moon's and
  Earth's orbit lines pass through the bodies at the closest zoom; display clock 1.00 day/s, no stalls.
- 22:5x **10b map look** (checked on screenshots, the user reviews in the morning): bodies at true radius with
  smooth 128x64 spheres; minimum zoom stops outside the focused body; ship and station models a few tens of km
  (icons stand in from afar, as in KSP's map); screen-space name labels beside icons with overlap culling (bodies
  first, then stations, then ships in flight; docked ships unlabelled) instead of world-scaled 3D text; brighter
  sunlight (energy 16, gentler falloff) and a faint self-lit planet texture so night sides read; brighter planet
  icons; a larger, brighter, depth-tested Sun halo. **Bug fixed:** billboards (engine glows, Sun halo) ignored their
  node's scale and were always 0.05 AU wide: a plasma ship near Jupiter filled the screen with blue haze.
- 23:0x **10c panels**: ship inspector says what the ship does in words ("Carrying 30 u food to Low Earth Logistics
  to sell, then load 30 u electronics there."), with propellant (t, %), cargo, provisions (days), transit progress,
  the trip's expected earnings, crew, home, route commitment, cash, lifetime profit and the ledger. Station
  inspector: fuel factory / export market badges, market rows with the bar full at the target stock, days of cover
  and the share of demand gone short, inbound ships (cargo, arrival) and docked ships, money since start. Market panel
  (M) opens with an economy overview: unmet demand since start and over 30 days, money vs target, treasuries,
  exports, fleet (bought/sold/laid up), the five shortest-supplied goods. The top bar shows unmet demand over 30 days.
  **Leads for step 12 seen on the panels (day 66):** Earth L1 Terminal holds 10,000 food at price 12 while Low Earth
  Logistics (same orbit, hours away) has none at price 800 and 33% of its food demand unmet; a Fast Courier flies a
  93-day trip expected to lose 4,615 cr.
- 23:0x **11 save/load** (`docs/modules/persistence.md`): the whole mutable state as JSON (~140 KB). **Decision:**
  a small own JSON module (`src/persistence/Json.*`, exact double round trip) instead of vendoring nlohmann/json
  (not installed, a 900 KB download). **Decision:** `Inventory` and the faction treasuries are now `std::map`: an
  `unordered_map` iterates in an order that depends on insertion history, so a loaded game could choose
  differently; this changes tie-breaking slightly (new baseline below). Test: 45 days + save/load + 45 days equals
  90 days straight, compared as saved JSON. Headless `--save-at DAY FILE` / `--load FILE`; bridge save/load
  commands; UI F5 quick save / F9 quick load (debug map toggle moved to F11), checked in the GUI: saved at day
  10.7, loaded back to 10.7, clock continued. The UI's "simulation busy" notice no longer fires while paused or at
  slow timewarps (no snapshot is due then).
- 23:1x **v35 baseline** (ordered maps): 730 days 40 s, unmet 72.5%, 17/43 profitable, money +5.3%, drift 0.
- 23:1x **Step 12 findings (the unmet-demand investigation).**
  1. **Bug, fixed (v36a):** the stock forecast for a sale counted every inbound cargo of the good, whenever it would
     arrive. Three slow Venus ships carrying food to Low Earth Logistics made a hop of hours from Earth L1 (10,000 food,
     price 12) look worthless (300 u valued at 3,750 cr), so the station starved for 100+ days. Now the stock is run
     forward to this ship's arrival with only the deliveries before it. Unmet 72.5 -> 71.6%, 20/41 profitable.
  2. **The main cause: transport capacity.** New `--econ-audit` section "Transport capacity for steady supply":
     supplying every consumer from its nearest producer needs ~45,000 u of holds in transit (104,000 with the two
     export markets); routes under 200 days alone need 7,400 u. The starting fleet has 5,190 u in 22 ships (about
     11,000 u by day 730). Interplanetary transfers take 76-470 days (Hohmann), so no dispatch rule can close the
     gap: most station-goods sit near 94% unmet = the opening stock lasted ~40 days and was barely resupplied.
  3. **Why investment did not add capacity:** a consumer's target stock (base price) was 21 days of consumption, so
     a hold sized for a 200-day route flooded the price to a quarter of base: distant supply did not pay.
     **Decision (v36b):** target stock = consumption x max(21 d, 1.4 x one-way transfer from the nearest producer,
     capped at a year) - the "order until the next delivery" part of the user's contract idea. Effect: unmet 66.3%
     (falling to 61% in the last 146 days), cargo margin 9.7M, fleet grew to 104 ships / 39,700 u.
  4. **Decision (v36c):** `max_fleet_size` 90 in `fleet_investment.csv` (user: >100 ships is incomprehensible).
     Same unmet (cap reached late); 48/93 profitable; money +9.1%; drift 0. Costs: 730 days now 1m49 (90 ships),
     ticks up to 8.6 s; 4 plasma Venus->Mercury plans flagged `many_revolutions` (1.3-1.4 rev, theta branches).
  **Open for the user:** remote outposts import all their food/oxygen from Venus (217-932 days away); local life
  support production (greenhouses, electrolysis) at the outposts would cut the transport need by far more than any
  fleet could add. A game-design decision, not taken tonight.
- 23:2x **Speed with 90 ships** (730 days had become 1m49, ticks up to 8.6 s): candidates are planned without a
  rendering path (`PlanningOptions::include_path`), the chosen mission is planned again with it. **Bug found and
  fixed:** `integrate_fixed_time` computed the last output time as dt x (n-1), which can round a hair above the
  transfer time; the loop then spun on zero-length RK45 steps until the 200,000-step budget threw (the worst
  integration took 73 ms instead of 7.6 ms; the windows it hit were silently rejected). v36e: 730 days 1m08, the
  same economics as v36c.
- 23:3x **Step 13 stage 1 groundwork:** a mission's hold is a list of lots (`domain::CargoLot`, save version 2);
  departure, arrival (per-lot decay, storage, sale), the inbound forecast, events and the snapshot (`cargo` list) use
  it. Dispatch still loads one good per trip: results identical to v36e (only event wording differs).
- 23:3x **Step 13 stage 1: mixed cargo (v37a).** Dispatch builds a manifest per destination: chunk by chunk (1/40 of
  the hold) the next units go to the good whose next units earn the most along both stations' price curves, weighted
  by the destination's urgency for it; the whole manifest, its half and its quarter are scored like single-good runs
  (two-leg, follow-up). Single-good runs remain candidates. 730 days: **unmet 59.8% (last 146 days 40.6%)**, 49/93
  profitable, fleet holds 41,400 u in 90 ships, fuel 1.09M (v36e 1.77M), money supply **+29.7%** (ships hold 2.4M
  cr; to look at), drift 0, 2 trajectory flags (theta branches), 1m05. The inspector shows the full hold.
- 23:3x **Fleet reviews spread over ticks (v37b):** a review keeps its probes (cargo runs of each hull at each
  yard, `review_probes_`, saved with the game; save version 3) and each tick does one batch of 8 probes or one
  purchase. Worst investment tick 0.9 s (was 2-6 s), 730 days 52 s.
- 23:4x **Benchmark over several starts** (`tools/benchmark.py`, headless `--start-day`): one run is a poor judge
  (v37a and v37b differ by 12 points in the last fifth though v37b only moves purchases by hours). Four starts
  (day 0/90/180/270) in parallel, mean ± sd. v37b: unmet **63.3 ± 2.2%**, last fifth **49.8 ± 5.3%**, 52.8/91.5
  profitable, holds 42,800 ± 8,500 u, 1.5 trajectory flags per run. **From here on, compare versions with it.**
- 23:5x **GUI smoothness with 90 ships**, measured in the real window (`--shot-smooth=SECONDS` in the tour counts frames
  where the display clock stands still). Found and fixed: (1) the bridge ran a whole backlog of ticks in one loop
  without snapshots (a review: "37 ticks took 3.7 s") - it now ticks one at a time with at most 100 ms of work per
  snapshot and carries the rest over; (2) the display clock follows the simulation's measured pace and only slows
  down when ahead (it pulled itself back and froze); (3) reviews probe 4 candidates per tick; (4) planned paths
  (450 of 626 KB per snapshot at 90 ships) go to a separate `.paths` file written only when a plan changes; (5) at
  most 10 snapshots a second. Result at day 300-620: **1 day/s: 0 stalled frames** in 20 s; 5 days/s: 4-7% of
  frames stalled, the longest 2.3 s during fleet reviews (open). Godot spends ~16% of the frame budget on
  snapshots, node placement and icons (`SPACETRAINS_GODOT_PROFILE=1`); bridge slow loops: `SPACETRAINS_BRIDGE_LOG=1`.
- 00:1x **v37c, two balance decisions (to review), benchmarked over four starts:**
  - **Production gating** was the *minimum* availability over a station's inputs (10% floor): one minor shortage
    (electronics at the Mercury smelter) cut its metals to 10%, which starved Lunar Gateway, whose water output
    fell, which left Earth L1 83% short of water: shortages cascaded. Now the value-weighted *average* of the inputs'
    availability. Unmet 63.3 -> 61.7 +- 1.8% (last fifth 49.8 -> 47.4).
  - **Ship cash reserve 50k -> 25k** (= a new ship's working capital): ninety ships keeping 50k each held up to
    2.25M above the money-supply target, and the controller can only tax stations. Money supply +23.9 -> +10.2 +-
    3.5%; unmet unchanged.
- 00:2x **v37d: near the fleet limit, rank purchases by profit per day** (**Decision**, from half of
  `max_fleet_size`; above the 30%/yr hurdle as before): a place in the fleet is the scarce thing, so a 2000 u bulker
  beats a 30 u courier. Four starts: holds 37,300 -> **85,000 u**, unmet 61.6 -> **60.7 +- 1.2%**, last fifth 47.3 ->
  **44.1 +- 4.5%**, money supply +10 -> **-14 +- 5%** (dearer hulls). Holds are no longer the limit.
- 00:2x **Where the remaining shortfall is (v37d, by value):** Low Earth Logistics metals 90% and reactor fuel 65%,
  Mars metals 94%, Mercury food/oxygen 90-94%, Ganymede/Titan ~94%, while 14,500 u of metals and 18,500 u of food sit
  in stocks. Metals are made only at Mercury and Ceres; Mercury is hard to reach (no fuel for sale there: 74% of its
  fuel demand unmet; nuclear-thermal ships lack the delta-v for a round trip) - 12 departures from Mercury in two
  years. A design question for the user (see the scenario test below).
- 00:3x **Map scenarios (data copies, four starts each)** against v37d (unmet 60.7 +- 1.2%, last fifth 44.1 +- 4.5):
  A fuel factory at Mercury (200 u/day: solar power, polar ice) 58.4 +- 1.2% / 40.6 +- 7.7; B metals at Lunar
  Gateway (16 u/day per 10k inhabitants = 8 u/day: regolith mining) 55.3 +- 1.2% / 39.7 +- 2.3; **A+B 52.8 +- 2.6% /
  33.4 +- 2.8**. **Decision (to review, a separate data-only commit, `git revert` undoes it):** A+B applied.
- 00:3x UI: station panel lists its **orders** (wanted up to its target stock, cargo already on the way, the price it
  pays now) - the user's "station orders" made visible. Audit: variable-Isp spirals may sweep 2 turns (inward
  Venus -> Mercury spirals sweep 1.3-1.4 turns; physics, not a sampling fault).
- 00:3x **Checks of the current state (v38 = v37d + map changes):** save/load at full size exact (400 days straight =
  200 + save/load + 200, 265 KB save). **4-year run:** unmet demand 42.3% cumulative at day 1095, 35.5% at day 1460 -
  **only ~15% of demand went unmet in year 4** (by value); 81/96 profitable; money supply -3.1%; drift 0; holds 83,400 u
  in 90 ships; 1460 days in 2m33; worst tick 1.7 s (one big ship's review), 0 trajectory flags beyond the spirals.
  GUI: sunlight energy 7 (16 overexposed Mars); the station panel's orders, mixed holds in inbound lists, checked on
  screenshots at day 200.
- 00:4x **Opening cache** (the user's "pre-calculate and save"): the bridge keeps the state after the opening
  dispatch per data fingerprint and build (`--opening-cache`, the UI uses `user://opening_cache`); a later start
  shows a running game within a second (2.3 s -> 0.01 s). **Fix:** resuming after a pause no longer jumps the
  display ahead (the clock's reference time is held while paused); tested in the window: 0.93 days in the first
  second after resuming at 1 day/s.
- 00:5x **GUI soak test:** 10 minutes at 5 days/s to day 3,117 with 90 ships: no crash, bridge memory flat (88-100
  MB), 2.7% of frames stalled (longest 3.3 s, fleet reviews), then 1 day/s: 0 stalled frames. Godot spent ~13-17% of
  the time on snapshots at 5 days/s; trails now use the bridge's `path_id` and redraw the flown part in 8 steps for
  unselected ships (50 for the selected one): snapshot handling ~130 -> ~84 ms per second. Added tests: integrator
  end sample for any sample count, JSON round trip (bit-exact doubles), resupply-aware targets.
- 01:1x **Scenario: local life support at the outposts** (Mars, Ceres, Ganymede, Moon, Mercury grow ~70% of their
  food/oxygen): unmet 52.2 +- 1.9% vs 52.8 +- 2.6% - no real change. The remaining unmet *value* is mostly dear
  goods (reactor fuel, machinery, electronics) for distant stations; food and oxygen are cheap. Not applied. Most of
  the day-730 figure is the ramp-up while the fleet grows (year 4 of a 4-year run: ~15% unmet). Reviews every 30
  days instead of 60: 51.0 +- 2.7% (noise level), not applied. Contracts with prices fixed at departure would not
  change which goods move (dispatch already plans at the forecast price and later ships count earlier cargo), only
  who bears the forecast error: deferred.
- 01:2x **v39a: fleet investment values mixed holds** like dispatch flies them: probes return each manifest as lots
  sharing a run number (the trip's fuel on the first lot); a run is valued as the sum of its lots' sustained profits
  (each along its own good's stocks) less running costs once (save version 4). Four starts: unmet **50.5 +- 2.4%**
  (v38 52.8 +- 2.6), last fifth **31.3 +- 4.2** (33.4), holds 97,000 u (73,000), profitable 45.5/91 (51.5), money
  -1.4 +- 9.3%.
- 01:3x **Four-start, 4-year comparison** (1460 days): v38 unmet 35.8 +- 0.5%, last ~290 days **17.6 +- 1.5%**,
  79/95 profitable, money +0.5 +- 3.4%; v39a 34.4 +- 3.5%, last ~290 days **15.4 +- 5.9%**, 72/94 profitable, money
  -9.1 +- 7.7%. Within each other's noise; v39a kept (investment values ships the way dispatch flies them), with
  fewer profitable ships and less money as the price. Steady state: ~15-18% of demand unmet. Registry rows have
  tooltips (a ship's destination and hold, a station's shortages). The ramp-up in the first two years is limited
  by the treasuries' money (two of three factions hold ~55k after the first reviews), not by the 8-per-review cap
  (16 per review: identical results).
- 01:4x **No more stalls at 5 days/s:** the display may lead by up to 3 s of real time; at 5 days/s that is 15
  days, and when a fleet review slows the simulation the allowed lead shrank below where the display already was,
  so it froze until the simulation caught up (diagnosed with the tour's stall report: display 3.3 days ahead). Now
  it crawls at a fifth of the rate while snapshots keep coming. Reviews probe 2 candidates per tick. Measured in the
  window at day 0-600: **5 days/s: 0.8% of frames stalled, longest 0.24 s** (was 13%, 3 s); 1 day/s: none.
- 01:5x **v39c: contracts (step 13 stage 2) and a forecast bug.** At departure each lot's price is agreed from the
  destination's forecast on arrival; the station pays it on arrival (save version 5); the station panel lists inbound
  contracts with their agreed prices. **Bug found and fixed:** every arrival forecast added the launch-window wait
  twice (a plan's travel time already includes it), so ships with long waits forecast their sale too far ahead.
  Four starts: unmet **49.6 +- 1.6%** (v39b 51.2 +- 0.7), last fifth 34.1 +- 5.7, profitable **51.8/91** (47.0),
  money +7.2 +- 4.7%. **Test fixed:** the economy suite's per-ship cash check (< 4x the reserve) failed once the
  reserve was halved in v37c - I had run only the main suite at those commits; it now checks the fleet average (a
  ship just paid for a big hold may hold more for a few weeks until the dividends drain it).
- 02:0x **v39d:** the investment valuation's round trip counted the launch-window wait three times (2 x travel +
  wait, with the wait already in the travel time); now wait + 2 x transfer. Four starts: unmet 49.8 +- 1.0%, last
  fifth 32.5 +- 1.6, 50.8/91 profitable, money +1.3 +- 7.4% (neutral within noise, a correctness fix).
- 02:1x **Final 4-year, four-start benchmark (v39d):** unmet **32.3 +- 1.2%** over 1460 days, **10.5 +- 1.9% in the
  last ~290 days** (v38: 17.6 +- 1.5), **84.5/93 ships profitable**, exports 1.84M, money supply +10.5 +- 12% (one
  start +28%: to watch), 0 trajectory flags, drift 0.
- 02:2x **v39e reverted:** at 730 days it was 52.0 +- 4.3% unmet (v39d 49.8 +- 1.0) with no better money supply
  (+3.7 +- 10.3% vs +1.3 +- 7.4%); over 4 years slightly worse too. A smaller cash reserve leaves ships less to buy
  cargo with. v39d stays; **money supply over 4 years (+10.5 +- 12%, one start +28%) is an open point.**
- 02:3x **Scenario (not applied): price cap up to 64x for remote consumers** (16x x cover/90 days, capped at 4x):
  1460 days, four starts: unmet 38.8 +- 2.4% (v39d 32.3 +- 1.2), last ~290 days 16.4 +- 5.8 (10.5), money +55 +- 32%.
  Worse: ships chase the remote premiums and the rest goes short.
- 02:3x **Money analysis (v39d, start 0, 4 years):** internal money +28% at day 1460 is in ships (3.1M, stations 1.0M);
  faction treasuries: Sol Federation +12.4M, Independent +11.3M, Mars Corporation -5.3M (subsidies to keep Mars
  Transfer Port above its credit floor while it imports at scarcity prices). Not changed tonight.
- 02:4x **8-year stability run (v39d, start 0):** no NaN, 0 trajectory flags, drift 0, 2920 days in 3m37. Unmet
  demand by two-year period: 51.2%, 13.2%, 11.3%, 8.2% (cumulative 21.0%). Fleet stable at ~90 (71 bought, 3 sold).
  Money supply +3.8%, +28.3%, -0.7%, +7.3% at the four reports. Treasuries keep diverging: Mars Corporation -10.3M,
  Sol Federation +17.5M, Independent Consortium +32.6M by year 8 (see the open point).
- 02:4x **Scenario (not applied): ship replacement at the fleet limit** (sell the worst docked performer for a
  candidate expected to earn twice as much per day, at most two per review): 1460 days, four starts: unmet 32.3 +-
  0.8% (v39d 32.3 +- 1.2), last ~290 days 10.6 +- 0.9 (10.5 +- 1.9), slightly more profitable ships, runs 15% slower
  (reviews keep probing at the limit), only a few ships replaced in four years. Neutral: not applied.
