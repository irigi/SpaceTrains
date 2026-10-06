# SpaceTrains Roadmap

## Done

### Phase 1 — VariableISP Integration and Headless Verification (2026-05)

- Electric ion ship classes with tech-level parameters (α, ε).
- `VariableIspTrajectoryPlanner` backed by the precomputed trajectory atlas; per-sample propellant via the I-invariant scaling.
- Multi-planner dispatch by propulsion type; Lambert solver and NTR ships for the Kepler side.
- Headless executable with `--verbose`, `--econ-audit`, and periodic reports.

### Phase 2 — Godot Observer Frontend (2026-06)

- Bridge snapshots carry `trajectory_path`, destination ghost, and snapshot timing for smooth interpolation.
- Procedural sci-fi HUD (top bar, entity browser, inspector, market panel, event ticker).
- Starfield, Sun bloom/halo, planet textures, orbit rings, procedural ship/station meshes, trails, engine glow.
- Economy: dynamic prices, credits, trade settlement, storage caps, profit-based dispatch, fleet growth; fleet-collapse fix (730-day audit: 0 stranded).

## Next — Trajectory Correctness Over Long Runs (current)

Unphysical trajectories appear after months of self-play:

- A VariableISP path ends with a sharp dent: the integrated trajectory misses the destination and the final sample is snapped onto the station.
- A path loops around the Sun many times with only ~10 samples per revolution.

Audit tooling is in place (`--trajectory-audit`, `--trajectory-sweep`; see `docs/modules/trajectory.md`).
Findings from the first 2-year sweep (192k plans, 2026-10-06):

- **Bug 2 = Kepler wait prefix — FIXED.** Paths for awaiting ships prepended only 11 samples of the origin orbit; a
  573-day wait at Venus drew 2.5 turns as a star polygon (83° per step). Now sampled at ≤ 5° of heliocentric sweep
  (11–360 segments); `coarse_wait` went from 11,982 plans to 0.
- **Bug 1 = VariableISP endpoint snap — FIXED.** 96% of nearest-cell seeds and 52% of interpolated seeds missed the
  target by > 0.01 AU (worst: Mars -> Titan overshoots to 22 AU); the snap drew a dent. The planner now refines a real
  atlas cell's seed by minimum-norm Newton shooting onto the destination station's actual position at arrival (and
  starts from the origin station's position at departure), trying up to 4 windows. Sweep: max miss 13.3 AU -> 0.0013 AU,
  `end_dent`/`endpoint_miss`/`start_miss` -> 0. Cost: ion plan p50 ~8 ms, p99 ~94 ms; 3-year headless run 1m36s -> 3m24s.
- **Integrator hang.** Some ion -> Titan plans got seeds that dive into the Sun and RK45 never finished (would
  freeze the simulation). Now guarded by a step budget; the planner treats failure as no window.
- **Theta wraparound.** Atlas theta spans ±1.1 rev; planner accepts retrograde/near-full-loop targets, and the
  integrated theta can land 2-4π away from the target.
- **Lambert path to Titan — FIXED.** A full-tank NTR Ceres -> Titan picks a hyperbolic Lambert arc; the path sampler
  only handled ellipses, so all samples sat at Ceres' radius with the departure time and the snap drew a 6.8 AU line.
  Now samples ellipses, hyperbolas, and near-parabolas (Barker). Sweep: Lambert max miss 6.8 AU -> 0.0001 AU.

Next fixes:

- Decide which theta branches (retrograde, >1 rev) are acceptable; atlas θ labels are only valid mod 2π
  (~25% of cells actually fly θ ± 2πn).
- Atlas time-optimality: ~55% of solved cells end with > 2% fuel left (the generator only penalised overuse), so
  transfer times are not minimal for the ship's kappa. Consider enforcing m_end = m_dry in the shooting.
- Planning cost: 3-year headless run 1m36s (before) -> 4m22s now (~0.24 s per simulated day). Cache refined plans
  (path depends on (ρ, θ, T) only, not κ) if the live bridge stutters.
- **Hohmann fallback dents — FIXED.** The half-ellipse is now drawn through the actual stations with Kepler timing,
  and elliptic arcs (Hohmann and Lambert) are sampled evenly in eccentric anomaly so the sharply curved far end of
  eccentric ellipses has no kinks. Sweep: Hohmann flagged 1,647 -> 0; Lambert end dents 3 -> 0.
- **Perihelion limit — FIXED.** No planner bounded perihelion: 8 Lambert and 4 ion plans passed *through the Sun*.
  Both planners now enforce 0.1 AU (`kMinPerihelionM`); ion feasible plans -5%.
- **Path sampling — FIXED.** Rendered paths are thinned by curvature (≤ 6° per point, ≥ 40 segments) from a dense
  generation. Sweep: chemical plans 0 flagged; ion 1 `coarse_sampling` left (was 3,034).
- Regression tests for each fixed case.

## Economy Follow-ups

- Re-run the 730-day audit; check whether goods still pile up at producers while consumers starve.
- Check whether short-hop clustering around Earth L1 survived the profit-based dispatch.
- **Ship operating costs**, parts A (done, calibrating) -> B mission-sized fuelling with cargo mass in the
  rocket equation -> C modular tanks and refits. See `docs/plans/ship_operating_costs.md`.
- **Technology plausibility review** of all ship classes (one consistent near-future tech level). See
  `docs/plans/tech_plausibility_review.md`. Do it with or before part B.
- **Fleet investment.** The starting fleet is not final, and some classes may simply be uneconomic on some
  routes. Owners should respond: sell or scrap ships laid up for long periods (salvage value), and commission
  new ships of the class with the best observed return on capital for the routes that need serving. Needs
  per-class, per-route earnings (the ship ledgers already hold most of it).
- **Open economy with exact accounting.** Stations produce for free and consumers only spend, so consumer
  stations bleed money (Lunar Gateway ended a 730-day run at -87k cr) while producers and home ports pile it up.
  Add an explicit external account (Earth's economy and faction treasuries): population income, subsidies to
  stations below a floor, taxes or stashing above a ceiling, and a slow controller that keeps the money supply
  roughly constant. The audit keeps checking that every credit moves between named accounts (internal money +
  external account = constant), so it still catches economy bugs.
- **Outer-system exports.** Give outer stations something worth shipping inward, not exotic matter.
  Candidates: platinum-group metals and other high-value metals from the Ceres belt; deuterium (heavy water)
  from Ganymede and Titan ice for fusion research and reactors; nitrogen and ammonia from Titan for the
  nitrogen-poor Moon and Mars; physical science samples (low mass, high value, limited demand). Earth's
  economy (the external account) buys them through the Earth stations at stable prices.

## Phase 3 — Persistence and Scenario Control

- Add save/load snapshots.
- Add seeded scenario setup and reproducible simulation runs (also needed to replay bad trajectories deterministically).
- Add scenario-level data for alternative starting economies or factions.

## Phase 4 — Richer Orbital and Mission Fidelity

- Improve Kepler planner with patched-conic SOI handoffs.
- Replace local direct-transfer approximation with proper planet-SOI arc (also fixes ~100% propellant use on Earth–Moon VariableISP-style hops).
- Add stranded-ship handling, support/refuel behavior, clearer mission state transitions.
- Consider outer Solar System bodies (Jupiter and beyond).

## Phase 5 — Ships with Individual Variation

- Add per-ship power multiplier (±α variation) and size multiplier.
- Size scaling preserves κ; power multiplier changes κ slightly per ship.
- Reflect individual ship specs in UI selection panel.

## Phase 6 — Post-v1 Systems

- Add pirates, police, inspections, rescue, and later combat resolution.
- Expand content: outer-system stations, more factions, science mission chains.
- Support custom star systems through data-only content changes.
