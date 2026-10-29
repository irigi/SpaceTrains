# Unified implementation card — revised manuscript

Use with the manuscript and earlier packages' detailed cards.

## Core quantities

For each primary and length R0, use T0 = sqrt(R0³/μ), V0 = sqrt(μ/R0), and J0 = V0²/T0. Physical effort is j = J0 J.

At the actual entry mass m_in, compute χ = 2P/(m_in J0):

~~~text
w = χ + accumulated_effort
m = 2P / (J0 w)
u = 0 or w/ν_plus <= |u| <= w/ν_minus
ν_minus = V_exhaust_min/V0
ν_plus  = V_exhaust_max/V0
1/m_final = 1/m_initial + sum(J0_i * J_i / (2 P_i))
~~~

An internal phase's entry mass is the preceding phase's output. Do not reset it to launch mass.

## One query

1. Resolve physical endpoints and epochs, including station phase, engine model, patches and safety floors.
2. Normalize actual entry mass, power and exhaust limits.
3. Retrieve branches with model, winding, active-set and parameter-domain labels.
4. Reconstruct/correct endpoints before using a feasible upper bound.
5. Audit engine, continuous-time radius and resource constraints; use the correct event map and covectors.
6. Combine valid lower bounds by maximum and feasible upper bounds by minimum.
7. Return infeasible if L > budget, feasible if U <= budget, otherwise unresolved. Include provenance and numerical status.
8. Search duration with a maintained bracket. A branch derivative is a local aid, not a global value oracle.

## Derivative and event dispatch

| Situation | Formula or action |
|---|---|
| Ideal geometric endpoint | dJ/dh = −H |
| Ideal ballistic moving arrival | dJ/dh = −\|u_f\|² |
| Ideal common date shift at fixed duration | dJ/dd = \|u_0\|² − \|u_f\|² |
| Bounded ordinary endpoints | Replace squared thrust by optimized gain; include resource transversality |
| Matched stop | Difference of outgoing and incoming endpoint gains in common objective units |
| Point flyby | Block 9 reset and turn KKT; velocity derivative includes changing turn angle |
| SOI crossing | Block 9B/6 chart; ζ = −p_v·ΔG / (x·v_relative) for transverse crossings |
| Radial floor | Block 4 tangency, measure costates and multiplier signs |
| Grazing or coincident events | Refine the event treatment; do not clamp the denominator |

For fixed-time effort, C = 1+p_b, s = \|p_v\|, a_low = w/ν_plus and a_high = w/ν_minus. For C>0, compare coast with a = clip(s/(2C),a_low,a_high) using gain s a−C a². For C<=0 compare coast with the upper bound. On a fixed-exhaust arc C′=(s−2Ca)/ν; on an interior burn or coast C′=0.

## Fixture 1 — timing

The inherited branch has ρ=1.01, h=0.712023798759808, Θ=0.005+ρ^(-3/2)h and effort approximately 0.0032. Verify:

~~~text
H − L = |u_0|²
H − ρ^(-3/2)L = |u_f|²
F_h = −|u_f|²
F_d = |u_0|² − |u_f|²
~~~

Maximum discrepancy on its supplied endpoint data is approximately 1.28e−16. C++ separately checks the ideal limit of the bounded selector.

## Fixture 2 — mass through local units

Artificial accounting data: initial mass 1000 kg, both useful powers 1 MW, physical efforts 20 and 40 m²/s³, local effort scales 2 and 5 m²/s³.

| Phase | Entry mass (kg) | χ | Normalized effort | Exit mass (kg) |
|---|---:|---:|---:|---:|
| 1 | 1000 | 1000 | 10 | 990.099009901 |
| 2 | 990.099009901 | 404 | 8 | 970.873786408 |

Direct physical accounting agrees. Resetting phase 2 to launch mass incorrectly gives 980.392156863 kg.

If the second power actually changes to 2 MW, its entry χ becomes 808 and the correct final mass is 980.392156863 kg. These numbers check accounting only; no corresponding orbit is asserted.

## Fixture 3 — SOI matching

Seed 20261029; 256 transverse entry/exit cases in Python:

- chart stationarity and relative-Hamiltonian mismatch below 3.6e−15;
- saltation/covector mismatch below 2.4e−15;
- normalized action-pairing relative error below 6.6e−15.

C++ compares the Block 9B and Block 6 APIs on 256 cases, and checks compatibility of their planetary necessary bounds. Grazing is separately required to raise an error.

These tests do not certify near-grazing conditioning or an optimized complete route.

## Performance and limits

Keep boundary-value solves, continuation, branch enumeration and validated cell construction offline. The inherited approximately 0.071 ms median measures only Block 6's reduced scalar spiral kernel.

Current code supplies reference kernels and checks. It does not implement a general mission optimizer, validated atlas, bounded-engine pursuit solution, constrained second-order verification for all events, or finite-pulse endpoint correction.

## Corrections in this revision

- Ceiling-only free-space shooting must propagate C_m on boundary burns.
  Use primer-zero time and initial mass costate, with exhaustion as the
  stopping event; do not use the old switching-mass roots or sliding tests.
- In the global gap coefficient use gamma = 1 - 3 P_v A_r / ell^4.
  The sharp gravity Hessian bound is 6/|r|^4.
- Time-budget inversion at an unattained nonconvex boundary requires an
  actual feasible control, not only an infimum numerically below a cutoff.
- The worked solar witness has kappa = 0.0320066666667 and a proven time
  bracket 0.049 < h* <= 0.05. It is not a claim of an exact optimal time.
- The audited original Jupiter-class candidates fail the operational band;
  they cannot be used as bounded-engine upper bounds.

- The full two-sided free-space switching topology remains unresolved here;
  do not restrict 50–250 km/s solves to Appendix A.2's ceiling-only sequence.
- The 70-day Earth–Mars circular-orbit fixture in §9.3 is an endpoint-exact
  finite-band witness for its specified launch phase. Use U = 2.6 and
  T_star <= 70 days; do not interpret this as an optimum or ideal/band delta.
