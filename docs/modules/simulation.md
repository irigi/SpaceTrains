# simulation

## Purpose

`simulation` is the authoritative runtime coordinator. It owns the mutable game state and advances the sandbox over time by invoking economy updates, mission generation, refueling, and ship state transitions.

## Responsibilities

- Own current game time and timewarp.
- Own station state, ship state, active missions, and event history.
- Advance the simulation in deterministic ticks.
- Refuel ships when local inventory allows it.
- Score and assign cargo missions using economy signals and trajectory results.
- Expose snapshots for presentation and debugging.

## Non-responsibilities

- Direct Godot scene control
- Static data loading from disk
- Raw trajectory math internals
- Combat and policing logic in the current stage

## Public Interfaces

```cpp
class Simulation {
public:
    static Simulation from_data_root(const std::string& data_root);

    void step(double real_dt_s);
    void set_timewarp(double timewarp_factor);

    const UniverseDefinition& universe() const;
    SimulationSnapshot snapshot() const;
    std::string build_report() const;
};
```

## Data Flow

```
Simulation.step()
    -> economy.step()
    -> refuel logic
    -> mission scoring
    -> trajectory planner call
    -> ship mission phase update
    -> event log append
```

## Invariants

- The simulation is the only owner of mutable ship/station runtime state.
- Mission assignment consumes cargo and reserves fuel before departure.
- Snapshot queries do not mutate simulation state.

## Deferred Work

- Save/load
- Stronger mission state machine
- Rescue/support missions
- Explicit deterministic random seeding for scenario variation

## Tests

- Time advances when stepping.
- Missions can be dispatched when supply, demand, and fuel align.
- Event history records departures and arrivals.
- Stranded state appears when refueling fails.
