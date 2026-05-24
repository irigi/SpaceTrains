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

## Deferred Work

- Patched-conic handoffs and richer local orbital transfers
- Runtime `VariableISPPlanner`
- Runtime integration of the already verified `/VariableISP` atlas and solver outputs

## Tests

- Feasible routes return positive travel time and fuel cost.
- Impossible fuel cases return `feasible == false`.
- Sampled path generation is deterministic and matches departure/arrival station geometry.
- Local-transfer timing, Hohmann coast time, launch waits, and rocket-equation propellant accounting are covered by regression tests.
