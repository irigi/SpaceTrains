# Block 2 — minimum-effort sensitivities

## Scope and conventions

Planar Sun-only Kepler motion, ideal constant useful power with unbounded variable exhaust speed, squared thrust acceleration objective, rendezvous with circular coplanar planets. Coasting is admissible. Engine-band and active perihelion constraints are NOT covered by these explicit endpoint-thrust identities. All derivatives below are of a smooth regular extremal branch; they are value derivatives only where that branch is minimizing and the value is differentiable.

Use departure radius `R0`, `T0=sqrt(R0^3/mu)`, `V0=R0/T0`, `A0=R0/T0^2`, `J0=R0^2/T0^3`. Dimensionless state is `(r,v)`, control is `u=a/A0`, time is `tau=t/T0`, and effort is `J=integral |u|^2 dtau`. Physical effort equals `J0*J`.

The remaining effort budget is

\[
b_i=\frac{2P_i}{J_0}\left(\frac1{m_{id}}-\frac1{m_i}\right),\qquad b_i(0)=\kappa_i.
\]

`P` means useful jet power. Convert electrical power using the efficiency if needed. The quoted 540 W/kg alone does not fix whether power or specific power stays constant when fuel load changes.

Use a **maximizing** Hamiltonian, with no half in the effort:

\[
H=p_r\cdot v+p_v\cdot g+|p_v|^2/4,\quad g=-r/|r|^3,\quad u=p_v/2.
\]

`H` is constant, generally nonzero. The constant angular costate is

\[
\ell=p_r\cdot\mathsf Jr+p_v\cdot\mathsf Jv,
\quad \mathsf J(x,y)=(-y,x).
\]

If an original ideal atlas extremal uses `u=k_T*lambda_v`, the effort costates are `p=2*k_T*lambda` for all kinematic components, in the same units, provided `k_T` is constant. This conversion does not certify optimality.

## Runtime formulas

Let `E(rho,Theta,h)` hold the geometric arrival state fixed when differentiating `h`. Set `omega=rho^(-3/2)`, `alpha=omega-1`. For departure date `d`, arrival date `a=d+h`, and initial phase `phi`, use `Theta=phi+omega*a-d+2*pi*N` on the selected branch. Define `F(d,h)=E(rho,Theta,h)`.

| Quantity | Dimensionless derivative |
|---|---|
| Geometric angle | `E_Theta = ell` |
| Geometric duration, fixed endpoint state | `E_h = -H` |
| Radius, fixed departure scale, angle and duration | `E_rho = p_r,f dot e_r - 0.5*rho^(-3/2)*(p_v,f dot e_theta)` |
| Arrival date, departure fixed | `-dot(u_f,u_f)` |
| Departure date, arrival fixed | `dot(u_0,u_0)` |
| Duration, departure fixed | `F_h = -dot(u_f,u_f)` |
| Departure date, duration fixed | `F_d = alpha*ell = dot(u_0,u_0)-dot(u_f,u_f)` |

Use the endpoint squares for the two one-date derivatives; they avoid subtracting Hamiltonian terms. Use `alpha*ell` for the fixed-duration launch derivative near `rho=1`. Angle differentiation rotates both arrival position and velocity. The radial derivative is not a derivative with respect to the scale `R0`.

Consistency residuals (should be near solver error):

\[
H-\ell-|u_0|^2=0,\qquad H-\omega\ell-|u_f|^2=0.
\]

A physical date/duration gradient is the normalized gradient times `J0/T0=A0^2`. A physical date Hessian is the normalized Hessian times `J0/T0^2`.

The best unconstrained departure date at fixed duration satisfies `|u_0|=|u_f|`. For `rho != 1`, this is `ell=0`; for `rho=1`, a launch shift merely rotates the whole problem and the gradient is identically zero.

At a smooth active budget root `F(d,h)=b` with `|u_f|>0`:

\[
\frac{dh^*}{db}=-\frac1{|u_f|^2},\qquad
h_{new}=h+\frac{F-b}{|u_f|^2}.
\]

Safeguard Newton inside a feasible/infeasible bracket. The true minimum effort is nonincreasing with available duration, **not generally convex**. Zero terminal thrust or a branch switch invalidates the simple root derivative. Feasibility at an uncertified atlas row gives an upper bound on minimum time; infeasibility of one row cannot discard that time globally.

## Curvature: calculate offline, store three scalars

For `s=(r,v)`, integrate the 8-by-8 state/costate variational system in the paper along each stored extremal. Let `M_sp` and `M_pp` be its final 4-by-4 blocks. Let `J4=diag(J,J)` and `B=(0;I)`. Solve, without forming an inverse,

\[
q_\Theta=M_{sp}^{-1}\mathsf J_4s_f,\quad
q_m=-M_{sp}^{-1}Bu_f,\quad
p_{f,h}^{mov}=M_{pp}q_m+\dot p_f.
\]

Store

\[
C=(\mathsf J_4s_0)\cdot q_\Theta=E_{\Theta\Theta},\quad
D=(\mathsf J_4s_0)\cdot q_m,\quad
K=-\tfrac12 p_{v,f}\cdot[p_{f,h}^{mov}]_v.
\]

Then the runtime Hessian in `(d,h)` coordinates is

\[
\nabla^2F=\begin{pmatrix}\alpha^2 C&\alpha D\\\alpha D&K\end{pmatrix}.
\]

For fixed duration, propose `d_new=d-F_d/F_dd` only when curvature is usable; use line search and branch comparison. Poor conditioning of `M_sp` requires continuation or a different representation. It is not a global optimality test. The combined moving-endpoint formulas above avoid cancellation near a coast.

## Fuel and intermediate stops

For fixed hardware power and fixed initial mass, put `chi=2P/(m0*J0)`:

\[
\frac{m_f}{m_0}=\frac\chi{\chi+F},\quad
\frac{m_{prop}}{m_0}=\frac F{\chi+F},\quad
\frac{d(m_{prop}/m_0)}{dh}=\frac\chi{(\chi+F)^2}F_h.
\]

For fixed hardware power and prescribed arrival mass, put `chi_d=2P/(md*J0)`:

\[
\frac{m_0}{m_d}=\frac1{1-F/\chi_d},\quad
\frac{d(m_0/m_d)}{dh}=\frac{\chi_d}{(\chi_d-F)^2}F_h,
\quad F<\chi_d.
\]

No finite load is possible for `F>=chi_d` under these assumptions. With `eta=m0/md`, `kappa=chi*(eta-1)=chi_d*(1-1/eta)`. A redesigned vehicle family with fixed `P/m0=beta` instead obeys `m0=md*(1+J_phys/(2*beta))`.

For adjacent velocity-matched planetary legs, shift their common stop date while holding the other endpoints fixed:

\[
\frac{dJ_{total}}{dt_{stop}}=|a_{next,0}|^2-|a_{previous,f}|^2.
\]

Thus equal physical endpoint acceleration magnitudes are necessary at an interior optimum of total effort. With different leg normalizations multiply each dimensionless square by its own `A0^2`. With different powers and no mass resets, minimizing propellant uses weights `1/(2P_leg)` on physical effort. Refueling or cargo changes require differentiation of the mass maps. This stop formula is not the flyby junction condition.

## Three worked test cases

All data are dimensionless. JSON contains full states, costates, derivatives, solver diagnostics and finite-difference comparisons. Cases 2–3 are computed regular extremals; global optimality at their finite parameters is not certified. Suggested C++ fixture tolerance: `2e-12 + 2e-9*abs(expected)` for ordinary outputs; use tighter absolute tolerances for the tiny values in case 3.

### 1. Circular coast: zero and singular limits

`rho=1`, `phi=0`, `d=0`, `h=Theta=1`. The exact solution is `r=(cos(tau),sin(tau))`, `v=(-sin(tau),cos(tau))`, `u=p=0`.

- `J=H=ell=F_d=F_h=0`; the moving-date Hessian is zero.
- The geometric Hessian is not zero: `E_ThetaTheta=26.339557333915135`, `E_Theta,h=-26.339557333915135`, `E_hh=26.339557333915135`.
- The budget-time inverse slope is singular and must not be returned as a finite number.

### 2. Active budget at kappa = 0.0032

Inputs: `rho=1.01`, `phi=0.005`, `d=0`, `h=0.712023798759808`, `Theta=0.7064754060203982`.

| Stored quantity | Value |
|---|---:|
| `J` | 0.003199999999999321 |
| `H` | -0.005299294556583272 |
| `ell` | -0.019268535328496705 |
| `u_0` | (0.11403418474318612, 0.03107161859109259) |
| `u_f` | (-0.1081697178839428, -0.044531966545096655) |
| `C` | 70.99411843752485 |
| `D` | -0.4464008665372112 |
| `K` | 0.08094313528993778 |

| Expected output | Value |
|---|---:|
| `E_rho` | 0.6296123048440272 |
| `E_h` (geometric) | +0.005299294556583272 |
| `F_h` (moving planet) | -0.013683783911465384 |
| Departure gradient, arrival fixed | +0.013969240771913433 |
| `F_d` (duration fixed) | +0.0002854568604480495 |
| `F_dd` | 0.015581380507851765 |
| `F_dh` | 0.006613278471378538 |
| `dh/db` | -73.07920137222563 |

The local departure Newton proposal is a date shift `-0.018320383120366143`; it is not a certified best departure. With `chi=0.02`, `mf/m0=0.8620689655172666`, propellant fraction `0.1379310344827334`, and its duration derivative `-0.5084640276258243`. With `chi_d=0.0232`, required `m0/md=1.1599999999999606` and load-ratio derivative `-0.7936594668649385`. At a stop followed by initial acceleration `(0.1,0.05)` in the same units, total-effort date gradient is `-0.0011837839114653818`.

The finite-difference gradient checks have maximum absolute error below `3.2e-12`; the moving Hessian check is below `3.5e-10`.

### 3. Nonconvexity close to a circular coast

Inputs: `rho=1.001`, `phi=0.001`, `d=0`, and `Theta=phi+rho^(-3/2)*h`.

| Duration `h` | Computed branch effort | Moving `F_hh` |
|---:|---:|---:|
| 10 | 8.276603557144356e-8 | +8.444034727584247e-10 |
| 10.5 | 8.142016227042594e-8 | -5.901729418188876e-10 |
| 11 | 7.993321803191304e-8 | -1.7009968205878503e-9 |

The midpoint exceeds the endpoint average by `7.053546874764434e-11`. Do not label these finite-parameter rows globally certified. The paper separately proves nonconvexity of the exact **global** Kepler value for sufficiently small positive `epsilon`, using `rho=1+epsilon`, `phi=epsilon` and an interval-certified leading Jensen gap `0.00007630286301036809876... * epsilon^2 + O(epsilon^3)`. No numerical epsilon threshold is asserted.

## Integration checklist

Store branch id, feasibility/optimality status, `J,H,ell,u_0,u_f,C,D,K`; add final costates if radius gradients are needed. Online evaluation in `kepler_sensitivities.hpp` needs only scalar arithmetic and one square root. Offline trajectory/variational integration is not part of that runtime cost. An indicative optimized C++ run on this host measured about `0.04 microseconds` per derivative evaluation; this is not a benchmark of the full planner.

Do not use an infeasible nonminimal row to certify global infeasibility, interpolate across unrelated branches, or assume convexity. Block 5 will address optimality certification; Block 3 will add the finite exhaust band and repair the capped appendix.
