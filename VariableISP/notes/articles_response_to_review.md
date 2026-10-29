# Response to the two-original-articles review

## Mathematical qualifications to the requested fixes

1. **Affine primer:** the raw free-space velocity costate remains affine,
   lambda_v = lambda_x(t_z - t), including boundary arcs. What fails is
   global affinity of P lambda_v / (lambda_m m^2). The corrected text
   preserves the former and withdraws the latter.
2. **Free-time transversality:** H = 0 applies to fixed terminal states.
   A moving target requires H - lambda_f · x_d_dot = 0. The corrected
   numerical section distinguishes a fixed final polar angle from a target
   whose angle advances with arrival time.
3. **Unused propellant:** a one-sided penalty does not establish a dry-mass
   terminal extremal or minimum-time optimality. The revision does not claim
   that every imaginable time-optimal problem must exhaust propellant;
   degenerate, already-reached, or additional constrained problems can differ.
   The audited family explicitly enforces the dry-mass terminal condition.
4. **Historical confirmation:** rounded time tables and figure captions do
   not identify their trajectory branches. Fresh reconstructions can be
   checked, but their diagnostics cannot honestly be attributed to every old
   plotted control. The manuscript and the complete cell ledger retain this
   distinction. Per-cell original costates or trajectory histories would be
   needed to close the remaining historical audit.
5. **Infeasibility:** L(h) > kappa rejects a horizon, not all horizons of a
   missing geometric cell. A global exclusion needs coverage of the whole
   admissible time/branch domain. None is asserted without such evidence.
6. **Time range:** 170–220 days is correct for the final-angle 5.2 AU table.
   The initial/departure-phase 5.2 AU table reaches 281.19 days (281.20 in the
   return table), so describing all Jupiter tables as 170–220 would be false.

## KeplerVariableISPRockets.tex

1. **Capped section and Appendix A — corrected.** Replaces the false boundary
   invariant with lambda_m_dot = F |lambda_v| / m^2 and the evolving C_m.
   The selector uses current C_m. The corrected z dynamics establish the
   regular coast-to-braking switch and the finite-switch ceiling-only order.
   Sliding continuation, old switching roots, the (M, omega) reduction and
   the old five-arc calculation are removed. The state maps, interior mass
   formula and kinematic exchange argument are retained; the exchange is a
   separately stated and proved lemma. The endpoint reduction shoots in
   (t_z, lambda_m(0)). The P=1/2, V=2, m0=1, md=1/2, L=1 example and its
   12/24/48-cell transcription check are included. Abstract and heliocentric
   boundary-adjoint remark agree. A lower exhaust limit and its a_+ arcs are
   explicitly distinguished from the proved ceiling-only topology.
2. **Numerical procedure — corrected and selectively verified.** Documents
   both paths in the pinned original solver: fuel-out event versus fixed-time
   one-sided mass penalty. Replaces automatic PMP acceptance with five
   equality residuals, including effort = kappa and unknown final time.
   Positive effort-Hamiltonian normalization imposes the normal free-time
   condition. Fresh checks for all 73 reconstructed Jupiter rows give maximum
   endpoint error 4.8e-11, effort error 1.47e-10, mass error 1.3e-5 kg, and
   normalized time-Hamiltonian error 1.1e-9. Normal scaling is positive for
   all 73. These are necessary-condition candidates, not proven minima.
3. **Radius/exhaust diagnostics — supplied with coverage explicitly limited.**
   Every table cell and every figure has an audit-status record. All 73
   reconstructed Jupiter final-angle rows include refined extrema. The three
   stated path-figure geometries have new independent reconstructions:

   | Geometry | Time, days | Minimum radius, AU | Exhaust range, km/s |
   |---|---:|---:|---:|
   | 1.5 AU, 34.7° | 42.54575 | 1 | 20.37–993.91 |
   | 5 AU, 273.1° | 206.55983 | 0.169926 | 42.35–13001.03 |
   | 5.2 AU, 385.2° | 214.23607 | 0.000273206 | 4.85–1203.49 |

   The 1.5 AU reconstruction is not identified with the historical 44-day
   image. All three and all 73 Jupiter rows fail the 50–250 km/s band.
   Twelve Jupiter rows also fail 0.1 AU. Remaining historical trajectory
   extrema are unavailable, not guessed. The text labels the figures and
   tables as ideal benchmarks and describes the hard radial state constraint
   and measure costate. Full per-cell reconstruction of the other 1,790
   numeric historical entries is not claimed.
4. **Time range — corrected with the table distinction above.**
5. **N/A cells — relabeled U (unresolved).** All 181 remain solver outcomes;
   none is misclassified as mathematically infeasible. The lower-bound
   requirements for a genuine exclusion are stated.
6. **Optional atlas additions — included.** Moving-endpoint F_h and F_d,
   fixed-angle distinction, safeguarded effort-budget inversion/Newton step,
   kappa/eta/nu mapping and the additional capped-engine parameters.
7. **Housekeeping — completed.** The conclusion cites the companion's d=2/3
   free-space rendezvous and local planar Kepler results. Both return tables
   are now referenced. All five original figure assets were recovered from
   the pinned compiled PDF, and a complete compiling references.bib was
   reconstructed from its printed bibliography and extended for the additions.

## RocketPursuitEvasion.tex (formerly RocketsChaseAstra.tex)

1. **Local Kepler result — included with proof.** Dimensional planar CW Gramian
   and value equation; pursuer response; opposing strategy through parity;
   constructive parity feedback; n -> 0 reduction to the free-space cubic.
   The nonlinear extension includes explicit N_epsilon/L_epsilon bounds and
   a first-exit capture tube. It retains the inertial half-space certificate.
   The local bound can be sharper by compensating only the nonlinear
   remainder; uniform dominance over the half-space bound is not claimed.
2. **Conventions and finite bounds — aligned.** A table gives the DP/PMP sign
   change, physical budget units, covector pullback through b(m), interior
   thrust gain, and normalized resource/exhaust parameters. The finite-bound
   section explicitly matches the corrected companion boundary adjoint and
   states the ceiling-only scope of its topology.
3. **Information assumption — prominent.** Abstract, exact rendezvous and
   active-interception theorem statements, CW theorem, and endpoint minimax
   discussion state current-acceleration observation. Delayed observation
   loses the cancellation proof and the asserted exact guarantee; it is not
   silently treated as equivalent sampled feedback.
4. **Front matter — completed.** Restored Jan Olšina's author/affiliation block,
   removed Astra from the corrected filename, retained and extended the
   bibliography, and added a matching AI-assistance acknowledgment.
5. **Optional numerical example — included.** With r0=1000 m, w0=-10 m/s and
   ballistic evader, c(100)=0 and c(200)=0.375 m²/s³. The first-hit deadline
   value at 200 s is zero, while enforcing coincidence exactly at 200 s costs
   0.375. An active-evader endpoint comparison with b_E=0.04 gives
   M(200)=0.659949. Fuel conversion is shown for a 1 MW, 1000 kg pursuer.

## Validation

Both manuscripts compile with resolved references and no overfull boxes.
Every page was rendered and checked, including the new proofs, diagnostics,
original figures and time tables. The new numerical reconstruction audit and
independent CW/nonlinear/ballistic checks passed. The unchanged capped fixture
and transcription report are carried over from their prior verified run.
