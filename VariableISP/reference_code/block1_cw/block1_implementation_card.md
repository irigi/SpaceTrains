# Block 1 implementation card: planar CW and certified Kepler pursuit

Status: exact ideal CW rendezvous game and reachable sets; sufficient nonlinear Kepler capture bounds. All motion is planar. Observation of the evader's current thrust acceleration is instantaneous. No delayed-observation block is planned.

The working priority is 1 → 2 → 3 (including the capped-appendix repair) → 4 → 9 (patched conics first) → 5. This card completes Block 1; engine-band capture guarantees belong to Block 3.

## 1. Conventions and units

Choose the central body's `mu` and a circular reference orbit of radius `R0`. Define

\[
n=\sqrt{\mu/R_0^3},\quad T_0=1/n,\quad V_0=nR_0,\quad A_0=n^2R_0,
\quad J_0=R_0^2n^3.
\]

All formulas below use dimensionless time `h = n * flight_time`, position divided by `R0`, rotating velocity divided by `V0`, acceleration divided by `A0`, and effort/budget divided by `J0`.

State order is **`[x, y, vx, vy]`**, with radial-outward `x` and prograde `y`. The velocities are derivatives in the rotating frame. Relative state is **evader minus pursuer**, `z = sE - sP`.

\[
b_i=\frac{2P_i}{J_0}\left(\frac1{m_{id}}-\frac1{m_i}\right),\qquad
b_i'=-|u_i|^2,\qquad b_i(0)=\kappa_i.
\]

Thus the atlas's `kappa` and the game's remaining `b_i` use exactly the same normalization when the same central body and reference radius are chosen. Do not reuse a heliocentric numerical `kappa` around a planet without rescaling it.

For a ship at central polar coordinates `(rho, Theta)`, with `rho = r/R0` and `Theta = theta - theta_reference`,

\[
q=(\rho\cos\Theta-1,\rho\sin\Theta),\qquad
v=\bar v_r(\cos\Theta,\sin\Theta)
 +(\bar v_\theta-\rho)(-\sin\Theta,\cos\Theta).
\]

For inertial calculations at reference orientation zero,

\[
X=(1,0)+q,\qquad V=v+\mathsf J[(1,0)+q],\quad
\mathsf J(x,y)=(-y,x).
\]

Dimensionless effort `j` converts to mass by `m = 1/(1/m0 + J0*j/(2P))`.

## 2. Dynamics and the fast Gramian

\[
A=\begin{pmatrix}0&0&1&0\\0&0&0&1\\3&0&0&2\\0&0&-2&0\end{pmatrix},\qquad
B=\begin{pmatrix}0&0\\0&0\\1&0\\0&1\end{pmatrix},\qquad z'=Az+B(u_E-u_P).
\]

With `S = sin(h)`, `C = cos(h)`,

\[
\Phi(h)=\begin{pmatrix}
4-3C&0&S&2(1-C)\\
6(S-h)&1&-2(1-C)&4S-3h\\
3S&0&C&2S\\
6(C-1)&0&-2S&4C-3
\end{pmatrix}.
\]

`W(h) = integral_0^h Phi(t) B B^T Phi(t)^T dt`. Its upper triangle is:

| Entry | Formula (`S2 = sin(2h)`) |
|---|---|
| `W11` | `6.5*h - 8*S + 0.75*S2` |
| `W12` | `-3*(h-S)^2` |
| `W13` | `4*(1-C) - 1.5*S^2` |
| `W14` | `-11*h + 14*S - 1.5*S2` |
| `W22` | `3*h^3 + 14*h + 24*h*C - 32*S - 3*S2` |
| `W23` | `5*h + 6*h*C - 8*S - 1.5*S2` |
| `W24` | `6*S^2 + 4*(1-C) - 12*h*S + 4.5*h^2` |
| `W33` | `2.5*h - 0.75*S2` |
| `W34` | `3*S^2 + 6*(C-1)` |
| `W44` | `19*h + 3*S2 - 24*S` |

Use symmetry for the lower triangle. `W` is positive definite for **every `h > 0`**, including full orbital periods; singularities of two-impulse rendezvous formulas are not Gramian singularities here.

Numerical stability:

1. Set `D = diag(h^1.5, h^1.5, sqrt(h), sqrt(h))`.
2. Factor `Wtilde = D^-1 W D^-1` by Cholesky; do not form an inverse.
3. For `h < 0.1`, evaluate its precomputed Taylor coefficients. The accompanying `cw_small_h.inc` supplies terms through degree 16 and `make_cpp.py` regenerates them using exact rational arithmetic.
4. For `Phi`, use `1-cos(h) = 2*sin(h/2)^2` and a series for `sin(h)-h` near zero.

The coefficient identity is

\[
W(h)=\sum_{j,k\ge0}\frac{(A^jB/j!)(A^kB/k!)^T}{j+k+1}h^{j+k+1}.
\]

The small-time limit is `Wtilde → [[I/3,I/2],[I/2,I]]`. At `h=0`, return the initial-state singleton separately. Very large horizons can also require conditioning checks; the mathematical theorem does not guarantee all floating-point inputs are well conditioned.

## 3. Exact ideal reachable sets

For an endpoint `sf`, set `d = sf - Phi(h)*s0`. Minimum effort is

\[
J^*(h;s_0,s_f)=d^T W(h)^{-1}d.
\]

The full-state reachable ellipsoid is `d^T W^-1 d <= b`.

For position only, use the top-left `2 x 2` block `Wqq` and displacement `dq = qf - [Phi(h)*s0].position`. The exact position ellipse is

\[
dq^T W_{qq}^{-1}dq\le b.
\]

Its support in direction `ell` is `sqrt(b * ell^T Wqq ell)`. Its center is the **unforced** path. This is not an ellipsoid centered on any arbitrary planned powered path with the same remaining budget.

These are fixed-time sets. A route-wide threat check must cover the whole time interval; checking a few isolated times can miss an encounter between samples. An ellipse indicates reachability, not an attack probability. Position-only arrival does not require velocity matching.

## 4. Exact ideal rendezvous game

At each candidate horizon compute

\[
y=\Phi(h)z_0,\qquad W(h)\lambda_h=y,\qquad
C_J(h)=y^T\lambda_h.
\]

For numerical evaluation, if `L L^T = Wtilde`, solve `L v = D^-1 y` and compute `C_J = dot(v,v)`. Recover `lambda_h` by the transpose solve and scaling.

The decision rules are:

| Condition | Exact CW result |
|---|---|
| `z0 == 0` | Already rendezvoused, time zero |
| `z0 != 0` and `bP <= bE` | Evader can avoid exact rendezvous forever |
| `bP > bE` | Unique finite game time `H` solving `C_J(H) = gap^2` |

Compute `gap = (bP-bE)/(sqrt(bP)+sqrt(bE))`. For a deadline `h`, capture by the deadline is guaranteed iff `C_J(h) <= gap^2`, provided `bP>bE` and the state is nonzero.

`C_J` is strictly decreasing, and

\[
\frac{d C_J}{dh}=-(\lambda_{h,3}^2+\lambda_{h,4}^2).
\]

Bracket the time by doubling/halving, then bisect or use safeguarded Newton. A derivative can vanish at an isolated horizon, so an unguarded Newton division is inappropriate.

The correction, evaluated after the one-time solve, is

\[
a_h(t)=B^T\Phi(h-t)^T\lambda_h.
\]

Optimal pursuer: **`uP = uE + aH(t)`**.

Optimal evader: initially **`uE = k*aH(t)`**, where `k=sqrt(bE)/gap`; use initial budgets in `k`. If remaining resources become equal before `H`, reset the clock and apply the parity policy below. The initial baseline alone is not a strategy against every pursuer.

Parity policy at nonzero state `zstar` and `bE >= bP`:

\[
l=z_*/|z_*|,\quad q(0)=b_P(0),\quad q'=-|u_E|^2,
\quad\delta=q-b_P,
\]
\[
Y=l^T\Phi(-t)z(t),\quad c=B^T\Phi(-t)^Tl,\qquad u_E=(\delta/Y)c.
\]

`q` is a virtual evader budget, distinct from a position vector. This policy uses state and resource information, not the pursuer's current control. The proof preserves `Y>0` and `q>=bP`. For finite-precision game integration, treat tolerances explicitly; replacing exact rendezvous with a finite capture region changes the game theorem.

The deterministic relative path is

\[
z_h(t)=\Phi(t)z_0-W(t)\Phi(h-t)^T\lambda_h.
\]

## 5. Certified nonlinear Kepler extension

The exact rotating-frame gravity remainder is

\[
N(q)=-\frac{(1,0)+q}{|(1,0)+q|^3}+(1,0)-\operatorname{diag}(2,-1)q.
\]

For any chosen `0 < eps < 1`, if both ships lie in `|q|<=eps`,

\[
N_\epsilon=\frac{3\epsilon^2}{(1-\epsilon)^4},\quad
L_\epsilon=\frac{6\epsilon}{(1-\epsilon)^4},\quad
|N(q)|\le N_\epsilon,\quad
|N(q_E)-N(q_P)|\le L_\epsilon|q_E-q_P|.
\]

Use the controller

\[
\boxed{u_P=u_E+a_h+N(q_E)-N(q_P).}
\]

It follows the *exact same relative path* `z_h` in the nonlinear central field. To certify the fuel and the state tube without integration, calculate

\[
g=h+h^2+2h^3/3,\quad G_1=h^2/2+h^3/3+h^4/6,
\]
\[
G_2=h^3/3+h^4/2+7h^5/15+2h^6/9+4h^7/63,
\]
\[
F_h(s)=|s_{pos}|+(1.5h^2+h^3)|s_x|+g|s_{vel}|,
\]
\[
D_h=F_h(z_0)+\sqrt{C_J(h)G_2},\qquad
E_h=F_h(s_{E0})+\sqrt{b_EG_2},\qquad
\Gamma=L_\epsilon\sqrt h\,D_h.
\]

**Both** conditions must pass:

\[
\boxed{E_h+D_h+N_\epsilon G_1<\epsilon,\qquad
b_P\ge\big(\sqrt{b_E}+\sqrt{C_J(h)}+\Gamma\big)^2.}
\]

The first is a first-exit proof of the state tube against every evader input. The second budgets the gravity compensation. This is a sufficient capture test. Failure returns **unknown**, not escape or impossibility. A few candidate epsilon values can be tested cheaply. The fast envelopes are intentionally conservative.

For a nonlinear single-ship threat ellipse, first certify its tube by

\[
F_h(s_0)+\sqrt{bG_2}+N_\epsilon G_1<\epsilon.
\]

Then the CW ellipse with budget `b_out = (sqrt(b)+N_eps*sqrt(h))^2` contains every true nonlinear endpoint. A target outside this ellipse cannot be reached at that horizon under these assumptions. For a solar exclusion radius require `R0*(1-eps) >= 0.1 AU`; for planetary use substitute the actual central exclusion radius. Neither third-body forces nor station obstacles are modeled here.

## 6. Wide-range inertial fallback

Use inertial, dimensionless `r=XE-XP`, `w=VE-VP`. If converting rotating data, `w=(vE-vP)+J*(qE-qP)`.

\[
C_{fs}=12|r|^2/h^3+12r\cdot w/h^2+4|w|^2/h,
\]
\[
I_{fs}=13h|r|^2/35+11h^2r\cdot w/105+h^3|w|^2/105,
\qquad D_{fs}=|r|+4h|w|/27.
\]

Choose a unit vector `ell` and `rstar>0`. Require

\[
\min\{\ell\cdot X_E,\ell\cdot(X_E+hV_E)\}
-h^2/(2r_*^2)-\sqrt{b_Eh^3/3}-D_{fs}>r_*,
\]
\[
b_P\ge\big(\sqrt{b_E}+\sqrt{C_{fs}}+2\sqrt{I_{fs}}/r_*^3\big)^2.
\]

This certifies a convex half-space corridor and capture under

\[
u_P=u_E+g(X_E)-g(X_P)+a_{fs}(t),\qquad g(X)=-X/|X|^3,
\]
\[
a_{fs}(t)=6r/h^2+4w/h-(12r/h^3+6w/h^2)t.
\]

No CW approximation is used. It can work at separations outside a station neighborhood. It is conservative and may fail for long or strongly curved transfers. Enforce `rstar*R0 >= rmin` where applicable. The paper gives the proof; `cw_reference.py::inertial_fallback` implements the inequalities.

## 7. Real-engine boundary of this block

The ideal capture policies above are **not yet guarantees for the finite-band engine**. Restricting both players' controls can change the game's value and winner. An ideal single-ship reachable ellipsoid remains a valid outer bound for the restricted ship.

If `alpha=P/m0=540 W/kg` is useful jet power per initial whole-ship mass, define `chi=2*alpha/J0` and `nu_min,max=ve_min,max/V0`. With accumulated effort `j`, the actual full-power/on-off admissible set is

\[
u=0\quad\hbox{or}\quad
(\chi+j)/\nu_{max}\le |u|\le(\chi+j)/\nu_{min},\qquad
\kappa=\chi(m_0/m_d-1).
\]

At the initial mass, the powered acceleration band is `0.00432–0.0216 m/s²` for 250–50 km/s, respectively. If 540 W/kg is electrical power, apply efficiency first. Fixed `P` means specific power increases as mass is spent.

This is an immediate feasibility diagnostic, not the bounded optimal-control solution. Clipping a compensation command invalidates its exact rendezvous proof. Checking only the saddle also does not establish robust admissibility against every evader. Block 3 will address this and repair the original capped appendix.

## 8. Worked numerical test cases

Inputs and outputs below are dimensionless. `cw_test_cases.json` contains all 16 Gramian entries, correction coefficients and intermediate states at full double precision. Suggested cross-language tolerance: `atol=2e-12`, `rtol=2e-9`. These fixtures exercise the ideal mathematics, not band-feasible missions.

### Case 1: short horizon and free-space limit

Input: `h=0.01`, `z0=[1e-5,0,0,0]`, `bE=0.0003`, `bP=0.0027001320030447355`.

| Output | Expected |
|---|---:|
| `C_J(h)` | `0.0012000880025676134` |
| `C_J'(h)` | `-0.3600087997432371` |
| Game time | `0.01` |
| Free-space comparison `12*x0^2/h^3` | `0.0012` |
| Correction at start | `[0.6000240002838023, 0.004000033334587273]` |
| Correction at finish | `[-0.5999939995338147, 0.004000033331587316]` |

The correction's squared integral equals `C_J`; integrating `z'=Az-B*a_h` ends at zero. This fixture detects wrong Coriolis signs and small-h cancellation. Do not set the transverse correction to zero: rotation produces it even with a purely radial initial displacement.

### Case 2: the atlas's low-budget regime

Input: `z0=[0.01,-0.02,0.003,-0.004]`, `bP=kappaP=0.0032`, `bE=0.0008`.

| Output | Expected |
|---|---:|
| `gap^2` | `0.0008` |
| Game time `H` | `4.204601204168997` |
| `C_J'(H)` | `-0.00035111334490092234` |
| `k` | `1` |
| `a_H(0)` | `[0.02437242916363848, 0.030714468879498455]` |
| `a_H(H/2)` | `[-0.00022323792989040192, -0.001167211604946372]` |
| `a_H(H)` | `[0.017106440769193883, -0.007647419768195267]` |

At the saddle `uE=a_H`, `uP=2*a_H`; efforts are `0.0008` and `0.0032`. A deadline below `H` fails the ideal guaranteed-rendezvous test, and a deadline above it passes.

At this same horizon, the position-ellipse boundary displacement for budget `0.0032` and support direction `[1,2]` is `[-0.24606876475507752, 0.9019027710698595]`; its `Wqq^-1` metric is `0.0032`. The large extent illustrates why a bare CW threat ellipse must not be treated as a physically accurate global zone.

Edge checks: with this nonzero state and `bP=bE`, time is infinite; with `z0=[0,0,0,0]`, time is zero even with zero budgets.

### Case 3: nonlinear certificate, plus wide-range fallback

Near-reference input: `h=0.2`, `z0=[0.05,0,0,0]`, `sE0=[0,0,0,0]`, `bE=1e-5`, `eps=0.25`, `bP=5.5`.

| Output | Expected |
|---|---:|
| `C_J` | `3.8612832434711466` |
| `D_h` | `0.17180799928850335` |
| `E_h` | `0.0001905527465200888` |
| `N_eps` | `0.5925925925925926` |
| `L_eps` | `4.7407407407407405` |
| Tube left side | `0.18558867549181354 < 0.25` |
| `Gamma` | `0.36425421320280077` |
| Required pursuer budget | `5.440235820667281 < 5.5` |

Thus the **ideal-engine nonlinear capture certificate passes**. For the sample evader `uE(t)=[sqrt(2*bE/h)*sin(pi*t/h),0]`, independent full-Kepler integration spends about `3.866424326424529` pursuer effort and `1e-5` evader effort. The terminal relative-state residual was about `3.1e-14`. The certificate is worst-case; this sample effort is not the worst-case value.

The broad-offset subcase uses inertial inputs `h=0.2`, `r=[0.4,0]`, `w=[0,0.4]`, `XE=[1,0]`, `VE=[0,1]`, `bE=1e-5`, `rstar=0.5`, `ell=[1,0]`, `bP=301`.

| Output | Expected |
|---|---:|
| `C_fs` | `243.2` |
| `I_fs` | `0.011897904761904764` |
| Corridor left side | `0.5079848488319625 > 0.5` |
| Required pursuer budget | `300.78913586240276 < 301` |

This verifies the fallback well outside a small station neighborhood. The large budget is deliberate for a positive certificate fixture; it is not a representative engine mission. Change the budget below the threshold and return `unknown` rather than declaring evasion.

## 9. Included code and validation

- `cw_planar.hpp` + `cw_small_h.inc`: dependency-free C++17 hot-path formulas, correction controls, minimum-time root and nonlinear tube certificate.
- `test_cw.cpp`: C++ assertions for the numerical fixtures and a small benchmark.
- `cw_reference.py`: NumPy reference, including the wide-range inertial fallback.
- `validate_block1.py`: independent SciPy matrix-exponential quadrature, endpoint and energy integration, derivative checks and full nonlinear compensation integration.
- `cw_test_cases.json`: machine-readable expected values.
- `make_cpp.py`: exact-rational generator of the small-h coefficients.

Build and run from this directory:

```sh
g++ -std=c++17 -O3 -Wall -Wextra -pedantic test_cw.cpp -o test_cw
./test_cw
python3 validate_block1.py
```

The C++ tests passed. On the execution host, an optimized run took approximately **0.19 microseconds per effort evaluation** and **10 microseconds per full bisection root**. These are indicative single-host measurements, not a target-platform guarantee. The planner's fixed-time budget test needs only one evaluation; it does not need a root or a trajectory integration.

Independent scaled-Gramian error against matrix-exponential quadrature was below `4.3e-13` across horizons from `1e-6` to `100`. Numerical tests supplement the proofs; sampled checks do not certify a whole continuous domain. For formal machine-level certificates, round upper bounds outward and lower bounds inward; otherwise use explicit conservative margins.

## 10. What remains

Block 1 proves the exact ideal planar CW rendezvous game and provides nonlinear sufficient certificates and reachable-set outer bounds. It does not solve the nonlinear game value, the finite-band game, finite-radius interception value, or a stochastic expected-loss model. None of those is needed to use the ideal endpoint formulas or the conditional nonlinear certificates as stated.

Next: Block 2, fixed-time minimum-effort sensitivities in the Kepler field, with moving circular-orbit endpoints and the mass/time trade used by the planner.
