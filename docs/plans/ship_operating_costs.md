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
2. At least half of the fleet ends the 730 days with positive lifetime profit; no ship stays laid up for the
   whole run.
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

**Status vs targets:** money conserved, no strandings, the fleet as a whole earns more than it costs (v12: 459k
margin vs ~377k costs). Not met: profits concentrate in 5-7 ships, most ships end laid up, and stations starve
more than in the baseline (39 vs 14 CRITICAL lines). The fleet moves only ~1.9 units/day against ~11 units/day of
demand: long launch windows (Mars synodic ~780 days), provisioning range, and refuel constraints make many
individual trips unprofitable or infeasible even when the destination pays the 16x cap.
