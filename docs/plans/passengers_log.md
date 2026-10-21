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
