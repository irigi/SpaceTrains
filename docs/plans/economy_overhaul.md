# Economy overhaul (plan, 2026-10-22 night)

This is the working plan for the overnight session. It makes `background_economies.md` and
`economy_points_7_10.md` precise with the numbers agreed on 2026-10-22. Branch `feature/economy_overhaul`.
Decisions and results: `economy_overhaul_log.md`.

## Goals (the user, 2026-10-22)

1. An economy much closer to the realistic state. Ships keep flying, and some passengers keep travelling (fewer
   travellers than today is fine).
2. Anything can be bought at the Earth stations for its surface price plus launch.
3. Ships and stations pay their people. The background economies plus the station and faction economy conserve
   money exactly.
4. Stations earn from the chosen sources. Some are subsidized. No station ends bankrupt.
5. Wages are distributions, not averages.
6. The old checks still hold: unmet demand stays low, drift 0, save/load identity, trajectory audit, 5 days/s.

## Prices

**Credit scale:** kept at 0.00078 cr/$ (ships are priced from hardware dollars).

**Launch and landing**, per kg (credits in brackets):

| Body | Up | Down |
|---|---|---|
| Earth | $100 (0.078 cr) | $50 (0.039 cr) |
| Mars | $40 (0.0312 cr) | $20 (0.0156 cr) |
| Luna | $20 (0.0156 cr) | $10 (0.0078 cr) |
| Ceres | $5 (0.0039 cr) | $5 (0.0039 cr) |
| Venus clouds | $400 (0.312 cr) | $20 (0.0156 cr) |

**Earth surface prices** ($/kg, at scale for space buyers):

| Good | Price | Note |
|---|---|---|
| food | 8 | shelf-stable |
| water | 0.5 | |
| oxygen | 0.3 | |
| metals | 8 | aerospace alloy stock |
| fuel | 1.5 | plasma propellant is water; nuclear-thermal propellant is hydrogen |
| electronics | 4,000 | |
| machinery | 200 | |
| medicine | 3,000 | |
| reactor fuel | 1,000 | |
| platinum | 32,000 | |
| deuterium | 5,000 | |
| science samples | 100,000 | bought only |
| helium-3 | 3,000,000 | bought only; Luna |
| microgravity products | 300,000 | bought only; made at Earth orbit and Earth L1 |

**At an Earth gateway** (`earth_orbit`, `earth_l1`):
- **ask** = surface + launch, sold in any amount;
- **bid** = surface − landing, for the space-sourced goods Earth buys: platinum, deuterium, samples, helium-3,
  microgravity products, and propellant for Earth's own traffic at low Earth orbit (0.9 × ask, limited volume).

Ordinary goods (food, electronics and the rest) are not bought by Earth: it makes them itself, and buying them
would print money from free station production.

**Base prices** become the Earth ask, or the bid for goods Earth only buys. Reference prices and fares are
regenerated with the existing tools.

| Good | Old base (cr) | New base (cr) |
|---|---|---|
| food | 50 | 8.4 |
| water | 20 | 7.8 |
| oxygen | 30 | 3.9 |
| metals | 75 | 16.8 |
| fuel | 8 | 7.9 |
| electronics | 200 | 64 |
| machinery | 300 | 70 |
| medicine | 250 | 121 |
| reactor fuel | 400 | 429 |
| platinum | 3,000 | 249 |
| deuterium | 1,500 | 77 |
| samples | 2,000 | 780 |
| helium-3 (1 kg) | — | 2,340 |
| microgravity products (1 kg) | — | 234 |

## Accounts and conservation

- **Earth** is the old outside account, now named and recorded. It pays and receives through explicit
  flows only.
- **Mars surface, Luna surface and the belt** have their own accounts and finite trade.
- **Station:** government and industries, as today.
- **Households** per station: the residents' savings, a new account.
- **Ships and factions:** as today.
- **The identity:** the sum of all accounts stays the seed total (drift 0).

### Flows

- **Wages:** the station pays its residents; households spend their income back at the station. Imports are
  charged at import cost, local services are the rest, and savings relax toward a buffer.
- **Earth gateways:** the station buys from Earth at the ask to keep its stock topped up, and sells
  space-sourced goods to Earth at the bid.
- **Small background economies:** extra consumption and production lines at their gateway station, paid to and
  from their account, with rates that respond to price.
- **Station incomes** (`data/economy/station_incomes.csv`), paid by Earth:
  - science budgets: Titan, Ganymede, Mercury, and the Venus cloud base;
  - relay and traffic services: Mercury, Ganymede, Earth L1;
  - settlement subsidies: Mars, Ceres, Luna.
- **Science stations** pay high prices within their budget.
- **Tourism:** visitors spend at the station during their stay, from their home account.
- **Migrants** bring savings into the households of their new station.
- **Ship purchases:** the yard station (Mercury Yard or Low Earth Logistics) receives the yard's share of the
  price; the part made on Earth goes to Earth.
- **Interest and faction credit:** as today (Earth investment).

## Labour (phase B)

- **Occupations** (unskilled, technician, engineer, specialist) with lognormal wages in each place: the stations
  and the Earth, Mars, Luna and belt economies.
- **Migration** per occupation, by Gauss-Hermite quadrature over the origin's distribution. What a traveller will
  pay is their own gain, and the gain quantiles become the passenger bands.
- **Visitors** come from the top of Earth's income distribution.
- **Emigrants** pay from their savings.
- **Rotation** is costed against permanent hire.

## Industries and households (phase C, if time)

- Industries: utilization follows the margin, labour per unit of output, capacity follows retained profit.
- Households: spending by a linear expenditure system (survival goods first, the rest by income and price).

## Venus

- Renamed **Venus Orbital Exchange**.
- Its fuel factory scoops the upper atmosphere for propellant; its biotech and chemistry are kept.
- A small cloud science base below it, with its budget and its supplies.
- No tourism.

## Checks

- Every step: ctest, `--trajectory-audit`, drift 0, `tools/benchmark.py` (4 starts × 1,460 days).
- New audit lines: account balances, per-station money flows (wages, Earth trade, incomes), stations below their
  floor.
- **Targets:**
  - unmet demand within about 2× of today (1.1%);
  - no station ends below its credit floor without its faction able to cover it;
  - ships fly (profitable share above 60%);
  - passengers still travel;
  - 5 days/s.
