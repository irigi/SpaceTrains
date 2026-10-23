# Economy overhaul (design as built, 2026-10-23)

Branch `feature/economy_overhaul`. Decisions with their reasons and every benchmark: `economy_overhaul_log.md`.
Mechanics in detail: `docs/modules/economy.md`, "Economy overhaul". Earlier plans it grew from:
`background_economies.md`, `economy_points_7_10.md`.

## Goals (the user, 2026-10-22)

1. An economy much closer to the realistic state. Ships keep flying, and some passengers keep travelling (fewer
   travellers is fine).
2. Anything can be bought at the Earth stations for its surface price plus launch.
3. Ships and stations pay their people. The background economies plus the station and faction economy conserve
   money exactly.
4. Stations earn from chosen sources. Some are subsidized. No station ends bankrupt.
5. Wages are distributions, not averages.
6. The old checks hold: unmet demand low, drift 0, save/load identity, trajectory audit, 5 days/s.

## Prices

**Credit scale:** 0.00078 cr/$, as for ships.

**Launch and landing**, per kg:

| Body | Up | Down |
|---|---|---|
| Earth | $100 | $50 |
| Mars | $40 | $20 |
| Luna | $20 | $10 |
| Ceres | $5 | $5 |

**Base prices** are Earth's ask: surface price + launch. Goods Earth only buys are based at its bid
(surface − landing).

| Good | Unit | Base (cr) | Earth surface $/kg |
|---|---|---|---|
| food | 100 kg | 8.42 | 8 |
| water | 100 kg | 7.84 | 0.5 |
| oxygen | 50 kg | 3.91 | 0.3 |
| metals | 200 kg | 16.85 | 8 |
| fuel | 100 kg | 7.92 | 1.5 |
| electronics | 20 kg | 63.96 | 4,000 |
| machinery | 300 kg | 70.2 | 200 |
| medicine | 50 kg | 120.9 | 3,000 |
| reactor fuel | 500 kg | 429 | 1,000 |
| platinum | 10 kg | 249 | 32,000 (bought only) |
| deuterium | 20 kg | 77 | 5,000 (bought only) |
| science samples | 10 kg | 780 | 100,000 (bought only) |
| helium-3 | 1 kg | 2,340 | 3,000,000 (bought only; Luna) |
| microgravity products | 1 kg | 234 | 300,000 (bought only; Earth orbit, Earth L1) |

**Local propellant:** fuel factories sell at their own cost: Luna and Ceres 2.5, Venus, Titan and Ganymede 3.0,
Mercury 3.5 cr per 100 kg.

**Reference prices** (curve centres) are the cheapest producer's price + 1.5 × carriage. Science stations
centre theirs 50% higher.

## Accounts

The money identity: Earth + colonies + stations + households + ships + faction treasuries = the seed total.
Drift 0.

- **Earth:** the old outside account. It sells everything at its gateways (stocked 1,500 units of each good)
  and buys space goods.
  - It buys low-orbit propellant for its own traffic: 0.9 × ask, 100 u/day, at Low Earth Logistics.
  - It pays the station incomes and invests in the colonies.
  - Visitors from Earth pay their fares from it.
- **Colonies** (Mars surface, Luna surface, belt miners):
  - accounts with a credit line;
  - limited daily trade at their gateway station;
  - people with a lognormal wage, hired up to the gateway's jobs;
  - recruiting part of their growth on Earth.
- **Households** (residents) of every station:
  - paid wages by the station, and by their ships for crews;
  - pay for the goods they use and spend the rest back at the station;
  - keep a 90-day buffer and a 3% travel budget.
- **Stations:** as before (government and industries in one account), plus wages, incomes and yard sales.
- **Factions:**
  - tax above 750k;
  - top up below 25k, and cover a station in the red within 2 days;
  - borrow from Earth at 5%/yr.

  The money-supply controller is off.

## Labour

- **Jobs and wages:** every station off Earth has jobs and a wage per worker.
  - Wages follow vacancies.
  - Jobs grow 3%/yr while the station is supplied, solvent and its open jobs are under 5%. Science
    stations keep their staffing. Jobs are cut 5%/yr when the station is short of supply or deep in the red.
  - Output follows employed workers ÷ workers; consumption follows residents.
- **Who moves**, each paying from their own distribution:
  - **settlers from Earth:** for vacancies, paying out of the gain (station wage × 0.8 − their lognormal Earth
    wage) × a 10-year stay;
  - **the unemployed going home:** on an Earth wage;
  - **visitors:** when half a year's income covers the trip;
  - **colonists:** up to the gateway's jobs, and the gateway's unemployed down to the surface, by the same rule
    with the ascent as the fare.

## Incomes (`station_incomes.csv`)

| Station | Income (cr/yr) | Kind |
|---|---|---|
| Titan | 800k | science station, need-based |
| Ganymede | 400k + 50k | science station, need-based; relay |
| Mercury | 100k + 50k | science, need-based; relay |
| Venus (cloud base) | 100k | science, need-based |
| Mars | 700k | settlement, need-based |
| Ceres | 300k | settlement, need-based |
| Earth L1 | 30k | relay |
| Luna | 60k | tourism and survey |

- Need-based grants taper from full at half the credit ceiling to nothing at the ceiling.
- **Also:**
  - yards in space take half a new hull's price (and build it from 20% of the price in goods);
  - microgravity products and helium-3 sold to Earth;
  - low-orbit propellant;
  - visitors' spending;
  - migrants' savings.

## Not built, or off

- Industries as accounts of their own (point 7 in full).
- Supply following price: built, off.
- Per-occupation wages, surface price relaxation, station-to-station migration.

## Checks

Every change was benchmarked with `tools/benchmark.py` (4 starts × 1,460 days). The benchmark now also reports
stations below the floor, in the red, the worst station's red days, boardings and fares. Speed with `--profile`,
save/load by full-JSON comparison, ThreadSanitizer for 800 days. Results: `economy_overhaul_log.md`.
