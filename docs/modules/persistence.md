# persistence

## Purpose

Save games: the whole mutable state of a `Simulation` as JSON, loaded back into a `Simulation` built from the same
data files.

## Interfaces

- `Simulation::save_state_json()` / `Simulation::load_state_json(text)` (`src/persistence/SimulationPersistence.cpp`).
  A load throws on a bad or foreign file and leaves the game as it was: it saves the state first and loads that
  back if anything in the file fails.
- `Simulation::data_fingerprint()`: FNV-1a over the definitions a save depends on (stations, ship classes, goods,
  recipes, the passenger, environment and labour settings); a save made with other data is refused. It is
  deliberately narrower than the bridge's opening-cache key, so a save still loads after, say, a price tweak.
- `persistence::Json` (`src/persistence/Json.*`): a small JSON value with ordered objects; numbers are written in the
  shortest form that reads back to the same double, non-finite ones as the strings `"inf"`, `"-inf"`, `"nan"`.
- Headless: `--save-at DAY FILE`, `--load FILE` (then `--days N` more). Saves are written aside and renamed.
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

Version 20 (`kSaveVersion` in `SimulationPersistence.cpp` lists what each version added): `version`,
`data_fingerprint`, game time and the tick remainder, timewarp, the background accounts, treasuries and faction
credit, investment state and probes, stations (stock, orders, residents, jobs, ledgers), colonies, ships (missions
with their planned paths, fits, wear, doses, passengers, contracts), sold ships, recent events and trades, the
random generators, and the money journal with its monthly history. About 1.2 MB at day 730.
