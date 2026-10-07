# Overnight log, 2026-10-07/08

Working through `docs/plans/speed_gui_orders.md` without the user (asleep). Decisions taken on my own are marked
**Decision** with the reason; the user reviews them in the morning.

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
