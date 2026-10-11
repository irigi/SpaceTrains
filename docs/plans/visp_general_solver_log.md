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
