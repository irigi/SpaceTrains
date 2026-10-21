# Passengers

## Context
Tier 2 (PR #17) left passengers to their own plan. Today the economy moves only goods. People appear by formula:
- `step_population` grows supplied stations by 3%/yr;
- `migration` events add +5% out of nowhere.

The user decided (2026-10-21) that the game models:
- **Demand:** work rotation, migration from Earth, and discretionary trips.
- **Growth:** station growth happens only when migrants arrive by ship; decline means emigrants leave by ship.
- **Fares:**
  - the employer (the work station) pays for rotation;
  - migrants and visitors pay from the outside account.
- **Carriers:** a cabin module any ship can refit, plus dedicated passenger liner classes from the user's linopt fare study.

Goals:
- Realistic and sourced, and decided by costs, not by rules.
- Everything behind switches.
- Money drift 0, exact save/load, TSan clean.
- Judged with `tools/benchmark.py` (4 starts × 1460 days; now unmet 0.5 ± 0.2%, last fifth 1.0 ± 0.3%, price 1.5×, 101/138 profitable, money −20 ± 10%).

Work on a new branch `feature/passengers` from `main` at `5442d96`. Commit per step; push only when asked.
Decisions and results go to `docs/plans/passengers_log.md`; design to `docs/plans/passengers.md`.

## Magnitudes (from the repo's numbers; checked again in step 0)
- **Berth:** 2.5 t (linopt; includes the berth's share of common space) × 1000 $/kg × 0.00078 cr/$ ≈ 1,950 cr.
  That is ≈ 0.45 cr/berth-day of capital (`daily_capital_cost`: 5% + 1/30 per year).
- **Provisions:** crew rate ≈ 0.35 cr/person-day. Body + luggage 100 kg (EASA 2009 standard masses).
  Provisions bought at the crew rate; linopt's 2 kg/person-day is carried as mass.
- **Fares that follow from cost:**

  | Destination | Fare |
  |---|---|
  | Luna | ~20 cr |
  | Mars (≈200 d) | ~350 cr |
  | Ceres and beyond | 600+ cr |

  A resident spends ~0.2 cr/day on goods.
- **Dose on a Mars leg:** ~330 mSv ≈ 140 cr at the existing 0.43 cr/mSv. The 1 Sv career limit allows ~1.5 Mars tours
  per worker, so rotation groups carry a cumulative dose.
- **Ship size:** a 600-berth liner is worth ~1.5 M cr, about the whole starting fleet. Liners must be sized from demand
  (60–120 berths); rerun `passenger_ticket_optimizer.py fare --passengers 60/120` for masses and fares.
- **Solvency risk:** 3% of the 64k people off Earth on rotation costs ~2.8k cr/day. That is 20–27% of an outer
  station's household income; a station at 100k reaches the 25k floor in ~4 months. The rotation share is set
  by a measured sweep, not assumed.

## Model

### Who travels
1. **Migration** (step 3):
   - A station whose supply index ≥ `grow_above` and whose credits ≥ the floor posts settler demand at
     `growth_per_year` × population. The settlers wait at the Earth gateways (`earth_orbit`, `earth_l1`; Earth's
     population is unlimited).
   - `migration` events become a group of settlers waiting at Earth.
   - Decline posts emigrants to Earth.
   - Earth gateways themselves keep today's formula growth (surface launch is outside the simulation).
   - Population changes only on arrival.
2. **Rotation** (step 4):
   - A share of each off-Earth station's population is rotating staff, at the engineer grade; their home is the
     cheapest Earth gateway.
   - **Tour length minimises cost per day on site:** T* = √(C·365/(η·w)), where
     - C = 2 fares + 2 · w · flight days + recovery;
     - w = wage;
     - η = hardship premium per year of tour (estimate, ~0.2).
   - Checks, not inputs: Luna should come out near the ISS's 6 months, and the Antarctic winter-over is 12–14 months.
   - **Relief:** a relief is posted only while the station's credits ≥ floor + fare. Otherwise the tour runs long,
     and the overdue cost on the same η curve shows in the audit.
   - **Starting fleet:** tours are staggered at the start.
   - **Solvency:** the station's rotation cost per day joins `consumption_value_per_day(…, upkeep_only=true)`,
     so the faction's floor top-up covers it like core crew (`step_treasuries`).
3. **Discretionary trips** (step 5):
   - Gravity model A·Pᵢ·Pⱼ, with Earth's weight as its own estimated parameter.
   - Generated deterministically, as fractional counts, in bands of maximum generalized cost:
     count per band ∝ (rₖ^ε − rₖ₊₁^ε).
   - Elasticity ε ≈ −1: IATA/InterVISTAS 2007 national-level long-haul −0.8 to −0.9, route-level −1.4.
   - Visit length is an estimate; visitors return.

### Generalized cost
Generalized cost is what a passenger weighs when choosing a ship:
- fare;
- value of time × (wait + flight days), at the person's wage from `labour.csv`;
- dose cost: 0.43 cr/mSv × leg dose, with the same per-leg 1 Sv limit as crews;
- microgravity recovery: 0.25 × zero-g flight days × value of time. Derived from NASA's 45-day post-flight
  reconditioning after ~6-month ISS flights (NTRS 20110020318).

Fast ships therefore win passengers through cost.

### Fares (hedonic, contestable)
1. **Reference fare per station pair:** computed at startup next to `compute_reference_prices`. It is the cheapest
   cabin carrier's cost per berth plus propellant, with `fill_ref`.
2. **A ship's fare:** reference fare + value of time × (reference days − its days) + its recovery and dose difference.
3. **Bounds:** floored at the marginal cost (provisions and propellant for ~150 kg), capped by the group's maximum
   cost minus its non-fare costs.
4. **Board or wait:** a group boards if this ship's cost ≤ min(its maximum, value of time × expected wait for the
   next departure + that departure's cost). The expected wait comes from past departures per pair, like `order_stats`.

### Ships
- **Cabin modules:**
  - `cabin` slot rows in `hull_slots.csv`, plus cabin modules in `modules.csv` with effect `berths=N`, sized from
    linopt.
  - Generated by `data/ship_classes/build_ship_classes.py` and checked by `tests/test_build_ship_classes.py`.
  - `register_design` applies `berths`. Active only when `passengers` is on (via `active_effects_`), so
    `neighbouring_fits`, `consider_refit` and the variant pick handle cabins with no new code.
- **Liner classes:** 1–2 dedicated liners (plasma; 60–120 berths, small hold or none), purchasable by fleet investment.
- **Persons aboard:**
  - A `persons_aboard(ship)` helper replaces crew_size only in the provision formulas (~1701, 1716, 2169, 2180,
    `provisionable_days`).
  - Wages and hazard pay stay crew-only.
  - Passengers' provisions are bought at boarding, for the leg.
  - Passenger mass enters `payload_kg` (rocket equation).
- **Storm shelter:** sized for the crew. Passengers take the hull's storm dose unless the cabin has its own share.
- **Spin gravity:** assessed, not built, unless a sourced mass premium for a rotating habitat is found. The recovery
  cost above gives its value; the decision is logged like hydroponics.

### Integration points (`src/simulation/Simulation.cpp`)
- **Manifest builder** (`choose_mission_pass`, ~2815–2985):
  - Per destination, fill berths by fare − marginal cost; add passengers' mass before `plan_to`; add fare revenue to
    the leg profit.
  - Passenger-only legs must survive `if (offers.empty()) break;`, `if (goods.empty()) continue;`, the empty-leg loop,
    `follow_ups_from` and the liner's "fly the next stop empty" fallback.
- **Determinism:**
  - The search is serial; only `DeferredPlan::compute` runs in parallel, so queues and passengers aboard are
    read-only there.
  - Boarding (queue → `ShipState.passengers`, fares paid) happens only at the mission commit, after the cargo
    purchase loop (~3490).
  - Arrival (~3780) unloads, fills rotation positions, queues relieved workers home and calls `set_population`.
- **Queues:** sorted `std::vector`s with whole-person counts and carried fractions; no new random draws when the
  switch is off.
- **Liners** (`liners.csv`): passengers stay aboard through scheduled stops to their destination. Fares reduce
  liner subsidies.
- **Contracts:** contracted ships take spot passengers on their loop. Pure liners (no hold) do not sign cargo contracts.
- **Investment** (`commission_step`, `value_candidate`, variant pick ~5023):
  - `MissionChoice::PassengerOption` (destination, open flow per day, fare, days, two-way flag).
  - A passenger term in `margin_bound`.
  - Sustained passenger profit per day over the route commitment, using forecast flows (rotation schedule, settler
    and visitor rates, minus passengers other ships already carry), not today's queue. This avoids refit flapping.
- **Value of time in the planner** (step 7):
  - Legs with passengers aboard get `time_cr_per_day` += Σ value of time.
  - The plan cache key gains a value-of-time bucket.
  - Measure plan count and run time.

### Money
Fares are paid at boarding:
- the employer station's credits → the ship (rotation);
- `outside_economy_credits_` → the ship (migrants, visitors), as `settle_local_economy` does.

New ledger fields: ship `fare_revenue`, station rotation spend. The money identity stays exact. No refund if a ship
is stranded.

### Data and persistence
- **`data/economy/passengers.csv`:**
  - `passengers` master switch (0 ⇒ byte-identical to today) and sub-switches `rotation`, `migration`,
    `discretionary`;
  - rotation share, η, body + luggage kg, recovery ratio, gravity A and Earth weight, ε and bands, visit days;
  - each with its status (sourced / derived / estimate) in the docs.
- **Types:** a `PassengerDefinition` in `Types.hpp`, loaded in `DataLoader.cpp` with the `market.csv` pattern.
- **Save:** version 14 → 15 (passengers aboard, station queues, rotation pools, wait statistics). The fingerprint mixes
  `passengers.csv`, cabin modules/berths and `crew_size`.

### Audit and UI
- **`--econ-audit` line `Passengers:`**
  - trips by kind, passenger-days, fares by payer, load factor;
  - unmet and overdue person-days, mean wait;
  - rotation spend ÷ household sales per station;
  - settlers delivered vs posted;
  - profitability of cabin ships and liners.
- **Godot:**
  - berths gauge and passengers in `InspectorPanel.gd` `_build_ship`;
  - waiting passengers in `_build_station`;
  - cabins in `ShipSchematic.gd`;
  - fields in `build_bridge_snapshot_json` / `build_bridge_designs_json`.

## Build order (benchmark after each step from 3 on)
0. **Baseline:** 365-day `--econ-audit` and benchmark on `main`. Rerun linopt for 60/120 berths.
1. **Data only:** `passengers.csv`, cabin slot/modules, liner classes, fingerprint, save v15 with empty fields. The
   switch-off audit must stay byte-identical; liner classes stay out of the fleet when the switch is off.
2. **`persons_aboard` refactor:** byte-identical.
3. **Migration by ship:** cabins, fares, boarding/arrival, settlers replace formula growth and Migration events,
   emigrants. Outside money only.
4. **Rotation:**
   - positions, T*, cumulative dose, solvency rules;
   - share sweep 0 / 1 / 3 / 5%: keep the largest that holds money in its band and unmet demand unchanged.
5. **Discretionary travel:** demand bands, the board-or-wait rule.
6. **Investment:** passenger options and bounds; liners purchasable.
7. **Value of time in the planner;** through-passengers on liners.
8. **UI, docs** (`docs/modules/ships_and_fleet.md`, `docs/ship_technology.md` sources, roadmap), memory.

## Verification
- **Unit tests** (`tests/test_main.cpp`):
  - persons-aboard provisions;
  - fare = reference ± the hedonic terms, with floor and cap;
  - T* against the closed form;
  - demand bands sum to the gravity total;
  - fares move money exactly (station or outside → ship);
  - migration arrival raises population by the number landed;
  - switch off ⇒ no queues, no draws.
- **Builder:** `--check`; `tests/test_build_ship_classes.py` (cabin rows add up; liner classes match linopt's mass terms).
- **Switches off:** 365-day `--econ-audit` byte-identical to the baseline from step 0.
- **Every step:**
  - ctest suite;
  - `--trajectory-audit` 0 flags;
  - drift 0;
  - save at day N / load / run to M identical;
  - TSan 800 days (scratchpad tsan build, `setarch -R`) at the end.
- **Benchmark** after steps 3–7, results and every decision in `docs/plans/passengers_log.md`.
- **Godot:** run once to see the berths gauge and station queues; kill `spacetrains_bridge` by PID afterwards.
