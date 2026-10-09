# Steps 14–17 in one push (2026-10-08): log and decisions to review

The user asked for steps 14–17 (`production_dependencies.md`, `station_finances.md`, `science_trade.md`) to be
implemented in one push, with the open points decided in the spirit of the game and reviewed afterwards. Every
such decision is listed under **Decisions to review**. Benchmarks are `tools/benchmark.py` (four starting days,
mean ± sd). Baseline v39d at 730 days: unmet 49.8 ± 1.0%, last fifth 32.5 ± 1.6%, 50.8/91 profitable, money
+1.3 ± 7.4%.

## Summary (final, four starts; steps done in the order 14, 15, 17, 16)

| | v39d | after steps 14–17 |
|---|---|---|
| 730 d unmet | 49.8 ± 1.0% | 48.5 ± 2.9% |
| 730 d last fifth | 32.5 ± 1.6% | 29.9 ± 5.2% |
| 1460 d unmet | 32.3 ± 1.2% | 33.0 ± 2.5% |
| 1460 d last fifth (~290 days) | 10.5 ± 1.9% | 11.8 ± 2.6% |
| 1460 d profitable ships | 84.5/93 | 73.5/92 |
| 1460 d money vs target | +10.5 ± 12% | −7.0 ± 7.6% |
| 1460 d share of money in ships | ~75% (3.1M of 4.1M) | 26 ± 7% |
| 1460 d exports | 1.84M | 2.07M (science included) |
| 1460 d emergencies / premiums | – | 17 ± 4 / 255k cr per run |
| 1460 d factions over their credit limit | – | 0.5 per run (Mars Corporation) |
| trajectory flags, money drift | 0, 0 | 0, 0 |

Against the plan's acceptance checks: drift 0 (met); emergencies rare, about 4 a year at 1460 days (met); ships'
share of the money much lower (met); faction debts within limits (Mars Corporation over its limit in two of four
runs, and it stops buying ships then); no emergency open for more than 120 days (**not met in the first two years**:
the remote stations' first emergencies stayed open 450–660 days, since "enough coming" needs a freighter months
into its trip; in years 3–4 the longest was 119 days); outpost unmet demand lower than v39d (**mixed**: one
four-year run against the v39d report: Ganymede 45–73% (50–77%), Mars food 45% / fuel 67% / metals 51% (52–60%),
Ceres 36–51% (30–51%), Titan 51–95% (60–90%)).

Unmet demand overall is about where it was, but it now measures something more physical (upkeep shortages
count in full, stopped plants no longer hide their inputs' shortage), and the money no longer piles up in ships.
Not built from the plans: Earth stations pinned to the outside account (decision 12), the station budget gate
switched on (13), a test for it, and the money-supply controller at zero (14). The UI shows the new state in the
station panel (production limits, upkeep shortages, emergencies, landed costs) and the economy overview (faction
debt, emergencies), but nobody has looked at it in the game yet.

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

## Step 17: station money (done before step 16)

### What was built

- **Station and residents are one account.** Local production costs the station nothing. Residents pay for
  what they use up, with income from outside like wages, at **what their station paid ships for it**
  (`StationState::import_unit_cost`: deliveries averaged over about 30 days of use; the reference price until the
  first delivery). Earth's export markets work as before (the outside pays for exports at their flat price).
- **Faction credit.** A faction may borrow (negative treasury) up to 50% of its fleet's hull value plus two years
  of its tax income (365-day average), and pays 5%/yr interest to the outside on its debt. Fleet investment buys
  with treasury plus credit; a faction over its limit buys no ships. The audit shows each faction's limit, the
  interest paid and how many factions are over their limit.
- **Core-crew subsidy.** A station below the 25k floor is topped up by at most the upkeep of 30% of its
  population per day (at reference prices). The 250k ceiling tax stays.
- **Station budget gate** (`affordability_days`), built and then switched off (see decision 13): a station's cash
  plus a 90-day credit line, less the contracts on their way (in full when due within 90 days, in proportion
  90/days when later), scales what it offers ships.
- Money-supply controller kept at 60 days (decision 14).

### Results (730 days, four starts)

| Variant | unmet | last fifth | profitable | holds | money | ship cash | interest |
|---|---|---|---|---|---|---|---|
| v39d | 49.8 ± 1.0% | 32.5 ± 1.6% | 50.8/91 | 99,000 u | +1.3 ± 7.4% | ~75% | – |
| 15 alone | 56.3 ± 1.2% | 51.7 ± 3.8% | 30/54 | 33,500 u | +6.6% | – | – |
| 17a: residents at reference, gate counts all contracts, no controller | 65.5 ± 1.7% | 58.0 ± 6.1% | 33/89 | 69,000 u | −16.8% | 40% | 281k |
| 17b: gate counts contracts due within 90 days | 50.5 ± 4.3% | 39.0 ± 2.7% | 42/90 | 83,300 u | −79.1% | – | 291k |
| 17c: + controller 180 days | 49.4 ± 3.0% | 33.3 ± 2.3% | 42/90 | 85,200 u | −54.9% | 34% | 322k |
| 17d: gate weights later contracts 90/days | 54.8 ± 2.7% | 45.2 ± 4.6% | 38.5/90 | 76,900 u | −30.1% | 38% | 308k |
| 17e: residents pay the import cost | 54.4 ± 2.7% | 43.3 ± 4.3% | 38.5/90 | 77,800 u | −15.1% | 27% | 294k |
| 17g: + local production free, gate off | 47.6 ± 2.3% | 30.0 ± 8.2% | 50/90 | 98,400 u | −47.6% | 59% | 121k |
| **17h: + controller 60 days (final)** | **47.6 ± 2.3%** | **30.0 ± 8.2%** | **50/90** | **98,400 u** | **−32.2 ± 10%** | **45%** | **127k** |

- 17a: Low Earth Logistics could not pay for food from Earth L1 (0.1 days away): freighters from Ganymede were
  booked to arrive in years and their contracts used up its budget. 1 Earth-pair hop in 400 days (v39d: 25).
- 17b: Mars, 200 days from its suppliers, ordered 1.3M cr of goods it could not pay for (−498k cash).
- 17d/17e: the gate swung stations between 0 and 100%: Venus with 212k cash offered nothing because reactor-fuel
  freighters from Ceres were contracted at the 4x cap; Mercury, Mars, Ceres and Ganymede at 0% for months.
- 17e: until then the station still paid the outside for its own new output, and a producer selling its glut to
  ships for less went broke.

### Decisions to review

11. **Residents pay the station's import cost**, not the reference price (the plan) and not the scarcity curve
    (before). With the reference price every chronically short station lost money on each import; with the
    import cost a station breaks even on average, and new money matches what ships were actually paid (bounded
    by the 4x cap), not a hypothetical 16x price.
12. **Earth stations are not pinned to the outside account** (the plan's "outside-backed" flag was not built).
    Earth's stations sell the colonies far more (food, electronics, machinery) than they buy back, so a money
    supply fed only by trade with Earth would have drained in months; residents' wages are the money source.
13. **The station budget gate is off** (`affordability_days` 0; the code stays). Every version of it starved
    stations (unmet +5 to +18 points) while the ships' contracts kept paying; station cash is a measure again,
    as before step 17. Revisit with a smoother gate (for example, limit only non-upkeep goods) if wanted.
14. **The money-supply controller stays at 60 days**, as the plan's fallback allowed (money −79% without it). Money
    still ends at −32% of its target, because the target includes the 25k working capital of every new ship
    (1.7M by day 730), which the factions now borrow.
15. **Faction credit:** 50% of fleet hull value + 2 years of taxes, interest 5%/yr. Mars Corporation (one station,
    a small fleet) ends over its limit in every run and stops buying ships: the debt-and-austerity story.
16. **Core-crew subsidy:** at most 30% of the population's upkeep per day.

## Step 16: science trade

### What was built

- `science_samples`: 10 kg/u, base 2000 cr (200 cr/kg), no decay. Made at Titan, Ganymede, Ceres and Mercury
  (station recipes, with 0.1 u of electronics per unit as a material input), used up at Lunar Gateway and Mars
  Transfer Port (0.1 u/day per 10,000 people, role `consume`), bought by Earth L1 and Low Earth Logistics as an
  export market (flat base price).
- An export good made **or used** at a station is priced on its curve there (before, only goods made there
  were; Lunar Gateway and Mars would have paid 0.25x).
- Reference prices regenerated (42 rows; science at Lunar Gateway 2008, Mars 2013).

Calibration: each producer's rate makes its science worth about half its import bill at reference prices:

| Station | import bill (cr/day) | science (u/day) | per 10,000 people |
|---|---|---|---|
| Titan (7,000) | ~721 | ~0.18 | 0.26 |
| Ganymede (6,000) | ~880 | ~0.22 | 0.37 |
| Ceres (9,000) | ~1,218 | ~0.30 | 0.34 |
| Mercury (8,000) | ~2,102 | ~0.53 | 0.66 |

### Results

730 days, four starts: unmet 48.5 ± 2.9%, last fifth 29.9 ± 5.2%, 50.5/90 profitable, holds 94,600 u, money
−23.9 ± 8.2%, exports 0.79M, 13 emergencies and 211k cr of premiums per run, drift 0 (step 17 alone: 47.6 / 30.0).
In a four-year run (start day 0) Earth bought 122 u of science for 244k cr, mostly brought by plasma couriers
from Mercury; Lunar Gateway and Mars bought more along their curves.

### Decisions to review

17. **Science producers and rates** as above; Venus makes no science.
18. **Earth's science price is flat** (export market, like platinum); Lunar Gateway and Mars pay their curve.
19. **Science value does not decay with age** (left for later, as the plan says).
