# Notes for SpaceTrains from The Long Burn port (2026-11-02)

**Status (2026-11-02, `feature/solver_frame`):** 1 and 2 fixed as suggested (`docs/modules/trajectory.md`,
"General-Boundary Variable-Isp Transfers"); on the 2,000-case check below the old code had 13 effort differences
and 3 one-sided cases, the new code none. 3 not done: never seen in the planar solver, and the game stays 2D.

The Long Burn ported `src/variable_isp/GeneralTransfer.*` to 3D in Rust. On SpaceTrains' own planar cases the port
gives the same answers (397/397 solved cases, efforts equal to 1e-6). Property tests on the port turned up the
items below. Each was then checked against SpaceTrains' unmodified C++ (commit `d97226a`).

## 1. `solve_transfer` depends on the in-plane orientation of the problem (worth fixing)

**Finding.** The same transfer, rotated about the z axis by a random angle (start state and coasting-target
state both rotated), does not always give the same answer. Test: 2,000 cases drawn like `visp_bench` sets C
and D (half each), `min_radius` 0.1, default options:

- both orientations solved: 1,992; **effort differs by more than 1e-6 in 8 of them**;
- **solved in only one orientation: 3**.

So about 0.55% of cases change outcome with orientation. In the game, stations and ships sit at every angle
around the Sun, so this is a small, real loss of robustness and a source of non-reproducible plans.

**Cause.** `shoot` and `multiple_shoot` scale the Jacobian columns per coordinate (`scale[c]` from the column
norm, like scipy's `x_scale="jac"`), and Levenberg-Marquardt damps with `λ diag(JᵀJ)`, also per coordinate.
Neither is invariant under rotation, so the iterates, and in marginal cases the branch found, depend on how
the problem is oriented. The DOPRI5/DOP853 error norm is per component too, but that only matters at the
integration tolerance.

**Fix (as done in The Long Burn).** Solve every request in a canonical frame built from the request itself,
then rotate the answer back:

- frame x axis: the start position direction (in 2D: rotate by `−atan2(start.r.y, start.r.x)`);
- in 2D, optionally also mirror (y → −y) when the transfer is retrograde (`cross(r0, v0) + cross(rf, vf) < 0`),
  so the frame is the same for mirror-image problems;
- wrap the `Target` so it returns rotated states, velocities and accelerations;
- rotate `primer.p`, `primer.w`, `end.state` and `end.primer` back (and `end.stm` if any caller needs it: rows
  in 2-vector blocks r, v, p, w with E unchanged; columns in blocks p0, w0).

Do it in `solve_transfer` and `solve_min_time` (inside it, call the unrotated core so the frame is fixed for the
whole search, e.g. built at `T_lo`). The atlas cases start on +x already, so for them the frame is the identity
and nothing changes. A good regression test: solve random cases, rotate them by a random angle, solve again,
and require the same effort (1e-6) and a rotated primer.

## 2. `multiple_shoot` returns an incomplete `EndPoint` (latent; cheap to fix)

`multiple_shoot` assembles `solution.end` from its segments and sets neither `end.stm` nor `end.hamiltonian`
(`hamiltonian` stays 0). Callers then get a different kind of answer depending on which seed stage won:

- no game code reads `end.stm` from a `solve_transfer` result today;
- but `effort_time_slope` uses `end.hamiltonian`, and `tools/visp_bench` (the chase re-plan around line 490)
  passes a `solve_transfer` result straight into `fit_effort_by_time` / `spend_effort_by_time`. When the
  "multiple" stage produced it, the first slope is computed with h = 0, which is wrong. The game planner in
  `VariableIsp.cpp` calls `shoot` directly and is not affected.

**Fix.** In `solve_transfer`, when the best answer came from `multiple_shoot`, propagate it once from the start
with the transition matrix (`propagator.propagate(start, best.primer, T, true)`) and use that `EndPoint`. It
costs one propagation, only in that case. Alternatively compute the Hamiltonian in `multiple_shoot` from the last
segment's state and primer.

## 3. `solve_min_time` refinement can land on the rising side of a branch (not seen in SpaceTrains; just a guard)

In 3D the port found `solve_min_time` returning a time where the effort was rising with T, so the same branch
had an earlier crossing of the budget (returned T = 5.89 when the branch crossed at T ≈ 5.28). Checked in
SpaceTrains' planar solver: **0 of 3,467 answers** on bench-style and random-orbit cases have dJ/dT > 0, so it
does not seem to happen in 2D. Optionally, as a cheap guard: after `solve_min_time`, compute
`effort_time_slope(end, target(T))`; if it is positive, warm-start backwards along the branch until the effort
exceeds the budget, then refine that crossing on the budget equation.

## How these were checked

A throwaway program linking `GeneralTransfer.cpp`: cases generated like `visp_bench` `general_cases` (sets C, D)
and from random planar orbits (a 0.6–2, e ≤ 0.3, T 0.3–3, rendezvous or intercept), each solved twice (original
and rotated by a random angle), plus `solve_min_time` with budgets 0.3–0.9 of the fixed-T effort over
[0.5 T, 3 T], checking the sign of `effort_time_slope` at the answer.
