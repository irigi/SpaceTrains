# Landed-cost prices and station money (steps 15 and 17, done 2026-10-08; what changed from this plan is in `steps_14_17_log.md`)

Two open problems from the v39d four-year runs: money piles up in ships (3.1M cr in ships, 1.0M in stations by
year 4; windfalls of up to 16x base, such as 693k cr for 185 u of reactor fuel), and faction treasuries split apart
(Mars Corporation -5.3M). The outposts (Titan, Ganymede) stay short of goods. All three come from how prices and
money are set, not from the ships' planning.

## Division of work (unchanged)

Stations post prices and ships do the planning. This is already how step 13 works: a station's price curve,
read from the forecast stock at a ship's arrival (with the cargo other ships deliver first), *is* its order. Each
further unit is worth less, and a ship fills its hold until the next unit no longer pays. The contract agreed at
departure fixes the price. The changes below move where that curve sits and who pays for what. Ships keep all the
planning.

## Step 15: landed-cost reference prices

### Problem

A ship delivers more units while the marginal sale price at the destination exceeds the marginal purchase price
at the origin plus the transport cost per unit. In steady state the destination's stock settles where

    price_dest(stock) = price_origin + transport cost per unit

Today every station's curve is centred on one global `base_price` per good. Where the transport cost is small
(Earth L1 to Low Earth Logistics) the stock settles near the target. Where it is large (Titan, Ganymede), the
stock settles well below the target, because only scarcity pays for the trip. The outposts are therefore built to
be short of goods, and the 16x cap exists mainly to make far trips pay at all. The windfalls are a side effect:
a curve steep enough to pay for Titan also pays 9x for a short hop to a starving Earth station.

### Change

- Each consumer station gets a **reference price** per consumed good:
  `reference = base_price + carrier_margin x transport_cost_per_unit(nearest producer -> station)`.
  The transport cost is computed once at start, like `cover_days_`, from a reference cargo class on the same
  Hohmann estimate. It covers fuel at the producer's port price, capital, wages and provisions over the round
  trip, divided by the hold. The carrier margin starts at 1.2. Producers keep `base_price`.
- The curve becomes `reference x clamp((target / stock)^1.3, 0.25, cap)`, with `cap` lowered from 16 to
  about 4. The steady state then sits near the target everywhere, so scarcity is no longer what pays for distance.
- Local producers stay paid at most their base price, not the reference.
- UI: the station panel shows the reference price next to the current price.

### Expected effect and checks

- The outposts' unmet demand falls (Titan and Ganymede supply becomes normal business).
- Windfalls shrink: the largest single delivery value and the money held in ships both drop.
- Benchmark at 730 and 1460 days against the step 14 result. Watch whether the lower cap slows recovery from
  real shortages; emergency resupply (step 14) is the safety net for life support.

## Step 17: where station money comes from

### Today (`docs/plans/open_economy.md`)

The **outside economy** is an unlimited counterparty inside every station. Residents buy every consumed unit
from it along the station's price curve, and local producers sell every produced unit to it at up to base price.
A starving station's residents pay scarcity prices with money that comes from nowhere: that is the main source
of new money, and why the money supply drifts upward (+10.5 +- 12% at four years). Faction treasuries keep each
station's cash in a band (subsidies below 25k, taxes above 250k) and a controller corrects the total money supply.
Station cash gates no decision.

### Model

A station and its residents are one account, the station. They eat and build from their own stock, so local
consumption and local production are no payments. Money moves only between these accounts, each time for a
reason:

| Flow | From | To |
|---|---|---|
| Imports (contract price) | station | ship |
| Sales of local output | ship | station |
| Exports and science sold to Earth | outside (Earth's economy) | station |
| Earth stations' imports and sales | outside | Earth station (Earth's economy backs them) |
| Ship hulls, refits, salvage | faction | outside |
| Wages, capital, dividends | ship | home station (as now) |
| Taxes on surplus | station | faction |
| Core-crew subsidy and emergency contracts | faction | station or ship |
| Loans and interest | outside | faction, and back |

- **Earth is the outside world.** Earth L1 and Low Earth Logistics trade with a planet of billions behind them,
  so they settle any surplus or deficit with the outside account. The money in the simulated economy then
  changes only through the balance of trade with Earth (exports, science and Earth's purchases in, hulls and
  Earth's sales out) and faction loans.
- **Outposts pay their way with exports and science (step 16).** A station that imports food and sells nothing
  runs out of cash, which is the correct signal.
- **Station cash limits its orders.** A station's committed contracts may not exceed its cash plus a credit line
  from its faction. Beyond that, its curve is scaled down so the next unit is worth what it can pay. A broke
  station gets less, and falls into upkeep penalties and, for life support, emergency resupply paid by the
  faction.
- **Core-crew subsidy.** Each faction guarantees the upkeep of a core crew at every station it owns (a share of
  the population, start at 30%): it pays the station's import cost of the core crew's food, water, oxygen and
  medicine. No station goes bankrupt to zero. (This is the floor the user wants for later station growth.)
- **Faction budgets.** Income: taxes and the export and science revenue share of its stations. Spending:
  core-crew subsidies, emergency contracts, new ships. A faction may borrow from the outside at interest up to a
  limit (start at two years of income). Over the limit it buys no ships and cuts subsidies above the core crew.
  The Mars Corporation's deficit becomes a visible story: debt, austerity, then recovery through exports.
- **Money-supply controller:** kept only as a measure at first, gain set to zero. Turn it back on only if the
  benchmark shows a drift that no flow explains.

### Checks

- Audit: stations + ships + factions + outside = seeded money, drift 0.
- The money supply follows the trade balance with Earth. The benchmark reports exports, Earth imports, hull
  purchases and faction debt per year.
- No station's cash stays below zero for long. Faction debt stays inside its limit in the four starting dates.
- Unmet demand does not get worse than after step 16. If broke outposts starve, raise the core-crew share
  before adding any other mechanism.

## Open

- Whether residents should cut consumption at high prices (demand elasticity) instead of only the station
  ordering less. Leave this out until the model above has been benchmarked.
- The SF Reservoir ship ran at -23k cr. Ship debt is anticipated at salvage; check where it comes from when
  implementing step 17.
