# data_loader

## Purpose

`data_loader` is the single module responsible for loading static game content from disk into the simulation. It converts CSV files today into `UniverseDefinition`, and later may support richer formats, but other modules must not read gameplay content files directly.

## Responsibilities

- Scan the configured data root.
- Parse content files into strongly typed structures.
- Validate required fields and cross-references.
- Build the complete `UniverseDefinition`.

## Non-responsibilities

- Save/load runtime snapshots
- Godot scene loading
- Runtime mission logic
- Planner calculations

## Public Interfaces

```cpp
class DataLoader {
public:
    UniverseDefinition load_universe(const std::filesystem::path& root) const;
};
```

`load_universe()` must either return a complete static scenario definition or fail clearly. Partial silent loads are not acceptable once validation is hardened.

## Data Flow

```
CSV files -> DataLoader -> UniverseDefinition -> Simulation
```

## Invariants

- IDs are unique within each domain type.
- Every station references an existing body and faction.
- Every ship seed references an existing station, faction, and ship class.
- Every recipe references a known economy profile and commodity.

## Deferred Work

- Stronger validation reporting
- Alternative file formats
- Mod layering / override support
- Explicit schema versioning

## Tests

- Load seeded data successfully.
- Fail cleanly on missing required files.
- Detect invalid cross-references.
