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

**κ at the origin's radius (v33).** The atlas is solved at r0 = 1 AU and scaled to the origin's orbit: lengths by
r/1 AU, times by (r/1 AU)^1.5, accelerations by (r/1 AU)^-2, so the transfer needs (r/1 AU)^-2.5 of the canonical
∫a²dt. The planner therefore looks up and integrates with κ·(r/1 AU)^2.5, where κ = 2P(1/m_dry − 1/m0)·(1 AU)^2.5/μ^1.5;
with that the propellant formula (I_real = I_canonical·δ_real/δ_canonical) holds unchanged. Before, κ was taken at
1 AU for every origin: plasma ships leaving Jupiter were planned about 60x too weak and those leaving Saturn 280x
(no feasible way home to Earth), those leaving Mercury 10x too strong (unphysically fast and cheap).

**Missed windows (v33).** Refinement moves a window a little. When it moves just into the past (phase error under
0.3 rad), the ship leaves now and the station-targeting pass absorbs the error; only if that does not converge does
it wait for the next window. Before, the wait wrapped to almost a whole synodic period (Ganymede -> Ceres planned
3133 days, 2705 of them waiting; now 432).

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

## Deferred Work

- Patched-conic handoffs and richer local orbital transfers

## Tests

- Feasible routes return positive travel time and fuel cost.
- Impossible fuel cases return `feasible == false`.
- Sampled path generation is deterministic and matches departure/arrival station geometry.
- Local-transfer timing, Hohmann coast time, launch waits, and rocket-equation propellant accounting are covered by regression tests.
