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

`load_universe()` either returns a complete static scenario definition or throws with the file and line.

**Files:** `DataLoader.cpp` (the loading order, core data: bodies, factions, goods, stations, recipes, events, and
the cross-file checks), `LoadShips.cpp` (classes, components, modules and hull slots, the starting fleet, spares,
operations, the ship environment and station radiation), `LoadEconomy.cpp` (labour, fuel, households and colonies,
incomes, treasuries, investment, liners, growth, pricing, orders, the market, reference prices),
`LoadPassengers.cpp` (travel settings, reference fares), `Csv.cpp` (CSV reading and the shared checks;
`LoaderInternal.hpp`). One `load_<file>()` function per data file, called in a fixed order: a file may refer only
to what an earlier one defined.

## Data Flow

```
CSV files -> DataLoader -> UniverseDefinition -> Simulation
```

## Invariants

- IDs are unique within each domain type.
- Every station references an existing body and faction.
- Every ship seed references an existing station, faction, and ship class.
- Every recipe references a known economy profile and commodity, and every `input:` feeds an output of the same
  station.
- Station ids in per-station files (radiation, incomes, orders, surface markets) exist and appear once; goods named
  in settings (colony essentials, stock days) exist.
- `stations/works.csv` and `stations/station_systems.csv` (optional) feed the station systems view only; every
  works' staffing must sum to 1. `economy/station_radiation.csv` has a `site` column (orbit, surface,
  underground, aerostat).
- Every ship class has its parts in `ship_classes/ship_components.csv`; top-level parts add up to the class's dry
  mass and value (within 1 kg and 1 cr), habitat parts to the habitat. The file is written by
  `build_ship_classes.py`, never by hand (the `ship_class_builder` test checks it).

## Deferred Work

- Mod layering / override support

## Tests

- Load seeded data successfully.
- Fail cleanly on missing required files.
- Detect invalid cross-references.
