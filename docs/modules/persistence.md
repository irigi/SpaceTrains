# persistence

## Purpose

Save games: the whole mutable state of a `Simulation` as JSON, loaded back into a `Simulation` built from the same
data files.

## Interfaces

- `Simulation::save_state_json()` / `Simulation::load_state_json(text)` (`src/persistence/SimulationPersistence.cpp`).
  A load parses and checks everything first and throws on a bad file, leaving the game unchanged.
- `Simulation::data_fingerprint()`: FNV-1a over station, ship class, goods and recipe definitions; a save made with
  other data is refused.
- `persistence::Json` (`src/persistence/Json.*`): a small JSON value with ordered objects; numbers are written in the
  shortest form that reads back to the same double, non-finite ones as the strings `"inf"`, `"-inf"`, `"nan"`.
- Headless: `--save-at DAY FILE`, `--load FILE` (then `--days N` more).
- Bridge: the command file's `request` number with `save_path` or `load_path`; the snapshot's `bridge.status` reports
  the result and `bridge.epoch` changes on every load.
- UI: F5 quick save, F9 quick load (`user://saves/quicksave.json`); on a new epoch the UI forgets its timeline
  (display clock, cached paths, price and economy history).

## Invariants

- A game saved, loaded and continued equals one that never stopped, byte for byte (test in `tests/test_main.cpp`:
  45 days + save/load + 45 days against 90 days, compared as saved JSON).
- Plan caches are not saved: `estimate_leg` and the Lambert grid are pure functions of their keys.
- Maps the simulation iterates are ordered (`Inventory`, faction treasuries), so a loaded game iterates, and
  chooses, as the original did.

## Format

Version 1: `version`, `data_fingerprint`, game time and the tick remainder, timewarp, outside-economy credits,
treasuries, money-supply target, investment state, stations, ships (with their missions and planned paths), sold
ships, recent events and trades. About 140 KB at day 45.
