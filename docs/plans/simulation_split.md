# Plan: split `Simulation` into state and systems (Architecture 1)

Written 2026-11-16 from the roadmap's Architecture item 1 (outside review 2026-11-12). Steps 1-2 done (PR #41);
the order of the rest was revised after step 2 (see "Steps").

**This is a refactor: behaviour must not change.** Every commit must keep the test suites green, reproduce the saves
and the economy audit byte for byte, and keep `--profile` at least as fast as before (see "Checks").

## Where it stands (main at PR #39)

- `Simulation` (`src/simulation/Simulation.hpp`, 982 lines) is one class with all running state and every rule as a
  member. Sixteen files define its members: the fifteen topic files in `src/simulation/` (`modules/simulation.md`)
  and `src/persistence/SimulationPersistence.cpp`, which reads and writes nearly all state. `CrewEnvironment.cpp`,
  `MoneyJournal.cpp` and `FlowRecord.cpp` are already standalone.
- Two nested helpers reach into its private members: `Simulation::MissionPass` (`MissionPlanning.cpp:194`, about
  150 uses) and `Simulation::InvestmentReview` (`FleetInvestment.cpp:299`, about 120).
- `SimulationInternal.hpp` (namespace `detail`) holds constants and pure helpers only; it does not touch the class.

### Data members by group (`Simulation.hpp` ~867-979)

| Group | Members | Saved? |
|---|---|---|
| Static data and lookups | `universe_`, `station_defs_by_id_`, `ship_classes_by_id_`, `modules_by_key_`, `active_effects_`, `starting_inventories_`, `rotation_tours_` | no (from data) |
| Engines | `mechanics_`, `economy_`, `atlas_`, `atlas_loaded_`, `kepler_planner_`, `variable_isp_planner_`, `thread_pool_` | no |
| Clock | `game_time_s_`, `untimed_s_`, `timewarp_factor_` | yes |
| Entities | `stations_`, `ships_`, `sold_ships_`, `derelicts_`, `patrols_`, `emergencies_` | yes |
| Money | `outside_economy_credits_`, `background_credits_`, `background_population_`, `colony_state_`, `faction_treasuries_`, `faction_tax_per_day_`, `faction_interest_paid_`, `seeded_money_supply_`, `emergency_paid_`, `emergencies_opened_` | yes |
| Journal | `journal_`, `flows_`, account indices (`colony_accounts_`, `faction_accounts_`, `fleet_accounts_`, `first_station_account_`, `first_household_account_`) | journal and flows yes |
| Investment and salvage | `next_investment_review_s_`, `investment_purchases_left_`, `next_purchase_s_`, `review_probes_`, `investment_ledger_`, `replacement_claims_`, `replacement_tried_`, `next_salvage_review_s_`, `next_exposure_sample_s_` | yes |
| Randomness and events | `event_rng_`, `environment_rng_`, `events_enabled_`, `events_started_`, `recent_events_`, `recent_trades_` | yes |
| Caches and pass state | `designs_`, `leg_estimates_`, `deferred_plans_`, `deferred_keys_`, `deferred_prefix_`, `missing_plans_`, `mutable station_radius_cache_` (:959) | no (pure) |
| Audits | `failure_audit_`, `patrol_audit_`, `radiation_audit_`, `passenger_audit_`, `delivered_paid_cr_`, `delivered_reference_cr_`, `trajectory_audit_enabled_`, `trajectory_audit_records_` | no |

### Who calls whom (from a grep of member calls; rough but complete)

| Topic file | Changes | Calls into |
|---|---|---|
| Simulation.cpp | setup of everything; clock | all topics (it runs the tick) |
| Market | station orders and prices, emergencies | Contracts, MissionPlanning (`estimate_leg`), ShipOperations rates |
| ShipOperations | ships, stations, journal, radiation audit, `environment_rng_`, radius cache | Market (`trade_value`, `station_price`), Statistics (accounts), `register_design` |
| MissionPlanning | planning caches only (+ designs) | Market quotes, ShipOperations (~12), Passengers, Contracts, Snapshot (`mission_phase_name`) |
| ShipPhases | ships, stations, journal, flows, treasuries, emergencies, audits, events | nearly everything |
| Contracts | planning caches only | MissionPlanning, Market, ShipOperations |
| FleetInvestment | treasuries, ships, investment, journal | MissionPlanning, Passengers, Contracts, Market, ShipOperations, Households, Statistics |
| Failures | derelicts, ships, claims, audit, journal | Passengers (`land_passengers`), Contracts, Snapshot (positions), Market, ShipOperations, Statistics |
| Patrols | patrols, audit, treasuries, journal | Failures (`sun_mu`, `primary_body_id`, `local_leg`), Snapshot (positions), Market, ShipOperations, Statistics |
| Passengers | station queues, ships, audit, flows, journal | Households (11), ShipOperations, Market, Statistics |
| Households | stations, background economies, colonies, journal, flows | Passengers (`post_travellers`, `gateway_for`), Statistics, Market |
| Statistics | journal and flows (setup, monthly close) | FleetInvestment (`faction_credit_limit`), StationSystems, Households |
| StationSystems | nothing (const) | Households (`in_labour_market`, `station_workers`) |
| Snapshot | nothing (const; only the radius cache) | Passengers, Contracts, FleetInvestment, Patrols, Market, ShipOperations, StationSystems, Households |
| Persistence | everything saved | `register_design`, Statistics (`flow_stocks`), Failures (`init_failure_clocks`) |

Cycles: Passengers ↔ Households; Market ↔ Contracts ↔ MissionPlanning; Failures → Snapshot ← Patrols → Failures (only
through pure position and orbit helpers). Queries used everywhere: `get_station_state`, `get_station_definition`,
`ship_design`, `get_commodity`, `add_event`, `account_*`, `station_price`, `daily_capital_cost`.

### Tick order (`Simulation.cpp:504-608`), to be kept exactly

1. clock + `TICK_S`; copy each station's stock to `stocks_before`;
2. `update_market_responses` → `economy_.step` → decay of trade flows and order sales;
3. `settle_local_economy` → `step_households` → `step_surface_markets` → `step_labour` → `step_colony_labour` →
   `step_population`;
4. `step_passengers` (if on) → `step_events` → `step_particle_events` → `step_treasuries`;
5. `step_emergencies` → `update_market_prices` (first tick of a day) → `step_orders` → `step_fleet_investment` →
   `step_failures` → `step_patrols`;
6. each ship in `ships_` order: `accrue_operating_costs`, then its phase step;
7. optional `SPACETRAINS_TRACE_FLEET` line; `close_journal_period` on the close day.

Constructor set-up order: `setup_money_journal` → `compute_rotation_tours` → `seed_order_stats` →
`init_failure_clocks` → `init_patrols`.

### The move hazard

`EconomySystem`, `CelestialMechanics` and both planners keep `const&` references to `universe_`, `mechanics_` and
`atlas_` (e.g. `VariableIspTrajectoryPlanner.hpp:47`); the lookup maps hold pointers into `universe_`.
`from_data_root` builds a local `Simulation sim` and ends in `return sim;` (`Simulation.cpp:169-188`): it works
only because the compiler elides the move (NRVO, not guaranteed by the language). Any change that moves the
object breaks these references silently. The split fixes this for good (below).

### Threading today

`util::ThreadPool` (created only in `from_data_root`; a hand-built `Simulation` has none) runs deferred plans
(`MissionPlanning.cpp:148, :183`), `with_parallel_plans` (`Contracts.cpp:119`), reference prices
(`Market.cpp:187, :206`), reference fares (`Passengers.cpp:1127, :1134`) and `plan_sortie` (`Patrols.cpp:287`).
`DeferredPlan` (`Simulation.hpp:802`) is a `compute` lambda (workers) and a `commit` lambda (serial); today `compute`
captures `this`, so nothing stops it from touching state. `register_design` is serial-only. The planners guard their
own caches with mutexes.

## Target

```
src/simulation/
  World.hpp                 struct World: all state, grouped; not copyable, not movable
  Simulation.{hpp,cpp}      thin owner: std::unique_ptr<World>, tick(), unchanged public interface
  query/                    const World& helpers used by every system
    Lookups.{hpp,cpp}       get_station_state/definition, get_ship_class, get_body_definition, get_commodity,
                            find_module, radiator_bucket, ship_design (+ register_design, World&)
    Accounts.{hpp,cpp}      account_* (journal account indices)
    Positions.{hpp,cpp}     get_ship_render_position, sun_mu, primary_body_id, local_leg
    Events.{hpp,cpp}        add_event, record_trade (World&)
    Labour.{hpp,cpp}        households_on, labour_on, is_earth_gateway, in_labour_market, station_workers,
                            station_wage_cr_per_day
  systems/                  one per topic: step_* and helpers, taking World& (or const World&)
    Market, Contracts, MissionPlanning, ShipOperations, ShipPhases, FleetInvestment, Failures,
    Patrols, Passengers, Households, Statistics, StationSystems, Snapshot
src/persistence/SimulationPersistence.cpp  save(const World&) / load(World&)
```

`World` groups (sub-structs, so a reader sees what is saved):

```cpp
struct World {
    World(const World&) = delete; World& operator=(const World&) = delete;   // never moves:
    World(World&&) = delete;      World& operator=(World&&) = delete;        // engines hold references into it
    StaticData data;      // universe, lookups, effects, starting inventories, rotation tours
    Engines engines;      // mechanics, economy, atlas, planners, thread pool
    State state;          // everything save_state_json writes (clock, entities, money, journal, investment, rng, events)
    Caches caches;        // pure caches and per-pass planning state; empty after a load
    Audits audits;        // counters for reports; not saved
};
```

Rules for a system:
- It declares in its own header the functions it offers; it includes only `World.hpp`, `query/*` and the headers of
  systems it calls. No system includes another system's `.cpp` details.
- A function that only reads takes `const World&`. That is the list of what may run in parallel.
- The tick order and every iteration order stay as today. Nothing is reordered "while we are here".
- `MissionPass` and `InvestmentReview` become the systems' own classes (`systems::MissionPass(World&)`), with no
  friend access.
- `DeferredPlan::compute` becomes `std::function<domain::TrajectoryPlan(const World&)>`: the compiler then refuses a
  worker that changes state. `station_radius_cache_` stays the one cache that a const path may fill, with its
  existing bypass on worker threads (`ShipOperations.cpp:113`).

## Steps

Each step is one commit (or a few), each builds and passes every check below. While a topic is half moved, the old
`Simulation` member stays as a one-line forwarder to the new function; forwarders are deleted in step 15.

The order below is option (a), chosen 2026-11-16 after step 2: the Market cycle moves right after the small steps,
because almost every other topic prices goods through it. The dependencies were measured on `main` at PR #41 with a
call-graph script over the `Simulation::` definitions (members already forwarding to `query::` count as moved).

1. **`World` by inheritance** (done 2026-11-16, PR #41). Create `World.hpp` with the data members moved verbatim
   (same names, flat for now) and make `class Simulation : private World`. No function body changes. The move hazard
   is unchanged in this step (`from_data_root` still ends in `return sim;`, `Simulation.cpp:188`, and relies on
   NRVO); it goes away in step 15.
2. **Query layer** (done 2026-11-16, PR #41). The true leaves of the call graph became free functions in `query/`:
   lookups and designs, account indices, positions, the event and trade logs, and the labour leaves
   (`in_labour_market`, `station_workers`, `station_wage_cr_per_day`, `is_earth_gateway`). The `Simulation` members
   of the same names forward to them. Prices (`station_price`, `trade_value`) are not leaves: they call the order
   machinery (`priced_order`, `order_bid`, `order_levels`, `order_ceiling`), so they move with Market (step 5).
3. **StationSystems** (done 2026-11-16; 264 lines, one member: `station_systems_json`, const, calls nothing unmoved) into
   `systems/StationSystems.{hpp,cpp}`. First real system; sets the header layout for the rest. Callers: Statistics
   and Snapshot, through the forwarder. The same PR tidies what the review of PR #41 found: the `account_earth()`
   member forwards to `query::account_earth()` instead of repeating the constant, and `Simulation.cpp` loses a
   double blank line and the includes it no longer needs.
4. **More query leaves** (done 2026-11-16), the read-only functions the Market cycle and the later systems need. Each was checked to
   call nothing unmoved:
   - `query/ShipRates.{hpp,cpp}` (from `ShipOperations.cpp`): `daily_capital_cost`, `hazard_cr_per_day`,
     `wear_cr_per_flight_day`, `wear_cr_per_propellant_kg`, `wear_cr_per_drive_hour`, `overhaul_bill`,
     `drive_life_h`, `drive_hours`, `leg_boiloff_kg_per_day` (both overloads), `leg_dose_allowed`,
     `leg_dose_msv_per_day`, `docked_dose_msv_per_day`, `cabin_shield_g_cm2`, `cargo_decay_per_day`,
     `purchasable_propellant_kg`, and `station_r_au` with its cache. `station_r_au` keeps its rule exactly: inside
     `ThreadPool::in_parallel_for()` it computes directly, otherwise it uses `station_radius_cache_` (the only
     `mutable` member; it stays in `World`). Also `boiloff_kg_per_day` and `ship_r_au` (leaves too). The nested
     `OverhaulBill` moves with them (`Simulation::OverhaulBill` stays as an alias).
   - `query/Finance.{hpp,cpp}`: `economy_balance`, `economy_credit_line`, `station_lifetime_profit`,
     `colony_wage_median` (from `Households.cpp`), `station_profit_last_year` (from `Statistics.cpp`) and
     `faction_credit_limit` (from `FleetInvestment.cpp`). This also breaks the Statistics ↔ Households loop.
     `colony_state` (inline in `Simulation.hpp`) moves with them, and `EARTH_ECONOMY` from `Households.cpp` to
     `SimulationInternal.hpp`.
   - Found while preparing step 5: `MissionPass` calls more than the member definitions show (125 calls through
     `sim_.`). Its read-only leaves move here too: `drive_life_allows`, `provisions_mass_kg`, `provisionable_days`,
     `persons_aboard`, `credit_line` (`ShipRates`); `resident_wages_per_day`, `household_travel_budget`
     (`Finance`); a new `query/Passengers` with `passengers_on` and the other travel switches, `passenger_kg`,
     `reference_fare`, `passenger_leg_waiting`, `value_of_time_cr_per_day`, `passenger_nonfare_cr`,
     `employer_pays`; `mission_phase_name` (`Lookups`, no `World` needed); and a new `query/Clock` with
     `first_tick_of_day` (`TICK_S` moves to `World.hpp`, `Simulation::TICK_S` stays as an alias).
   After this step the Market cycle depends only on moved code. Steps 3 and 4 are one PR.
5. **Market + Contracts + MissionPlanning** (the cycle; 1,085 + 382 + 1,316 lines, 44 members) into
   `systems/Market`, `systems/Contracts` and `systems/MissionPlanning`. Its own PR.
   - **One commit**, because the three files call each other and a free function cannot call a `Simulation`
     forwarder. The commit is mechanical only: member access becomes `w.`, member calls become calls with `w`
     first, nothing else. Review with `git diff --color-moved=zebra`; the PR lists every non-mechanical line (if
     any) separately.
   - `Simulation::MissionPass` (`MissionPlanning.cpp:194`) becomes `systems::MissionPass`, holding a `World&`
     instead of reaching into `Simulation`'s privates.
   - `with_parallel_plans` and the deferred-plan machinery move with Contracts/MissionPlanning; `deferred_plans_`,
     `deferred_keys_`, `deferred_prefix_` and `missing_plans_` stay in `World`. `DeferredPlan::compute` lambdas
     capture `const World&` from here on (the parallel part must not write; the compiler now checks it).
   - Four pricing functions outside the three files call `station_price` and are called by `MissionPass`, so they
     join the cycle: `daily_crew_cost` (into `systems/ShipOperations`) and `passenger_offer`, `live_fare_cr`,
     `port_fuel_price` (into `systems/Passengers`). The rest of those two topics follows in steps 6 and 9.
   - Forwarders stay for every member called from outside the cycle (27, measured): `station_price` and
     `trade_value` (8 topics), `under_contract`, `liner_stops`, `scheduled_stops`, `contract_dwell_days`,
     `find_contract`, `fleet_hold_units`, `fleet_ship_count`, `faction_name`, `sale_split_on_arrival`,
     `observe_contract`, `observe_delivery`, `priced_order`, `order_levels`, `quote_split`, `order_bid`,
     `consumer_bid`, `choose_mission`, `choose_missions`, the five called from `tick()` and the constructor
     (`starting_inventory`, `seed_order_stats`, `update_market_prices`, `step_orders`, `step_emergencies`), and the
     public `order_view` and `set_station_stock` (used by the tests).
   - Risk specific to this step: the parallel planning (`parallel_for` in `compute_reference_prices`, the
     deferred plans in `choose_missions` and the contract passes). The default-thread and one-thread saves must both
     match; run the one-thread check twice.
6. **ShipOperations**, the parts that change state and price goods (`daily_crew_cost`, `buy_provisions`,
   `buy_propellant`, `consider_repair`, crew pay, wear and repair, boil-off, particle events). Needs only Market and
   the rates from step 4.
7. **Statistics** (journal set-up and the monthly close, `build_stats_json`, `flow_stocks`). Needs StationSystems
   and `query/Finance`.
8. **Patrols** (453 lines; needs `station_price` and `daily_crew_cost`).
9. **Households + Passengers** together (their cycle). Two commits if the diff is too big: Households first with
   forwarders for the four Passengers functions it calls, then Passengers.
10. **Failures** (needs `land_passengers` from step 9, Contracts and ShipOperations).
11. **FleetInvestment** (`InvestmentReview` becomes `systems::InvestmentReview`). Check first why the call graph
    shows `step_fleet_investment` calling `tick` (likely a name in a comment or a nested trial run).
12. **ShipPhases** (it calls nearly everything, so it goes last among the rules).
13. **Snapshot** (`const World&`; it reads every topic, so it goes after them).
14. **Persistence:** `save_state_json(const World&)` and `load_state_json(World&)`, still called through
    `Simulation`'s public members.
15. **Thin `Simulation`:** drop the inheritance, hold `std::unique_ptr<World>` (this removes the move hazard: the
    engines' references now point into a heap object that never moves), make `World` non-copyable and non-movable,
    group the members into the `World` sub-structs, delete the forwarders. `tick()` reads as the list in "Tick
    order" above.
16. **Optional:** drop the trailing underscores in `World` (mechanical rename, its own commit).

Suggested PRs: 1-2 (done), 3-4, 5, 6-10, 11-16. Each PR states the before/after numbers of the checks.

## Tests to add

- `tests/test_world.cpp` with a helper `make_world(data_root, adjust)` that builds a `World` the way
  `from_data_root` does (shared code, not a copy).
- After step 7: a Statistics test (a hand-posted transfer appears in the right account and the monthly close
  samples it).
- After step 8: a Patrols test (one cutter, one ship to inspect; the sortie is planned and paid).
- These show the gain the change is for; more such tests come with later work, not in this refactor.

## Checks after every commit

Take a baseline on `main` before step 1 and keep it in `tmp/split_baseline/` (not committed):

```
for s in 0 90 180 270; do
  build-release/bin/spacetrains_headless . --start-day $s --days 365 --save-at 365 base_$s.json > /dev/null
done
build-release/bin/spacetrains_headless . --days 365 --econ-audit > econ_audit.txt
build-release/bin/spacetrains_headless . --days 365 --profile > profile.txt
```

Then for each commit:

- `cmake --build` with no new warnings (`-Wall -Wextra -Wpedantic`), `ctest`;
- the four saves and the audit identical to the baseline (`cmp`), once with `SPACETRAINS_THREADS=1` and once with
  the default thread count;
- `--profile`: the slowest 20-day stretch stays above 5 days/s and the total is not slower than the baseline beyond
  run-to-run noise (run twice);
- a sanitizer build (`-fsanitize=address,undefined`) once per PR, as for earlier refactors.

At the end: rewrite the file table in `docs/modules/simulation.md` (systems and queries instead of "one class, its
members spread over files") and the matching lines in `docs/architecture_overview.md`.

## Risks

- **Silent reordering.** Moving code can change the order of iteration or of random draws. The byte-identical saves
  catch it; when one differs, bisect the commit rather than patch forward.
- **Big cycles.** Step 5 moves about 2,800 lines in one commit. Keep it purely mechanical (no clean-up in
  the same commit) and keep the outside forwarders until their callers have moved.
- **Rebuild time** gets better only at the end; during the move every file still includes `Simulation.hpp`.
- **Work in parallel with feature branches.** The roadmap's to-do items (engine-failure rescue, gravity wells) touch
  the same files. Do this refactor between features, or rebase features onto finished PRs, never the other way.
