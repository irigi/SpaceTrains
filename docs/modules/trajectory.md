# trajectory

## Purpose

`trajectory` owns ship transfer planning. It converts origin/destination/station/body context plus ship capability into a mission-feasibility result that the simulation can use without knowing the planner internals.

## Responsibilities

- Define the stable planner interface.
- Provide `KeplerTrajectoryPlanner` as the default implementation.
- Return feasibility, ETA, wait/coast timing, rocket-equation propellant cost, and timed sampled render path.
- Preserve the extension point for future planner families.

## Non-responsibilities

- Ship state updates over time
- Economy scoring
- Save/load
- Godot rendering

## Public Interfaces

```cpp
class ITrajectoryPlanner {
public:
    virtual ~ITrajectoryPlanner() = default;

    virtual TrajectoryPlan plan_transfer(
        const StationDefinition& origin,
        const StationDefinition& destination,
        const ShipState& ship,
        const ShipClassDefinition& ship_class,
        double current_time_s,
        const PlanningOptions& costs = {}) const = 0;
};
```

`PlanningOptions` carries what the owner pays per kg of propellant and per day of wait plus transit, the
payload mass (cargo and provisions), the propellant the ship could still buy before departure, and the
reserve fraction. With costs set, the Kepler Lambert search minimises `burn * cr_per_kg + days * cr_per_day`
over feasible candidates, and the VariableISP planner picks among fuel budgets (100/75/55/40% of what it
could load) by the same cost. Both load the burn plus the reserve, never less than is aboard, and report it
as `TrajectoryPlan::propellant_load_kg`. All defaults reproduce plain physics: the fastest transfer with
the propellant aboard and no payload (tests, sweeps).

`TrajectoryPlan` must remain the common contract across Kepler and later VariableISP implementations. Its timed samples are copied into active missions and are authoritative for bridge ship positions and selected trajectory rendering.

## Data Flow

```
simulation -> ITrajectoryPlanner -> TrajectoryPlan -> mission assignment / rendering
```

## Invariants

- Planning is read-only with respect to the simulation state.
- The planner must not mutate inventory, ships, or stations.
- A feasible plan includes enough information to reserve fuel, estimate launch/arrival, and render the mission path.
- Same-parent station transfers use a bounded local direct arc instead of a Sun-centered transfer.
- Interplanetary Kepler planning is currently circular-orbit and coplanar. It uses a launch-window wait, Hohmann half-period coast time, and exact station endpoints.

## VariableISP Seed Refinement

Atlas cells are solved for their own grid (ρ, θ), and their θ label is only valid mod 2π. The planner ranks launch
windows from neighbouring cells, then refines a real cell's seed with
`VariableIspIntegrator::refine_seed()`: a minimum-norm Gauss-Newton shooting over the four costates plus T
(Jacobian-column scaled), hitting r, θ, v_r = 0, and v_θ = v_circ. The target is the destination station's actual
position at departure + T (a moving target, since the ship arrives co-moving with it), and the path is scaled and
rotated from the origin station's position at departure. Windows whose refinement fails or whose corrected path
needs more fuel than the ship carries are skipped (up to 4 tried). The path shape does not depend on κ: κ only
decides whether the fuel suffices.

**κ at the origin's radius.** The atlas is solved at r0 = 1 AU and scaled to the origin's orbit: lengths by
r/1 AU, times by (r/1 AU)^1.5, accelerations by (r/1 AU)^-2, so the transfer needs (r/1 AU)^-2.5 of the canonical
∫a²dt. The planner therefore looks up and integrates with κ·(r/1 AU)^2.5, where κ = 2P(1/m_dry − 1/m0)·(1 AU)^2.5/μ^1.5;
with that the propellant formula (I_real = I_canonical·δ_real/δ_canonical) holds unchanged. Before, κ was taken at
1 AU for every origin: plasma ships leaving Jupiter were planned about 60x too weak and those leaving Saturn 280x
(no feasible way home to Earth), those leaving Mercury 10x too strong (unphysically fast and cheap).

**Missed windows.** Refinement moves a window a little. When it moves just into the past (phase error under
0.3 rad), the ship leaves now and the station-targeting pass absorbs the error; only if that does not converge does
it wait for the next window. Before, the wait wrapped to almost a whole synodic period (Ganymede -> Ceres planned
3133 days, 2705 of them waiting; now 432).

## VariableISP Refinement in Primer Form

`VariableIspIntegrator::refine_seed_stm` solves the refinement above with the general solver below:
the atlas costates map exactly to the Cartesian primer (p = k·λ_v, w = −k[λ_r e_r + (C_θ + v_θλ_vr − v_rλ_vθ)/r e_θ]),
the shooting uses the analytic transition matrix instead of finite differences, and the result maps back
to atlas costates, so the rest of the planner is unchanged. The finite-difference `refine_seed` remains for tests.

- **One pass.** The station-targeting refinement starts from the atlas seed; the launch wait comes from the
  seed's T. The seed's winding branch is checked inside the shooting (its first evaluation), so the seed is
  not integrated separately.
- **Perihelion inside.** Paths diving below half the perihelion limit fail inside the shooting; the shooting
  also reports the exact closest approach, end state and effort.
- **Scoring plans** (`include_path` off) take mass (from the effort) and perihelion from the shooting; only
  plans with a path integrate the trajectory (4000 samples).
  - **Fuel check (fixed 2026-10-21).** Until then the scoring
    plan's check read the closest-approach marker, which carries the start mass, so a window short of fuel
    scored as feasible. The chosen mission's replan with its path then failed and the ship picked the same
    trip the next day (1460 days: 2,821 of 3,300 departure replans failed with passengers on, 1,876 of 2,374
    off, mostly Mercury and Venus routes).
  - **Larger budget for the same path.** The path does not depend on the fuel budget, so a refined path over
    its budget is flown with the smallest of the ship's larger budgets that carries it, before any time fit
    and with no further shooting. Checking the end state alone refined the next windows instead (+46%
    shootings, 25% slower); with the larger budget, 37% fewer shootings than the old check and no failed
    departure replan. 79% of the over-budget paths came from the 40% and 55% budgets.
- **Fuel fit.** When the corrected path needs at most 10% more effort than the fuel allows and no larger
  budget carries it, `fit_effort_by_time` changes T until it fits (56% of candidates used to be rejected as
  fuel-limited). Fits up to 1.6x and 2x over were measured on 2026-10-21: 7x the planning time.
- **Shared refinements.** The geometric refinement of an atlas cell is cached per (route, plan time, cell):
  the path does not depend on the ship, so plans differing only in payload or fuel reuse it and apply their
  own fuel check.
- **Fuel fits** are cached too, by refinement, departure option, rho, kappa and perihelion limit: a pure
  function of these. The chosen mission's replan with its path then finds the fit its scoring plan made.
- Optional, off: `PlanningOptions::direct_fallback` (solve a departure now directly when no window converges)
  and `ShootingSettings::spend_fuel_budget` (shorten T to use all the fuel); both measured without economy gain.

The Kepler planner caches its Lambert grid rows at departures aligned to multiples of the grid step (plus a
fresh "leave now" row), shared across plans and steps.

## General-Boundary Variable-Isp Transfers (prototype)

`variable_isp/GeneralTransfer.hpp` solves constant-power transfers between arbitrary states — the primitive
for chases (pirates, police). Minimum propellant is minimum effort J = ∫|a|²dt, so the path never depends on
the mass; the unknowns are the primer (thrust p0 and its rate w0). Normalised units: μ = 1, caller's length.

- `shoot` — Levenberg-Marquardt shooting on (p0, w0[, T]); rendezvous (match r, v) or intercept (match r,
  thrust ends at zero); fixed, coasting, sampled or thrusting targets with their motion in the Jacobian.
- `solve_transfer` (fixed T) — seed portfolio: zero primer (Newton from it is the linear-quadratic solution
  about the coasting arc), polar shape method (Hermite + Legendre bubbles, Gauss-Newton on ∫a²) with a primer
  fit, then multiple shooting, boundary homotopy and continuation in T as fallbacks. 99–100% valid, median
  0.4–0.8 ms on the bench sets.
- `solve_min_time` — earliest arrival for an effort budget (fuel load), never shorter than its lower bound
  `T_lo` (a first grid point that already fits is kept as it is).
- `trajectory/ChaseSolver.hpp` — SI adapter: `path_target` follows another ship's `TrajectoryPlan` samples,
  `plan_chase` returns the earliest interception or rendezvous for a ship's power and propellant.

Benchmarks: `spacetrains_visp_bench <A|R|AG|B|C|D|E|F>` (`tools/visp_bench/main.cpp`: A/R atlas cells as
intercepts and rendezvous, AG the same through the general solver, B-F general-boundary sets). The solver's design
brief is `docs/plans/variable-isp-solver-directions.md`.

## Shared Helpers

`trajectory/OrbitMath.hpp`: `positive_mod`, a body's orbital rate, and the heliocentric rate a moon's station
phases with (its planet's). `VariableIspAtlas::lower_cell_index` is the one grid lookup (planner and atlas).
The atlas file (`tests/data/variable_isp/variable_isp_atlas.bin`, next to the data root) is checked on load: axis
counts, file size and increasing axes; a bad file leaves plasma ships unable to plan, with a message.

## Perihelion Limit and Path Sampling

Both planners reject transfers that come closer to the Sun than `kMinPerihelionM` (0.1 AU,
`TrajectoryPlanner.hpp`): Lambert candidates via `conic_arc_min_radius()`, ion windows via the integrated path.
Paths are generated densely and thinned for rendering by `select_for_rendering()` (`PathSampling.hpp`): a point is
kept whenever the accumulated turn reaches 6° or the segment reaches 1/40 of the path length.

## Trajectory Audit

`trajectory/TrajectoryAudit.{hpp,cpp}` checks a `TrajectoryPlan` for unphysical geometry. Planners fill
`TrajectoryPlan::diagnostics` with what the final path hides: the pre-snap start/end miss and, for VariableISP,
the atlas inputs (rho, kappa, target/actual theta) and whether the seed was interpolated or a nearest-cell fallback.

Flags (thresholds in `TrajectoryAuditThresholds`):

- `endpoint_miss` / `start_miss` — unsnapped path endpoint is > 0.01 AU from the station
- `end_dent` — sharp turn at the penultimate sample plus an oversized last segment (the snap made visible)
- `many_revolutions` — transfer sweeps > 1.25 turns around the Sun (> 2 for variable-Isp spirals: an inward
  spiral from Venus to Mercury sweeps 1.3-1.4 turns in ~245 days)
- `coarse_sampling` — the rendered transfer path changes direction by > 15° at one point
- `coarse_wait` — wait-period samples > 20° of heliocentric angle apart
- `below_min_perihelion` — path comes closer to the Sun than `kMinPerihelionM` (0.1 AU)
- `non_finite`, `non_monotonic_time`

The headless `--trajectory-audit` and `--trajectory-sweep` flags drive it (see README).

## Porkchop Plots

`trajectory/Porkchop.{hpp,cpp}` (2026-10-27), display only (the statistics screen's Routes tab and the station
inspector's launch windows; nothing in the simulation reads it).

- **Per pair of planets** (stations around one planet have no windows; a moon's station flies from its planet):
  departures at multiples of a step = span / 96, span = min(synodic period, 730 days), 48 rows per drive. The axes
  come from the bodies' fixed orbits, so a column is a pure function of (pair, departure index).
  - Nuclear-thermal rows: 0.3-1.5 Hohmann times, the planner's Lambert search range.
  - Variable-Isp rows: log spaced from 0.08 Hohmann times (at least 20 days) to 4 (at most Hohmann + 730 days).
    Measured over two game years (2026-10-27): plasma trips take 0.12-0.25 Hohmann times to and between the outer
    planets and up to 3 between Mercury and Venus, while thermal trips stay at 0.97-1.23. The first version shared
    the thermal axis (0.25-2.0), which wasted most plasma rows on flights nobody makes and left holes.
- **Cells are geometry only**, one grid for every ship:
  - nuclear-thermal: Δv of `KeplerTrajectoryPlanner::lambert_cell`, the same cell the planner's grid uses (against
    circular speed at both ends; empty when the arc does not solve or passes inside 0.1 AU);
  - variable-Isp: the effort J = ∫a²dt of the minimum-effort rendezvous between the planets
    (`general::solve_transfer`, μ = 1, length 1 AU: the atlas's κ scale), rejected inside 0.1 AU. Fast settings first
    (no fallbacks, two branches); a cell they miss is solved again with the full portfolio (Earth-Saturn: 6% of cells
    on the old axis, all solved, ~0.2 s each). Holes left: at most 3 of 4,608 cells per pair measured.
    The 0.1 AU rule is a filter, not a constraint of the solver, so the map has sharp steps where the cheapest family
    starts to pass inside it (Earth-Mercury 2026-10-27: closest approach 0.109 → 0.102 AU, then the next family at
    ×2.7 effort). The planner rejects the same paths, so the step is what ships can fly; the plasma note says so.
  - A class's reach (UI, `ui/Porkchops.gd`), with the planner's 10% reserve kept back: thermal Δv = v_e ln(m0/m_f);
    plasma 1/m_f − 1/m0 = J_SI / 2P, J_SI = J μ^1.5 / AU^2.5, P = specific power × dry mass. Its fastest trip at a
    departure is the shortest row it reaches (interpolated: Δv linearly, effort on log scales).
- **Routes in use:** the bridge sums the flow record's lanes (by class) over the last year, planet to planet, once a game
  day (`set_routes`). The UI's request may say `earth>*` for every route flown from Earth (the station inspector).
- **Cheap:** one background thread at low priority (nice 15 on Linux) with its own celestial mechanics.
  - Pairs on screen first, nearest departures first; when idle, each route in use is filled once (its window from when
    the prefetch began), inner planets first. Pairs off screen are not kept up to date: at 5 d/s an outer pair needs
    about 0.4 of a core to keep adding columns, so opening one again computes only the days that have passed.
  - Disk cache (`--porkchop-cache`, the UI passes `user://porkchop_cache`): each column is appended to a file per pair
    with a fingerprint of the orbits, axes, algorithm tag and a hash of the code and data the plots depend on
    (`src/{math,celestial,trajectory,variable_isp}`, `data/bodies`; made at every build by
    `cmake/PorkchopSourceHash.cmake`); a file whose fingerprint differs is ignored and rewritten, so any change there
    recomputes the plots and nothing else does. A later session reads it back. Every game starts on the same
    date, so the windows of the first years are computed once ever. Columns stay in memory (their primers, the seeds,
    are dropped once well behind the present).
  - Variable-Isp cells are warm-started from the shorter flight below, and for pairs with a Hohmann time under 500 days
    also from the same flight one departure earlier, keeping the lower effort (the two seeds can land on different
    local minima, which showed as speckle). A down pass then solves again, from the longer flight above, each cell
    much dearer (×1.25) than the one above, dearer than the one below, or missing: seeded from below, Earth-Saturn
    columns stayed on a family of solutions up to 3× dearer than the one found at longer flights, and the reach lines
    wrapped around islands (the "kink"). Rises left after it are at flights of a Hohmann time or more, far from where
    plasma ships fly.
  - Measured (one core, whole window): Earth-Mars 16 s, Mercury-Earth 35 s, Earth-Jupiter 26 s, Earth-Saturn 74 s.
- **Bridge:** the UI names pairs in the command file (`"porkchop":"earth>mars;earth>*"`); the bridge writes
  `<snapshot>.porkchop` (+ `.seq`) as columns arrive, at most twice a second, with both row axes, the routes in use, the
  classes' masses, cargo units, exhaust speed and power, and each station's planet.

## Deferred Work

- Patched-conic handoffs and richer local orbital transfers

## Tests

- Feasible routes return positive travel time and fuel cost.
- Impossible fuel cases return `feasible == false`.
- Sampled path generation is deterministic and matches departure/arrival station geometry.
- Local-transfer timing, Hohmann coast time, launch waits, and rocket-equation propellant accounting are covered by regression tests.
