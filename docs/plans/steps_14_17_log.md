# Steps 14–17 in one push (2026-10-08): log and decisions to review

The user asked for steps 14–17 (`production_dependencies.md`, `station_finances.md`, `science_trade.md`) to be
implemented in one push, with the open points decided in the spirit of the game and reviewed afterwards. Every
such decision is listed under **Decisions to review**. Benchmarks are `tools/benchmark.py` (four starting days,
mean ± sd). Baseline v39d at 730 days: unmet 49.8 ± 1.0%, last fifth 32.5 ± 1.6%, 50.8/91 profitable, money
+1.3 ± 7.4%.

## Step 14: production dependencies, upkeep penalties, emergency resupply

### What was built

- `recipes.csv` and `station_recipes.csv` have a `role` column (`input:<outputs>`, `upkeep`, `consume`, empty
  for outputs); the loader checks that every consumed row has a role and every input feeds an output of the same
  station.
- `data/economy/upkeep_penalties.csv`: output multiplier at a full shortage and whether the good triggers
  emergencies.
- `EconomySystem::step`: upkeep multiplier = product of `1 - (1 - m)(1 - availability)`; each output runs at the
  lowest availability of its own inputs times the upkeep multiplier. A plant that is not running uses no
  feedstock. Station panels and the audit show each output's run rate and the upkeep multiplier.
- Emergency resupply (`Simulation::step_emergencies`): opens when an emergency good's forecast stock (with the
  cargo on its way) runs out within 21 days, closes when the 21-day forecast covers 14 days of use. One emergency
  is one contract for 30 days of use, shared by the ships that take it. Its floor price is a premium times the
  reference price, starting at 3x and rising by 1.5x every 15 days nobody takes it, up to 24x. The faction pays
  the part of the contract above the station's own curve. Events on open, raise, take and close; the audit and
  the benchmark count emergencies and the premiums paid.
- Unmet demand: an input no longer runs out when its plant stops, so its stock-out would vanish from the count.
  An input's shortfall is counted where it is the input that limits an output.

### Results (730 days, four starts)

| Version | unmet | last fifth | profitable | money | emergencies / run | premiums / run |
|---|---|---|---|---|---|---|
| v39d | 49.8 ± 1.0% | 32.5 ± 1.6% | 50.8/91 | +1.3 ± 7.4% | – | – |
| 14a (plan classification) | 33.9 ± 2.5% | 25.5 ± 5.4% | 51.2/91 | −1.8 ± 6.9% | 17 | 11k |
| 14c (final) | 48.9 ± 2.1% | 36.6 ± 2.6% | 50.2/91 | −3.7 ± 3.3% | 17 | 87k |

14a looked better only because stopped plants stopped consuming. Venus produced nothing (reactor fuel, made
only at Ceres and Ganymede, was a hard input of all its outputs), Earth L1 grew 2% of its food (its water comes
from Lunar Gateway, which made 2.5 u/day against Earth L1's 5.5), Titan stopped for machinery. 14b doubled the
emergency premium up to 96x and paid 2.2M cr per run, several ships collecting the same emergency. 14c fixed
both (below).

### Decisions to review

1. **Maintenance goods are upkeep, not inputs.** Machinery, electronics and reactor fuel are spare parts and
   power: a shortage costs output gradually (multipliers machinery 0.6, electronics 0.8, reactor fuel 0.5) instead
   of stopping it. Hard inputs are feedstock only: water for farms (agri_hub), metals for factories
   (industrial_hub), electronics as machinery parts (smelter_hub). chem_hub (Venus, Titan) has no hard input: it
   processes the local atmosphere. Metals at chem_hub and frontier_hub, and electronics at frontier_hub, are
   `consume` (construction).
2. **Lunar polar ice:** Lunar Gateway gets 20 u/day of water per 10,000 people (10 u/day at 5,000) in
   `station_recipes.csv`, so Earth L1's farms have a local water supply.
3. **Emergency goods:** oxygen and water only, and only where they are upkeep (Earth L1's farm water is an input,
   so no emergency there).
4. **Emergency premium:** starts at 3x, ×1.5 every 15 untaken days, at most 24x, for one contract of 30 days of
   use. A higher cap paid too much while ships were months away.
5. **Requisition below a station's reserve** was left out (production is 8–10x consumption).
6. **Penalties multiply**, so a station short of several upkeep goods can fall to under 1% (Mercury with no
   oxygen, water and food: 0.1 × 0.1 × 0.5). It never stops entirely, as agreed.

## Step 15: landed-cost reference prices

### What was built

- `spacetrains_headless --write-reference-prices data/economy/reference_prices.csv` plans, for every consumer
  and every good it consumes (fuel and export markets excepted), the round trip from each of its two nearest
  producers (Hohmann estimate) in every standard-tank hull of at least 150 u: a full hold out (or a half or a
  quarter where a full one is too heavy, as to Mercury), back empty, at 6 departure dates over a year. A hull that
  cannot fly the route at most dates is skipped; the median trip sets its cost. Cost per unit = (capital + crew +
  life support at base prices) x round-trip days + propellant at base price, divided by the units carried. The
  reference price is base + `carrier_margin` (1.2) x the cheapest. The table (40 rows, 0.6 s to compute) is data, so
  the loader reads it and it can be reviewed and edited. Producers and goods not listed keep the base price.
- Each consumer's price curve is centred on its reference price; `price_cap` (pricing.csv) went from 16 to 4.
- The Hohmann estimate moved into `EconomySystem::transfer_days` (shared with `cover_days`).

Examples: Titan machinery 525 cr (base 300), Ganymede food 76 (50), Mercury food 334 (Venus by NTR freighter at
half load), Mercury water 56 (20, from Mars), Earth L1 water 21.7 (20, from Lunar Gateway).

### Results (730 days, four starts)

| Version | unmet | last fifth | profitable | ships | holds | money | emergencies / premiums |
|---|---|---|---|---|---|---|---|
| 14c | 48.9 ± 2.1% | 36.6 ± 2.6% | 50.2/91 | 91 | 92,700 u | −3.7 ± 3.3% | 17 / 87k |
| 15 | 56.3 ± 1.2% | 51.7 ± 3.8% | 30.0/54 | 54 | 33,500 u | +6.6 ± 4.9% | 18 / 266k |

The fleet stops growing at about 54 ships. Fleet reviews still find ships worth 128–533%/yr, but the treasuries
cannot pay for them (day 365: Sol Federation 41k, Mars Corporation 25k, Independent Consortium −85k). Their income
was taxes on stations, whose cash came mostly from residents paying scarcity prices to the outside account. The 4x
cap removed most of that money. Step 17 lets factions borrow against their fleets, so the push does step 17 next
and step 16 after it.

### Decisions to review

7. **Reference prices are precomputed data**, not computed at every start (thousands of plans per start would slow
   every test and game start; 0.6 s offline). Rerun the command after changing stations, recipes or ship classes.
8. **Fuel keeps its depot pricing** (v29) and gets no landed cost, since fuel prices drive every ship's costs.
9. **Price cap 4x the reference** (from 16x base); carrier margin 1.2 on the cheapest trip.
10. **The reference trip is a full hold out and back empty in the cheapest hull**, two nearest producers, median
    over 6 dates. Carriage is cheap per unit in big holds: most outposts' references are only 10–40% above base;
    Mercury's are high because full holds cannot reach it.
