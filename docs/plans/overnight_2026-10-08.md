# Overnight log, 2026-10-07/08

Working through `docs/plans/speed_gui_orders.md` without the user (asleep). Decisions taken on my own are marked
**Decision** with the reason; the user reviews them in the morning.

## Log

- 22:25 Plan copied. Defaults: fixed tick 0.1 day, nlohmann/json vendored, hold capacity stays in units.
- 22:50 **9a measured** (`spacetrains_headless --profile`, new `--step-days`; perf is blocked by
  perf_event_paranoid=4, so scoped timers in `src/util/Profiling.*`). 60 days, 1-day steps: run 65.4 s, first step
  (opening dispatch) 50.1 s; ~100% of the time is mission planning (economy, treasuries, investment < 1 ms).
  `estimate_leg` (follow-up legs) 49 s of it, VariableISP plans 46 s (307k RK45 integrations), Kepler 19 s.
  With the GUI's 0.1-day steps: 111 of 600 steps over 50 ms (p90 166 ms, p99 295 ms) = the hitch.
- 23:05 **9f.1 double integrator**: `long double` -> `double` (SciPy reference is float64). Identical 60-day
  report, run 65 -> 33 s, VariableISP plan 10.2 -> 3.2 ms.
- 23:30 **9f.5 parallel planning, exact**: `choose_mission` runs in passes; plans a pass lacks are queued (read as
  infeasible), computed on a thread pool (`src/util/ThreadPool.*`, SPACETRAINS_THREADS, default cores-2), and the pass
  repeats until nothing is missing. Reports with 1 and 14 threads are byte-identical. 60 days: 33 -> 5.9 s, opening
  step 25 -> 4.2 s. **Decision:** `estimate_leg` now plans at the bucket start with base-price crew costs and
  measures travel days from each caller's own departure (before: from whichever ship asked first, at "now"): no
  history dependence, needed for save/load. The 60-day report did not change.
- 23:55 **9b fixed tick**: `Simulation::step()` now runs whole ticks of `TICK_S` = 0.1 day (an accumulator), so a
  1-day headless step and the bridge's 0.1-day step simulate the same game (60-day reports byte-identical). The
  headless benchmark now plans as often as the GUI (reviews every 6 h instead of once a day), so it is a little
  slower than with 1-day steps, but it measures the game the player sees. **New baseline needed (v34).**
- 00:10 **Lambert grid memo**: the Kepler planner's 30x10 Lambert grid depends only on the two bodies and the time,
  not on the payload or fuel; one mission choice plans the same pair many times. Cached (thread-safe, 2048 grids),
  identical reports. 60 days 11.5 -> 5.4 s, opening dispatch 3.3 s (was 50 s at the start of the night).
