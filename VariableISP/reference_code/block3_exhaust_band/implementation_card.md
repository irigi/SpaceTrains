# Block 3 — finite exhaust band

Planar Kepler gravity. Constant available useful power P. Baseline engine is full-power/on–off, with 50–250 km/s exhaust when powered. Throttling is a separately identified extension. Formulae are necessary optimality conditions, not global certification of an atlas row.

## Dimensionless definitions

Use R0, T0=sqrt(R0^3/mu), V0=R0/T0, A0=R0/T0², J0=R0²/T0³.

`nu_minus=50000/V0`, `nu_plus=250000/V0` (SI).

`chi=2P/(m0 J0)`, `chi_d=2P/(md J0)`, `kappa=chi_d-chi`, `w=chi_d-b`, initially `w=chi`, `b=kappa`.

`r'=v`, `v'=-r/|r|³+u`, `b'=-|u|²`. At `b=0` only coast.

Powered accelerations satisfy `a_minus=w/nu_plus <= |u| <= a_plus=w/nu_minus`. A ceiling on exhaust is a *minimum powered acceleration*. Maximum thrust occurs at minimum exhaust.

General atlas parameters: `(rho,Theta,kappa,chi,nu_minus,nu_plus)`; add h for fixed-time queries. Equivalently use `eta=m0/md` instead of chi, with `chi=kappa/(eta-1)`. Fixed band ratio 5 leaves two added coordinates. If central mu, physical exhaust band and initial P/m0 are identical throughout a vehicle family, `chi=2*beta*mu*nu_plus^5/V_plus^5` collapses those two coordinates to one. Fixed hardware with different fuel loads generally does not satisfy that exception.

## Scalar control law

Maximizing Hamiltonian convention: `H=p_r.v+p_v.g-l0+p_v.u-C*|u|²`, `C=p_b+l2`.

- Fixed-time minimum effort: `(l0,l2)=(0,1)`.
- Minimum time: `(l0,l2)=(1,0)`.

Let `s=|p_v|`. For `C>0`, form `a_hat=clamp(s/(2C),a_minus,a_plus)`. For `C<=0`, use `a_hat=a_plus`. Compute `G=s*a_hat-C*a_hat²`. If `G<=0`, choose coast (ties permit either). Otherwise `u=a_hat*p_v/s`. If `s=0` and powered operation is selected, direction is undetermined by this maximization.

| Mode for C>0 | Range | a | C' |
|---|---|---|---|
| Coast | s < C*a_minus | 0 | 0 |
| Maximum exhaust | C*a_minus < s < 2C*a_minus | a_minus | (s-2C*a)/nu_plus |
| Interior | 2C*a_minus < s < 2C*a_plus | s/(2C) | 0 |
| Maximum thrust | s > 2C*a_plus | a_plus | (s-2C*a)/nu_minus |

`p_r'=-Dg^T p_v`, `p_v'=-p_r`. H and `L=p_r.Jr+p_v.Jv` survive as first integrals across ordinary switches. C is constant only on interior/coast arcs. States/costates are continuous at ordinary control switches; dry-mass boundaries need state-constraint conditions. Kepler dynamics impose no universal ordering of the four modes.

For regular fixed-effort endpoints with fixed initial reserve and free inactive or fixed terminal reserve, replace Block 2 timing squares by Hamiltonian gains: `F_h=-G_f`, `F_d|h=G_0-G_f`. Active path-constraint junctions require augmented conditions. In the ideal limit, C=1 and G=a².

## Ideal-row audit

Compute `w_I(t)=chi+I(t)` from cumulative ideal effort. A powered point passes iff `w_I/nu_plus <= a <= w_I/nu_minus`; exact zeros are coast. Also require final `I<=kappa`.

For certified segment bounds `a in [amin,amax]`, `w in [wmin,wmax]`, sufficient pass is `amin>=wmax/nu_plus && amax<=wmin/nu_minus`. Classify exact coast segments separately. Subdivide ambiguous segments or report unresolved. Dense samples alone are not a continuous certificate. Store segment enclosures offline for a cheap online audit; include interpolation error when certifying an underlying trajectory.

A globally ideal-optimal row that passes is also globally band-optimal at that time, with zero penalty. An uncertified row that passes is merely feasible. Do not clip a violating trajectory and assume endpoint rendezvous survives.

## Appendix repair

The original claim `C_m=lambda_m*m²=constant` is false on a fixed-exhaust boundary. In physical free-space 1D, raw `lambda_v` is affine, but the scaled primer `P*lambda_v/C_m` is not globally affine.

For fixed exhaust V, `F=2P/V`, `sigma=2P/V²`, `L=|lambda_v|=L_a+n*t` on a fixed-sign interval:

`m=m_a-sigma*t`

`C_m'=2P*L/V-4P*C_m/(m*V²)`

`lambda_m=lambda_m,a+V*[A*(1/m-1/m_a)+B*log(m/m_a)]`, with `B=n/sigma`, `A=L_a+B*m_a`.

Use the paper's exact boundary state maps, ordinary coast maps and local interior affine-acceleration maps. Re-evaluate the selector at switches. Retire the old switching masses and six-topology enumeration; they depend on the erroneous global constant. At the old coast/braking tie, the corrected vector fields cross in the same direction, so the purported attracting sliding surface is absent. This repair does not assert a new global topology classification.

## Penalty bounds

**Strict full-power/on–off:** a certified ideal lower bound JL and a verified band-feasible endpoint-exact trajectory JU give `JL <= J_band* <= JU`. A certified ideal minimum-time lower bound hL and feasible band arrival hU similarly bracket minimum time. The lower bound cannot be replaced by an arbitrary ideal atlas row. At fixed m0, if ideal J* is known, extra fuel is at most `m0*chi*[1/(chi+J*)-1/(chi+JU)]`.

**Continuous throttling / relaxed model:** to reproduce a prescribed ideal acceleration vector exactly, integrate the scalar equation

`w'=max(a²,w*a/nu_plus)`, `w(0)=chi`.

Choose `nu=min(nu_plus,w/a)` and power fraction `delta=a*nu/w`. Coast at a=0. Check `a<=w/nu_minus` and `w(h)<=chi_d`. This is minimum fuel among throttled realizations of that path. Its resource cost is `w(h)-chi=integral a²/delta`, NOT the ideal `integral a²`.

For an explicit upper bound use `D(t)=integral_0^t a`, `R(t)=max(w_I*a/nu_plus-a²,0)` and

`E=integral_0^h exp((D(h)-D(t))/nu_plus)*R(t) dt`.

Then `0<=w(h)-w_I(h)<=E`. A sufficient certificate is `a<=w_I/nu_minus` throughout and `w_I(h)+E<=chi_d`. Extra fuel fraction is bounded by `chi*[1/w_I(h)-1/(w_I(h)+E)]` with zero change in flight time. Numerical quadrature requires error bounds for a formal certificate.

Throttling is exact for that extended engine model. Rapid on/off switching approximates it while preserving average mass flow, but exact endpoint rendezvous and finite minimum pulse duration require a separate correction. Do not use the relaxed bound as an unconditional finite-pulse upper bound.

## Three worked test cases

Case 1 uses dimensionless w=0.2, nu_minus=0.4, nu_plus=2 (band ratio 5), C=1. Thus a_minus=0.1 and a_plus=0.5.

| s | Selected a | Gain G | C' | Mode |
|---:|---:|---:|---:|---|
| 0.05 | 0 | 0 | 0 | Coast |
| 0.15 | 0.1 | 0.005 | -0.025 | Maximum exhaust |
| 0.4 | 0.2 | 0.04 | 0 | Interior |
| 2 | 0.5 | 0.75 | 2.5 | Maximum thrust |

Case 2 tests the repaired fixed-exhaust map in consistent arbitrary units: P=0.5, V=1, m_a=1, lambda_m,a=0.4, L_a=0.6, n=0.2, t=0.1. This remains inside the ceiling boundary regime.

- m=0.9; lambda_m=0.46781678575732366.
- C_m starts at 0.4 and ends at 0.37893159646343216.
- Final C_m'=-0.22207021436318253.
- z=V*L*m/(2*C_m)=0.7362806443271199, between the coast tie 0.5 and interior boundary 1.
- Numerical ODE comparison error is below 4e-14. The old constant-C_m model fails this fixture.

Case 3 tests exact same-path throttling: w0=0.2, a=0.05 constant, nu_minus=0.4, nu_plus=2, h=1. These engine calculations apply to any nonsingular Kepler path generated by that prescribed acceleration magnitude; this is not a solved planetary rendezvous fixture.

- Ideal J=0.0025; ideal final w=0.2025; ideal exhaust initially 4, violating the ceiling 2.
- Throttled `w(t)=0.2*exp(0.025*t)`, final w=0.20506302410488578.
- Resource cost=0.005063024104885766; extra resource=0.0025630241048857656.
- Fuel fraction=0.024690087971667274, versus ideal 0.012345679012345734.
- Duty fraction decreases from 0.5 to 0.48765495601416636.
- At kappa=0.0032 the ideal path fits the budget, but this exact-path throttled realization does not. This does not prove that all other bounded trajectories are infeasible.

Physical calibration: useful P/m0=540 W/kg and 50–250 km/s give initial powered acceleration [0.00432,0.0216] m/s². If 540 W/kg is electrical power, apply jet efficiency first. Acceleration bounds scale with m0/m.

## Runtime and reproducibility

`band.hpp` contains the O(1) selector, audit predicate and boundary costate map. `test_band.cpp` checks the four modes and compares against dense scalar maximization, including C<=0. `validate.py` independently integrates the repaired adjoint and produces `test_cases.json` (requires SciPy). Engine fixtures are unit tests, not global transfer solutions.

Keep full Kepler shooting, certified segment-bound construction and endpoint corrections offline. A few dozen stored segment checks and scalar selector evaluations fit the intended architecture; no full-planner timing benchmark is claimed.
