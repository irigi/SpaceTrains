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
        double current_time_s) const = 0;
};
```

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
- `many_revolutions` — transfer sweeps > 1.25 turns around the Sun
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
