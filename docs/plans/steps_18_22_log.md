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
