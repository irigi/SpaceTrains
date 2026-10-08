# Steps 18–22 (2026-10-08): log and decisions to review

Plan: start stocked (18), production at 3x consumption (19), landed-cost fuel at depots without a factory (20),
fleet cap by hold capacity (21), scheduled liners (22). The user chose the options for 19–22 on 2026-10-08.
Benchmarks: `tools/benchmark.py`, four starting days, mean ± sd. Baseline (after step 16), 730 days: unmet
48.5 ± 2.9%, last fifth 29.9 ± 5.2%, 50.5/90 profitable, money −23.9 ± 8.2%, exports 0.79M, 13 emergencies.

## Summary (final): steps 18, 20 and 22 kept; 19 and 21 reverted in data

Four starts, 1460 days:

| Configuration | unmet | last fifth (~290 d) | profitable | ships | holds | money | emergencies | run |
|---|---|---|---|---|---|---|---|---|
| after step 16 (baseline, reproduced) | 33.0 ± 2.5% | 11.8 ± 2.6% | 73.5/92 | 92 | 97,600 u | −7.0% | 17 | 322 s |
| step 18 alone | 16.3 ± 2.1% | **5.3 ± 3.2%** | 78.8/94 | 94 | 114,800 u | −1.7% | 19 | 328 s |
| steps 18–22 as planned (3x, hold cap, backstop 130) | 24.1 ± 2.8% | 20.4 ± 5.5% | 108.5/138 | 138 | 65,800 u | +1.5% | 28 | 422 s |
| … with production at 5x | 23.7 ± 2.1% | 21.8 ± 4.4% | 106.8/135 | 135 | 67,700 u | −6.5% | 31 | 418 s |
| … with backstop 200 | 23.7 ± 2.7% | 18.0 ± 3.1% | 126/188 | 188 | 80,100 u | −7.2% | 28 | 522 s |
| … without step 21 (ship cap 90) | 25.2 ± 1.3% | 23.5 ± 3.1% | 79.2/97 | 97 | 115,000 u | −9.5% | 22 | 364 s |
| … without step 21 and liners | 25.4 ± 1.1% | 21.8 ± 3.6% | 75.8/95 | 95 | 107,400 u | −7.2% | 20 | 381 s |
| … without step 21 and fuel landed cost | 24.6 ± 1.9% | 22.8 ± 3.0% | 79.8/97 | 97 | 107,700 u | −3.7% | 22 | 346 s |
| **final: original production, steps 18, 20, 22, ship cap 90** | **17.4 ± 2.2%** | **7.0 ± 4.3%** | 76.8/93 | 93 (+2 liners) | 112,900 u | −21.8% | 15 | 331 s |
| final with hold cap 100k, backstop 200 | 18.8 ± 0.5% | 11.7 ± 2.7% | 124/195 | 195 | 91,800 u | −17.3% | 21 | 547 s |

- **Step 18 (start stocked) is most of the gain:** the steady state goes from 11.8% to 5.3% unmet.
- **Step 19 (production cut) caused the regression** (last fifth ~20%): any cut, 3x or 5x, removes the slack that
  covers transit stock, spoilage and imperfect dispatch, and the Earth cluster spirals (Earth L1 grows food from
  water and needs spare parts; with less slack a shortage of either cuts the food that Low Earth Logistics needs
  to make those parts). **Reverted**: recipes are back to their step 18 levels; `tools/rebalance_production.py`
  stays for later. My expectation that 3x would bring producer prices to base was wrong (see step 19).
- **Step 21 (hold cap) is switched off** (`max_fleet_hold_units` 0, `max_fleet_size` 90; the code stays and ranks
  per hold unit only while a hold cap is set). It filled the fleet with small ships, did not bring back the Earth
  shuttles it was for, and with original production it is worse (11.7% vs 7.0%) and 65% slower.
- **Steps 20 (fuel) and 22 (liners) are about neutral overall** and kept for what they fix locally: Mars gets fuel,
  the outposts get scheduled calls.

Year 4 of one final run (start day 0): Titan electronics 94% unmet, Ganymede food 66%, Earth L1 machinery 56%,
Low Earth Logistics food 44%, Earth L1 water 32%, Mars fuel 30%; everything else under 30%. Earth-pair hops still
fall from 28 to ~10 a year. 11 emergencies in four years, none open at the end.

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


## Step 22: scheduled liners

`data/economy/liners.csv` names seed ships that fly a fixed loop forever. Two Independent Consortium plasma bulk
tankers (2,000 u), home Low Earth Logistics:
- **IC Outer Loop East:** LEO → Ganymede → Titan → Earth L1;
- **IC Outer Loop West:** LEO → Titan → Ganymede → Earth L1.

How a liner behaves:
- **Next stop only:** its only allowed destination is the stop after the one it is at. The usual mixed-cargo fill
  chooses what to carry; when nothing pays, it flies the leg empty.
- **Never retired:** it is never laid up, sold or refitted, and doesn't count against the fleet cap.
- **Top-ups:** its faction tops its cash up to the 25k reserve when it runs out (`ShipLedger::subsidies`, saved;
  save version 8).
- **Shown in the game:** the ship panel shows the loop; each departure is logged as "(liner) leaves X on schedule
  for Y"; the audit has a line per liner.

In practice each outer leg takes ~600 days and a loop ~3.5 years, so each outpost gets about two calls in that
time. Over four years (start day 0) the liners made 217k and 90k cr profit, with 36k and 128k cr of faction
top-ups. Their first legs carried 500 u machinery + 150 u electronics to Ganymede and 164 u electronics to Titan.
Titan's electronics are still 94% unmet in year 4: the liners call too rarely for its use.

### Decisions to review

4. **Two liners on opposite loops** through Low Earth Logistics, Ganymede, Titan and Earth L1, owned by the
   Independent Consortium. More liners, or shorter loops, would call more often.
5. **Liners stay outside the fleet cap** and get faction top-ups instead of being laid up.
6. **Production cut reverted, hold cap switched off** (above). The user chose both; the benchmark says both hurt.
