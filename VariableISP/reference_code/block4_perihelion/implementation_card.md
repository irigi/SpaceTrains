# Block 4 — active perihelion constraint

Planar, normalized Kepler dynamics, squared thrust acceleration effort. Normal regular extremals are assumed for the costate formulas. Necessary conditions are not optimality certificates.

## Scaling and notation

Use `T0=sqrt(R0³/mu)`, `V0=R0/T0`, `A0=R0/T0²`, `J0=R0²/T0³` as in Blocks 1–3. The new parameter is `Rc=r_min/R0`; it equals 0.1 only when `R0=1 AU`. Keep `(rho,Theta,h)`, remaining budget `b`, and `b(0)=kappa`. For strict full-power/on–off propulsion `b'=-|u|²`.

Write `R=|r|`, `er=r/R`, `et=J*er`, `v=y*er+z*et`. Here y is radial speed and z is signed tangential speed. Define the constraint with this exact sign and scale:

`c=(Rc²-r.r)/2 <= 0`.

## Contact and boundary formulas

`c'=-r.v`, `c''=-v.v+1/R-r.u`.

A contact requires `R=Rc`, `y=0`. A smooth isolated nondegenerate touch has `R''>0`. Tangential contact permits `u_r >= A`, where

`A = 1/Rc²-z²/Rc`.

A boundary interval requires `u_r=A`, `theta'=z/Rc`, `z'=u_t`, `R=Rc`, `y=0`. Second-order state constraints can have boundary intervals; do not restrict the solver to isolated touches.

## Original costates and jumps

Maximize `H=p_r.v+p_v.(g+u)-|u|²`, so the ideal control is `u=p_v/2`.

Let `dM>=0` be the constraint measure, supported where `R=Rc`:

`dp_r=-Dg^T*p_v*dt-r*dM`, `dp_v=-p_r*dt`.

At an interior contact atom of mass `gamma>=0`:

`p_r_after=p_r_before-gamma*r`, `p_v_after=p_v_before`.

Position and velocity remain continuous. Ideal thrust remains continuous; its derivative jumps by `gamma*r/2`. At a regular boundary entry/exit, thrust on both sides must match `u_r=A`. Do not add an independent velocity-costate jump in this representation. Higher-order transformed adjoints in some references use different jump formulas.

H and `L=p_r.Jr+p_v.Jv` remain constant, including across contact jumps. The ideal mass-costate integral also survives because the floor is mass independent. With an exhaust band, the mass-costate correction of Block 3 is still needed; the floor does not cancel it. Ordinary exhaust-mode switches may jump thrust.

## Ideal boundary propagation

The boundary objective is `integral(A²+z_dot²)`. The constant L gives

`z_ddot = -L/(2*Rc)-2*A*z/Rc`.

Recover vector costates:

`p_v=2*A*er+2*z_dot*et`

`p_r=(6*z*z_dot/Rc)*er-2*(z_ddot+A*z/Rc)*et`.

Required measure density:

`mu=(-4*A/Rc³-6*z_dot²/Rc-8*z*z_ddot/Rc-2*A*z²/Rc²)/Rc >= 0`.

Reject a negative density. Also enforce nonnegative contact atoms, outside-arc feasibility, endpoint matching and budget. A reduced boundary solution alone is not enough.

For constant tangential speed:

`L=-4*A*z`, `mu=-2*A*(2+Rc*z²)/Rc⁴`.

Below circular speed, A>0 and mu<0, excluding such a constant-speed normal ideal boundary optimum. Above circular speed, A<0 and the sign is admissible, but sufficiency is unproved. At `|z|=1/sqrt(Rc)`, coast gives zero effort and is globally minimal for its own boundary endpoint states and duration.

## Exhaust-band intersection

Use `w=chi_d-b`, `a_min=w/nu_plus`, `a_max=w/nu_minus`. At each boundary point require `|A|<=a_max`. Then powered tangential controls satisfy

`sqrt(max(0,a_min²-A²)) <= |u_t| <= sqrt(a_max²-A²)`.

Coast is an additional option only when A=0 and u_t=0. At b=0 it is the only option. The allowable speed window is

`max(0,1/Rc-Rc*a_max) <= z² <= 1/Rc+Rc*a_max`.

These are feasibility conditions. Use the constrained optimality system to choose thrust; do not greedily minimize the tangent component. For an isolated touch, A>a_max is impossible, while A<-a_max can still permit an outward departure.

## Penalties and sensitivities

An autonomous radial running penalty Psi(R) preserves the modified Hamiltonian, angular costate and the ideal mass integral. Replace H by `H-Psi` and add `Psi'(R)*er` to p_r'. A finite smooth exponential penalty `epsilon*exp((Rc-R)/sigma)` does not guarantee feasibility.

The interior barrier `epsilon/(R-Rc)²` is smooth for R>Rc and prevents finite-cost contact for bounded-speed trajectories. It cannot represent boundary arcs at positive epsilon. A logarithmic barrier can have finite integral at a touch, so do not infer strict continuous-time feasibility from it alone. Use an interior barrier to initialize a hard-constraint solve, then certify the result.

The penalized cost K is not physical effort J. Penalized costates differentiate K; ballistic-endpoint formulas become `K_h=-|u_f|²+Psi(R_f)` and `K_d|h=|u_0|²-|u_f|²-Psi(R_0)+Psi(R_f)`. They do not directly give derivatives of physical fuel expenditure along penalized solutions.

At a regular differentiable constrained optimum, with endpoints and h held fixed:

`dJ*/dRc = Rc*M_total >= 0`.

Include atoms and density integrals in M_total. Multiply by J0/R0 for the physical derivative with respect to r_min. Add endpoint terms if the endpoints themselves change with Rc. Branch switches may only have one-sided derivatives.

## Cheap radius audit

For cubic Bezier position controls B0..B3, squared radius has degree-six Bernstein coefficients

`c_k=sum_{i+j=k} choose(3,i)*choose(3,j)/choose(6,k) * dot(B_i,B_j)`.

If `|actual_position-interpolant|<=E` over the whole segment, then `min(c_k)>=(Rc+E)²` certifies safety in exact arithmetic. A point with `|interpolant|+E<Rc` certifies violation. Otherwise subdivide by de Casteljau, or return unresolved at the depth limit. Contacts may require dedicated event enclosures. Checking only segment endpoints misses interior dips.

`perihelion.hpp` implements the formulas in floating point. Formal deployment certification requires outward-rounded arithmetic and validated trajectory/interpolation error bounds. `certify_audit.py` verifies both audit fixtures with exact rational arithmetic. Neither a polynomial fit nor dense samples by themselves certify the underlying integrated orbit.

## Three worked cases

All values are dimensionless. Ordinary formula tolerances of 1e-10 relative are adequate; density values are large because of the chosen constraint normalization and Rc powers.

**1. Globally optimal boundary coast.** Rc=0.1, z=sqrt(10)=3.1622776601683795, h=0.02. Start at r=(0.1,0), v=(0,sqrt(10)). Coast through angle 0.6324555320336759 rad and use the resulting state as target. A=0, u=0, J=0, all costates and multipliers may be zero. This is a positive-length globally optimal boundary interval for those endpoints, not a transfer between distinct planetary orbits.

**2. Powered boundary and budget.** Rc=0.1, z=3.2, z_dot=0, L=30.72, h=0.0005:

- A=-2.4, z_ddot=0, density mu=145152.
- J=5.76*h=0.00288. At kappa=0.0032, remaining b=0.00032.
- Initial r=(0.1,0), v=(0,3.2), p_v=(-4.8,0), p_r=(0,153.6).
- Angle rate is 32, so final angle is 0.016 rad. Rotate these initial vectors by that angle for the exact final state and costates.
- With chi=1.6, nu_minus=0.4 and nu_plus=2, initial powered acceleration bounds are [0.8,4]. Initially allowed |u_t| is [0,3.2]; u_t=0 is feasible. w increases by 0.00288 during the arc, and the powered radial thrust remains admissible.
- Full state/costate ODE integration agrees with the analytic solution to better than 1e-10. This is a necessary-condition fixture, not a globally certified transfer.
- Contrast z=3: A=10, L=-120, mu=-580000. It fails the multiplier sign and the same initial maximum-thrust bound.

**3. Hidden-dip audit.** Rc=0.1, E=0.00001. A straight cubic with controls `(-0.02,0.101),(-0.02/3,0.101),(0.02/3,0.101),(0.02,0.101)` is safe. A cubic with controls `(0.12,0),(0.08,0),(0.08,0),(0.12,0)` has endpoint radii 0.12 but midpoint radius 0.09 and is violating. Both classifications are checked using exact rational arithmetic in the supplied certificate script.

## Planner use

Store inactive/touch/boundary branch labels, free event times, multipliers, segment error bounds and physical J. Continue branches onto the floor offline; enforce jumps and tangency jointly with endpoint constraints. The existing unconstrained variational equations need event corrections before their sensitivities can be reused. Compare branches instead of simply discarding the unconstrained one. This removes one source of artificial porkchop gaps, but genuine branch switches and feasibility boundaries can remain nonsmooth.

Online boundary formulas are O(1); a stored-segment audit is cheap, with adaptive subdivision when ambiguous. Full constrained optimization remains offline.
