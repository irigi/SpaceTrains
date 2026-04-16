# bridge

## Purpose

`bridge` is the planned boundary between the authoritative C++ simulation and the Godot frontend. It will replace the current Godot CSV preview shell with a live query-and-command surface.

## Responsibilities

- Own or reference a running `Simulation`.
- Expose read-only query methods for Godot.
- Convert core state into Godot-friendly shapes.
- Accept a narrow set of player/debug commands and route them into the simulation.

## Non-responsibilities

- Owning game rules
- Loading static scenario files directly in GDScript
- Presentation layout and widget logic

## Planned Public Interfaces

```cpp
class SimulationBridge {
public:
    void initialize(const std::string& data_root);
    void step(double real_dt_s);

    SimulationSnapshot get_snapshot() const;
    std::vector<Vec3d> get_body_positions() const;
    std::vector<Vec3d> get_station_positions() const;
    std::vector<Vec3d> get_ship_positions() const;

    void set_timewarp(double factor);
    void pause(bool paused);
};
```

The exact Godot binding surface may change, but the architectural rule should not: Godot reads through the bridge and does not own simulation state directly.

## Data Flow

```
Godot input -> SimulationBridge commands -> Simulation
Simulation -> SimulationBridge queries -> Godot rendering/UI
```

## Invariants

- The bridge mirrors the simulation; it does not duplicate gameplay logic.
- Queries are safe to call every frame.
- Commands are intentionally narrow during early stages.

## Deferred Work

- GDExtension implementation details
- Selection-detail APIs
- Orbit sample queries and event streaming
- Pause/resume and stepping semantics for the editor/debugger

## Tests

- Godot can initialize and tick a simulation through the bridge.
- Query calls are stable across frames.
- Timewarp and pause commands affect the simulation as expected.
