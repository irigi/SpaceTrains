# celestial

## Purpose

`celestial` provides the spatial model of the Solar System for the rest of the game. It answers where bodies and station anchors are at a given time and supplies the geometry needed by trajectory planning and rendering.

## Responsibilities

- Store body lookup by ID.
- Propagate body positions from simple orbital definitions.
- Compute station anchor positions around parent bodies.
- Expose root-body and heliocentric-radius queries.

## Non-responsibilities

- Ship integration
- Economy logic
- Mission generation
- Godot transforms

## Public Interfaces

```cpp
class CelestialMechanics {
public:
    explicit CelestialMechanics(const UniverseDefinition& universe);

    const CelestialBodyDefinition& get_body(const std::string& body_id) const;
    Vec3d get_body_position(const std::string& body_id, double time_s) const;
    Vec3d get_station_position(const StationDefinition& station, double time_s) const;
    double get_heliocentric_radius(const std::string& body_id, double time_s) const;
    std::string get_root_body_id() const;
};
```

## Data Flow

```
UniverseDefinition -> CelestialMechanics -> trajectory / bridge / simulation
```

## Invariants

- The universe has exactly one root body.
- Body queries are deterministic for the same input time.
- Station anchors are derived from body position plus station-local orbit/placement data.

## Deferred Work

- Kepler element propagation with higher fidelity
- SOI lookup and patched-conics helpers
- Derived orbital-state utilities for moving ships

## Tests

- Root body lookup works.
- Body positions are stable and non-zero for non-root bodies.
- Station positions change consistently with time.
