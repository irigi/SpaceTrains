# Steps 18–22 (2026-10-08): log and decisions to review

Plan: start stocked (18), production at 3x consumption (19), landed-cost fuel at depots without a factory (20),
fleet cap by hold capacity (21), scheduled liners (22). The user chose the options for 19–22 on 2026-10-08.
Benchmarks: `tools/benchmark.py`, four starting days, mean ± sd. Baseline (after step 16), 730 days: unmet
48.5 ± 2.9%, last fifth 29.9 ± 5.2%, 50.5/90 profitable, money −23.9 ± 8.2%, exports 0.79M, 13 emergencies.

## Step 18: start stocked

`Simulation::starting_inventory`: every good a station consumes starts at least at its target stock (its
resupply cover: three weeks, or 1.4x the transfer from the nearest producer, at most a year), scaled down to fit
85% of storage. The data files keep their inventories; the rule follows the recipes.

| | unmet | last fifth | profitable | holds | money | exports | emergencies | interest |
|---|---|---|---|---|---|---|---|---|
| after step 16 | 48.5 ± 2.9% | 29.9 ± 5.2% | 50.5/90 | 94,600 u | −23.9 ± 8.2% | 0.79M | 13 | 123k |
| step 18 | **24.0 ± 2.9%** | 25.0 ± 4.2% | 50.2/90 | 110,200 u | −30.3 ± 16% | 1.15M | 14 | 36k |

Most of the old two-year ramp-up was the stations starting nearly empty.

### Decisions to review

1. **Stations start at their target stock**, not at the data files' few weeks. For the outposts that is up to a
   year of consumption, the stock they would hold if they had been supplied all along.

## Step 19: production at 3x consumption

`tools/rebalance_production.py` scales every profile industry's output to 3x what the stations consume of that
good (system-wide, population-weighted), and material inputs with the outputs they feed, iterated until stable.
Fuel, export goods, upkeep and consume rows, and station-specific rows (local resources: Lunar ice and metals,
platinum, deuterium, science) are left alone. Reference prices regenerated.

| good | produced/day before | consumed | ratio | produced after | consumed | ratio |
|---|---|---|---|---|---|---|
| food | 195.0 | 22.3 | 8.8 | 66.8 | 22.3 | 3.0 |
| water | 81.0 | 7.5 | 10.8 | 11.5 | 3.8 | 3.0 |
| oxygen | 98.0 | 10.5 | 9.3 | 31.5 | 10.5 | 3.0 |
| metals | 163.0 | 16.9 | 9.7 | 36.2 | 12.1 | 3.0 |
| electronics | 72.0 | 11.0 | 6.5 | 31.6 | 10.5 | 3.0 |
| machinery | 36.6 | 6.1 | 6.1 | 18.2 | 6.1 | 3.0 |
| medicine | 24.3 | 2.8 | 8.6 | 8.5 | 2.8 | 3.0 |
| reactor fuel | 15.0 | 3.7 | 4.1 | 11.0 | 3.7 | 3.0 |

| | unmet | last fifth | profitable | holds | money | exports | emergencies / cr | interest |
|---|---|---|---|---|---|---|---|---|
| step 18 | 24.0 ± 2.9% | 25.0 ± 4.2% | 50.2/90 | 110,200 u | −30.3 ± 16% | 1.15M | 14 / 208k | 36k |
| step 19 | 25.5 ± 2.3% | 33.1 ± 5.3% | 49.8/91 | 101,000 u | −17.7 ± 15% | 1.29M | 14 / 109k | 11k |

Year 2 of one run (start day 0) shows where it hurts: Low Earth Logistics food 33% → 82% unmet, Lunar Gateway
food and oxygen from fine to ~70%, Earth L1 water 48% → 65%; Mars metals improved (100% → 57%). Earth L1 grows
food from water carried from Lunar Gateway. That water is 1.8 u/day, worth about 150 cr/day to a carrier, so no
ship bothers, and with a third of the slack a water shortage now starves the whole Earth cluster of food.

Two findings for the user:
- **The 3x does not bring producer prices to base**, as I expected when proposing it: two-thirds of the output
  still accumulates to the producers' 90-day cap, where the price sits at 0.25x. Only stock levels fall.
- **An input's price ignores what it enables.** Earth L1 pays at most 87 cr for water that keeps food worth
  thousands of credits a day flowing. A station could bid for an input by the value of the output it limits.

Kept for now: step 21 (hold cap, ranking per hold unit) targets the neglected short hops. The 3x gets
re-checked after it.

### Decisions to review

2. **Station-specific rows are not scaled** (local resources); with them scaled, Lunar Gateway's ice would fall
   from 10 to 1.8 u/day, below what Earth L1's farms need.

## Step 20: landed-cost fuel at depots without a factory

`Simulation::compute_reference_prices` now also prices fuel at every depot without a factory, from the nearest
factories: Earth L1 9.69 and Low Earth Logistics 9.84 (from Lunar Gateway), Mars Transfer Port 26.81 (from Venus,
plasma bulk tanker at half load, 372-day round trip; up to 107 cr/u at the 4x cap). Factories keep the base
price. This reverses step 15's decision 8 (fuel excluded).

| | unmet | last fifth | profitable | holds | money | exports | emergencies / cr |
|---|---|---|---|---|---|---|---|
| step 19 | 25.5 ± 2.3% | 33.1 ± 5.3% | 49.8/91 | 101,000 u | −17.7 ± 15% | 1.29M | 14 / 109k |
| step 20 | 26.6 ± 1.5% | 37.2 ± 4.0% | 54.0/90 | 104,100 u | −21.7 ± 3.0% | 1.19M | 12.5 / 91k |

Mars fuel, 88% unmet in year 2 after step 19, drops below 5%: fleet reviews order bulk tankers from Venus and
Lunar Gateway for it. The overall numbers are within noise of step 19.

## Step 21: fleet cap by hold capacity

`max_fleet_hold_units` (100,000 u) replaces the ship count as the fleet limit; `max_fleet_size` stays as a
backstop for run time, raised to 130. A candidate whose hold would pass the cap is skipped. From half the cap on,
candidates rank by profit per day **per hold unit**.

| | unmet | last fifth | profitable | ships | holds | money | exports | emergencies / cr |
|---|---|---|---|---|---|---|---|---|
| step 20 | 26.6 ± 1.5% | 37.2 ± 4.0% | 54.0/90 | 90 | 104,100 u | −21.7 ± 3.0% | 1.19M | 12.5 / 91k |
| step 21 | 25.1 ± 2.3% | 32.8 ± 4.1% | 56.5/115 | 115 | 59,300 u | −7.4 ± 9.5% | 1.27M | 13.5 / 83k |

The fleet is now many smaller ships (115 ships, 59,000 u): ranking per hold unit favours plasma freighters and
couriers, and within two years the fleet reaches neither cap (purchases are limited by the review rate and the
hurdle). Slightly better on every measure, within noise.

It did **not** bring back the Earth shuttles: hops between Earth L1 and Low Earth Logistics stay at ~30–40 a year,
and Earth L1's water from Lunar Gateway stays 72% unmet in year 2. That route is worth about 30 cr/day (1.8 u/day
at a margin of ~17 cr), however the cap counts it. The blocker is that an input's price ignores the output it
enables (step 19), not the cap.

### Decisions to review

3. **Hold cap 100,000 u, ship backstop 130, ranking per hold unit** near the cap.
