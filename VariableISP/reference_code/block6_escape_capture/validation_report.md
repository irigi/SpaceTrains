# Block 6 validation report

- Python checks: passed.
- C++17 checks: passed with `-O2 -Wall -Wextra -Werror`.
- Scalar progress/time integrals checked against adaptive quadrature and time-domain ODEs.
- Minimum reduced time decreases as the allowed effort increases across the tested range.
- Reduced profiles exercise early minimum-exhaust, constant acceleration, late maximum-exhaust and maximum-exhaust/coast cases.
- Exact-shape effort quadrature agrees with 651/16000 + pi*sqrt(3)/240.
- Independent vector ODE forward endpoint max error: 1.73881e-11.
- Independent reflected-capture endpoint max error: 5.64515e-12.
- Work and angular-momentum increments agree with their endpoint identities.
- Full-time exact-shape band bounds are analytic; samples are not used to establish that feasibility.
- SOI chart derivative max finite-difference error: 6.99363e-12.
- Kinematic covector rescaling preserves endpoint pairings with the action scale.
- The paper compiled to six pages with no overfull boxes or unresolved references in the final compile log; pages were rendered for visual inspection.

## Scalar-query timing

300 C++ queries, optimized with `-O3`, in this shared execution environment:

- Median: 0.070978 ms.
- Mean: 0.0746179 ms.
- 95th percentile: 0.099551 ms.
- Maximum observed: 0.156576 ms.

This measures only the finite-band monotone averaged minimum-effort kernel, including its nested scalar roots. It excludes orbit integration, exact BVP solves, station phase optimization, atlas lookup, interface optimization and certification. It is an observed timing, not a hardware-independent latency guarantee.

## Limits

No interval computations or certified general BVP roots are supplied. Reduced-model optima are not full Kepler lower bounds. The exact shaped path is feasible and has a symbolic lower/upper effort bracket for its endpoints; it is not shown stationary. SOI charts assume a transverse event, unchanged engine and consistent units. Degenerate, simultaneous or active-constraint events require extra treatment.
