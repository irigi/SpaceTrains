# trajectory

## Purpose

`trajectory` owns ship transfer planning. It converts origin/destination/station/body context plus ship capability into a mission-feasibility result that the simulation can use without knowing the planner internals.

## Responsibilities

- Define the stable planner interface.
- Provide `KeplerTrajectoryPlanner` as the default implementation.
- Return feasibility, ETA, propellant cost, and sampled render path.
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

`TrajectoryPlan` must remain the common contract across Kepler and later VariableISP implementations.

## Data Flow

```
simulation -> ITrajectoryPlanner -> TrajectoryPlan -> mission assignment / rendering
```

## Invariants

- Planning is read-only with respect to the simulation state.
- The planner must not mutate inventory, ships, or stations.
- A feasible plan includes enough information to reserve fuel and estimate arrival.

## Deferred Work

- Richer Kepler transfers with windows and orbital state samples
- `VariableISPPlanner`
- Verification against `/VariableISP` atlas and solver outputs

## Tests

- Feasible routes return positive travel time and fuel cost.
- Impossible fuel cases return `feasible == false`.
- Sampled path generation is deterministic.
