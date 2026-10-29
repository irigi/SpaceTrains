# Block 5 implementation card: optimality and effort bounds

## Purpose and delivery scope

Classify a stored planar fixed-time minimum-effort candidate; reject a genuinely negative second variation; retain conservative lower/upper effort bounds; and make budget decisions cheaply at runtime. Do not treat a converged PMP solution as a minimum.

This package supplies analytic derivations, Python reference routines, C++17 scalar/matrix kernels and three reproducible fixtures. It does **not** supply a validated interval ODE solver, an atlas database, a complete bounded-engine second variation, or a production conjugate-crossing counter. Numerical outputs are explicitly diagnostics. The new neighborhood argument can certify global optimality or a gap when its hypotheses and inputs are rigorously established.

The PDF wrapper is two-column. `block5_section.tex` is insertable; its typesetting follows the containing document. Use `amsmath`, `amssymb`, `amsthm`, `booktabs`, and a `proposition` environment. The wrapper also uses `lmodern`, `microtype`, `hyperref`, `balance` and `geometry`.

## 1. Units and problem identity

- State: `x = [rx, ry, vx, vy]`; multiplier: `p = [prx, pry, pvx, pvy]`.
- `R0 = departure radius`, `T0 = sqrt(R0^3/mu)`, `V0 = R0/T0`, `J0 = R0^2/T0^3`.
- Normalized duration `h = T/T0`; physical effort `Jphys = J0 * J`.
- Dynamics `r' = v`, `v' = -r/|r|^3 + u`; cost `J = integral |u|^2 dt`.
- No factor 1/2 in the cost. Maximizing `H = pr.v + pv.g + |pv|^2/4`, so `u = pv/2`.
- Initial state `(1,0,0,1)`; final state `(rho*cos Theta, rho*sin Theta, -sin Theta/sqrt rho, cos Theta/sqrt rho)`.
- Floor `a = physical solar floor/R0`; the value is 0.1 only at R0 = 1 AU.
- Ideal fixed-time geometry depends on `(rho, Theta, h)`; `kappa` is the effort budget, not a further ideal geometry variable.
- Keep physical angle modulo 2π separate from continuous unwrapped path angle/winding. C++/Python Cartesian endpoint evaluation does not impose winding.

The ideal bounds are relaxations of the 50–250 km/s full-power/coast model. They do not make an ideal trajectory engine-feasible. To compare against the bounded-engine value, check the candidate's mass history, exhaust band and all path constraints before assigning its effort as an upper bound. Preserve the fixed-power versus fixed-specific-power distinction from earlier blocks.

## 2. Local second-order test

Along an exact smooth ideal extremal outside the floor, define

```
G = -I/R^3 + 3*r*r^T/R^5
K = 3*(pv*r^T + r*pv^T + dot(pv,r)*I)/R^5
    - 15*dot(pv,r)*r*r^T/R^7
A = [[0,I],[G,0]]
B = [[0],[I]]
C = [[K,0],[0,0]]
D = B*B^T/2
```

For `z' = A z + B w`, `z(0)=z(h)=0`, the second variation is

```
Q[w] = integral (2*|w|^2 - z^T*C*z) dt.
```

`K` must use the effort multiplier `p`, not an arbitrarily normalized time-optimal multiplier. For the original ideal convention `u = kT*lambda_v`, use `p = 2*kT*lambda` only when `kT` is constant and the same normalized units are used.

Propagate the 8×4 frame, saving work over the full 8×8 sensitivity matrix:

```
X' = A*X + D*Y       X(0)=0
Y' = -C*X - A^T*Y    Y(0)=I4
```

A conjugate time is a rank loss of `X(t)` for t>0. Absence on `(0,h]` implies coercivity in this regular fixed-state/fixed-time ideal problem. A conjugate point before h signals loss of local minimality; at h itself the test is degenerate. Do not impose free-time `H=0` or append a time column: those belong to a different variation problem.

When X is invertible, solve `S*X=Y`; never explicitly invert X. Then

```
S' = -C - A^T*S - S*A - S*D*S
Q = [z^T*S*z]_0^h + 2*integral |w - B^T*S*z/2|^2 dt.
```

S is singular at departure. Start the Jacobi frame at zero time, not a Riccati equation with `S(0)=0`. The leading blocks of X are `[-t^3 I/12, t^2 I/4; -t^2 I/4, t I/2]`. For diagnostic singular values use

```
Lt = diag(t^-1.5,t^-1.5,t^-0.5,t^-0.5)
Xscaled = Lt * X * Lt
```

The limiting matrix has finite nonsingular blocks. Preserve/check `X^T Y = Y^T X`. Frame QR or symplectic chart changes improve conditioning; chart changes must preserve the represented subspace and multiplicities.

### Certifying, rather than sampling

`jacobi_diagnostics` samples scaled singular values and returns `SAMPLED_DIAGNOSTIC_ONLY`. It deliberately does not label a branch a local minimum. A time mesh can miss roots, and a determinant sign can miss multiplicity-two crossings. To upgrade it, use validated ODE enclosures and root exclusion on every time slab, or a validated reduced-Hessian coercivity bound. Resolve the initial singularity analytically or with a bounded small-time series.

A convenient conservative alternative uses the scalar kernels below. If `b <= min |r|` and `P >= max |pv|`, then

```
Q[w] >= 2*(1 - 12*P/b^4 * Ar(h,2/b^3))*||w||_L2^2.
```

A positive bracket proves local coercivity if the extremal and envelopes are certified. A negative bracket does not prove a negative mode. This sufficient screen can also certify a whole regular branch cell using uniform b, P and maximum h.

For a genuinely negative-mode proof, construct a continuous endpoint-kernel variation and bound its Q above by a negative number. A discrete Hessian eigenvalue alone has discretization error. Active perihelion arcs, engine boundaries, coasts/switches and SOI events need constrained critical cones, switch-time variations and junction terms. A negative unconstrained direction may be inadmissible there. If an ideal global minimum is also feasible for the stricter model, it remains globally minimizing in the stricter model.

## 3. Lower bounds with no stationary solution required

Let `gm = 1/a^2`, `dv = vf-v0`, `dr = rf-r0-h*v0` and `pos(x)=max(x,0)`.

```
Lv = pos(norm(dv)-gm*h)^2/h
Lr = 3*pos(norm(dr)-gm*h^2/2)^2/h^3
```

Both bound every trajectory with floor a, whatever its winding. For a fixed nonzero dual direction `lambda=(alpha,beta)`, use

```
q(t) = (h-t)*alpha + beta
W = [[h^3 I/3, h^2 I/2], [h^2 I/2, h I]]
Llambda = pos(dot(lambda,(dr,dv))-gm*integral norm(q(t))dt)^2
          / (lambda^T W lambda).
```

`dual_moment` uses an upper composite-trapezoid estimate of the convex norm integral. Optimizing the dual direction is optional: any direction is valid at any query after recomputing this formula. An upper bound on the integral must be rounded upward and the final lower result downward in a rigorous implementation.

Energy/angular bounds can use any nonnegative cutoff U:

```
E(x) = |v|^2/2 - 1/|r|
Lmech(x) = rx*vy - ry*vx
c0 = |v0|^2 + 2/a - 2/|r0|
Iv = (h*sqrt(U) + sqrt(h^2*U + h*c0))^2
RU = |r0| + h*|v0| + gm*h^2/2 + sqrt(U*h^3/3)
LE = (Ef-E0)^2/Iv
LM = (Lmech_f-Lmech_0)^2/(h*RU^2)
L = max(0,Lv,Lr,Llambda,min(U,max(LE,LM)))
```

The `min(U,...)` is mandatory because the energy/angular envelopes were derived only for competitors with effort ≤ U. The cutoff need not itself be a feasible effort to produce a lower bound. For an upper bound and a gap statement, U must cover a feasible trajectory under the actual model.

Runtime tests:

- `L > kappa`: proven impossible at that duration if L is certified.
- `U <= kappa`: proven feasible if U is a certified feasible upper bound.
- Otherwise unresolved; another branch or refinement may help.
- Maximum additive effort gap: `U-L`. Maximum relative gap, when L>0: `U/L-1`.

If numerics give L>U for a supposedly feasible upper bound, report an inconsistent certificate; do not hide the inconsistency by clamping L to U. These bounds often become weak when a=0.1 and h is long. Free-space effort, a CW approximation, or the cheapest sampled branch is not automatically a global Kepler lower bound.

## 4. Global containment and approximate-stationarity gap

This can be much stronger near a low-effort reference. It requires an exactly endpoint-feasible trajectory in the mathematical representation, not just a small shooting residual. Let its effort be U, let `b <= min |rbar|`, choose `0 < delta < b`, and let `P >= max |pv|` for a continuous piecewise differentiable multiplier p. Define

```
ell = b-delta
M = 2/ell^3
omega = sqrt(M)
Kr2 = sinh(2*omega*h)/(4*omega^3) - h/(2*omega^2)
Kv2 = sinh(2*omega*h)/(4*omega) + h/2
Ar = (cosh(2*omega*h)-1)/(8*omega^4) - h^2/(4*omega^2)
Av = (cosh(2*omega*h)-1)/(8*omega^2) + h^2/4
```

Positive series avoid cancellation. For M=0, the four values are `h^3/3, h, h^4/12, h^2/2`. Both code implementations reject `M*h^2>100`; a production scaled-hyperbolic/interval implementation may extend this domain. Rejection means inconclusive, never optimal or infeasible.

Compute the L2 norms (not point samples) of

```
eu = 2*ubar - pv
er = pr' + Dg(rbar)^T*pv
ev = pv' + pr
```

and then

```
containment_margin = delta - 2*sqrt(U*Kr2)
gamma = 1 - (12*P/ell^4)*Ar
eta = ||eu||_L2 + sqrt(Ar)*||er||_L2 + sqrt(Av)*||ev||_L2
```

If both margins are strictly positive, **every** competitor with cost ≤ U stays in the position neighborhood, and

```
gap <= eta^2/(4*gamma)
V >= max(0,U-gap).
```

For an exact extremal eta=0 and the same inequalities prove unique global minimality, including other windings. Unlike a local-only neighborhood estimate, this proof includes a first-exit argument covering all cheaper competitors. It can fail for a globally optimal trajectory; failure is inconclusive.

### Interval accounting for a deployment

If only an effort interval `[Ulo,Uhi]` is available, use Uhi for containment and a certified residual/curvature gap D. Then `V >= max(0,Ulo-D)`, with feasible upper Uhi. The reported candidate gap is at most `Uhi-Ulo+D`. Never use Uhi as the subtracted reference cost in a lower bound. Lower b and upper P/residual norms must hold over every polynomial interval. Integrate norm squares with validated quadrature and round square roots upward. Do not declare sampled extrema plus an arbitrary padding to be enclosures.

A C1 piecewise cubic position curve with exact boundary positions and velocities supplies a practical feasible ideal representation: set `v=r'`, `u=r''-g(r)` on each interval. Thrust jumps at knots are permitted by the ideal measurable-control problem. The p curve must remain continuous or its jumps add terms to the integration-by-parts identity. The fixture uses 81 knots, copies the intended boundary states explicitly and retains all residuals. Its floating-point circular endpoint evaluation is itself not an interval representation of transcendental input coordinates.

## 5. Cell storage and runtime

Recommended record:

```
model_id, units_id, active_set, branch_id, unwrapped_angle, parameter_box
reference/reconstruction coefficients
endpoint_error enclosure, minimum_radius enclosure
Ulo, Uhi
local_status: UNCHECKED | NUMERICAL_ONLY | VERIFIED_LOCAL | VERIFIED_NEGATIVE | DEGENERATE
local_coercivity_margin and proof provenance, if available
conjugate_time enclosures and multiplicities, if available
lower_bound recipe/directions and their applicable domain
b_lower, pv_upper, residual_norm_upper[3], tube_delta, effort interval
cell_validation_status, method_version
```

Pointwise positive checks at cell corners are not cell certificates. A cell certificate needs regular branch continuation and uniform inequalities, with correct physical-time rescaling. Using the scalar sufficient local screen or containment formulas with uniform bounds is one conservative path. A sharper operator perturbation proof must transport the moving endpoint kernels to a common space before comparing Hessians.

Keep query evaluation limited to lookup, a few scalar formulas and fixed-size vectors. Perform continuation, interval checks, winding comparisons and mode-sensitive second variations offline. Interpolation by itself does not preserve exact endpoints or a feasible effort upper bound; repair/enclose it, or label it an estimate. No millisecond latency claim has been benchmarked here.

## 6. Three fixtures and checks

1. **Two-revolution circular coast:** rho=1, h=Theta=4π, exact cost zero. It is globally minimizing by nonnegativity, despite the revolution count. The Jacobi form is positive on every finite endpoint kernel. The numerical frame checks are diagnostics of this known solution.
2. **Budget branch:** rho=1.01, h=0.712023798759808, Theta=0.005+rho^(-1.5)*h. Reconstructed effort is about 0.00320000000648; delta=0.08. The numerical containment margin is about 0.03534 and gamma about 0.8990. Residual-based estimated gap is about 7.21e-12. This is not interval-certified and has not been tested for bounded-engine feasibility. The cheap global lower estimate is much looser, about 1.77e-6.
3. **Synthetic conjugate regression:** two copies of `Q/2=integral [(z'')^2-600*z^2]dt` with clamped ends. First conjugate time 0.9557120662747031 has multiplicity two. Determinants on both sides are positive. `z=1-cos(2πt)` gives Q/2=-120.72727172798068 at h=1. This is an accessory-system test, explicitly not a claimed Kepler atlas trajectory.

Python checks independent kernel quadrature, finite differences of the Kepler Hessian contraction, symplectic consistency, the exact finite-perturbation cost-difference identity, cubic-reconstruction refinement, positive elementary lower bounds, and the even-multiplicity conjugate root. C++ checks the scalar/matrix formulas and fixtures. The two codes do not constitute two independent proofs of the underlying theorems.

## 7. Reproduction

Python dependencies: Python 3, NumPy, SciPy. C++: any C++17 compiler. TeX: pdfLaTeX and BibTeX with the listed packages.

```bash
python3 validate.py
python3 make_outputs.py
g++ -std=c++17 -O2 -Wall -Wextra -Werror test_optimality.cpp -o test_optimality
./test_optimality
pdflatex -interaction=nonstopmode -halt-on-error block5_paper.tex
bibtex block5_paper
pdflatex -interaction=nonstopmode -halt-on-error block5_paper.tex
pdflatex -interaction=nonstopmode -halt-on-error block5_paper.tex
```

`make_outputs.py` refreshes the numerical table and the C++ fixture include from `test_cases.json`. The source helpers under `support/` are copied from Block 2; they solve a nearby ideal branch and provide the initial CW guess. `optimality_reference.py` contains the new work.
