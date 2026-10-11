**Three implementation briefs for a general constant-power, variable-Isp trajectory solver**

Prepared from the supplied manuscript, *Time-Optimal Candidate Heliocentric Transfers With a Constant-Power, Variable-Isp Engine*. This is a design specification, not an implementation. The mathematical reductions below are derived from the supplied model; the external references support the numerical techniques, not a performance guarantee for this particular problem.

**Recommendation.** Start with Direction 1 if the existing atlas contains shooting data. Build Direction 2 as an inexpensive source of initial guesses for unfamiliar boundary conditions. Add Direction 3 for difficult cases, independent cross-checks, and later path constraints. These are three distinct solution strategies that can share one dynamics and validation core.

| Direction | Main advantage | Principal limitation |
|---|---|---|
| 1. Sensitivity-based continuation from a sparse atlas, with indirect multiple shooting | Best expected speed for repeated requests related to known solutions | A known branch need not connect to the desired solution or contain the fastest trajectory |
| 2. Analytic arbitrary-endpoint free-space solution, followed by gravity homotopy | Almost no precomputation; works directly with arbitrary endpoint velocities | Strong solar passes and winding branches can be difficult to reach from the analytic seed |
| 3. Fixed-time minimum-effort optimization by successive convexification, with a search over flight time | Larger initialization tolerance; useful fallback and foundation for constraints | More unknowns and repeated subproblems; still a local method |

**Shared problem contract — include this with whichever brief is handed to a coding agent.**

The initial implementation should solve the manuscript's **uncapped, constant useful jet-power, central-gravity model**, with arbitrary specified departure and arrival position and velocity vectors. Start in 2D; write the vector equations so that 3D is a straightforward extension. Generic 3D endpoint velocities cannot generally be reduced to a planar problem by rotation.

Inputs are gravitational parameter mu > 0, power P > 0, initial mass mi > md > 0, initial state (r0, v0), final state (rf, vf), numerical tolerances, and a finite search/time budget. Positions are heliocentric. Initially the endpoint states are fixed independently of flight time. Optional requested winding branches are separate inputs. A target on a moving ephemeris is a distinct boundary-condition mode, described below.

The physical problem is

\[
\min t_f,\qquad
\dot{\mathbf r}=\mathbf v,\qquad
\dot{\mathbf v}=-\mu\mathbf r/\|\mathbf r\|^3+\mathbf a,
\]
\[
\int_0^{t_f}\|\mathbf a\|^2\,dt\le B,
\qquad B=2P(1/m_d-1/m_i).
\]

This inequality is exactly the dry-mass bound, not a surrogate cost. It removes mass from the uncapped trajectory dynamics. The manuscript uses equality at arrival. The regular normal minimum-time branch has an active budget; preserve the inequality in the public problem definition and handle zero-duration or degenerate cases separately.

Normalize with

\[
R=\|\mathbf r_0\|,\quad T_0=\sqrt{R^3/\mu},\quad
V_0=R/T_0,\quad A_0=R/T_0^2,
\quad \kappa=B R^{5/2}/\mu^{3/2}.
\]

Rotate the initial position onto the positive x axis. Below all states, accelerations, and flight time T are dimensionless, and a prime denotes differentiation with respect to dimensionless physical time tau, unless explicitly changed to s = tau/T. Then

\[
\mathbf r'=\mathbf v,\quad
\mathbf v'=\mathbf g(\mathbf r)+\mathbf u,\quad
\mathbf g(\mathbf r)=-\mathbf r/\|\mathbf r\|^3,\quad
\int_0^T\|\mathbf u\|^2d\tau\le\kappa.
\]

For planar requests the continuous normalized parameter vector can be

\[
\beta=(\log\rho,\Theta,\log\kappa,
v_{r,0},v_{\theta,0},v_{r,f},v_{\theta,f}).
\]

All four velocity components use V0, not a different circular-speed scale at each endpoint. The old atlas occupies the submanifold (0, 1, 0, rho^(-1/2)) in those last four coordinates. Generic velocities enlarge the family but **do not increase the number of shooting unknowns**. Keep an unwrapped angle and branch identifier for planar transfers. Cartesian endpoint equality alone cannot distinguish different revolutions. Do not add a redundant angle equality to an already square Cartesian shooting system: track the unwrapped angle separately and validate/select the requested branch.

Recover physical mass from E(tau) = integral |u|^2 d tau through

\[
q=E/\kappa,\qquad m=\big[m_i^{-1}+(m_d^{-1}-m_i^{-1})q\big]^{-1}.
\]

Recover dimensional acceleration as A0 u and exhaust speed as 2P/(m |a|) where |a| > 0. The unbounded exhaust speed as thrust approaches zero is part of this ideal model.

**A smaller indirect formulation to share between Directions 1 and 2.**

Absorb the manuscript's constant thrust gain into Cartesian costates:

\[
\mathbf p=k\boldsymbol\lambda_v=\mathbf u,
\qquad \mathbf w=\mathbf p'=-k\boldsymbol\lambda_r.
\]

The gain k here must be expressed in the same normalized Hamiltonian convention as the normalized costates. Propagate

\[
\boxed{
\mathbf r'=\mathbf v,\quad
\mathbf v'=\mathbf g+\mathbf p,\quad
\mathbf p'=\mathbf w,\quad
\mathbf w'=G(\mathbf r)\mathbf p,\quad
E'=\|\mathbf p\|^2,
}
\]
\[
G(\mathbf r)=D\mathbf g(\mathbf r)
=-\frac{I}{r^3}+\frac{3\mathbf r\mathbf r^\top}{r^5},\qquad r=\|\mathbf r\|.
\]

G is symmetric. There are 4d+1 propagated scalar quantities, including the effort quadrature. A single-shooting root problem uses only

\[
z=(\mathbf p_0,\mathbf w_0,\log T),\qquad
F(z;\beta)=
\begin{bmatrix}
\mathbf r(T)-\mathbf r_f\\
\mathbf v(T)-\mathbf v_f\\
E(T)-\kappa
\end{bmatrix}=0.
\]

That is five unknowns/residuals in 2D and seven in 3D. Cartesian propagation deliberately retains a few more differential states than the polar symmetry reduction in exchange for simpler derivatives and direct 3D support.

Do not omit the free-time PMP condition accidentally. In this reduced formulation it is recovered through the conserved scalar

\[
h=-\mathbf w\cdot\mathbf v+\mathbf p\cdot\mathbf g
+\tfrac12\|\mathbf p\|^2.
\]

For the regular normal, active-budget, fixed-endpoint time-minimizing branch, require **h > 0**. Set k = h to reconstruct the normal PMP scale: lambda_v = p/h, lambda_r = -w/h, and the optimized time Hamiltonian is h/k - 1 = 0. Equivalently the positive effort multiplier for the constraint E <= kappa is 1/(2h). Thus a separate unknown k and separate Hamiltonian residual are unnecessary here. A root with h < 0 is not the desired normal minimum-time candidate; h near zero is a degeneracy requiring separate treatment. Positivity alone does not prove a minimum.

Another conserved diagnostic is the rotational quantity

\[
\mathbf J=-\mathbf r\times\mathbf w+\mathbf v\times\mathbf p,
\]

with the scalar planar cross product in 2D. Neither this nor h should be imposed redundantly as extra endpoint equations.

If retaining the original costate/gain formulation instead, explicitly remove its scale redundancy or include the corresponding normal free-time Hamiltonian equation. Do not search independently over unidentifiable gain/costate combinations.

**1. Sensitivity-based atlas continuation with indirect multiple shooting.**

**Implementation brief.** Build a fast local solver for F = 0, then use exact first-order solution sensitivities to move from one of several atlas seeds to the requested boundary data. Replace repeated derivative-free endpoint-penalty minimization with scaled Newton/trust-region corrections. Use multiple shooting when long integrations amplify sensitivity.

**Seed representation and conversion.** Each record should contain beta, p0, w0, log T, branch metadata, residuals, minimum radius, and optionally samples of (r,v,p,w) on normalized time. Store more than the fastest branch at an atlas location. A table of transfer times alone is insufficient; recover shooting data from the existing solver or regenerate a small seed set.

For an existing atlas with normalized polar costates and gain k, the conversion is

\[
\mathbf p=k(\lambda_{v_r}\mathbf e_r+\lambda_{v_\theta}\mathbf e_\theta),
\]
\[
\mathbf w=-k\left[
\lambda_r\mathbf e_r+
\frac{\lambda_\theta+v_\theta\lambda_{v_r}-v_r\lambda_{v_\theta}}{r}
\mathbf e_\theta\right].
\]

The extra velocity terms matter because changing the polar angle rotates the velocity basis. Do not convert the position costate by a simple two-vector rotation. Check the conversion by comparing the physical thrust history from both formulations. If the atlas uses dimensional costates, first derive consistent normalization; alternatively reconstruct normalized p from its acceleration history and w from the analytic time derivative of that history.

**Local correction.** Integrate over s in [0,1] with dy/ds = T f(y). Integrate variational equations alongside the nominal state, or use forward-mode automatic differentiation of the RHS to generate them. For a parameter z_j,

\[
\frac{dS_j}{ds}=T f_y S_j+T f\,\delta_{j,\log T}.
\]

Include parameter-dependent initial conditions and endpoint residual derivatives when computing F_beta. Avoid differentiating through adaptive step acceptance logic; propagate continuous sensitivities with the accepted ODE steps. Use scaled residuals, pivoted QR or SVD, and a damped/trust-region Newton update. Do not invert the Jacobian explicitly or form normal equations merely for convenience. Solve all endpoint and budget conditions to their own tolerances.

**Predictor-corrector continuation.** At an accepted solution solve

\[
F_z S=-F_\beta.
\]

For a proposed parameter increment delta beta, predict z_new = z + S delta beta, then correct F(z_new; beta_new) = 0. The important new continuation coordinates are the four endpoint velocity components. They can initially move linearly from circular values to the requested values; vary log rho and log kappa for scale robustness. Large changes may require staged or curved paths through parameter space rather than changing every component at once.

Use correction size, residual reduction, Jacobian conditioning, and Newton iteration count to adapt the continuation step. For example, enlarge after several corrections requiring at most three iterations; halve after rejection. Make these configurable heuristics, not fixed mathematical constants. Reuse accepted Jacobians/factorizations as predictors where useful, while refreshing derivatives for reliable correction.

**Folds.** If ordinary parameter continuation stalls near a rank-deficient F_z, use pseudo-arclength continuation. For beta = beta(sigma), regard (z,sigma) as unknowns. Find a scaled null tangent of [F_z, F_beta beta_sigma], predict along it, and correct F = 0 together with one tangent hyperplane equation. Continue until the target parameter value is crossed and then correct to the exact target. This can traverse a regular fold; it does not guarantee connection through a bifurcation or between disconnected branches. Bound this effort and try another seed if necessary.

**Multiple shooting.** Start with about 8–16 segments for difficult solar-pass trajectories; retain single shooting for easy cases. Use unknown (r,v,p,w) at interior segment boundaries and one global log T. Enforce all segment continuity conditions, the fixed initial and final r/v, and one total effort equation. Sum per-segment effort quadratures instead of carrying a free effort value at every node. For K segments and fixed initial r/v, this produces (4d)(K-1)+2d+1 unknowns and the same number of residuals. Exploit the sparse block structure. Place shorter segments near perihelion or where transition matrices become poorly conditioned; do not put all segments at uniform physical-time spacing if that is ineffective. Multiple shooting improves numerical conditioning but does not eliminate genuine singularities of the boundary-value problem.

**Sparse adaptive cache.** Retrieve several seeds using normalized parameter distances and branch metadata. Rank them additionally by the predicted correction size |S delta beta| and conditioning, rather than geometric distance alone. Never average costates from incompatible branches. After a successful request, cache its shooting data and optionally S. Add records when a query is far from existing records, has a large predictor error, or discovers a distinct branch; do not build a dense seven-dimensional grid. Store only a small local neighborhood or use an eviction policy if memory is bounded. A local sensitivity chart is a warm start, never an accepted solution without correction.

**Expected behavior and failure modes.** This is the strongest first choice when a circular atlas already exists and many nearby requests will be solved. Most work is small ODE integrations and small/sparse linear solves. Arbitrary boundary velocities can still put a request far from the circular manifold. A continuation failure says nothing conclusive about physical feasibility. Compare candidates from different winding and perihelion families within the configured search budget.

**Acceptance experiment.** Start from stored circular cases, perturb all four endpoint velocity components, and compare warm-start continuation against cold correction on the same requests. Report convergence rate, RHS evaluations, correction count, elapsed-time distribution, and returned branch—not just average runtime. Include inward/outward radial velocities and retrograde cases to expose inadequate atlas coverage.

**2. Analytic arbitrary-endpoint free-space solution followed by gravity homotopy.**

**Implementation brief.** Generalize the manuscript's rest-to-rest, gravity-free solution to arbitrary vector positions and velocities. Use the resulting closed-form trajectory as a problem-specific seed, then turn on central gravity continuously while preserving the actual endpoint states and effort budget. This avoids searching a high-dimensional boundary atlas for the first guess.

Let D = rf - r0 and S = v0 + vf. At a fixed flight time T, the unique unconstrained free-space minimum-effort acceleration is

\[
\mathbf u(\tau)=\mathbf A+\mathbf C\tau,
\]
\[
\mathbf A=\frac{6\mathbf D}{T^2}-\frac{4\mathbf v_0+2\mathbf v_f}{T},
\qquad
\mathbf C=-\frac{12\mathbf D}{T^3}+\frac{6(\mathbf v_0+\mathbf v_f)}{T^2}.
\]

Consequently

\[
\mathbf v(\tau)=\mathbf v_0+\mathbf A\tau+\tfrac12\mathbf C\tau^2,
\quad
\mathbf r(\tau)=\mathbf r_0+\mathbf v_0\tau
+\tfrac12\mathbf A\tau^2+\tfrac16\mathbf C\tau^3.
\]

Its minimum required effort is

\[
E_0(T)=\frac{12\|\mathbf D\|^2}{T^3}
-\frac{12\mathbf D\cdot(\mathbf v_0+\mathbf v_f)}{T^2}
+\frac{4Q}{T},
\]
\[
Q=\|\mathbf v_0\|^2+\mathbf v_0\cdot\mathbf v_f+\|\mathbf v_f\|^2.
\]

The active-budget times are positive real roots of the cubic

\[
\boxed{\kappa T^3-4QT^2+12\mathbf D\cdot\mathbf S\,T
-12\|\mathbf D\|^2=0.}
\]

Use a numerically stable real-root routine or isolate roots using derivative critical points and bracketed scalar solves. Scale the polynomial before solving and verify roots using the original E0(T). Inspect all positive roots, including near-multiple roots. For the generic free-space problem choose the earliest boundary of the feasible set E0(T) <= kappa. Other regular descending crossings can be retained as additional seeds. Do not assume E0 is monotone: prescribed nonzero endpoint velocities can produce multiple feasible time intervals. Coincident initial/final states and zero-duration solutions need an explicit degenerate-case policy.

For each suitable seed set p0 = A and w0 = C. Sample the exact polynomial state and primer histories to initialize multiple-shooting nodes. The rest-to-rest special case must recover T = (12|D|^2/kappa)^(1/3), equivalent to the manuscript's dimensional formula.

**Gravity continuation.** Replace the gravity field and its Jacobian by

\[
\mathbf g_\alpha(\mathbf r)=-\alpha\mathbf r/r^3,
\qquad G_\alpha=\alpha G,\qquad \alpha\in[0,1].
\]

Keep the units fixed to the true physical mu throughout; alpha changes the dynamics, not the normalization. Keep the requested endpoint states and kappa fixed. Continue the same square system F(z;alpha) = 0 from alpha = 0 to alpha = 1, allowing T, p0, and w0 to change. Use the derivative predictor F_z dz/dalpha = -F_alpha, adaptive correction, pseudo-arclength when needed, and the multiple-shooting machinery specified in Direction 1.

Monitor h_alpha = -w·v + p·g_alpha + |p|^2/2, minimum radius, and branch geometry. If a normal branch approaches h_alpha = 0, reduce the step and diagnose degeneracy rather than dividing by it to reconstruct costates.

**Paths near the origin.** The free-space cubic may pass through the Sun. That is not a useful collision-free seed for inverse-square gravity. At alpha = 0 implement g = G = 0 directly, without evaluating expressions such as 0/r^3. For near-singular continuation, an optional two-parameter numerical homotopy is

\[
\mathbf g_{\alpha,\epsilon}
=-\alpha\mathbf r/(r^2+\epsilon^2)^{3/2},
\]
\[
G_{\alpha,\epsilon}
=-\alpha I/(r^2+\epsilon^2)^{3/2}
+3\alpha\mathbf r\mathbf r^\top/(r^2+\epsilon^2)^{5/2}.
\]

Turn gravity on with epsilon > 0, then remove epsilon while maintaining a collision-free branch. The only acceptable final solution uses alpha = 1 and epsilon = 0 and passes independent unsoftened integration. Softening may preserve an unwanted symmetric collision path; it is not a guaranteed cure. In that case obtain a genuinely different seed from the atlas or Direction 3, or continue from slightly offset boundary geometry and then restore the exact endpoints. A numerical singularity guard must be reported separately from any user-requested physical perihelion constraint.

**Small seed portfolio.** Try the earliest free-space candidate first, then other suitable positive-h roots if they exist. If these fail, use a modest set of solved neighboring geometries or circular-atlas seeds. Free-space cubic trajectories do not span arbitrary multi-revolution families; do not promise that gravity homotopy from a single analytic seed will find every branch. If minimum perihelion or prescribed winding is required, the free-space formula remains an initializer, not the constrained solution.

**Expected behavior and failure modes.** This is attractive for high-power, relatively direct transfers and for endpoint velocities far from circular values. It needs essentially no offline trajectory table and extends unchanged to 3D. It can take many steps or reach an unsuitable branch when the best transfer depends on a deep solar pass. Free-space time is neither a general lower bound nor a general upper bound on the gravity-assisted minimum time.

**Acceptance experiment.** Verify the analytic formulas algebraically/numerically for randomly selected vector endpoints, then sweep alpha from zero to one for direct and solar-pass cases. Record where a branch folds, becomes singular, or changes character. Compare with the atlas results on shared circular requests and with Direction 3 on unfamiliar boundary conditions.

**3. Fixed-time minimum-effort optimization by successive convexification, then search flight time.**

**Implementation brief.** At each selected T, minimize quadratic thrust effort with fixed endpoint states. Use sequential convex subproblems to handle nonlinear gravity, then search for the earliest verified time at which the effort fits the budget. This works directly with state and control histories and supplies an independent fallback when indirect initialization is poor.

The conceptual fixed-time value function is

\[
E_*(T)=\min_{\mathbf r,\mathbf v,\mathbf u}
\int_0^T\|\mathbf u\|^2d\tau
\]

subject to the original dynamics and fixed endpoint states. If this inner minimization were solved globally, the original minimum-time problem would be exactly

\[
\inf\{T>0:E_*(T)\le\kappa\}.
\]

In practice, retain separate local branches E_b(T). A verified feasible trajectory with E <= kappa establishes feasibility at that T; a local minimum above kappa or a failed solve **does not establish infeasibility**. Do not label local results as evaluations of the global value function.

**A concrete first transcription.** Use K intervals and node variables (r_i,v_i,u_i), starting with roughly 40–80 intervals as a tunable initial mesh. Impose exact endpoint positions and velocities. For fixed T and interval duration h_i, use first-order-hold controls and trapezoidal state defects. At iteration k linearize only gravity:

\[
\widetilde{\mathbf g}_i(\mathbf r_i)
=\mathbf g(\mathbf r_i^{(k)})
+G(\mathbf r_i^{(k)})(\mathbf r_i-\mathbf r_i^{(k)}).
\]

The convex subproblem enforces

\[
\mathbf r_{i+1}-\mathbf r_i
=\tfrac{h_i}{2}(\mathbf v_i+\mathbf v_{i+1})+\boldsymbol\nu_{r,i},
\]
\[
\mathbf v_{i+1}-\mathbf v_i
=\tfrac{h_i}{2}(\widetilde{\mathbf g}_i+
\widetilde{\mathbf g}_{i+1}+\mathbf u_i+\mathbf u_{i+1})
+\boldsymbol\nu_{v,i}.
\]

The virtual defects nu permit early iterations to remain solvable; both must converge to zero. Minimize

\[
\sum_i\frac{h_i}{3}
\left(\|\mathbf u_i\|^2+\mathbf u_i\cdot\mathbf u_{i+1}
+\|\mathbf u_{i+1}\|^2\right)
+\lambda_\nu\sum_i\|W_\nu\boldsymbol\nu_i\|_1,
\]

subject to the affine defects, exact endpoints, and scaled box trust regions around the current state/control history. The quadratic term is the exact effort integral of the linearly interpolated control. W_nu scales position and velocity defects consistently; account for mesh spacing when choosing penalty weights. With auxiliary variables for absolute values, this is a sparse convex QP. A modest positive quadratic regularizer on step size can be used if the subproblem is poorly conditioned.

Use a merit function combining actual effort and **nonlinear** dynamics defects. Compare actual and predicted improvement to accept/reject a step and enlarge/shrink the trust region. Raise the virtual-defect penalty if residuals persist; stop with failure if they cannot be removed. An optimizer's successful QP status is not successful trajectory convergence. The virtual-control and trust-region pattern follows the SCvx literature [1]; convergence guarantees depend on assumptions and should not be imported wholesale [2].

Trapezoidal defects are an initial implementation choice, not a high-accuracy certificate. Refine intervals based on nonlinear rollout error and perihelion curvature. A later implementation can use integrated variational dynamics or higher-order collocation without changing the outer strategy.

**Initial histories and geometry.** Use the analytic free-space trajectory, resampled atlas trajectories, or previously solved fixed-time trajectories. For an intentionally different geometric seed, add a smooth displacement that preserves endpoint position and velocity, for example

\[
\delta\mathbf r(s)=b\,s^2(1-s)^2\mathbf n,\qquad s\in[0,1].
\]

Derive consistent seed velocity from dr/ds divided by T and seed control from d2r/ds2 divided by T^2 minus g(r). Try both signs and a few scales b when selecting paths around the Sun. Avoid a seed crossing r = 0. These are geometric initial guesses, not claims of feasibility or extremality. Multi-revolution requests need loop-shaped or atlas seeds that carry the desired winding.

**Flight-time search.** Search a configured finite T interval using a modest logarithmic/adaptive sample grid, warmed by analytic, atlas, or neighboring-time solutions. Continue each successful branch in T and record verified feasible and over-budget samples separately. Where a branch crosses E_b(T) = kappa, refine that crossing with a safeguarded scalar root search, repeatedly correcting the inner trajectory. Search for earlier crossings within the configured interval and compare branches. Blind global bisection is invalid: even the free-space E0(T) above can be nonmonotone, and branch changes create further difficulties.

On a smooth, unconstrained fixed-endpoint stationary branch, a reconstructed primer gives

\[
\frac{dE_b}{dT}=-2h.
\]

This supplies a useful local Newton predictor T_new = T - (E_b-kappa)/(dE_b/dT). Keep the step inside a verified local bracket/trust interval, and use a bracketed method if the derivative is small or branch continuity is uncertain. The sign check h > 0 selects a descending budget crossing for the normal time-minimizing candidate.

**Final indirect refinement.** Once the direct solution is accurate, recover p approximately from u and w from its smooth time derivative, or obtain costates from correctly scaled transcription multipliers. Fit the primer equations across several nodes rather than trusting a noisy endpoint difference. Initialize multiple shooting from this history and solve the full endpoint-plus-budget system. This returns a high-accuracy unconstrained PMP candidate and creates a reusable atlas/cache entry. If an actual path constraint is active, the unconstrained primer equations are insufficient; polish with the constrained formulation instead.

**C++ choices.** Keep QP sparsity patterns fixed between iterations when the mesh is unchanged. A C++ application can call OSQP's C interface for these QPs; it provides warm-start and matrix-update support [4]. Matrix values change with gravity linearization, so do not assume numerical factorizations are always reusable. As an alternative backend for the same direct transcription, formulate the nonlinear defects in a sparse NLP and use Ipopt's C++ TNLP interface [5]. That alternative remains part of this direct-method direction, not an additional proposed strategy. Verify derivatives and variable scaling before comparing backends.

**Why this remains promising despite the manuscript's spline experiment.** That experiment optimized spline control points with Nelder–Mead. Here the numerical work uses explicit local dynamics constraints, analytic/automatic sparse derivatives, convex quadratic control cost, and controlled step sizes. The reported spline stagnation therefore does not settle the performance of this transcription. This still cannot guarantee escape from every local optimum.

**Acceptance experiment.** Compare direct results against both indirect strategies on common cases; use held-out generic velocities and difficult solar passes. Require vanishing virtual defects, mesh convergence, and independent nonlinear rollout. Measure total inner iterations and outer time samples; these are the main potential costs.

**Rules that apply to all three briefs.**

- **Branches and optimality:** Return the best verified candidate among the attempted branches and report which branches/time interval were explored. Positive h and small shooting residuals establish necessary-condition consistency, not local or global optimality. Optionally inspect the reduced second variation or constrained NLP reduced Hessian to reject saddles. A singular shooting Jacobian alone is not a complete conjugate-point test. Return `converged_candidate`, `iteration_limit`, `numerical_singularity`, or `no_candidate_found` as appropriate; reserve `infeasible` and `globally_optimal` for results actually proved.
- **Minimum radius:** The manuscript's uncapped heliocentric examples impose no physical minimum perihelion. Do not silently introduce one. Always record minimum radius and stop unsafe numerical integration at a documented singularity guard. If a user requests r >= rmin, Direction 3 can convexify it locally with (r_i^(k)/|r_i^(k)|)·r_i >= rmin and verify the continuous path after mesh refinement. Active boundary arcs require constrained optimality equations; the smooth unconstrained primer system is no longer the complete necessary-condition model.
- **Finite exhaust-speed ceiling:** Treat this as a separate model extension, not a clipped version of the solution. At full power ve <= V means a = 0 or |a| >= 2P/(mV), a nonconvex on/off constraint. Mass then enters control admissibility, the scalar effort reduction no longer decouples the dynamics/control problem, and the constant-gain smooth primer equations must not be carried onto boundary arcs without rederiving the constrained PMP. The manuscript's gravity-free switching topology is not a theorem for arbitrary heliocentric endpoints. A finite ceiling is best added after the uncapped solver is validated, using explicit modes/switches or a separately justified relaxation with a feasibility recovery step.
- **Moving endpoints:** For rendezvous with an ephemeris at fixed departure epoch, replace rf and vf by rf(T) and vf(T) in the residuals and include their derivatives in Jacobians. For a smooth unconstrained branch, replace h in the time-scale/sign test by h_eff = h + w(T)·drf/dT - p(T)·dvf/dT. The effort derivative becomes -2h_eff. Evaluate derivatives in the normalized units. If departure epoch is also optimized or endpoint components are free, add the corresponding variables and transversality conditions; do not pretend that a fixed-state boundary solver already covers that problem.
- **Numerical architecture:** Share normalization, gravity and its derivatives, endpoint handling, effort quadrature, adaptive integration, trajectory reconstruction, and diagnostics. Use double precision initially; enforce consistent scaling before considering higher precision. Expose the dimension d as a small fixed-size template parameter if convenient. Use automatic differentiation for RHS/residual derivatives or analytic expressions with independent derivative checks. No large learned global surrogate is required.
- **Acceptance checks:** Independently propagate the final control using the original dynamics; verify endpoint position/velocity errors and the budget with separate tolerances. Check h and J drift on unconstrained primer solutions, mass monotonicity, finite states, requested winding, and sensitivity to tighter integration tolerances. Mesh-based results require refinement/rollout agreement. Convert physical user tolerances into normalized tolerances rather than using one universal residual threshold.
- **Benchmarks:** Include the analytic free-space arbitrary-endpoint cases; the manuscript's circular cases; held-out velocity perturbations; retrograde and radial endpoints; close solar passes; multiple branches for the same request; and 3D noncoplanar velocities if 3D is enabled. Check dimensional similarity by rescaling physical inputs while preserving the normalized request. Check mathematical time reversal by swapping endpoints and negating both endpoint velocities: the uncapped fixed-power model has the same effort budget under reversal, although the reconstructed mass history must be recomputed. Do not apply this symmetry unchanged to the finite-ceiling model.
- **Performance reporting:** Specify hardware, tolerances, search budget, cache state, and failure rate. Report median and tail latency alongside trajectory quality and branch coverage. Do not promise millisecond or real-time performance before measuring these algorithms on the intended request distribution.

**Suggested build order, once implementation is authorized:** shared normalized dynamics and reduced shooting equations; analytic seeds from Direction 2; exact-derivative correction and atlas continuation from Direction 1; then Direction 3 as fallback and independent comparison. This ordering gets an executable mathematical reference before optimizing atlas lookup or building a larger solver stack.

**External numerical references.** The detailed spacecraft-specific equations and architecture above are derived/proposed here; these references support individual numerical ingredients.

1. Mao, Szmuk, and Acikmese, *Successive Convexification of Non-Convex Optimal Control Problems and Its Convergence Properties* (2016): linearized dynamics, virtual controls, and trust regions. https://arxiv.org/abs/1608.05133
2. Luo, Elango, and Acikmese, *Remarks on “Successive Convexification: A Superlinearly Convergent Algorithm for Non-convex Optimal Control Problems”* (2024): corrections to earlier convergence arguments and stricter assumptions. https://arxiv.org/abs/2403.00733
3. Zhu, Trelat, and Cerf, *Geometric optimal control and applications to aerospace* (2017): background on shooting initialization and numerical continuation. https://doi.org/10.1186/s40736-017-0033-4
4. OSQP, official C interface documentation: https://osqp.org/docs/interfaces/C.html
5. Ipopt, official interface documentation, including C++ TNLP: https://coin-or.github.io/Ipopt/INTERFACES.html

