# Ship Operating Costs (Part A)

## Goal

Make time cost money, so that physically feasible but uneconomic trajectories (multi-revolution ion spirals,
retrograde transfers, year-long launch waits, empty repositioning loops) lose to better options on their own,
without heuristic cut-offs. Later parts build on it: mission-sized fuelling (B) and modular tanks/refits (C).

## Baseline (2026-10-06, 730-day `--econ-audit`, 22 ships)

The economy already loses money *before* any operating cost. Pre-session `main` (4b5d563) shows the same picture,
so this is not caused by the trajectory fixes.

| Metric | Value |
|---|---|
| Cargo runs / repositioning trips | 23 / 35 |
| Cargo revenue realised | ~14.6k cr |
| Fuel bought | ~30.0k cr (about 2x revenue) |
| Ships with lifetime profit > 0 | 1 of 22 (7 never earned anything) |
| Typical repositioning wait | 250-370 days (Earth L1 <-> Venus launch windows) |
| CRITICAL starvation lines | 14 |

Adding wages and capital charges on top would push the whole fleet into lay-up. Part A therefore calibrates
**costs and the revenue side together**.

## Model

Per ship, accrued continuously (prorated by `dt`, since the bridge steps in fractions of a day):

1. **Capital**: `ship_value_cr` per class (engine power priced into ion classes). Daily charge
   `ship_value_cr * (interest_rate + 1 / lifetime_years) / 365`. Accrues in every phase, laid up included.
2. **Crew wages**: `crew_size * wage_cr_per_crew_day`. Not charged while laid up.
3. **Life support**: food, water and oxygen per crew-day, *bought as real commodities*. A departing ship buys
   provisions for wait + transit + 10% from the origin station's market; a docked ship buys daily from its current
   station. Missions are infeasible if the origin cannot supply provisions. Provision mass is negligible next to
   propellant (a crew of 4 eats ~2.4 kg/day of food), so it is not added to the trajectory mass in Part A.

**Money flows** (total supply must stay constant, `drift = 0` in the audit):
- Wages and capital charges are paid to the ship's **home station** (crew spending, owner's return).
- Life-support purchases are ordinary trades with the selling station.

**Lay-up**: a ship whose credits fall below zero lays up at its current station. Its crew is discharged (no wages,
no life support) while capital keeps accruing. It reactivates when a mission's expected profit covers the cost of
rehiring and provisioning. Ships may already go into debt for fuel; that rule stays.

**Mission scoring** compares whole missions over their whole duration:

```
profit = revenue - cargo cost - fuel cost - provisions - daily_cost * (wait_days + transit_days)
score  = urgency * profit / max(1, wait_days + transit_days)
```

Repositioning trips (no cargo) are charged the same time and fuel cost against their sourcing benefit.

## Data

- `data/ship_classes/ship_classes.csv`: add `ship_value_cr`, `crew_size`.
- `data/economy/ship_operations.csv` (new, key/value): `wage_cr_per_crew_day`, `interest_rate`, `lifetime_years`,
  life support per crew-day for `food`, `water`, `oxygen` (in commodity units).

## Calibration

Targets, measured with the 730-day `--econ-audit` and the trajectory sweep:

1. A representative good interplanetary cargo run (Earth L1 <-> Mars / Venus, light freighter) earns a net
   margin of about 1.5-3x its fuel plus time cost.
2. At least half of the fleet ends the 730 days with positive lifetime profit. (A ship laid up for the whole
   run is acceptable when its class is uneconomic where it sits: the starting fleet is not final, and fleet
   investment, on the roadmap, will retire or move such ships.)
3. Waiting at least 200 days for a launch window should only win when the cargo margin justifies it, and
   multi-revolution ion trajectories should almost never be accepted, with no explicit rule against them.
4. Total money is conserved; no strandings; CRITICAL starvation does not get worse than the baseline (14).

Revenue levers, in order of preference: price spreads between producing and consuming stations (the base prices
are low next to the fuel cost of moving 100 kg between planets), then fuel price, then cargo capacities.

## Steps

1. Data and loading (new columns and the parameters file, with validation).
2. Continuous cost accrual, home-station payments, lay-up and reactivation, plus audit output (per-ship cost
   breakdown, laid-up count).
3. Provisions purchase on departure and daily docked consumption.
4. Mission and repositioning scoring with time cost.
5. Calibration loop against the targets above; record final parameters and audit numbers here.

## Calibration Log (2026-10-06)

730-day `--econ-audit` runs. Costs: wage 2 cr/crew-day, interest 5%, lifetime 30 y, ship values 35k-120k.

| Run | Change | Cargo margin | Fuel | Wages | Capital | Profitable | Laid up at end | CRITICAL |
|---|---|---|---|---|---|---|---|---|
| baseline | no costs (old prices) | ~1k net | 30k | - | - | 1/22 | - | 14 |
| v1 | costs, old prices | 0.2k | 0 | 130k | 274k | 0/22 | 5 | 14 |
| v2 | goods x5 base, price cap 4x -> 16x | 120k | 3k | 133k | 274k | 2/22 | 3 | 15 |
| v4 | consumer stocks 45 days, idle 30 d -> lay up | 200k | 2k | 17k | 274k | 4/22 | 22 | 41 |
| v6 | Earth-Moon transfers around Earth (was 183 d) | 390k | 35k | 31k | 274k | 7/22 | 21 | 44 |
| v9 | demand follows price signals, not just recipes | 695k | 45k | 40k | 274k | 7/22 | 20 | 41 |
| v12 | water/O2 recycled, tank fuel at base price, return-fuel check | 459k | 47k | 51k | 274k | 5/22 | 14 | 39 |
| v13 | two-leg mission scoring (alone) | 355k | 25k | 61k | 270k | 4/22 | 14 | 37 |
| v14 | + 2-year provisioning endurance, cost-optimal Lambert, rocket-equation return check | 809k | 53k | 98k | 274k | 11/22 | 13 | 35 |
| v16 | part B: mission-sized fuelling, cargo + provisions in the rocket equation | 690k | 10k | 108k | 274k | 10/22 | 9 | 34 |
| v17 | tech review: classes rebuilt bottom-up (heavier, NTR Δv halved), ship values unchanged | 324k | 3.6k | 38k | 274k | 7/22 | 13 (+3 stranded) | 43 |
| v18 | fuel depots at every station (refill to 3000 u at 200 u/day, outside cargo storage); fuel at base price | 648k | 168k | 70k | 274k | 8/22 | 7 (0 stranded) | 35 |
| v19 | ships priced from the hardware breakdown (fleet total kept); plasma 90k -> 40k, liquid-core NTR dearer | 790k | 171k | 71k | 274k | 9/22 | 6 (0 stranded) | 31 |
| v20a | dispatch: other ships claim one commodity each, part loads when a full hold is infeasible | 799k | 141k | 78k | 274k | 8/22 | 4 | 29 |
| v20 | + holds about each ship's dry mass (200-400 u freighters); sales still at the pre-trade price | 1.73M | 149k | 82k | 281k | 8/22 | 2 | 31 |
| v22 | + trades priced along the price curve, lot size by score: markets too thin for big lots | 53k | 42k | 23k | 281k | 1/22 | 14 | 38 |
| v23 | economy scaled with population: recipe rates per 10k inhabitants x10, storage and stocks x pop/1000 | 335k | 187k | 75k | 281k | 3/22 | 6 | 39 |
| v24 | + sales valued on the destination's forecast stock (inbound cargo, consumption by arrival) | 929k | 185k | 88k | 281k | 8/22 | 4 (0 stranded) | 36 |
| v25 | part C: tank refits at the home base (50/100/150/200% variants; 19k cr of yard bills) | 1.47M | 92k | 85k | 274k | 11/22 | 4 (0 stranded) | 36 |
| v26 | open economy (`docs/plans/open_economy.md`): fleet unchanged; money supply +16.7%, every station at or above the 25k floor | 1.47M | 92k | 85k | 274k | 11/22 | 4 (0 stranded) | 36 |
| v27 | fleet investment (`docs/plans/fleet_investment.md`): 7 ships bought, 4 sold; money supply +7.0% | 1.39M | 119k | 94k | 263k | 15/29 | 0 (0 stranded) | 34 |
| v28 | route commitment: new ships shuttle their route for 180 days; committed flow counts against demand; money supply +5.5% | 1.39M | 135k | 91k | 261k | 13/29 | 0 (0 stranded) | 33 |
| v29 | fuel factories (`docs/plans/fuel_factories.md`): only 5 stations make fuel, bulk tankers, ships carry return fuel; money supply +2.2% | 1.54M | 690k | 98k | 276k | 14/32 | 0 (0 stranded) | 32 |
| v30 | investment valued over the commitment at forecast stocks and observed flows; unmet demand 71.7% (v29 72.3%); money supply +8.8% | 1.99M | 699k | 97k | 273k | 16/32 | 1 (0 stranded) | 34 |
| v31 | up to 8 ships bought per review (19 bought, fleet 41); unmet demand 71.4%; money supply +6.2%, treasuries 69k left | 2.06M | 662k | 101k | 283k | 19/41 | 2 (0 stranded) | 35 |
| v32 | outer exports: Ceres platinum to Earth's markets (one 129u load, 387k); ships drawn off inner routes; unmet demand 74.3%; money supply +0.3% | 1.66M | 473k | 101k | 287k | 12/36 | 2 (0 stranded) | 33 |
| v33a | follow-ups forecast a producer's stock at arrival and leave other ships' planned pickups to them; unmet demand 73.1%; money supply +3.3% | 1.82M | 617k | 99k | 282k | 15/39 | 1 (0 stranded) | 34 |
| v33 | + plasma planner: κ at the origin's radius, missed windows flown at once; exports 1.02M (platinum 217u, deuterium 246u); unmet demand 72.9%; money supply +6.7%; fleet profit 1.035M | 2.06M | 599k | 104k | 300k | 17/43 | 2 (1 stranded) | 34 |
| v34 | fixed 0.1-day tick for headless and UI (reviews every 6 h now also headless); speed work, results otherwise unchanged; unmet demand 72.2%; money supply +5.5%; 730 days in 42 s | 1.87M | 601k | 102k | 297k | 18/41 | 1 (0 stranded) | 34 |
| v35 | ordered maps (`Inventory`, treasuries) for exact save/load; unmet demand 72.5%; money supply +5.3% | 1.98M | 687k | 103k | 299k | 17/43 | - | 35 |
| v36a | sale forecasts count only cargo arriving before the ship; unmet demand 71.6%; money supply +11.7% | 2.11M | 751k | 105k | 301k | 20/41 | - | 37 |
| v36b | consumers' target stock covers their resupply time (21 d to a year); fleet grows to 104 ships, 39,700 u of holds; unmet demand 66.3% (61% in the last fifth); money supply +7.3% | 9.72M | 1.86M | 238k | 572k | 48/104 | - | 28 |
| v36e | + `max_fleet_size` 90; candidates planned without paths, integrator end-sample fix; unmet demand 66.3%; money supply +9.1%; 1m08 | 9.75M | 1.77M | 237k | 566k | 48/93 | - | - |
| v37a | mixed cargo (holds filled by marginal value); 41,400 u of holds; unmet demand 59.8% (40.6% in the last fifth); money supply +29.7% | 13.16M | 1.09M | 241k | 640k | 49/93 | - | - |
| v37b | fleet reviews spread over ticks; unmet demand 63.3% (four starts: 63.3 +- 2.2%, last fifth 49.8 +- 5.3%); money supply +20.3%; 52 s | 12.28M | 1.20M | 236k | 642k | 47/94 | - | - |

Mechanisms found on the way (each fixed in the code):
- Earth <-> Moon hops were heliocentric Hohmann transfers (183 days); now planet-system transfers (commit 760e080).
- Provisions as real goods deadlocked crews at starving ports: departing crews may buy from the whole stock,
  docked crews do not eat the 120-day return reserve (they lay up instead), ships start with 180 days aboard,
  water and oxygen are mostly recycled (0.1 / 0.05 kg per crew-day make-up).
- Ships flew into fuel-dry consumers and stayed: every mission now checks the ship can refuel to leave again.
- Stations short of goods they do not consume themselves (water drunk by crews) were invisible to traders;
  destinations now follow price (price above base), not only recipes.
- Fuel already in the tank was valued at the origin's scarcity price (16x), so loaded ships never left
  fuel-starved ports; it is now valued at base price.

### Two-leg scoring (v13-v14)

- Every candidate leg (cargo, or empty toward a pickup) is scored with the best follow-up leg from its
  destination: `(urgency * leg profit + follow-up profit) / (leg days + follow-up days)`. Follow-ups are
  estimated from today's stocks and prices, minus the holds of ships already docked at or inbound to that
  port; with no profitable follow-up the ship is charged an empty trip back (or 30 idle days if even that is
  impossible). Follow-up plans are cached per class, 5-day departure bucket and 10%-of-tank fuel bucket
  (`Simulation::estimate_leg`). The old sourcing-score repositioning remains as a fallback.
- On its own (v13) this changed nothing: the trace (`SPACETRAINS_TRACE_SHIP`) showed the idle ships had no
  feasible legs to choose between. Three physical blockers, each fixed:
  1. Crews carried 180 days of food but outer legs take 400-1100 days and outer ports import food. Ships now
     keep 730 days aboard (start with it; top up where ports can spare it; on departure the mission's own
     need may come out of the whole stock).
  2. The Lambert search picked the fastest transfer the tank allowed, burning 70-100% of an NTR's fuel per
     leg. It now minimises fuel cost plus time cost (`PlanningCosts`).
  3. The return-fuel check demanded the outbound leg's full-tank propellant again. A ship arriving nearly
     empty needs far less for the same delta-v: `dry * (mass_ratio - 1)`.
- v14 trajectory audit (730 days, 108 accepted plans): 0 flagged; the most revolutions on an accepted ion
  path is 1.18.

**Status vs targets after v14:** money conserved (drift 0), no strandings, 11/22 ships profitable (target
met), no accepted multi-revolution trajectories (met). Not met: three ships never leave their port (two ion
freighters at Titan have no converged atlas window to any destination; an NTR at Ganymede has only
600-800-day legs whose cargo, 30 units worth about 900 cr, cannot pay about 25k of time cost), and CRITICAL
lines are 35 vs a baseline of 14, now concentrated in the outer system (Ceres, Ganymede, Titan) and Mercury.
The outer system is uneconomic because holds are tiny next to tanks: an NTR freighter carries 30 units
(3-6 t) on 190 t of propellant. Raising cargo capacities only makes sense once cargo mass enters the rocket
equation, which belongs with part B (mission-sized fuelling).

## Part B: Mission-Sized Fuelling (v16)

- `PlanningOptions` (was `PlanningCosts`) also carries the payload (cargo + provisions), the propellant the
  ship could still buy at the origin, and the reserve (10%). Plans report `propellant_load_kg`: what is
  aboard at departure.
- Chemical/NTR (`ChemicalLoading`): the load for a Δv is the smallest L with L = 1.1 x burn, burn =
  (m_dry + payload + L)(1 - e^{-Δv/ve}), but never less than what is already aboard. The Lambert search
  costs each candidate with that load, so payload and tank mass both enter the time-versus-fuel choice.
- VariableISP: engine power stays alpha x hull mass; payload only adds mass. The ship tries fuel budgets of
  100/75/55/40% of what it could load, ranks them by atlas estimate (fuel cost + time cost of the best
  window), and refines the two cheapest. Departs with 1.1 x the budget, so it arrives with the reserve.
- Ships no longer top up to 80% whenever idle; they buy the plan's load when a mission is assigned (the
  removed rules: refuel below 50%, ion 15% planning reserve, ion 25% arrival rule). Stranded = no
  mission, under 1% fuel and none for sale.
- Plans are cached per (destination, cargo tonne); docked ships look for work every 6 hours, laid-up ships
  every 5 days. 730-day run 2m44 (v14: 2m51).
- Result: fuel bought 53k -> 10k cr; trajectory audit 0 flagged of 102 accepted plans (max ion revolutions
  1.07). Profitable ships 11 -> 10 and the margin concentrates (one ion freighter earns 260k).

**Status vs targets (v12):** money conserved, no strandings, the fleet as a whole earns more than it costs (v12: 459k
margin vs ~377k costs). Not met: profits concentrate in 5-7 ships, most ships end laid up, and stations starve
more than in the baseline (39 vs 14 CRITICAL lines). The fleet moves only ~1.9 units/day against ~11 units/day of
demand: long launch windows (Mars synodic ~780 days), provisioning range, and refuel constraints make many
individual trips unprofitable or infeasible even when the destination pays the 16x cap.

## Part C: Tank Refits (v25)

- **Variants.** `data/ship_classes/build_ship_classes.py` builds every class with 50%, 100%, 150% and 200%
  of its nominal propellant. Engine, radiators, habitat and hold stay the same. Tankage (LH2 0.18 kg/kg,
  plasma 0.05 kg/kg) and the 10% structure follow the propellant, and dry mass, Δv, acceleration, α and
  price come from the same parts model. Variants share the class's `hull_id`; the nominal one keeps the
  class id (`ntr_freighter`, `ntr_freighter_tank150`, ...).
- **Where and when.** A docked ship at its home base (its owner's yard) compares the next smaller and
  next larger tank every 90 days. Each comparison is a full dispatch pass (`choose_mission`) with the
  ship as that variant, planned from the day the yard would finish, because launch windows close
  meanwhile. The first version scored the variants on today's windows: Mars freighters refit for a
  250-day window that was gone 20 days later, leaving 953 days to wait.
- **Decision.** Refit when (variant score − current score) × 180 days > yard bill + current score ×
  20 days. The scores are dispatch's two-leg profit per day (a repositioning or no-mission ship counts
  as 0). They already include each variant's capital charge. After a refit, the ship does not
  reconsider for the yard time plus 180 days; without this lock a Light Freighter swapped tanks four
  times in a year.
- **Costs.** 20 days in the yard with the crew discharged (capital is still charged). The yard bill is
  30% of the hardware value added or removed, paid to the home station (`ShipLedger::refits`). The
  owner finances the hardware itself, so the new ship value sets the capital charge and credit line.
  Propellant that no longer fits is sold to the yard's depot along the fuel price curve.
  `data/economy/ship_operations.csv`: `refit_days`, `refit_cost_fraction`.
- **v25 result:**
  - 10 refits on day 1, mostly to 50% tanks for inner-system work (lighter ships, lower capital, more
    payload Δv on short hops), plus 2 later ones: one back to nominal, one plasma freighter to 150%.
  - Cargo margin 929k → 1.47M, fuel 185k → 92k, 8 → 11 ships profitable. Money drift 0; trajectory
    audit clean.
  - The 730-day run takes 2m55 (v24: 2m07); the variant probes cost about 0.3 s each.
  - With refits turned off, the run reproduces v24 exactly.
- **Not solved by tanks:**
  - The NTR Freighter at Ganymede: even at 200% tanks every inward leg takes 1,200+ days and about 78k of
    time cost.
  - The Mars light freighters: no window within reach after the yard.
  - The Titan tanker and plasma freighter.
  These are fleet-investment and outer-export questions.
- **Side effects:**
  - The fleet now captures more of the station spread. Consumer stations bleed harder: Low Earth
    Logistics ended at −458k (v24: +194k), mostly paid to a half-tank Fast Courier shuttling food and
    electronics between Earth L1 and LEO (0.1-day hops, 489k margin). The open economy (roadmap step 5)
    should come before this grows further.
  - Ships that shrink their tanks resell the surplus at the port's fuel price. The NTR Freighter SF
    Icarus netted 22k that way at Venus.

