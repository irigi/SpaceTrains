# Block 6: planetary escape, capture and SOI handoff

## Deliverable and boundaries

This package follows Block 5. It supplies:

1. A globally solved **phase-free, monotone, slow-circular reduced model** with the finite exhaust band, including fuel/time conversion and coast placement.
2. The exact local controlled-Kepler boundary-value problem, actual resource rejection bounds, and station-phase conditions.
3. An analytic escape/capture pair satisfying the exact local dynamics and the full-power/coast exhaust band. These paths are feasible, not stationary or optimal.
4. SOI action gradients, adjoint matching and unit conversions for a station-to-station route.
5. Python/C++ kernels, three fixture groups, reproducible figures, a two-column paper and an insertable LaTeX section.

The reduced model is an approximation, not a proven relaxation or restriction of the full dynamics. Its lower bounds, infeasibility flags, optimality and switching order are **not certificates about the exact Kepler problem**. Exact necessary bounds are provided separately. No general escape/capture optimizer or new globally optimized heliocentric route is claimed.

## 1. Normalization and engine

Use the parking-orbit radius L and local primary GM mu_p:

```
Tp = sqrt(L^3/mu_p)
Vp = sqrt(mu_p/L)
Ap = mu_p/L^2
Jp = mu_p^(3/2)/L^(5/2)
```

The initial circular parking radius is 1. Unlike Block 9B, the SOI radius is generally not 1:

```
sigma = physical_SOI_radius/L
epsilon = physical_planetary_floor/L
x' = v
v' = -x/|x|^3 + u
J = integral |u|^2 dt
w = 2P/(m*Jp)
chi = w_initial
w' = |u|^2
mass_final/mass_initial = chi/(chi+J)
b = chi_d - w
chi_d = 2P/(m_dry*Jp)
```

P is constant useful jet power, not unconverted electrical input. The physical 50–250 km/s band becomes `nu_lo=50000/Vp`, `nu_hi=250000/Vp` when Vp is in m/s. Allowed acceleration is zero or `w/nu_hi <= |u| <= w/nu_lo`. At exhausted budget only coast remains. A smaller mass increases w; it does not change the hardware power.

Escape: begin at the station state, remain within the allowed annulus and stop at first outward `|x|=sigma` with `x.v>0`. Capture: enter with `x.v<0`, remain in the annulus and end at the station's position **and velocity at the actual arrival epoch**. A circular station in these units is `(x,v)=(e_r(phi+t), e_theta(phi+t))` for prograde motion. General station ephemerides can replace this.

Negative planetocentric energy at an outward finite-SOI crossing is not mathematically forbidden by the patch model. It has no real hyperbolic excess label. Add an E>=0 or E>0 endpoint requirement if the mission needs it; never manufacture a v_infinity for E<=0. Reaching E<0 during capture is not station rendezvous.

## 2. Slow circular model: what is being solved

For a nearly circular orbit define q=1/sqrt(r). In local units,

```
q' = -a_t
r' = 2*r^(3/2)*a_t
theta' = q^3
```

Positive a_t raises the orbit, negative a_t lowers it. These are averaged/leading-order relations, not exact kinematics for a trajectory started with zero radial speed. In physical units use q=sqrt(mu_p/r) and theta_dot=q^3/mu_p; q_dot=-a_t is unchanged.

For a monotone radius change and free phase,

```
D = abs(q_initial - q_final)
a = abs(a_t)
progress = integral a dt = D
```

The unconstrained reduced optimum at fixed h is constant acceleration `a=D/h`, effort `D^2/h`. Its angle advance is

```
Theta = h*(qi^3 + qi^2*qf + qi*qf^2 + qf^3)/4
```

Thus its endpoint phase is determined, not free to match an arbitrary station. Prescribing unwrapped phase adds `integral q^3 dt = Theta`. In the ideal smooth interior the Euler–Lagrange equation becomes `2*q''=3*lambda*q^2`, generally not a linear q(t). Keep integer windings separately.

Validity diagnostics:

```
eta_ad = abs(a_t)*r^2          # dimensional: abs(a_t)*r^2/mu_p
abs(radial_drift)/q = 2*eta_ad
```

Require eta_ad small, thrust slow on the orbital timescale and eccentricity controlled for an ordinary continuous spiral approximation. No universal error bound follows from this scalar check. The approximation can fail before the SOI, and its nonzero initial radial drift is not compatible with an exactly circular initial state without transition dynamics. Use it as an initial guess for an exact solve, or reconstruct and check an exact trajectory before claiming feasibility.

Deliberate pulsing requires its own averaging: powered magnitude a and duty fraction d have mean acceleration d*a but mean effort rate d*a^2, **not** (d*a)^2. Do not use fictitious continuous throttling to erase the finite-exhaust fuel floor.

## 3. Exact optimum of the finite-band reduced model

During a burn, use inverse mass w as the independent variable and put z(w)=1/a. For a chosen final W=chi+J,

```
D = integral_chi^W z(w) dw
Tburn = integral_chi^W z(w)^2 dw
nu_lo/w <= z(w) <= nu_hi/w
```

This is a strictly convex minimization of Tburn with one linear constraint. Its exact pointwise solution is

```
z(w) = clip(k, nu_lo/w, nu_hi/w)
```

where k>0 solves the progress equality. This k is an inverse acceleration, not kappa. Powered pieces can occur in this order, with any piece empty:

| w interval | Acceleration | Exhaust |
|---|---|---|
| w < nu_lo/k | w/nu_lo | nu_lo |
| nu_lo/k <= w <= nu_hi/k | 1/k | k*w |
| w > nu_hi/k | w/nu_hi | nu_hi |

The proof minimizes `(z-k)^2` pointwise and uses the fixed integral of z to cancel the linear term. It does not use the broken global mass-costate invariant from the original bounded appendix. This piece ordering is specific to this scalar approximation.

With

```
w1 = clip(nu_lo/k, chi, W)
w2 = clip(nu_hi/k, w1, W)
```

the closed integrals are

```
D(k,W) = nu_lo*log(w1/chi) + k*(w2-w1) + nu_hi*log(W/w2)
T(k,W) = nu_lo^2*(1/chi-1/w1) + k^2*(w2-w1)
         + nu_hi^2*(1/w2-1/W)
```

Use `log1p((y-x)/x)` and `(y-x)/(x*y)` to avoid cancellation. A bracket for k is `[nu_lo/W, nu_hi/chi]`. Feasible W values satisfy

```
chi*exp(D/nu_hi) <= W <= chi*exp(D/nu_lo).
```

The inner k root is monotone. The minimum burn time decreases strictly with W inside this range, so the outer root for a prescribed duration is bracketed too. The implementation uses bounded scalar roots; it does not integrate an orbit to solve this reduced problem.

### Constant-exhaust limits

For constant exhaust nu,

```
Jnu = chi*expm1(D/nu)
Tnu = nu^2/chi * (-expm1(-D/nu))
mf/m0 = exp(-D/nu)
```

Define `Jmin=J(nu_hi)`, `Jmax=J(nu_lo)`, `Tslow=T(nu_hi)`, `Tfast=T(nu_lo)`.

- h<Tfast: infeasible **within the reduced model**.
- Tfast<=h<Tslow: choose W so the optimal burn time equals h; no coast needed.
- h>=Tslow: J=Jmin, maximum-exhaust burning for Tslow, coast for the rest.
- For a budget b<Jmin: no reduced solution at any duration.
- Otherwise minimum reduced time uses `J=min(b,Jmax)` in the fixed-effort burn-time calculation.

The ideal constant-acceleration solution remains admissible exactly when

```
nu_lo <= chi*h/D
chi*h/D + D <= nu_hi.
```

For D=0, zero effort and pure coast solve the phase-free reduced problem at every h>0. For equal exhaust bounds, only the constant-exhaust burn and inserted coast are available. The routines cover these degeneracies. Invalid inputs and inverse-mass increments too small to represent must be treated as an unresolved numerical query, not physical infeasibility. For extreme parameter ratios, use log-mass coordinates or higher precision rather than expanding accepted tolerances.

### Phase on the long-time minimum-fuel plateau

Compute the maximum-exhaust burn's angle `Theta_b=integral q^3 dt`. For slack `c=h-Tslow`, coasts at the two endpoint circular radii can supply every angle in

```
[Theta_b+c*min(qi^3,qf^3), Theta_b+c*max(qi^3,qf^3)].
```

Compare this interval with the required station phase plus 2*pi*N. If it matches, the reduced minimum fuel can also satisfy that phase; otherwise the phase-free answer is insufficient. This does not prove exact station rendezvous.

## 4. Exact local optimization and genuinely valid rejection bounds

Use the established maximizing convention, cost `integral |u|^2` and remaining-budget state b:

```
H = px.v + pv.g + pv.u - C*|u|^2
C = 1 + pb
s = |pv|
```

For C>0, set `a=clip(s/(2C),w/nu_hi,w/nu_lo)` and compare `s*a-C*a^2` with coast's zero gain. A positive gain burns toward pv; a negative gain coasts. A tie must be handled explicitly. For C<=0 compare coast and maximum acceleration. If s=0 and burning wins, the selector does not specify a direction.

```
px' = -Dg(x)^T pv
pv' = -px
C' = (s-2*C*a)/nu   on a=w/nu
C' = 0             on interior/coast arcs
```

Use Block 4 at the radial floor and handle dry mass separately. A tangential-thrust assumption is not a necessary condition of the full optimal-control problem. Block 5's smooth Jacobi test cannot be copied across active engine/path junctions without their second variation.

For the **actual endpoint states** define

```
E = |v|^2/2 - 1/|x|
j = x cross v
WE = sqrt(2*E + 2/epsilon)
dstar = max(abs(WEf-WEi), abs(jf-ji)/sigma)
L = max(dstar^2/h, chi*expm1(dstar/nu_hi))
```

Then J>=L for every admissible exact local trajectory confined to the annulus. Negative E is permitted in this expression. Use the maximum, not a sum. This follows from `|WE'|<=|u|`, `|j'|<=sigma*|u|` and the exact engine inequality

```
integral |u| dt <= nu_hi*log1p(J/chi).
```

If a conservatively computed L exceeds the remaining budget, the exact local query is impossible in the stated patch model. Passing L is only a necessary condition. The circular quantity D is not a substitute for dstar here.

## 5. Exact feasible escape/capture fixture

Use mu=1, T=10, station radius=1, sigma=4, epsilon=0.8. The analytic path is

```
r(t) = 1 + 0.03*t^2
theta(t) = t/sqrt(r(t))
x = r*er
v = 0.06*t*er + r^(-1/2)*etheta
u_vector = 0.06*er + 0.03*t/r^(3/2)*etheta
```

Here `u_vector` denotes acceleration, not exhaust. It satisfies the exact vector equations, starts at `(1,0,0,1)` and first exits at r=4, theta=5, radial speed=0.6, tangential speed=0.5. Exit energy=0.055 and v_infinity=sqrt(0.11).

Exact effort:

```
J = 651/16000 + pi*sqrt(3)/240
  = 0.06335999205292772...
```

Take chi=0.9, b_initial=0.08, nu_lo=10, nu_hi=50. The analytic full-time bounds

```
0.06 <= |u| <= sqrt(181)/150 < 0.09
(chi+J)/50 < 0.06
chi/10 = 0.09
```

prove engine-band feasibility. The budget remains positive. `mf/m0=0.9342302020266514...`, energy gain=0.555, angular-momentum gain=1. These are synthetic local units, not Earth; Vp=5 km/s would map the band to 50–250 km/s.

For Q=diag(1,-1), construct capture with `xc(t)=Q*x(T-t)`, `vc(t)=-Q*v(T-t)`, `uc(t)=Q*u(T-t)`. It ends at the same prograde circular state with the same effort and band enclosure. The target station must have phase -10 at t=0 to be at this endpoint at t=10.

In general, reverse the geometry only, then recompute the fuel/exhaust history. Capture has `wc(t)=chi_c+integral_0^t |u(T-s)|^2 ds`, which is not the escape's reversed w. An arbitrary band-feasible escape is not automatically a band-feasible capture at a specified initial mass.

For this exact fixture, the energy-only lower bound is the explicit positive expression `(411-30*sqrt(174))/1000`, about 0.01527282125. Together with the analytic feasible effort it brackets the true same-endpoint local optimum; it does not make the constructed path stationary.

## 6. SOI chart and coupling derivatives

Use a single common physical or Sun-normalized coordinate system at interfaces. The translating local Kepler field corresponds to

```
GP(X,t) = Ap(t) - mu_p*(X-Rp(t))/|X-Rp(t)|^3
Gsun(X) = -mu_sun*X/|X|^3
```

The origin acceleration Ap is mandatory. For a constant SOI radius and chart `(theta,vr,vt,tau)`,

```
X = Rp(tau) + RS*er(theta)
V = Vp(tau) + vr*er(theta) + vt*etheta(theta)
```

Let the incoming terminal costates be minus and outgoing initial costates plus. Set `dr=pr_minus-pr_plus`, `dv=pv_minus-pv_plus`. The sum of adjacent regular branch actions has gradients

```
F_vr = dv.er
F_vt = dv.etheta
F_theta = RS*dr.etheta + dv.(vr*etheta-vt*er)
F_tau = Hplus-Hminus + dr.Vp + dv.Ap
```

At an unconstrained interior junction these vanish, with `pb_minus=pb_plus`. Equivalently,

```
zeta = -(pv dot (Gafter-Gbefore))/(x dot v_relative)
pr_after = pr_before + zeta*x
pv_after = pv_before
pb_after = pb_before
Hafter-Hbefore = zeta*(x dot Vp)
```

zeta is an equality multiplier with no sign restriction. The same formulas handle escape and capture. Near grazing is an event-conditioning failure; do not silently clamp the denominator. At simultaneous safety, fuel or switching events, add the appropriate conditions.

The relative Hamiltonian is `Hrel=Hglobal-pr.Vp-pv.Ap` and is continuous at a regular crossing. Hglobal generally is not. Do not impose H=0 on each embedded planetary leg. Include the state saltation correction from Block 9B when differentiating event-driven propagation; the four chart gradients are not a full shooting Jacobian.

### Units and moons

For lambda=L/R0 and gamma=mu_p/mu_sun,

```
ct = lambda^(3/2)/sqrt(gamma)
cv = sqrt(gamma/lambda)
cJ = gamma^(3/2)/lambda^(5/2)

position perturbation in Sun units = lambda * local perturbation
velocity perturbation in Sun units = cv * local perturbation
pr_Sun = (cJ/lambda)*pr_local
pv_Sun = (cJ/cv)*pv_local
H_Sun = (cJ/ct)*H_local
pb_Sun = pb_local  # both action and resource use their own effort scale
```

Rotate vectors/covectors if axes differ. The stated formulation uses common nonrotating axes. Test that endpoint covector pairings transform with the action scale cJ.

For a moon, patch its mode against the parent-planet mode, using the actual acceleration difference and the moon ephemeris, then repeat at the parent SOI. Do not double-count origin accelerations, impose solar-only dynamics immediately outside every moon, or reset mass. This is a hierarchy of patches, not continuous three-body gravity.

## 7. Planner integration

For unchanged useful P, sum physical efforts and physical durations:

```
Jroute = Jp_escape*Je + J0*Jsun + Jp_capture*Jc
Troute = Tp_escape*he + T0*hsun + Tp_capture*hc
```

Choose crossing epochs, SOI angles and velocities, local durations and mass histories jointly. With unequal powers and no mass resets, use the inverse-mass increment `sum Jphys_i/(2*P_i)` instead of summing unweighted effort.

At a geometrically fixed endpoint, the regular duration derivative is -H. At a moving station use its full ephemeris in `H_initial-p_initial.sdot_initial` or `p_final.sdot_final-H_final`. For an ideal ballistic station these reduce to +|u_initial|^2 and -|u_final|^2. Finite-band versions use the optimized augmented control gain with the correct resource sensitivities. Interior optimized interface terms cancel by stationarity.

Free orbital phase instead of a named station has a different transversality condition, `pr.(r*etheta)-pv.(r^-0.5*er)=0`. Never silently use it for a station with prescribed phase. Adding a coast after a valid station arrival gives mission-value monotonicity when coasting is allowed; fixed geometric SOI endpoint values do not inherit this argument.

Offline records should include model/units, station ephemeris, local duration, SOI angle and both velocity components, incoming mass/resource, safety radius, engine parameters, winding, active set, feasible control reconstruction and exact-trajectory error bounds. The scalar reduced model seeds or estimates; true resource lower bounds reject; feasible exact paths provide upper bounds. Retain distinctions between stationarity, local minimum, feasibility and global guarantees from Block 5.

## 8. Tests and reproduction

Fixture group 1, D=0.5, chi=0.01, band [10,50]:

| h | Minimum reduced effort | Mode sequence |
|---:|---:|---|
| 480 | infeasible in reduced model | — |
| 490 | 0.0005102480821346535 | minimum exhaust, constant acceleration |
| 1000 | 0.0002500000000000505 | constant acceleration |
| 2480 | 0.0001008064834786377 | constant acceleration, maximum exhaust |
| 3000 | 0.0001005016708416806 | maximum exhaust plus 512.458437292 coast |

At h=1000 the phase is 468.75 radians. At h=3000 the minimum-fuel phase interval is approximately [1231.88071839516,1680.28185102567] radians. A budget of 0.0002 gives minimum reduced time 1250.

Fixture group 2 is the exact shaped escape/capture. Fixture group 3 reproduces the Block 9B common-coordinate SOI jump and adds nonstationary chart gradients `(0.105,-0.05,-0.03,0.11602455532033683)` verified by finite differences.

The Earth illustration uses JPL parameters, chosen radii 7000 and 900000 km, and 540 W/kg **useful** initial specific power. The continuous-powered adiabatic diagnostic reaches 0.05 near 67922 km even at maximum exhaust. It is about 8.78 at the chosen SOI; the leading solar-tide ratio there is about 0.145. These diagnose model limits, not an optimized Earth-escape prediction.

Dependencies: Python 3, NumPy, SciPy, Matplotlib; C++17 compiler; pdfLaTeX/BibTeX with amsmath, amssymb, amsthm, booktabs, graphicx, lmodern, geometry, microtype, hyperref and balance.

```bash
python3 validate.py
python3 make_outputs.py
g++ -std=c++17 -O2 -Wall -Wextra -Werror test_escape.cpp -o test_escape
./test_escape
pdflatex -interaction=nonstopmode -halt-on-error block6_paper.tex
bibtex block6_paper
pdflatex -interaction=nonstopmode -halt-on-error block6_paper.tex
pdflatex -interaction=nonstopmode -halt-on-error block6_paper.tex
```

Checks compare scalar formulas with independent quadrature and time-domain ODEs, verify time monotonicity, integrate exact vector escape/capture trajectories, check work/angular identities and analytic thrust bounds, compare chart derivatives with finite differences and verify covector rescaling. Full values and errors are in `test_cases.json`. No interval computation is used; general numerical branches must not be labeled certified from these tests.

The section's figures reside in `figures/` and must accompany it when inserted into another paper. The full archive includes all sources and figures. The two-column layout is supplied by `block6_paper.tex`, not forced by the insertable section.

## 9. Measured scalar-query cost

A 300-query C++ run with `-O3` here gave median 0.070978 ms and 95th percentile 0.099551 ms for the reduced minimum-effort kernel. This excludes full orbit/route optimization and is not a latency guarantee. Reproduce with:

```bash
g++ -std=c++17 -O3 -Wall -Wextra -Werror benchmark.cpp -o benchmark
./benchmark
```
