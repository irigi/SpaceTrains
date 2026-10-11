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
