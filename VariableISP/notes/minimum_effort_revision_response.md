# Response to the manuscript review

This is the current revision of *Minimum-Effort Planning for Constant-Power
Spacecraft: Timing, Engine Limits and Planetary Encounters*. Earlier package
papers remain historical records. The revised PDF and source are authoritative
where they correct earlier claims. Equation numbers change after insertions;
the source uses cross-reference labels for stable navigation.

## A. Corrections and consistency

1. **Capped free-space model — Appendix A.** Explicitly separates surviving
   selector/state maps/U-arc mass formula/exchange argument from withdrawn
   constant-C_m consequences. Derives the regular coast-to-braking crossing,
   the finite-switch normal ceiling-only topology, and a two-unknown endpoint
   reduction that propagates the actual mass costate. Removes sliding and
   m6-based rejection from the corrected theory. Gives the state-only exchange
   proof. A new independent direct-transcription check at 12, 24 and 48 burn
   cells approaches the corrected shooting time 2.35199587. This is a numerical
   consistency check, not a claim of global uniqueness or optimality.

2. **Self-contained presentation.** Removes reader-facing Block/package/audit
   dependencies. Appendix D includes the inertial half-space first-exit
   certificate, K_g and the exact I_J quadrature. Appendix E gives the CW
   matrices, Taylor remainder construction, interval elimination procedure
   and explicit Jensen enclosures. The source articles and numerical atlas
   are pinned to repository commit bdc9698212c92cd0815d9251c7d777d47cefd706.

3. **Convention bridge — §1.4 and §2.3.** Adds time-PMP, effort-PMP and DP
   conventions, physical-to-normalized covectors, k_T, kappa, eta_m, exhaust
   limits and b_chase = J0 b. Distinguishes the exact value-level feasibility
   reduction from the rescaling of necessary conditions. A time extremal is
   an effort extremal at its duration; calling it a minimum requires optimality
   and the stated regularity. At an unattained nonconvex boundary the test must
   use actual feasible controls rather than an infimum alone.
   Fixed-final-angle targeting uses the intrinsic derivative -H, while
   moving planetary targeting uses -|u_f|^2; the time-inversion argument
   is stated for both without transferring the wrong Newton denominator.

4. **Notation — §1.4.** Adds a table of overloaded symbols and their scopes.
   Specific jet power is beta_jet (W/kg), while hardware closure uses beta_sys
   (kg/W). The corrected free-space primer slope is n, rather than omega.

5. **Derivations — §§2.2, 3.3, 4.1, 4.3, 6.1–6.3.** Adds the requested
   endpoint-Hessian, encounter-epoch, SOI-multiplier, gravity-Hessian,
   radial-measure-density and energy-bound steps. Proves the sharp diagonal
   gravity-Hessian constant 6/|r|^4 and changes the gap coefficient from
   1 - 12 P_v A_r/ell^4 to 1 - 3 P_v A_r/ell^4. Historical conservative
   numerical estimates are not relabeled as new validated certificates.

## B. Material from the original articles

6. **Interception and deadline fuel — §8.** Restores A1/B1 and A2/B2, the
   Isaacs operator and classical verification inequalities; ballistic
   interception; the b_P >= 4 b_E active-interception theorem and proof;
   the 4^(1/3) boarding penalty; zero-fuel coasting; endpoint minimax;
   F_infinity^J = b_P; zero and positive unattained escape infima; the
   five-state Gram reduction; and value scaling. States the n -> 0 reduction
   of Theorem 1 to the free-space cubic. Finite-band games are not silently
   identified with these ideal-engine results.

7. **Heliocentric evidence — §9.** Reconstructs all 73 final-angle rows at
   5.2 AU and both illustrated solar-pass geometries. All 73 table rows
   violate the 50–250 km/s band; rows 305°–360° also violate the 0.1 AU floor.
   The figure branches at (5 AU, 273.1°) and (5.2 AU, 385.2°) likewise fail
   the band, and the latter fails the floor. The extreme perihelion is refined
   by a radial-velocity root; uniform samples alone miss its depth.

   The new worked solar case is an exact continuously powered path, with
   an analytic 140–142 km/s exhaust enclosure and r >= 1 AU. It gives
   kappa = 0.0320066666667, L = 0.0316, U = kappa at h = 0.05, and a global
   minimum-time bracket 0.049 < h* <= 0.05, or
   2.84848 < T* <= 2.90663 days. Witness propellant is 25,225.4845 kg.
   The bracket inequalities are checked with exact rational arithmetic.
   The endpoint target follows the Kepler orbit through the witness endpoint;
   it is not described as a circular-orbit rendezvous. The witness duration
   is not falsely claimed to be the exact global optimum.

   Section 1.5 restores the ideal free-space power–mass closure, T_infinity
   and P_knee, with hardware and fixed-vehicle assumptions separated.

## C. Presentation

8. **Contributions — §1.4.** Separates inherited results from the additions
   developed in this article, while crediting the classical foundations.
9. **Front matter.** Adds Jan Olšina's author and affiliation information,
   removes “Block 7”, and adds an AI-assistance acknowledgment.
10. **References.** Adds Pontryagin, Lawden, Hartl–Sethi–Vickson,
    Evans–Souganidis, Isaacs and both original working articles.
11. **Dimensional scope — §§1.4 and 8.3.** States that the main planar
    restriction is deliberate and that the original chase work included d=3.

## Validation and limits

- Corrected capped shooting and the independent transcription mesh study passed.
- The worked-case rational inequalities and the inherited rational Jensen
  calculation passed. The latter certifies the leading perturbation coefficient.
- Analytic polar shooting derivatives agree with independent complex-step
  differentiation; all 73 reconstructed table rows satisfy their stated
  endpoint tolerances. Rejecting a band requires one continuous-interval
  violation; a sampled positive constraint check is not an interval proof.
- Existing integration checks passed, including physical mass across unit
  systems, moving-endpoint identities, SOI covectors and saltation, and C++
  comparisons. The historical eight-package report remains in the archive;
  it is not represented as a newly rerun proof of this revised article.
- The checked formulas, Figure 1 numbers, 67,922 km diagnostic, Jensen gap,
  conjugate-time fixture and mass-accounting fixture were retained. The
  requested additions do not certify a global constrained Jupiter optimum,
  a general bounded-engine game value or a complete station-to-station atlas.

The PDF was compiled with resolved references and visually checked after
layout corrections. The archive contains source, inputs, numerical results,
exact checkers and the historical packages needed for reproduction.


## Follow-up: two-sided scope and interplanetary feasibility

1. **Appendix A.2.** Retitles the result as ceiling-only, writes the complete
   two-sided selector with maximum-acceleration a_+ arcs, and gives their
   corrected z evolution. Explicitly leaves a complete two-sided switching
   topology open in this article. Appendix A.3's endpoint reduction and
   numerical fixture are explicitly ceiling-only. No claim that the full
   50–250 km/s topology was solved is made.
2. **Section 9.2.** Calls the example a local certificate demonstration:
   2.90663 days, radius 1 to 1.001 AU, arrival on a slightly eccentric orbit.
   It is not presented as interplanetary planning performance or a band penalty.
3. **Section 9.3.** Adds an exact 70-day Earth–Mars circular-orbit witness
   with 1 GW useful jet power, 3,000,000 kg initial mass and 1,000,000 kg
   dry mass. The target is at 1.524 AU and initially 17.3820101 degrees ahead.
   All eight cubic pieces and analytic endpoints are specified in the paper;
   no numerical endpoint residual is used to establish rendezvous.

   Outward interval arithmetic bounds the entire path: exhaust 99–102 km/s,
   radius above 0.999 AU, effort 2.5212–2.5307 (conservative U = 2.6), and
   propellant 1,201,400–1,204,100 kg. This proves a 70-day upper bound for the
   stated circular target and available budget, with no optimizer or sampled
   constraint check needed by the certificate. The separate numerical check
   gives J = 2.52593733984, propellant 1,202,751.4595 kg, a maximum normalized
   kinematic knot error below 1.8e-15, and mass agreement within 1e-6 kg.

   It does not claim a constrained optimum, the extra days due to the band,
   a Jupiter witness, a dated real-ephemeris mission, or planetary escape and
   capture costs. Abrupt thrust direction changes are allowed by the model.
   Both the certificate and independent cross-check passed.
