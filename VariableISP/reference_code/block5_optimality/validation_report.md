# Block 5 validation report

- Python numerical checks: passed.
- C++17 regression checks: passed with `-O2 -Wall -Wextra -Werror`.
- The four gain kernels agree with independent adaptive quadrature.
- The contracted gravity Hessian agrees with finite differences of Dg.
- The finite-perturbation cost-difference identity agrees after independent integration; absolute error: 3.46945e-18.
- Refining the feasible cubic reconstruction lowers its stationarity residual as expected.
- Synthetic conjugacy is verified both by the characteristic equation and a singular Jacobi matrix. Its multiplicity is two; determinant signs on both sides remain positive.
- The zero-effort two-revolution circular coast is globally optimal analytically.
- The nonzero-effort reconstruction is an ideal-engine numerical fixture, not an interval certificate and not a bounded-engine feasibility certificate.
- The Jacobi routines report sampled diagnostics. No validated all-time crossing counter or production interval solver is included.
- The two-column PDF compiled with BibTeX references, no undefined references and no overfull boxes in the final log.

All runtime bounds need conservative arithmetic/enclosures before use as certified planner decisions. The supplied pointwise tests do not certify any complete atlas cell. No numerical atlas database was included in the input archive.
