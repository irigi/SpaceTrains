# Passengers: build log and decisions

Plan: `docs/plans/passengers.md`. User's choices (2026-10-21):
- **Demand:** rotation, migration and discretionary trips.
- **Growth:** only by arriving migrants.
- **Fares:** the employer pays rotation; migrants and visitors pay from the outside account.
- **Carriers:** cabin modules plus liner classes.

## Step 0: baseline

- **Code:** `main` at `5442d96` (tier 2 merged).
- **365-day `--econ-audit`:** kept in the scratchpad as the byte-identical reference for switch-off runs.
- **Benchmark (4 starts × 1460 days):** the tier 2 follow-up run of the same code: unmet 0.5 ± 0.2%, last fifth
  1.0 ± 0.3%, price 1.5×, 101/138 profitable, money −20 ± 10%.

linopt `passenger_ticket_optimizer.py fare` at 1.5 AU (its defaults):

| Passengers | Fare | Payload | Transfer | Jet power | Initial mass |
|---|---|---|---|---|---|
| 600 (README baseline) | $104k | 1,945 t | 140 d | | |
| 120 | $189k | | | | |
| 60 | $295k | 478 t | 128 d | 464 MW | 710 t |

Small ships pay dearly in the study because its 300 t constant payload (shared structure, controls,
common life support) is fixed.

## Step 1: data

Decisions:
1. **A berth is linopt's per-passenger term plus a per-berth share of its constant term:**
   - 2,500 kg at 1,000 $/kg plus 0.5 t (300 t / 600) at 1,500 $/kg: 3.0 t and 3.25 M$ = 2,535 cr per berth;
   - module cabins add 10% truss;
   - scaling the shared part per berth (instead of a fixed 300 t) is an estimate, so that small cabins and
     liners are not charged a 600-berth ship's core;
   - volume per berth from the Deep Space Habitat's pressurized volume per kg (~18 m³), heat as a crew member's
     3.2 kW (estimates).
2. **One cabin module per hull** in a new `cabin` slot (game sizes): couriers 12 berths, freighters 24, bulk
   hulls 60. One size per hull keeps the refit search as small as today's.
3. **Two liner classes, 40 berths each, small hold (20 u), crew of 3-4:**

   | Class | Drive | Δv / α | Price |
   |---|---|---|---|
   | Plasma Liner | 100 MW jet | α ≈ 500 W/kg (a courier's) | ≈ 268k cr |
   | Passenger Shuttle | liquid-core NTR, the NTR Freighter's tanks | Δv 10.3 km/s | ≈ 269k cr |

   Both cost more than any ship today (the dearest is 170k cr); fleet investment decides whether they pay.
4. **Gating:** with `passengers,0` the loader drops the liner classes and their hulls' modules and slots. The
   cabin modules stay in the data but are never offered: their `berths` effect is active only with
   passengers on. The 365-day audit is byte-identical to the baseline.
5. **Save and data:**
   - save version 15;
   - the data fingerprint now mixes crew, berths, module effects and `passengers.csv`.

## Step 2: persons aboard

Passengers eat from the ship's provisions while aboard (they board at departure and land on arrival); the
docked top-up and the crew's return reserve stay crew-only. No change without passengers (byte-identical).

## Step 3: migration by ship

Model (`src/simulation/Passengers.cpp`):

**Demand bands.** People wait at stations in groups (kind, destination, employer, band).
- Demand is posted as the number who would travel at the reference cost (fare plus time and dose).
- The fare elasticity −1 spreads those people over 4 bands of highest acceptable cost: 0.5, 1, 2 and 4× the
  reference cost.
  - The cheap band only travels on trips cheaper than the reference.
  - Fixed head counts (a relief, someone going home, an event's convoy) are spread over the bands in the same
    proportions.

**Waiting.** A person gives up when the wait would use up their budget: a group thins at
value of time ÷ highest cost per day.

**Fares.**
- **Hedonic.** Each group pays the reference fare adjusted for how much its time and dose on this trip differ
  from the reference trip.
- **Clearing.** When more wait than fit, everyone pays the fare that clears the berths: what the first person
  left behind would pay (a uniform-price auction).
- **Bounds.** Never more than a group's budget, never less than the marginal cost: provisions plus the
  propellant its mass burns.

**Who pays.**
- Settlers, emigrants and visitors pay from the outside account.
- Rotation is paid by the work station.

**Population.** Settlers join the destination's population on landing; emigrants leave it at boarding. Earth
gateways keep formula growth (surface launches are outside the simulation).

**Reference fares** (`data/economy/reference_fares.csv`, `spacetrains_headless --write-reference-fares`). For
each station pair, the cheapest cabin carrier (each hull with its cabin module, and the liners):
- **Cost per berth:** carrier_margin (1.2) × (its days out + return_share (0.5) × the way back, at its daily
  cost, plus propellant) ÷ (berths × fill_ref (0.65)), plus the passenger's provisions.
- **Choice:** the trips are planned with the passengers' time value, and the carrier is chosen by the cost to
  the passenger (fare plus time).

| Trip | Fare | Days | Carrier |
|---|---|---|---|
| Earth orbit → Luna | 115 cr | 11 | Plasma Liner |
| Earth orbit → Mars | 1,325 cr | 204 | Plasma Liner |
| Earth orbit → Ceres | 1,634 cr | 268 | Plasma Liner |
| Earth orbit → Ganymede | 2,500 cr | 446 | Plasma Liner |

Fares are about 7 years of a resident's wage for Mars (linopt's 600-berth ship: $104k, about 2 years of US
median income). The game's carriers are small (12-60 berths), pay interest and fly back half empty.

**No reference fare for 28 of 72 pairs:**
- **Titan:** every leg exceeds the 1 Sv per-leg dose limit for an unshielded cabin, so Titan gets no passengers.
- **Mercury:** most outbound departures are infeasible for every carrier, likely the known low-κ atlas gap.

**Fleet investment** (moved ahead of rotation: without carriers nobody travels).
- **Candidates:**
  - every hull with its cabin module and the liners;
  - the profit bound adds full berths at the top band's fare against the passenger flow.
- **Probes:** a candidate with berths gets one passenger option per destination it can carry full berths to.
- **Valuation:**
  - the sustained flow (settlers posted, emigrants, rotation, visitors, and the queue spread over the route
    commitment), less the flow of ships committed to the pair;
  - at the fare that clears its berths. With elasticity −1 that is the reference cost × flow per trip ÷
    berths, between 1 and 4×.
- **Commitment:** a ship bought for passengers is committed to its route like a cargo ship (commodity
  `passengers`). A hull bought for its cabin keeps it.

**First result** (migration only, 4 × 1460 days): unmet 0.9 ± 0.4% (baseline 0.5 ± 0.2), last fifth
2.2 ± 2.0%, price 1.6×, 111/144 profitable, money −7 ± 13%, 218 s per run (164).

Start 0 at day 1460:
- 2,276 settlers boarded (mean wait 326 days, mean fare 3,497 cr); 3,907 gave up; 5,769 still waiting.
- 9.0M cr in fares; plasma ships' profit 30.4M cr; 33 cabins fitted.
- **Passengers become the fleet's main income**, and the queues clear only as far as the fleet cap (130) allows.

## Step 4: rotation

- **Who rotates:** rotation_share (3%, estimate) of each off-Earth station's people.
- **Tour length** minimises the cost per day on site: T* = √(C · 365 / (η · w)), where
  - C = the round trip's fares plus time and dose at the engineer wage w = 1.3 cr/day;
  - η = 0.2 per year of tour (estimate).
- **Rhythm:** each position is relieved once a tour. A relief lands (population +1) and sends one worker home
  (population −1 at boarding). The station pays both fares.
- **Running short:** a station below its floor + the fare sends no relief, and tours run long.
- **Solvency:** the faction's floor top-up cap counts the station's rotation fares per day like upkeep.

Tours that result (checks: ISS 6 months, Antarctic winter-over 12-14 months):

| Station | Tour | Fares per position-day |
|---|---|---|
| Luna | 603 d | 0.35 cr |
| Venus | 1,916 d | 0.94 cr |
| Mars | 2,213 d | 1.08 cr |
| Ceres | 2,535 d | 1.20 cr |
| Ganymede | 3,149 d | 1.56 cr |

Luna's tour is 1.5-3× the checks because its fare (115 cr) is 88 days of the wage.

**Simplified:** each tour hires a worker under the per-leg dose limit, as crews are; career doses across tours are
not tracked.

## Step 5: discretionary trips

- **Gravity model:** trips per day i → j = visit_trips_per_person_year (0.05, estimate) / 365 × P_i × P_j ÷ ΣP
  (the other stations) × (reference cost ÷ a year's income)^ε.
- **Earth** weighs in as earth_population_weight (100,000, estimate), shared by its gateways.
- **Stay:** visitors stay visit_days (90, estimate), then go home (fixed count).
- **Demand:** posted in the same bands, so the elasticity acts within a pair and the deterrence across pairs.

## Step 7: passengers' time in the planner

- A manifest with passengers plans its trajectory with their time and dose per day added to the ship's cost of
  time, so plasma ships fly faster when people are aboard.
- The plan cache key carries the riders' value in 10 cr/day buckets.

**Not built:** through-passengers on the scheduled freight loops (`liners.csv`). Those ships never refit, so they
never have cabins.

## Results with everything on (4 starts × 1460 days, data with `passengers,1`)

| Run | Unmet % | Last fifth % | Price ÷ ref | Profitable | Ships | Money % | Run s |
|---|---|---|---|---|---|---|---|
| baseline (main) | 0.5 ± 0.2 | 1.0 ± 0.3 | 1.5 ± 0.0 | 101 / 138 | 138 | −20 ± 10 | 164 |
| migration only | 0.9 ± 0.4 | 2.2 ± 2.0 | 1.6 ± 0.2 | 111 / 144 | 144 | −7 ± 13 | 218 |
| all passengers | 1.1 ± 0.1 | 2.4 ± 0.8 | 1.7 ± 0.2 | 112.5 / 140.5 | 140.5 | −6 ± 7 | 199 |

Start 0 at day 1460, all passengers:

| Kind | Boarded | Mean fare | Mean wait | Gave up | Still waiting |
|---|---|---|---|---|---|
| settlers | 2,058 | 3,862 cr | 326 d | 3,673 | 5,201 |
| emigrants | 489 | 3,267 cr | 149 d | 388 | 1,495 |
| rotation out | 195 | 3,295 cr | 228 d | 627 | 327 |
| visitors | 4,144 | 678 cr | 179 d | 4,885 | 2,677 |

- **Money:** 13.2M cr in fares (cargo margin 31.1M, as before passengers: 32.6M); 36 cabins fitted; 39% berth
  load factor.
- **Unmet demand doubles:**
  - cabins and passenger runs take fleet slots under the same cap (130) and the hold capacity falls
    (165k u against 178k);
  - growth now depends on ships, and settlers queue for a year.
- Money drift 0, trajectory flags 0, tests (save/load with passengers) pass.

**Open for the user:**
1. **Fares and incomes:** fares cost 7 years of a resident's wage for Mars, and settlers pay up to 4× that
   when berths are scarce.
   - Options: a budget cap tied to income, sponsors paying settlers' passage, bigger liners (linopt's 600
     berths) to cut fares.
2. **The fleet cap:** passenger carriers compete with freighters for the cap's 130 slots.
   - Options: exclude carriers as the scheduled freight liners are, or raise the cap.
3. **Titan and Mercury:** no passengers.
   - Titan legs are over the per-leg dose limit; Mercury has an atlas gap.
4. **Estimates:** rotation share 3%, hardship η 0.2, visit rate 0.05 per person-year, Earth weight 100,000.
5. **Not built yet:** the UI (berths gauge, station queues, cabins in the schematic); TSan run.

## Follow-up (2026-10-21): the user's decisions on the open points

The user's decisions:
- long rotations are fine;
- Titan needs more shielding; Mercury waits for the atlas update;
- no objection to the estimates;
- the fleet cap stays unless the game reliably holds 5 days/s;
- big liners and heavy shielded cabins;
- stations pay for settlers and recover it through their prices;
- settler demand falls with the ticket's payback time.

### Built
- **Large Plasma Liner** (`large_plasma_liner`): 200 berths, crew 8, 300 t water, 500 MW jet, ≈1.22M cr (α ≈ 565 W/kg).
  - linopt `fare --passengers 200`: $147k a seat, 746 MW, 1,258 t, 135 days.
  - Its defaults are idealised (25-year life, no interest, full both ways, no mark-up, a 20 kW/kg engine), which
    explains the ≈13× gap to the game's fare.
  - The large liner is only about 9% cheaper per berth than the 40-berth liner: our cost model scales the shared core
    per berth, so there is no economy of scale.
- **Shielded cabins.** Cabin shield (g/cm²) = cabin_shield_g_cm2 + propellant_kg ÷ tank_wrap_m2 ÷ 10.
  - Cosmic rays are multiplied by exp(−shield ÷ 46 g/cm²) (water's attenuation length, the same as the storm
    shelter).
  - Particle events see the shelter plus the cabin shield.
  - The crew lives behind the same shield.
  - The modules:
    - `shielded_cabin`, one per hull with a cabin slot: its berths plus a 20 g/cm² water wall;
    - `cabin_water_wall` on liners: a 20 g/cm² wall over the cabin area (a cylinder whose height equals its
      diameter);
    - `tank_wrap` on plasma liners: the cabin sits inside the propellant tank, so the water it carries is the shield.
      Planning assumes the tank is half full (`tank_wrap_mean_fill` 0.5); in flight the water actually aboard counts.
      This is the user's question "do cabins surrounded by fuel tanks make sense?": yes, water is the best shield per
      kg, and the shield shrinks as the tank empties.
  - Liners have 3 habitat slots, so both shields can be fitted together.
- **Reference fares.** The carriers are now each hull with each cabin module, and every liner tank variant bare, with
  each shield and with both. 70 of 72 pairs have a fare:

  | Trip | Before | Now |
  |---|---|---|
  | Earth orbit → Mars | 1,325 cr | 1,096 cr |
  | Earth orbit → Ganymede | 2,500 cr | 1,985 cr |
  | Earth → Titan | none | 3,307 cr, 658 days (large liner, 200% tanks, tank jacket) |

  Still missing: Mercury → Mars and Mercury → Titan (the atlas gap).
- **Stations pay for settlers** (`settlers_sponsored` 1).
  - The destination station pays the fare and is the settlers' employer. The offer is limited to its credits above
    its floor.
  - The debt (`passage_debt_cr`) is recovered at 1/`passage_recovery_days` (365) per day from outside money. The
    recovery is capped at `passage_max_markup` (1.0) × what the residents spend on goods, so prices rise to cover it.
  - The ledger gets `passages_paid` and `passages_recovered`; the identity paid = debt + recovered is tested.
- **Settler demand falls with payback.** T = reference generalized cost ÷ (365 × wage), in years.
  - share = 1 / (1 + (T / `settler_payback_half_years`)^`settler_payback_exponent`), with 5 years and exponent 2.
  - Today's settler rate is the maximum, reached when the ticket pays back quickly.
- **Investment** now values passenger berths with the same demand bands and clearing as boarding
  (`expected_passenger_revenue`). The old estimate (the elasticity −1 closed form) overrated the large liners.

### Results
- Passengers off: the 365-day audit is byte-identical to the baseline.
- Start 0 at day 1460: the settler fare is 866 cr (was 3,862). Stations paid 2.2M cr of passages and recovered 1.35M
  (+4.7% on residents' goods). Unmet demand 0.9%; 9 large liners bought.
- Benchmark (4 × 1460 days, passengers on):

  | Run | Unmet | Last fifth | Price ÷ ref | Profitable | Money | Run time |
  |---|---|---|---|---|---|---|
  | earlier, all passengers | 1.1% | 2.4% | 1.7× | 112.5 / 140.5 | −6% | 199 s |
  | follow-up | 1.0 ± 0.4% | 2.6 ± 1.0% | 1.8× | 105 / 144 | −12 ± 10% | 225 s |

- **Fleet cap and speed** (single run, start 0, 1460 days, all threads):

  | Run | ms per day | Slowest 20-day stretch |
  |---|---|---|
  | passengers off, cap 130 | 57.8 | 5.9 days/s |
  | passengers on, cap 130 | 99.7 | 3.3 days/s |
  | passengers on, cap 160 | 112.4 | 2.4 days/s |
  | passengers on, cap 190 | 104.8 | 3.3 days/s |

  Passengers already break 5 days/s at today's cap, so the cap stays at 130 until the passenger code is made faster.

### Speed (2026-10-21, follow-up)

**Where the time went** (gdb stack samples of the main thread, plus the `--profile` phase table, start 0):
- **Fleet investment:** 172 samples with passengers against 109 without. Most of it was valuing candidates,
  not planning:
  - every hull was a candidate three times (bare, cabin, shielded cabin);
  - the passenger bound took the top band's fare (4× the reference cost) for every berth and was added to the
    cargo bound, so cabin designs ranked high;
  - every tick of a review re-valued all the candidates probed so far. Valuations were 266k against 25k
    without passengers.
- **Ship reviews:** ships flew about twice as many missions (6,000 against 3,300), mostly plasma bulk tankers
  with cabins on short hops. That is work, not waste.
- **Departure replans:** during the slowest stretch, the replan of the chosen mission with its path took 40% of
  the main thread (the planner fuel-check bug, below).

**Changes that keep the game identical** (passengers off: the 365-day audit is byte-identical; passengers on:
a 730-day run's events are identical):
- each station's production rates are kept and rebuilt when its population is set, and read by reference
  (`EconomySystem::get_station_net_rates`, 9% of all time);
- investment looks up idle hulls once per review, not per candidate (the design lookup builds a string key);
- passenger flows and berth revenue per station pair are computed once per review;
- valuation labels are formatted only for the leading run;
- the planner caches each ship's fuel fit (a pure function of its key; save/load stays exact).

**Changes to investment with passengers on:**
- a hull fitted with a cabin module is a candidate for its berths only: passenger bound, passenger runs, a
  probe without cargo plans;
- the passenger bound fills the berths by band (as boarding does) for a ship twice as fast as the reference
  carrier, and is the larger of the cargo and passenger bounds.

Result: valuations 266k → 115k.

| Run (start 0, 1,460 days, cap 130) | ms per day | Slowest 20-day stretch |
|---|---|---|
| passengers off | 57.8 | 3.4 s (5.9 days/s) |
| passengers on, before | 99.7 | 6.1 s (3.3 days/s) |
| passengers on, now | 78.1 | 9.1 s (2.2 days/s), from day 1289 |
| passengers on, now, fuel check on | 74.3 | 5.2 s (3.8 days/s), from day 895 |

In today's run, failed departure replans fill the stretch from day 1289. With the fuel check on, that stretch
is gone and fleet investment's probes set the slowest one.

TSan: 800 days with passengers on, no warnings. Save at day 60, load and run 60 more: the save JSON is
identical to a straight 120-day run.

**Benchmark** (4 starts × 1,460 days):

| Run | Unmet | Last fifth | Price ÷ ref | Profitable | Ships | Money | Run s |
|---|---|---|---|---|---|---|---|
| passengers off | 0.5 ± 0.2 | 1.0 ± 0.3 | 1.5 | 101 / 138 | 138 | −20 ± 10 | 161 |
| passengers on, before | 1.0 ± 0.4 | 2.6 ± 1.0 | 1.8 | 105 / 144 | 144 | −12 ± 10 | 225 |
| passengers on, now | 1.0 ± 0.4 | 2.0 ± 1.1 | 1.8 | 109 / 142 | 142 | −16 ± 20 | 195 |
| passengers on, fuel check on | 1.1 ± 0.7 | 1.6 ± 1.2 | 1.8 | 113 / 141 | 141 | −12 ± 26 | 232 |
| passengers off, fuel check on | 1.2 ± 1.0 | 2.2 ± 1.0 | 1.5 | 105 / 138 | 138 | −22 ± 9 | 204 |

**Planner fuel-check bug** (found here, not caused by passengers):
- A VariableISP plan scored without its path checked its end mass on the closest-approach marker, which
  carries the start mass. A window short of fuel thus scored as feasible.
- The replan with the path then rejected it, and the ship chose the same trip the next day. Over 1,460 days:
  - passengers on: 2,821 of 3,300 replans failed;
  - passengers off: 1,876 of 2,374 failed;
  - mostly Earth or Venus → Mercury and back; one freighter tried Earth orbit → Mercury 265 times.
- `ship_operations.csv` `planner_fuel_check,1` checks the shooting's end state, and then no replan fails.
  - It costs speed: every candidate plan searches on past its windows that are short of fuel.
  - With passengers off the economy got worse: unmet 0.5 → 1.2%, last fifth 1.0 → 2.2% (3 of 4 starts).
  - In the start-270 run the extra shortage is Mars electronics, Titan machinery, Earth L1 water and LEO
    food, not Mercury: a cascade rather than one starved route.
- **Decision:** the switch stays 0 (the old behaviour). It is open for the user. A test
  (`fuel check without a path`) shows the old check disagreeing with the path and the new one agreeing.

**The fleet cap stays at 130:** passengers on does not hold 5 days/s.

### Planner fuel check fixed (2026-10-21, the user's decision: fix it properly)

**Why the end-state check alone was slow:** 365 days, passengers off, logged per plan:
- every rejected path needed at least 1.1× its budget's effort, beyond the time fit (10%);
- a refined path needs a median 1.12× the effort of the atlas cell it came from (0.97–1.58×), so even
  cells solved for a weaker ship end up over budget;
- 79% of the rejects were under the 40% and 55% budgets, and the ship's largest budget carried the same path;
- each reject sent the search on to the next window, which costs a new refinement: +46% shootings, +25% time.

**Fix:** the path does not depend on the fuel budget, so a path over its budget is flown with the smallest of
the ship's larger budgets that carries it. This comes before the time fit and costs no further shooting.
`planner_fuel_check` is now 1 in the data and the default; 0 restores the old check.

**Tried and dropped** (365 days, passengers off; planning CPU seconds):

| Variant | Planning CPU | Shootings | Unmet |
|---|---|---|---|
| old check (bug) | 58.5 | 98k | 0.5% |
| end state only | 84.3 | 143k | 0.5% |
| + time fit up to 1.6× over | 426 | 178k | 0.5% |
| + time fit up to 2× over | 438 | 182k | 0.5% |
| + skip the second seed after a reject | 92.9 | 157k | 0.6% |
| larger budget, only beyond the fit's 10% | 60.0 | 98k | 0.5% |
| **larger budget first (kept)** | **33.5** | **62k** | **0.4%** |

Departure replans in that year: 286 of 342 failed with the old check, 0 of 56 now.

**Benchmark, passengers off, 8 starts × 1,460 days** (0, 90, 180, 270 and 45, 135, 225, 315):

| Run | Unmet | Last fifth | Profitable | Ship cash | Run s |
|---|---|---|---|---|---|
| old check | 0.64% | 1.46% | 104.5 / 137 | +17% | 160 |
| fixed | 0.74% | 1.53% | 102 / 137 | +5% | 92 |

- The economy is the same within the spread between starts. Most of the end-state-only check's earlier
  4-start result (unmet 1.2 ± 1.0%) came from one start (day 270, 2.6%), whose shortages were away from the
  routes the fix changes.
- Ships keep less cash: they now pay for the fuel their trips need.
- The passengers-off 365-day audit changes with this fix (new baseline); with `planner_fuel_check,0` it is
  byte-identical to the old one.

### Passenger speed after the planner fix (2026-10-21)

**Speed** (start 0, 1,460 days, all threads, cap 130 unless stated; ms per simulated day, slowest 20-day stretch):

| Run | ms per day | Slowest 20 days |
|---|---|---|
| passengers off | 30.5 (was 57.8) | 10.3 days/s |
| passengers on, no travellers (all three demands 0) | 41.0 | 6.5 days/s |
| passengers on, no visitors | 38.7 | 7.9 days/s |
| passengers on, no settlers | 45.6 | 5.8 days/s |
| passengers on, no rotation | 44.7 | 6.9 days/s |
| passengers on | 47.1 (was 78) | 6.3 days/s |
| passengers on, cap 160 | 52.1 | 6.0 days/s |

- Passengers on, the other starts: 50.4 / 48.2 / 50.0 ms per day; 6.1 / 7.1 / 5.5 days/s. **5 days/s now holds
  at cap 130 with passengers on, in all 4 starts.**
- Cap 160, passengers on, the other starts: 52.6 / 55.3 / 56.1 ms per day; 6.2 / 6.2 / 5.6 days/s; 171–176
  ships; unmet 2.1 / 1.0 / 0.7 / 0.6% (starts 0 / 90 / 180 / 270, single runs). Measured on a 16-thread
  machine; a slower player machine has less margin.
- The slowest single day is 0.6–1.0 s with passengers on or off: fleet investment's purchases. Each purchase
  plans every tank and module variant of the winning hull (0.1–0.2 s), and a review's 8 purchases fall
  within 2–3 days of the 60-day cycle.
- Benchmark, passengers on (4 × 1,460 days): unmet 1.2 ± 0.6%, last fifth 2.4 ± 0.7%, 112 / 145 profitable,
  run 129 s (was 195 s with the old check). Save at day 60, load, run 60: identical to a straight 120 days.

**Where the passengers' 16.6 ms per day goes** (phase times, investment trace, plan requests, main-thread samples):
- **The machinery, with no traveller at all: 10.5 ms.** With the switch on, cabin modules are active designs:
  - fleet investment probes each hull bare, with a cabin and with a shielded cabin, plus the two liners:
    3,143 probes against 1,224 off, 1,490 of them cabin designs and 248 liners (investment 16.6 → 30.8 s);
  - every docked ship's refit check prices the cabin modules too (refit checks 5.5 → 9.3 s);
  - ships with berths and investment's passenger options ask for 24k more plans (730 days).
- **Visitors: 8.4 ms.** Visitor demand is a fractional flow between every pair of stations, so most stations
  hold small groups for many destinations. Each becomes a candidate leg (passenger-only or with cargo) with its
  own plans, and plans with passengers aboard carry their time (separate cache keys, 10 cr/day buckets).
- **Rotation 2.4 ms, settlers 1.5 ms** (fewer, larger groups on fewer routes).
- Fleet investment is about 60% of the main thread's time with passengers on or off; ship reviews about a third.

**Proposals to make passengers cheaper** (not built; ordered by expected gain per effort):
1. **A cabin design shares its hull's probe.** A hull with a cabin flies the bare hull's transfers with a few
   tonnes more payload. Value its berths from the bare hull's probe plans instead of a mission search of its
   own; the variant pick plans the bought design in full anyway. Removes most of the 1,490 cabin probes,
   about a third of investment's probing (expected 4–6 ms per day).
2. **One cabin candidate per hull; the shield is chosen in the variant pick,** like the tanks. The shielded
   cabin pays only on long or high-dose legs, which the variant pick sees. Halves the cabin candidates if 1
   is not built.
3. **Cabin refits only where passengers wait.** Price the cabin modules in a docked ship's refit check only
   when its station has passenger demand above a threshold (or on the slower refit cadence). Refit checks
   went from 5.5 to 9.3 s.
4. **Visitors in groups.** Generate visitor demand weekly into groups, and offer a destination to ships
   only from a minimum group (a few people, or fares of at least a set share of the trip's cost). Most of
   the visitors' 8.4 ms comes from small groups making every destination a candidate leg.
5. **Fewer plans with passengers aboard.** Reuse the cargo plan when the riders add little to the ship's
   cost of time (under 10–20%), and bucket the riders' time relative to it (25% steps), not in 10 cr/day.
   Passenger-only legs can take the propellant per kg from the empty plan (berths weigh little) instead of
   planning laden and half-laden transfers.
6. **Spread purchases (passengers on or off):** at most one purchase per day. Today a review's 8 purchases
   fall within 2–3 days and make the slowest days 0.6–1.0 s. Spread over 8 days of the 60-day cycle, the
   slowest day should fall to about 0.3 s.

Order: 1, 6 and 4 first; measure; then 3 and 5. Every change goes behind a switch, judged by the benchmark,
and the passengers-off audit stays byte-identical.

### Passenger speed proposals built (2026-10-22, the user's decision: build 1–3, measure, then 4–6)

Numbering as in the report to the user (log numbers in brackets). Switches in `data/economy/passengers.csv`
unless stated; each code default is the old behaviour, and the passengers-off 365-day audit with them at the
defaults is byte-identical.

**Built and on in the data:**
1. **A cabin design shares its bare hull's probe** [1] (`cabin_shared_probe,1`). A bare hull's investment
   probe also plans its cabin designs' passenger runs: the bare hull with the module's mass and full berths as
   payload, in the same probe's plans; crew-day, dose and drive-life limits are the fitted design's. The runs are
   tagged with the module (`ProbedRun::module_id`, saved), and a cabin candidate is valued from them with no
   probe of its own. Fleet investment 29.4 → 25.5 s over 1,460 days: less than the 4–6 ms per day estimated,
   because the cabin designs' plans are now made in the bare probe instead (they were not shared with cargo
   plans before either).
2. **At most one purchase per day** [6] (`fleet_investment.csv` `purchase_spacing_days,1`; `next_purchase_s`,
   saved; save version 16). Probes already made in the review are kept. Passengers off: slowest day 0.71 → 0.43 s,
   economy unchanged (4 starts: unmet 0.7 ± 0.1%, last fifth 1.5 ± 0.2%, 102.8 / 137 profitable, against 0.74%,
   1.53%, 102 over 8 starts before).
3. **Visitors in groups** [4] (`visitor_batch_days,7`, `min_leg_passengers,20`). A week of visitor trips is
   posted at once, and a station becomes a leg for passengers alone (or a passenger follow-up) only once 20
   people wait for it; smaller groups still ride on legs flown for cargo. A minimum of 5 hardly bound (queues hold
   thousands, mean waits over 200 days) and gave no speed; 20 did (41.5 against 43.6 ms per day with 1 and 2 on),
   with as many visitors carried as before (5,985 boarded against 5,866, single runs).
5. **Fewer plans with riders aboard** [5] (`rider_time_min_share,0.15`, `rider_time_relative_step,0.25`,
   `passenger_leg_from_empty,1`). Riders adding under 15% of the ship's own time cost fly on the plan without
   them; above, their time is bucketed in quarters of the ship's time cost (before: 10 cr/day steps); legs for
   passengers alone take the propellant per kg from the empty plan (same mass ratio) instead of planning laden
   and half-laden transfers. The largest single gain: 43.6 → 40.0 ms per day (variable-Isp planning −6%, ship reviews −14%).

**Measured and removed** (no gain; the switches would do nothing):
- 4 [3], cabin refits priced only where at least 10 people wait: the run was identical to the one without it
  (the refit's mission search already never picks a cabin with no one to fill it), refit checks 8.6 → 8.4 s.
- 6 [2], one cabin candidate per hull: with 1, the plain and shielded cabins share their plans, so only
  valuations were saved; within noise (43.7 ms per day either way).

**Results, passengers on, all of the above against the old values** (1,460 days, run one after another, all
threads, cap 130):

| Start | ms per day, before → now | Slowest 20 days, days/s | Slowest day, s |
|---|---|---|---|
| 0 | 45.1 → 43.9 | 6.6 → 7.6 | 0.76 → 0.65 |
| 90 | 49.3 → 42.0 | 6.0 → 6.9 | 0.71 → 0.38 |
| 180 | 46.4 → 43.5 | 7.1 → 7.3 | 0.62 → 0.51 |
| 270 | 48.0 → 42.2 | 5.6 → 7.0 | 0.94 → 0.53 |
| mean | 47.2 → 42.9 (−9%) | worst 5.6 → 6.9 | worst 0.94 → 0.65 |

Passengers off (only purchase spacing acts): 29.2 → 28.7 ms per day, slowest 20 days 10.2 → 10.8 days/s.

**Benchmark, passengers on, 8 starts × 1,460 days:**

| Run | Unmet | Last fifth | Profitable | Ships |
|---|---|---|---|---|
| old values | 1.2% | 2.2% | 112.9 | 145 |
| 1 + 2 + 3 (minimum 5) | 1.3% | 3.3% | 111.9 | 145 |
| 1 alone | 0.9 ± 0.3% | 1.9 ± 0.9% | 109.8 | 143 |
| 2 alone | 1.3 ± 0.7% | 2.1 ± 1.0% | 111.9 | 144 |
| 3 alone (minimum 5) | 1.1 ± 0.6% | 2.0 ± 0.8% | 110.1 | 144 |
| **final (1, 2, 3 at 20, 5)** | **1.1 ± 0.5%** | **1.8 ± 0.8%** | **110.2** | **143** |

The 1 + 2 + 3 run's last fifth came from two starts (8.7% and 5.4%); each change alone and the final set are
within the spread of the old values.

**Why the total gain is small:** most of the time with passengers on is trajectory planning for ordinary ship
reviews and investment probes of cargo runs (variable-Isp plans ~200 s of CPU over 1,460 days), which these
changes do not touch. Passengers now cost about 14 ms per day over passengers off (42.9 against 28.7).

Checks: ctest 5/5 (the passenger test now also checks, with the data's switches, at most one purchase a day,
visitors posted only at week edges, and save at day 65 / load / run to 130 equal to a straight run); the
passengers-off audit with `purchase_spacing_days,0` is byte-identical to the one before these changes.

## Activation (2026-10-22, the user's request: raise the cap if 5 days/s holds, check Titan, then switch on)

**Fleet cap sweep** (passengers on, 4 starts × 1,460 days, runs one after another with all 16 threads):

| Cap | Unmet | Last fifth | Ships | Profitable | Money | Slowest 20 days (worst start) | Boarded | Gave up | Waiting at end | Visitors' mean wait |
|---|---|---|---|---|---|---|---|---|---|---|
| 130 | 1.1 ± 0.3% | 2.1 ± 1.0% | 145 | 76% | −20 ± 7% | 5.1 days/s | 10,033 | 10,947 | 8,820 | 225 d |
| 160 | 1.1 ± 0.6% | 2.1 ± 1.1% | 177 | 70% | −41 ± 19% | 6.3 days/s | 9,246 | 10,977 | 8,836 | 233 d |
| 190 | 1.2 ± 0.2% | 1.1 ± 1.1% | 203 | 65% | −52 ± 18% | 6.2 days/s | 9,228 | 11,160 | 8,656 | 237 d |
| 220 | 0.9 ± 0.4% | 1.2 ± 1.0% | 208 | 65% | −43 ± 20% | 6.9 days/s | 8,066 | 11,369 | 9,038 | 253 d |

- 5 days/s holds at every cap (the slowest stretch is set by fleet investment and chaotic, not by the ship count).
- More ships carry no more passengers (explanation corrected in "Cap findings checked" below): the extra purchases are mostly bulk tankers with cabins, the berths' load
  factor falls (50% → 39%), and as many people give up. The queues are not short of berths; they wait for a
  trip that suits them (budget, minimum leg of 20, the carrier's other cargo).
- Unmet demand is the same within the spread; profitability and money fall as capital goes into hulls.
- Above ~200 ships the cap no longer binds (no candidate clears the hurdle).
- **Decision (default, reported to the user):** the cap stays at 130.

**Titan check** (2 starts × 2,920 days, with a new per-route passenger audit: `--econ-audit` prints passengers
by station and every route to or from Titan):

| Route | Start 0: boarded / landed | Start 180: boarded / landed | Trip | Highest leg dose | Carriers |
|---|---|---|---|---|---|
| Earth L1 → Titan | 223 / 223 | 236 / 236 | 613–790 d | 858–937 mSv | Large Plasma Liners with a water wall or tank wrap, Plasma Liners with 200% tanks |
| Earth orbit → Titan | 124 / 0 | 153 / 0 | still in flight at the end | 854–868 mSv | Large Plasma Liners with tank wrap |
| Titan → Earth orbit | 40 / 40 | 211 / 211 | 600–632 d | 780–887 mSv | liners with tank wrap or water wall, couriers with shielded cabins |

- Titan is served both ways, under the 1 Sv leg limit; the shields are what make it possible.
- Few ships leave Titan, so most of Titan's travellers home give up (2,428–2,684 posted); one Plasma Liner was laid up
  and sold at Titan. A matter of demand for the return leg, not a blocker.

**Switches removed** (the user's question: are the speed switches needed?): `cabin_shared_probe`,
`passenger_leg_from_empty` and `planner_fuel_check` were on/off choices between an old and a new code path, and
the old paths are gone. `rider_time_relative_step` lost its 0 = 10 cr/day fallback. The tunable numbers stay in
the data (`visitor_batch_days`, `min_leg_passengers`, `rider_time_min_share`, `rider_time_relative_step`,
`purchase_spacing_days`), with code defaults equal to the data. Runs before and after are byte-identical
(passengers on 365 and 1,460 days, passengers off 365 days).

**Switched on:** `passengers,1` in `data/economy/passengers.csv`. Tests of the economy before passengers (the
off check, station growth by step 27) run on a copy of the data with passengers off; the money identity test
now counts fares travellers pay and passages stations recover. ctest 5/5. Godot started headless with no script
errors; the user then checked the passenger UI in the game (fine). Mercury still has no passengers (atlas gap).

### Cap findings checked (2026-10-22, the user found both suspicious)

**"Faster with more ships" was a measurement artefact.** The table reported the worst of 4 starts. At cap 130
one start had a 5.1 days/s stretch; the other three had 7.0-7.7, the same as at cap 220 (6.9-7.4). Whole runs
take longer with more ships: mean wall time 64 s (cap 130), 76 s (160), 81 s (190), 82 s (220) per 1,460 days.

**Why more ships carry no more passengers.** A new audit line (`--econ-audit`: "Routes by people who gave
up", with legs flown, legs by ships with berths, and those that left empty while people waited) and "Legs
flown" show, start 0 / 180, cap 130 → 220:
- Ships +31%, legs flown +6% (1,424 → 1,514; 1,977 → 2,044). The extra ships are not idle (in transit 105 →
  147-163); they are bought from year 2 onward for interplanetary legs of 150-500 days, so each adds only a
  few legs within 4 years.
- Legs by ships with berths on routes where people post trips: 211 → 219 and 248 → 251. Unchanged.
- Ships with berths almost never leave empty while people wait (0-6 legs per route over 4 years), so boarding
  works; there are too few legs where the people are.
- The queue is mostly Earth ↔ Luna: 4 of the top 4 give-up routes, about 7,000 of 11,000 give-ups. Ships with
  berths fly 10-30 of the 120-190 legs on each of these. The purchase trace (`SPACETRAINS_TRACE_INVESTMENT`)
  values every Luna passenger design below the hurdle (shuttle −48%/yr, light freighter or tanker with a cabin
  −14 to −16%/yr, plasma courier −13%/yr), at an 86-93 cr reference fare. The passenger purchases go to Mars
  and Venus → Earth instead.
- The −20% in boardings (10,033 → 8,066 over 4 starts) is not fully pinned down: it falls on Luna and long
  routes alike, with the same berth legs. Lower faction money (sponsored settlers −300) explains part.
- The earlier wording ("the extra purchases are mostly tankers with cabins, berths fly half empty, people
  wait for a trip that suits them") was wrong and is replaced by this.

**Recommendation:** keep the cap at 130. The lever for waiting passengers is the Earth ↔ Luna service (fare
and shuttle economics, the long probe times), not the ship count.
