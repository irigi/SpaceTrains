# Station economies: producers, budgets, demand and wages (plan, 2026-10-22)

## Why

This covers points 7-10 of the 2026-10-22 economy review (scores 1, 2, 1, 1 of 5). They are solved here inside the
station model, without the background economies of `docs/plans/background_economies.md`. The two plans fit
together but neither needs the other.

| Point | Today |
|---|---|
| 7 Producers earn from sales | local production is free ("producers were paid 0"); the station and its residents are one account |
| 8 Every buyer spends what it earns | residents pay for imports with money from the outside account, without limit; factions top stations up |
| 9 Demand depends on income and price | consumption is a fixed rate per person; price changes who supplies, not how much is used |
| 10 Wages reflect scarcity | fixed grade wages, the same everywhere; growth follows a supply index |

## Model

A station gets three kinds of actor, each with its own account:
- its **industries** (the rows of its recipes);
- its **households** (the residents);
- the **station** (port, depot, government).

### Industries (point 7)

- Each output row of a station's recipes is an industry, with:
  - a capacity (today's `units_per_day`);
  - labour per unit, by occupation;
  - its inputs (today's `input:` rows).
- **Sales.** It sells its output into the station's market at the station's price: to ships, and to the station's
  households.
- **Costs.** It pays wages to the households, and pays for its inputs at the station's price.
- **Utilization follows the margin.** Utilization u is the share of capacity used:
  - it runs at the share of capacity whose unit cost (labour + inputs + upkeep share) is below the price;
  - unit cost rises with u, a convex supply curve with elasticity σ (data);
  - this replaces "zero while storage is 85% full" by a price signal: a glut lowers the price, and a lower price
    lowers output.
- **Profit** goes to the owning faction. Today's dividends from ships stay as they are.
- **Capacity responds slowly to sustained profit.** A slow investment step (also point 5):
  - capacity grows by a share of retained profit ÷ capital cost per unit of capacity;
  - it shrinks by depreciation when idle;
  - like fleet investment, but over years.

### Households (points 8 and 9)

**Income:**
- wages from the industries and the station;
- interest on savings;
- explicit transfers only: faction welfare, as today's floor top-up but booked as a transfer, and remittances from
  Earth if the other plan is built.

**Spending: a linear expenditure system (Stone-Geary).** Each good i has a subsistence quantity γᵢ per person and a
budget share βᵢ of what is left:

```
xᵢ = γᵢ + (βᵢ / pᵢ) × (income − Σⱼ pⱼ γⱼ)
```

- Upkeep goods (oxygen, water, food) are mostly γ, so their use barely responds to price.
- Medicine, electronics and the `consume` goods are mostly β, so their use rises with income and falls with price.
- The budget is exact by construction (spending = income − saving).
- Today's per-person rates become γ + β at the seed prices and income, so the seed state is the same equilibrium.
- **Saving:** a fixed rate, and savings are drawn down when income falls below subsistence cost. Savings that run
  out mean the station cannot pay for subsistence, which counts as a shortage (unmet demand) with real effects
  (point 11).

### Labour market (point 10)

- **Labour demand** = Σ industries' u × capacity × labour per unit + the station's own staff.
- **Labour supply** = population × participation, by occupation (data).
- **Wages adjust to vacancies.** Each occupation's wage at each station follows its vacancy rate:

  ```
  d ln w = k × (vacancies − unemployed) / labour force
  ```

  so a booming station pays more and an idle one less.
- **Distributions.** Wages within an occupation are lognormal around that median (σ data), as in the
  background-economies plan.
- **Migration** follows real wage differences: the wage ÷ the station's subsistence price index. It is computed as
  there, with fares from the traveller's gain:
  - from Earth: a fixed Earth wage distribution per occupation (a stand-in until background economies exist);
  - between stations.
- **This replaces step 27's growth thresholds** (95% / 70% supply index) for stations reached by ship: people come
  for jobs and leave when wages fall or subsistence fails.
- **Rotation** becomes a choice: a station hires rotating staff where permanent hires do not come at its wage.

### Money

- The station's three accounts replace today's single one; the faction treasury is unchanged.
- **The outside account no longer pays residents.** That money was the fleet's main revenue: households spent
  27M cr from it in 4 years, against 33M cr of cargo margin.
- **Imports are paid by exports.** Consumer stations must earn by:
  - selling their industries' output;
  - their factions' transfers;
  - Earth trade, at the export markets as today or at background economies' bids.
- **Step 0 measures each station's trade balance** under today's flows. A station that only imports cannot survive a
  closed budget without a transfer, and that transfer becomes an explicit, visible subsidy (faction or Earth), not
  hidden outside money.
- The money controller (`money_supply_days`) is retuned or removed: money now enters only through the external
  trades.

## Data

- **Recipes:**
  - `recipes.csv` / `station_recipes.csv` gain `labour_per_unit` by occupation;
  - `industries.csv` gets the supply elasticity, capital cost per unit capacity and depreciation.
- `households.csv`: γ and β per good (derived from today's rates), saving rate, participation by occupation.
- `labour.csv`: grades become occupation medians and σ; wage adjustment speed k.
- Switch `station_economies,0|1` (off ⇒ byte-identical audit), with sub-switches per step below.

## Build order (benchmark after each step from 2 on)

0. **Measure first:**
   - each station's trade balance and household spend under today's model;
   - what share of fleet revenue comes from household imports paid by outside money.

   This shows the size of the subsidy that point 8 removes, and whether consumer stations need transfers.
   The user decides the transfer policy before step 3.
1. **Data and types,** labour per unit derived so the seed state's employment is participation × population;
   switch off byte-identical; save version +1.
2. **Industries as accounts (7):** sales and wages, utilization by margin; households still paid from outside,
   so only the split of money changes. Check that output stays near today's (production 8-10× consumption), and
   that gluts now cut output.
3. **Household budgets (8):** wages and transfers as the only income; the controller retuned. The big step: watch
   fleet revenue, unmet demand and money against target.
4. **Linear expenditure (9):** prices and income move consumption.
5. **Wages and migration (10):** vacancy-driven wages, lognormal distributions, job migration with fares from the
   gain; step 27 thresholds off for stations reached by ship.
6. **Capacity investment (5):** slow growth and decline of industries.
7. **Audit and UI:** per station industry margins and utilization, household income and spending, wages and
   vacancies by occupation, migration; inspector panel; docs and memory.

## Verification

- **Unit tests:**
  - the linear expenditure system sums to income, and gives the seed rates at seed prices;
  - utilization against the closed-form supply curve;
  - wage adjustment converges on a toy station;
  - every money move exact;
  - the switch-off identity.
- **Every step:** ctest, `--trajectory-audit` 0 flags, drift 0, save/load identity, `tools/benchmark.py` (4 starts),
  `tools/purchase_returns.py`.
- **Plausibility checks (not inputs):**
  - a glut cuts its industry's output within months;
  - a station that loses its export market shrinks;
  - wages rise where industries expand;
  - consumption of `consume` goods falls when their price doubles, while oxygen's barely moves.

## Risks

- **Fleet revenue.** Today's revenue rests on households' outside money. With budgets closed, the fleet may shrink
  until exports and transfers pay for it. That is the realistic result, but it may need explicit Earth trade
  (background economies, or the Earth surface market on the roadmap) to keep the game busy.
- **Oscillation.** Wages, utilization and prices all adjust. Each needs a relaxation time well apart from the others
  (prices days, utilization weeks, wages months, capacity years).
- **Speed:** all of it is per station and per day, small next to planning.
