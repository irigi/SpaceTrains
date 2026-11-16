# Plan: saves that survive data changes (Architecture 4, conditional)

Written 2026-11-16 from the roadmap's Architecture item 4 (outside review 2026-11-12). **Not to be built unless its
trigger is met.**

- **Trigger:** long-running games matter, to the user or to other players: a game kept for weeks while the data is
  still being tuned.
- Independent of Architectures 1-3. If Architecture 1 is under way, do this after its persistence step (step 13), so
  the code moves only once.

## Where it stands (main at PR #39)

- Saves are `save_state_json` / `load_state_json` in `src/persistence/SimulationPersistence.cpp` (lines 885 and
  1082). `kSaveVersion = 22` (line 19, whose comment lists what each version added). `docs/modules/persistence.md`
  still says version 20.
- **Two gates refuse a save** (`read_state_json`, ~1097-1102):
  - the version must be 20, 21 or 22: `save: version N is not supported (expected 22)`;
  - the data fingerprint must match: `save: made with different data files (stations, ship classes, goods or recipes
    changed)`.
- **The fingerprint** (`data_fingerprint()`, 754-883) is one FNV-1a hash over ids **and** numbers: station ids,
  bodies, populations and storage; ship class masses, tanks, cargo, crew and berths; about 30 passenger settings; ship
  environment, wear and labour settings; spare parts; modules with mass, price and effects; hull slots; commodity
  prices; households, wages, labour market, yards, fuel factory prices, background economies, surface markets,
  station incomes; order settings; recipes. Retuning any of these numbers, or adding a station or ship class, makes
  every existing save unloadable.
- **Stations must match by position.** The `stations` array is written in data order; on load the count must equal
  the data's and `stations[i].station_id` must equal `universe.stations[i].id` (1120-1127).
- **The money journal** is copied as is (`MoneyJournal journal = journal_`, ~1109); its accounts are indices built
  from the current data by `setup_money_journal`, so a different station or faction list misreads it.
- **Older versions** load by "a missing key means its default": no flow record (v20) → empty, reseeded from
  `flow_stocks()`; no derelicts/patrols → none / the starting cutters; failure clocks drawn on load for v21
  (`init_failure_clocks()` always runs, 1351). Optional ship keys are read the same way.
- **Everything is referred to by string id** (ship `class_id`, `fit`, `faction_id`, home and current station; trades
  and emergencies by station and commodity). Some ids are checked on load (`register_design`, `get_ship_class`,
  `get_station_definition`), most only implicitly.
- **A failed load leaves the game unchanged:** `load_state_json` saves the current state first and restores it on any
  exception. The bridge reports `Load failed: <message>` in the snapshot status and the UI shows it for 3 seconds.
- **Tests** (`tests/test_main.cpp`): the exact-continuation test (~1148-1198: 45 days, save, load, 45 more equals 90
  straight; a loaded game saves back identically; version 999 refused; a save with a missing late field refused and
  the game restored), and variants for contracts, the environment tier and passengers. **No test loads a v20 or v21
  save, and none covers a fingerprint mismatch.**

## Design (save version 23)

### Two fingerprints and an id list

- `structure_fingerprint()`: the ids only (stations with their parent bodies, bodies, commodities, ship classes,
  hulls, modules and slots, factions, colonies). `tuning_fingerprint()`: everything else `data_fingerprint()` hashes
  today. Both are written; `data_fingerprint` is no longer written.
- The save also writes `data_ids`: the ids of each kind as they were at save time. The structure fingerprint is then
  only a quick "nothing changed" check; the id lists say *what* changed.

### What happens on load

| Situation | Result |
|---|---|
| both fingerprints match | loads as today |
| tuning differs | loads; status: "Loaded day X. Data changed since this save; the game continues under the new numbers." |
| ids added in the data (a new station, ship class, module, commodity) | loads; new stations start as in a new game; the status names them |
| an id the save refers to is gone | refused: "save: refers to ship class `X`, which the data no longer has" (each missing id, up to five) |
| version below 20 or above 23 | refused as today |

Missing ids are found by reading, not by guessing: every lookup on the load path goes through one helper that
throws `MissingData{kind, id}`, collected into the message. The current rollback (save first, restore on failure)
stays.

### Stations by id

- Read the saved `stations` into a map by `station_id` and fill `stations_` in **data order**. A station in the data
  but not in the save starts fresh: factor the per-station part of the constructor (starting inventory, households,
  order stats, incomes, journal accounts) into `init_station(index)` and call it here.
- A station in the save but not in the data is refused (ships, contracts and journal history refer to it). Allowing
  it would mean closing a station during play, which is the parked "world that changes", not this plan.
- **Money stays exact:** a new station's starting stock and credits are booked as seeded money supply
  (`seeded_money_supply_`) and its residents' money to the background economy, as at a new game's start, so the
  audit's drift stays 0.

### Journal accounts by id

- Save each account's key (`station:<id>`, `households:<id>`, `faction:<id>`, `colony:<id>`, `fleet:<faction>`, the
  six sectors) with its balance and history. On load, map keys to the current account indices; a new account starts
  at 0 with a zero-filled history; a saved account whose owner is gone refuses the save (as above).
- Same for `flow_record` series.

### Ships when their class or modules are retuned

- The ship keeps its class, fit and state; its design is rebuilt from the new numbers (`register_design`).
- Propellant above the new tank size is written off as a loss in the journal (category as for boil-off). Cargo above
  the new capacity stays aboard until it is unloaded; no new cargo is loaded above capacity.
- A ship in flight finishes its stored plan; the next plan uses the new numbers.
- Prices, orders and reference prices recompute from the new data on the first day, as they do now.

### Versions

- 23 writes the new keys. 20-22 load through defaults as now, plus: no `data_ids` → the old single-fingerprint check
  (they cannot say what changed, so they load only when the data matches). Write that rule down in
  `persistence.md` instead of loosening it further.
- `load_state_json` returns a `LoadReport {bool tuning_changed; std::vector<std::string> added; std::vector<std::string> notes;}`
  that the bridge turns into the status line; the public signature changes only in its return type.

### Guarantee

"Saved, loaded and continued equals never stopped" holds when the data is unchanged. With changed data the game
continues under the new numbers; nothing more is promised. The existing exact-continuation test stays as it is.

## Steps

1. **Fixtures first.** Commit small saves in `tests/data/saves/`: a v22 save made on `main` (day 60), and v20 and
   v21 saves made from the last commits that wrote those versions (`git log -S"kSaveVersion = 21"` and so on), if they
   still build. A test loads each and runs 10 days. This pins down today's behaviour before changing it.
2. **Structured missing-id errors** on the load path (no format change).
3. **Journal and flow record keyed by id** (version 23).
4. **Stations by id and `init_station`**, with the money booking and a test that adds a station to a copy of the data,
   loads a v23 save, and checks drift 0 after 30 days.
5. **Split fingerprints and `data_ids`**, the load table above, `LoadReport` and the status line.
6. **Ship clamps** (tank, cargo) with a test that shrinks a tank in a copied data set.
7. **Docs:** `persistence.md` (version 23, the table above, what is guaranteed).

## Checks

- `ctest`, with the new tests: each fixture loads; a tuning change loads with the notice; an added station loads and
  keeps drift 0; a removed station is refused and named; a shrunk tank writes off the excess and keeps drift 0.
- The exact-continuation tests unchanged and green.
- A 365-day run's save, with unchanged data, still loads and continues byte-identically (the existing test covers
  it).
- `tools/benchmark.py` once at the end: saving format changes must not change the game (unmet demand, profitable
  ships, drift identical to before for the same starts).
- By hand in the game: F5, change one number in `data/economy/*.csv`, restart, F9: the notice appears and the game
  goes on.

## Risks

- **New accounts mid-history** can confuse the Statistics charts (a series that starts at a later month). Zero-fill
  and check the People and Flows tabs by eye.
- **Silent nonsense after big retunes** (e.g. halving populations): the game continues, but the economy may lurch for
  a while. Acceptable for an observer; the notice says the numbers changed.
- **Fingerprint drift:** anything new added to the data must go into exactly one of the two fingerprints. Add a
  comment at `data_fingerprint()`'s successor saying so, and keep one list of ids in one place.
