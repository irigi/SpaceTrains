# Block 9 — unpowered planar gravity assists

## Model and units

Sun-only controlled legs, instantaneous unpowered flyby at the planet's ephemeris position. This is a zero-SOI event model: the node at the planet center represents the asymptotic encounter, not physical passage through its center. No flyby fuel or time is charged. Finite-SOI and powered flybys need different propagation.

Junction formulas assume no simultaneous solar-floor contact, budget exhaustion, mass reset or other constrained event. Coincident events require their additional multipliers.

Use a **common** heliocentric normalization: R0, T0=sqrt(R0³/mu_sun), V0=R0/T0, J0=R0²/T0³. Define `mu_bar=mu_planet/mu_sun`, `q_min=flyby_min_radius/R0`. This q_min is planetocentric and distinct from the 0.1 AU solar floor. Planet radius ratio is rho_p; its circular ephemeris has `omega_p=rho_p^(-3/2)`, `r_p=rho_p*er(phi_p+omega_p*t)`, `V_p=rho_p^(-1/2)*et`, `A_p=-r_p/rho_p³`.

Keep kappa and b as the normalized effort budget. Across an unpowered event `b_plus=b_minus`; the gravitational velocity reset is not a rocket impulse and adds nothing to J.

## Flyby map and feasibility

Incoming excess `w=v_minus-V_p`, `s=|w|>0`. Signed angle alpha and rotation Q(alpha) give

`r_minus=r_plus=r_p(t)`, `w_plus=Q*w`, `v_plus=V_p+w_plus`.

`e=1+q_min*s²/mu_bar`, `D=2*asin(1/e)`, `-D<=alpha<=D`.

For nonzero turn, `q_required=mu_bar/s²*(1/sin(|alpha|/2)-1)` must be at least q_min. Alpha=0 is the infinite-periapsis limit; if the game enforces finite outer encounter size, restrict q accordingly or use a separate no-assist branch. s=0 is not a hyperbolic flyby.

Fast candidate audit: check equal incoming/outgoing excess-speed squares, then `dot(w_minus,w_plus)>=s²*(1-2/e²)`. The latter identity assumes the equal-speed condition. Production certification needs conservative arithmetic margins or interval bounds; sample code uses ordinary floating point.

`delta_E=dot(V_p,w_plus-w)` is the heliocentric energy change. `max_kick=2*s/e`, and `|delta_E|<=|V_p|*max_kick`. Over all s, `max_kick<=sqrt(mu_bar/q_min)`. These bounds can reject implausible candidates but do not predict guaranteed time savings. For a circular planet, check `delta_E=omega_p*cross(r_p,w_plus-w)`.

## Four coupling variables and gradients

At fixed outer departure/arrival dates optimize `(w_x,w_y,alpha,t_encounter)`. The incoming leg ends with velocity V_p+w; the outgoing leg starts with V_p+Qw. Both require general endpoint-velocity actions, not circular-rendezvous-only atlas rows.

Use maximizing Hamiltonian costates, with the same cost scale on both legs. Minus denotes incoming-leg terminal data; plus denotes outgoing-leg initial data. For ideal effort `u=p_v/2`, `H=p_r.v+p_v.g+|p_v|²/4`. Gradients of the sum of leg actions are

`F_w=p_v_minus-Q^T*p_v_plus`

`F_alpha=-dot(p_v_plus,J*w_plus)`

`F_t=H_plus-H_minus+dot(p_r_minus-p_r_plus,V_p)+dot(p_v_minus-p_v_plus,A_p)`.

The time derivative holds **planet-relative w** fixed. At a free encounter epoch F_t=0. If using incoming heliocentric velocity as a variable instead, the partial time derivative differs; do not mix the two formulas.

For ideal effort and circular/ballistic planetary ephemerides, an equivalent cheap expression is

`F_t=dot(p_r_plus,w_plus)-dot(p_r_minus,w)+|u_plus|²-|u_minus|²`.

This is not the velocity-matched stop rule. Bounded-engine versions use augmented Hamiltonians and replace the squared-thrust difference by the optimized control-gain difference.

## Costate jumps and KKT signs

Let eta_plus, eta_minus multiply `alpha-D<=0` and `-alpha-D<=0`. Put `k=q_min/mu_bar` and

`d=grad_w(D)=-4*k*w/(e*sqrt(e²-1))`.

Use `sqrt((k*s²)*(2+k*s²))` to avoid subtracting close squares. Conditions:

`p_v_minus=Q^T*p_v_plus+(eta_plus+eta_minus)*d`

`dot(p_v_plus,J*w_plus)=eta_plus-eta_minus`

`eta_plus>=0`, `eta_minus>=0`, `eta_plus*(alpha-D)=0`, `eta_minus*(-alpha-D)=0`, `F_t=0`.

For an interior turn both multipliers vanish, so p_v_plus is parallel to w_plus. Maximum turn is not always optimal. At an active upper/lower turn retain the multiplier-sign check.

With xi defined as the multiplier of `r_minus-r_p(t)=0`, the remaining matching is `p_r_plus=p_r_minus+xi`, `p_b_plus=p_b_minus`. The position multiplier is determined with the full legs. H need not be continuous. Instead `H-p_r.V_p-p_v.A_p` matches at a free epoch. For circular planets and stationary velocity variables, `H-omega_p*L` matches as well; L is the costate angular integral, not mechanical angular momentum.

## Fixed-radius reset Jacobian

For a selected side epsilon=±1 and prescribed q, use `alpha=epsilon*D(s,q)`. Then

`M=d(w_plus)/d(w)=Q+outer(J*w_plus,epsilon*d)`

`p_v_minus=M^T*p_v_plus`.

At q=q_min this eliminates the active angle multiplier but does not eliminate its sign test. Rotating p_v alone is wrong: changing s changes the allowed turn. `det(M)=1` is a useful check; M is generally not orthogonal.

At fixed incoming heliocentric velocity, the explicit reset time derivative is `(I-M)*A_p` for fixed q, versus `(I-Q)*A_p` for independently fixed alpha. Very small s is ill-conditioned and physically outside the reliable instantaneous-flyby regime.

## Budget, minimum time and atlas integration

For one constant-power ship and common normalization, `J_total=sum(J_leg)` and `J_total<=kappa`. Pass the same remaining b through the event; with the exhaust band, do not reset the mass for the next leg. For unequal powers use physical inverse-mass increments `sum(J_phys/(2P_leg))`. Convert local costates before matching: `p_r_phys=(J0/R0)*p_r`, `p_v_phys=(J0/V0)*p_v`, `H_phys=(J0/T0)*H`, including axis rotations.

At smooth ideal optima with optimized interior flybys and ordinary ballistic outer endpoints, Block 2's outer duration derivative remains `dJ*/dh=-|u_final|²`. Global moving-endpoint effort remains nonincreasing when destination coasting is admissible; do not assume convexity. Use safeguarded budget-to-time searches and compare the direct route with all retained flyby branches. An uncertified atlas row or event stationary point is not a global value.

The current circular-rendezvous atlas has w=0 at a planet and therefore cannot directly supply flyby legs. General endpoint-velocity solves, branch generation and local action approximations belong offline. The event kernels are O(1); the number of coupling variables does not make repeated full leg solves fit a few milliseconds automatically.

## Three numerical unit-test cases

These are dimensionless synthetic event fixtures, not identified real planets or globally optimized complete transfers. H uses the ideal effort convention. Suggested ordinary scalar tolerance: 1e-11; Jacobian finite differences use about 5e-9.

**1. Active minimum-radius turn.** `mu_bar=0.01`, `q_min=0.01`, `r_p=(1,0)`, `V_p=(0,1)`, `A_p=(-1,0)`, `w=(1,0)`.

- e=2; maximum turn alpha=pi/3=1.0471975511965979.
- w_plus=(0.5,0.8660254037844387); v_plus=(0.5,1.8660254037844388).
- Kick magnitude=1; heliocentric energy gain=0.8660254037844387.
- M=[[1.5,-0.8660254037844387],[0.288675134594813,0.5]], det M=1.
- Choose p_v_plus=(-0.8660254037844387,0.5), giving p_v_minus=(-1.1547005383792515,1).
- Choose p_r_minus=(-1/3,0), p_r_plus=(0,0), eta_plus=1, eta_minus=0.
- H_minus=1.4047005383792515, H_plus=1.1160254037844388. Despite the Hamiltonian jump, the encounter-time residual is about 1e-16 and all event KKT conditions hold.
- Independent finite differences of the fixed-radius reset agree with M to about 1.3e-10.

**2. Interior turn.** Same planet and w; choose alpha=0.3. Then w_plus=(0.955336489125606,0.29552020666133955), required q=0.05691732447718235 > q_min.

- p_v_minus=(2,0), p_v_plus=(1.910672978251212,0.5910404133226791), both p_r=0.
- Both turn multipliers are zero; angle and incoming-excess gradients vanish.
- H_minus=-1, H_plus=-0.9106729782512121. The time residual is about -1e-16.
- This verifies interior-turn stationarity, not global sufficiency of these costates.

**3. Reject an impossible turn.** `mu_bar=3e-6`, `q_min=5e-5`, `w=(0.5,0)`, requested alpha=0.6, outgoing excess formed by rotating w through that angle.

- Equal excess speed passes.
- D_max=0.38955528546870716; requested turn fails.
- Required q=2.8606360341889475e-5 < q_min.
- Maximum allowed kick=0.1935483870967742.
- Separately, incoming (1,0) and outgoing (1.01,0) must fail the equal-speed test despite zero turn.

## Finite-SOI diagnostic

For outer planetocentric radius R_S>=q, let a_h=mu_bar/s², e=1+q/a_h, `F_S=acosh((R_S/a_h+1)/e)`. Planetary two-body passage time is

`Delta_t=2*sqrt(a_h³/mu_bar)*(e*sinh(F_S)-F_S)`.

Check omega_p*Delta_t and R_S/rho_p to judge whether instantaneous patching is adequate; these are diagnostics, not certified errors. A finite-SOI upgrade needs separate entry/exit states, dates and derivatives. Simply adding the passage time while retaining the point-event states is inconsistent. Powered flybys require finite-thrust, variable-mass propagation, not an arbitrary impulsive engine kick.
