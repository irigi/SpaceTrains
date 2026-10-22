# General-boundary variable-Isp solver — session log (2026-10-11/12)

Autonomous overnight session. Goal: a fast solver for constant-power variable-Isp transfers between
arbitrary states (chase primitive for pirates/police), compared against the atlas + refine planner.
Plan: `~/.claude/plans/a-new-task-for-resilient-torvalds.md`. Directions considered: own brainstorm +
`docs/plans/variable-isp-solver-directions.md` (Astra).

## Code

- `src/variable_isp/GeneralTransfer.{hpp,cpp}` — normalised Cartesian primer formulation
  (r, v, p = thrust, w = p′, w′ = G(r)p, E′ = |p|²), adaptive DOPRI5 with the analytic 9x4 transition
  matrix, Levenberg-Marquardt / minimum-norm shooting on (p0, w0[, T]) for rendezvous or intercept,
  fixed or moving targets, fixed time, minimum-norm free time, or effort budget. Seeds: free-space
  cubic, polar time-shape (Hermite + Legendre bubbles, Gauss-Newton), angle-shape, primer fit along a
  reference path.
- `tools/visp_bench/main.cpp` (`spacetrains_visp_bench`) — case sets from the atlas and comparisons.

## Findings

1. **Atlas costates map exactly to the Cartesian primer** (Astra's conversion formula verified: rollout
   reproduces the atlas end state to ~1e-9). Rollout of a transfer costs 0.03–0.1 ms.
2. **Many atlas cells are not effort minima for their own endpoints and T**: other seeds find a lower J
   on ~30% of game-range cells. 37% of game-range cells (ρ 0.25–12, κ ≤ 2000) dive below 0.1 AU, which the
   planner rejects anyway.
3. **Time-parametrised polar shapes** converge to the true optimum as terms grow (16 terms → 0.05% on
   regular arcs) but cannot follow fast perihelion passages. Shooting seeded by a primer fit along the
   shape solves 85% of all atlas cases (95% of cases with perihelion > 0.3) at ≤ atlas J, ~2 ms.
   The free-space cubic seed alone: 71% (84% for perihelion > 0.3), 0.2 ms median.
4. The angle-parametrised shape (r(θ), t(θ)) failed to initialise on most cases (cubic t(θ) not
   monotone when the mean angular rate differs from the end rates); parked.

## Adopted: STM refine in the planner

`VariableIspIntegrator::refine_seed_stm` solves the planner's refinement (atlas seed onto the station,
moving target, T free with a minimum-norm step) in primer form with the analytic transition matrix and
maps the result back to atlas costates, so the rest of the planner is unchanged. It also rejects paths
diving below half the perihelion limit early.

| | refine_seed (FD) | refine_seed_stm |
|---|---|---|
| bench R (286 usable cells, nearby targets) | 284 ok, t50 0.255 ms | 282 ok, t50 0.094 ms |
| in-game per call (365 d profile) | 0.57 ms | 0.22 ms |
| VISP planning CPU, 365 d | 356 s | 174 s |
| headless 365 d wall | 44.6 s | 26.0 s |
| sweep endpoint miss max / flags | 0.0013 AU / 57 | 0.0001 AU / 41 (no coarse_sampling) |
| benchmark.py (4 starts, 730 d) unmet | 4.9 ± 0.8 % | 4.8 ± 0.5 % |
| benchmark.py run time | 218 s | 122 s |

The two refines converge to slightly different T (minimum-norm in different variables): T within 2%,
J within 6% on bench R; both are valid solutions of the same equations.

## General solver results (fixed T, `spacetrains_visp_bench <set> --limit N`)

"valid" = converged with perihelion ≥ 0.1 (normalised); "best" = within 0.1% of the lowest effort any
method found for that case. Times are single-thread on this machine.

| set | cases | method | valid | best | t50 | t90 |
|---|---|---|---|---|---|---|
| AG: atlas circular→circular, no atlas seed | 300 | atlas's own solution | 63% | 58% | – | – |
| | | solve_transfer | 99.3% | 94% | 0.76 ms | 7.9 ms |
| B: atlas endpoints, velocities kicked ≤30% | 200 | solve_transfer | 99% | 96% | 0.66 ms | 7.2 ms |
| C: chase a coasting target (≤6 months) | 200 | solve_transfer | 100% | 100% | 0.39 ms | 0.67 ms |
| | | zero-primer Newton alone | 99% | 99% | 0.06 ms | 0.2 ms |
| D: hard chase (e ≤ 0.6, ≤1.6 yr, any phase) | 200 | solve_transfer | 99% | 96.5% | 0.68 ms | 6.7 ms |
| E: chase a thrusting variable-Isp ship | 880 | solve_transfer | 100% | 99.5% | 0.67 ms | 2.2 ms |

Seed/method comparison (share of solvable cases where the method finds the best solution):

| method | AG | B | D | E | notes |
|---|---|---|---|---|---|
| zero primer (= LQ/Gramian about the ballistic arc) | 78% | 82% | 83% | 92% | 0.2 ms; first try |
| boundary homotopy from the ballistic arc | 86% | 88% | 89% | 95% | heavy tail |
| free-space cubic (Astra D2 seed) | 77% | 79% | 72% | 89% | |
| polar shape → primer fit → single shooting | 89% | 89% | 94% | 98% | 0.5 ms (6 terms, 24 nodes) |
| shape → multiple shooting | 91% | 89% | 91% | 97% | needs row equilibration |
| time continuation from short T | 80% | 84% | 86% | 96% | robust but slow, finds other branches |

`solve_transfer` = warm start (if any) → zero primer → shape fit (always, keeps the lower effort) →
multiple shooting → boundary homotopy → time continuation (fallbacks only when nothing valid yet).
`solve_min_time` (geometric T grid, warm starts, budget refinement) on set E: 95% found, 1.9 ms median.

Not pursued: Astra D1 (atlas continuation in end velocities) — the zero-primer/shape seeds already
solve arbitrary velocities without the atlas, and the atlas only covers circular ends. Astra D3 (SCvx
with a QP solver) — the shape method is the same idea (direct method) without a QP dependency.

## Adopted: fit near-budget refinements to the fuel (2026-10-11 late)

Planner instrumentation (365 d): 237k candidates, **56% rejected as fuel-limited** after refinement,
16% refine failures, 2% perihelion; 17% of VISP plans found no window. `fit_effort_by_time` changes T
(safeguarded secant over warm-started fixed-T shootings, direction from dJ/dT = −2h) until the effort fits.
In game, fits within 10% over budget succeed 88%; beyond that about half fail (the effort minimum at that
departure is above the budget), so the planner only fits up to 10% over (`max_budget_overshoot`).

| | STM refine only | + fuel fit (≤10%) |
|---|---|---|
| no-window plans (365 d) | 13064 / 75570 | 10153 / 73016 |
| headless 365 d | 26.0 s | 30.2 s |
| benchmark.py unmet / profitable / run | 4.8 ± 0.5 % / 51.2 / 122 s | 4.6 ± 0.5 % / 54.0 / 138 s |

## Planner speed-ups (v35, 2026-10-12 night)

1. **Scoring plans skip the re-integration**: `refine_seed_stm` returns the end state, the mass (from the
   effort) and the closest approach measured at every integrator step. Cross-checked in game against the
   old 600-sample integration: mass within 0.002 kg canonical, end radius within 1e-6; the closest approach
   differs only on deep dives, where the 600 samples missed the true perihelion (the new value is exact).
2. **One refinement pass**: the station-targeting pass starts from the atlas seed; the fixed-target pass
   before it is gone (the launch wait now comes from the seed's T). Sweep: 68854 vs 67468 plans found.

| 365 d headless | baseline | STM refine | + fuel fit | + v35 |
|---|---|---|---|---|
| wall | 44.6 s | 26.0 s | 30.2 s | 22.9 s |
| VISP planning CPU | 356 s | 174 s | 203 s | 89 s |
| sweep plan ms p50 / p99 | 2.9 / 44.9 | – | – | 1.4 / 10.5 |

benchmark.py (4 starts × 730 d): unmet 4.8 ± 1.0 %, profitable 53.8, emergencies 5.0, **run 91 s (baseline 218 s)**.
The sweep shows 10 new `coarse_sampling` flags (of 68.8k plans): close solar passes on long transfers that are
now found; rendering only.

## Kepler planner: shared Lambert rows (2026-10-12 night)

The Lambert grid (30 departures x 10 transit times) was memoised by the exact plan time, so plans at
successive steps recomputed all 300 solves. Departure rows are now anchored to multiples of the grid step
(plus a fresh "leave now" row) and cached by bodies and absolute departure: 0.95 → 0.14 ms per plan, the
365-day headless run 22.7 → 19.1 s. Sweep: no Kepler flags. benchmark.py over 8 starts: unmet ≈ 5.3 % both
before and after (5.3 ± 0.9 and 5.3 ± 2.0 on the two 4-start sets vs 4.8 ± 1.0 and 5.9 ± 2.9 before).

## Direct fallback for plans without an atlas window (built, off by default)

With planner instrumentation: 13% of VISP plan requests found no window. 7.8% of all requests have κ below
the atlas grid (weak or laden ships, e.g. Mercury → Jupiter); the general solver serves 68% of those but
at ~36 ms each (minimum-time search over long multi-revolution spirals), too slow for routine planning —
extending the atlas to lower κ offline would be the cheap way. Of the in-grid requests without a window,
60% have a direct transfer leaving now (mostly 100–300 days), ~5–16 ms each.

`PlanningOptions::direct_fallback` (VariableIspTrajectoryPlanner) plans those directly between the
stations with `solve_min_time`. Sweep: +4120 plans (+6%), audits clean (3 many-revolution flags). Economy
(4 starts): unmet 5.1 ± 0.9 % vs 5.3 ± 0.9 %, no measurable gain, while the 365-day run took 25 s instead
of 17 s. Left off.

`Propagator::sample` now integrates once and fills output times by cubic Hermite between steps.
A "walk" minimum-time strategy (one short-T solve, then secant steps up the effort curve) was tried and
removed: fewer requests solved (154 vs 162 of 173 on set E; 129 vs 268 of 400 low-κ), no faster.

## DOP853 for the general solver

`PropagationSettings::high_order` (default on) uses DOP853 (tableau from SciPy) instead of DOPRI5. At the
general solver's 1e-10 tolerance `solve_transfer` medians drop: B 0.78 → 0.59 ms, C 0.41 → 0.35, D 0.62 →
0.56, E 0.74 → 0.51, AG 0.76 → 0.57, same results. The planner's refine (1e-8) stays on DOPRI5: DOP853 is no
faster there, and loosening to 1e-7 brings endpoint misses back to 0.0012 AU.

Slow ticks after all planner work (365 d, 0.1-day ticks): p99 359 → 112 ms, ticks over 50 ms 138 → 62, the
opening tick 2.7 → 0.96 s. The remaining slow ticks are fleet-investment probe batches (every ~60 days,
100–250 ms for ~7 ticks), VISP refinements in parallel.

## Spending the whole fuel budget (built, off by default)

`ShootingSettings::spend_fuel_budget` + `spend_effort_by_time`: shorten a refined transfer until it uses all
its fuel (minimum time for the fuel), staying above the perihelion limit. Planner test (365 d): same plans
found, planning +45%, coarse_sampling flags 10 → 29 (closer solar passes). Economy (4 starts): unmet
5.1 ± 1.1 % vs 5.3 ± 0.9 %, exports 1.31 M vs 1.21 M (noise). Left off.

## Shared refinements (2026-10-12 ~00:10)

91% of VISP plan requests repeat an earlier request's route and plan time (other payload or fuel). The
station-targeting refinement of an atlas cell does not depend on the ship, so it is cached per (route,
time, cell) without the fuel fit; each request applies its own fuel check (mass from the cached effort)
and the ≤10% fuel fit. Different kappas pick different cells, so only 32% of refinements are saved:
planning CPU 65 → 52 s per simulated year, headless 365 d 16.8 → 14.7 s, the same 88,602 plans.

## Where the session left the game's speed

| 365 d headless, this machine | session start | now |
|---|---|---|
| wall | 44.6 s | 14.7 s |
| VISP planning CPU | 356 s | 52 s |
| Kepler planning CPU | 50 s | 6.5 s |
| tick latency p99 / ticks > 50 ms / opening tick | 359 ms / 138 / 2.7 s | ~110 ms / ~45 / 0.9 s |
| benchmark.py 4 × 730 d | 218 s | ~85 s |

Godot side (headless profile at 432000x, ~120 ships): ~3.5 ms of script per frame at 60 fps (entity
updates 1.6 ms, snapshot apply 0.8 ms, map icons 0.5 ms), worst ~25 ms when a snapshot with new paths
arrives; path meshes are already rebuilt per ship only when its plan changes. Not changed.

## Real-time knobs for solve_transfer

| set (300) | default valid / t90 / t99 | `fallbacks = false` valid / t90 / t99 |
|---|---|---|
| B | 99.3% / 5.3 ms / 87 ms | 95.3% / 4.6 ms / 38 ms |
| D | 99.3% / 4.5 ms / 167 ms | 95.7% / 4.3 ms / 22 ms |
| E | 100% / 1.2 ms / 16 ms | 98.5% / 1.3 ms / 13 ms |
| AG | 99.3% / 4.6 ms / 147 ms | 96.7% / 3.9 ms / 49 ms |

For a game AI deciding chases every few seconds, `fallbacks = false` bounds the tail at a few percent of
unsolved requests. A shorter leash for the zero-primer attempt (`zero_max_propagations`,
`zero_stall_iterations`) changed little (kept at 60 / 8).

## Receding-horizon chase (bench `REPLAN=1`)

Pursuer with 3x the reference effort; four times during the chase the quarry dodges (5% velocity kick, then
coasts) and the pursuer re-solves from its current state with the fuel left: `solve_transfer` warm-started
from its current primer at the old remaining time, then `fit_effort_by_time` / `spend_effort_by_time` to the
remaining fuel, `solve_min_time` as a fallback.

| set | chases completed | replans ok | replan t50 / t90 / t99 |
|---|---|---|---|
| C (coasting quarry) | 144 / 150 | 586 / 592 | 0.36 / 0.49 / 1.6 ms |
| E (quarry mid-transfer) | 64 / 67 | 263 / 266 | 0.35 / 0.50 / 1.0 ms |
| D (hard) | 137 / 149 | 574 / 584 | 0.49 / 1.5 / 15 ms |

Sub-millisecond replanning makes a pirate/police AI that re-plans every few game days cheap.

## Options for the next steps (decisions for Jan)

1. **Wire chases into the game.** `trajectory/ChaseSolver.hpp` (`plan_chase`, `path_target`) is ready for
   pirates/police: earliest intercept or rendezvous of another ship's planned path for a given power and
   propellant, ~1–3 ms; replanning after a dodge ~0.4 ms. Needs game design (who chases whom, capture rules).
2. **Low-κ atlas extension.** 7.8% of plasma plan requests have κ below the atlas grid (0.1) and are always
   infeasible (e.g. Mercury → Jupiter, heavy loads). The general solver serves 68% of them, but at ~36 ms
   each; generating extra atlas κ levels offline with the C++ solver (minutes to hours) would make them cheap.
   Caveat: the similar +6% from the direct fallback showed no economy gain.
3. **Turn on the built options** if the gameplay wants them: `PlanningOptions::direct_fallback` (+6% plans),
   `ShootingSettings::spend_fuel_budget` (time-optimal use of the fuel: faster, closer solar passes). Neither
   moved the economy benchmark; both cost planning time.
4. **Pursuit-evasion** (RocketsChaseAstra.tex): the min-time and replanning primitives are the inner loop;
   the game-theoretic layer is not started.
5. ~~**3D**~~: not planned. The game is 2D by design (user, 2026-10-15); see `docs/game_design.md`.

## Atlas extension to low κ (2026-10-22, option 2 above; branch `feature/atlas_fill`)

**Why.** The planner rejects a request whose κ is below the atlas grid (0.1). κ scales with (origin
radius)^2.5, 0.093 at Mercury, so in a 730-day game 18.4% of plasma plan requests fell below the grid, 82% of
them leaving Mercury (also the cheaper part-tank budgets of heavy ships). 55,871 requests found no plan, 40,528
of them from Mercury; there were no reference fares Mercury → Mars, Ceres, Ganymede, Titan.

**Tool.** `tools/atlas_fill` (`spacetrains_atlas_fill`) solves atlas cells with the general solver: a
wavefront from solved neighbours (κ+1 first: the path does not depend on κ, only the time), `shoot` onto the
cell's target, then `fit_effort_by_time` / `spend_effort_by_time` to the budget 0.999 κ (minimum time on the
branch, perihelion ≥ 0.26 canonical when shortening, ≥ 0.1 accepted). Each new record is checked by
integrating the polar costates with `VariableIspIntegrator` (the planner's tolerances, m_end ≥ 0.998 m_dry).
The polar ↔ primer mapping is shared with `refine_seed_stm` (`primer_from_costates`, `costates_from_primer`).
Deterministic (synchronous passes), 8 minutes on 16 threads.

**What is in the game.** `--extend 14 --below-kappa 0.1`: 14 levels at the grid's own step (×1.279), κ down
to 0.0032, covering 99% of the below-grid requests (99% quantile 0.0041). 77,567 new cells; at ρ 2-30 (the
game's outward routes) every level from 0.0041 up is 100% solved. The old cells are untouched. Below-grid
requests fall from 46,885 to 715 and plan failures from 55,871 to 3,410 per 730 days. Reference fares exist
for all 72 pairs (Mercury → Mars 883 cr / 173 d, Mercury → Titan 3,558 cr / 734 d).

**Speed.** The new plans cost time: refinement calls +21%, mean cost +51% (slow spirals integrate long);
47-56 → 64-80 ms per simulated day. Loosening the refinement's integration (`refine_seed_stm`, DOPRI5
1e-8 → 1e-7 relative) recovered it: endpoint misses unchanged (≤ 0.0001 AU), bench set R still 98.6-98.7%
refined, median 0.11 → 0.07 ms. Final: 52-61 ms per day, slowest 20-day stretch 5.1-6.7 days/s (3 starts).
The flown path is still integrated at 1e-8.

**Not taken: filling the old grid's holes.** The same tool also fills 69% → 82% of the old grid (147k cells),
best economy (unmet 0.8%) but another ~12% planning time that the refinement tolerance did not cover (4 days/s
before the tolerance change). Fuel slack in the new cells (one κ level) did not help, so the cost is probably
the hard regions the holes sit in. Possible later, with more speed work.

**Results** (8 starts × 1,460 days, against the overvaluation-fix baseline): unmet 1.55 → 1.11%, last fifth
1.96 → 1.98% (noise: 1.64% in a near-identical run), 137 ships (was 144), 114 profitable, money −12.5 ± 16%,
4-year run 122 → 140 s. Purchase check: cargo earned 96 → 76 %/yr (more ships serve Mercury, whose earlier
few buyers earned 196%), passengers 51 → 46%. The audit's continuous-thrust revolution limit is now 4: low-κ
Mercury legs spiral 2-3.3 turns in 300-530 days, with exact endpoints.
