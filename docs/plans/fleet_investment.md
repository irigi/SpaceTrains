# Fleet Investment (roadmap step 4, v27)

The starting fleet is not final. Some classes cannot earn on the routes they start near, and some routes
have no ship at all (v26: one 30-unit courier fed Low Earth Logistics, whose residents paid about 15× base
for food while Earth L1, 0.1 days away, had a glut). Owners now respond: they sell ships that stay laid
up, and the faction treasuries buy new ships for the routes that pay best.

## Mechanisms (`Simulation::step_fleet_investment`)

1. **Selling.** A ship laid up for `layup_sale_days` (180) without interruption is sold to the outside
   economy for `salvage_fraction` (0.4) of its value; the money goes to its faction's treasury. Its cash
   (or debt) goes to its home station, booked as a dividend. Sold ships keep their ledger and stay in the
   audit, marked `(sold)`.
2. **Commissioning.** Every `review_days` (60) the treasuries consider one new ship:
   - Candidates: every hull in its standard tanks, built at every station whose yard is not still
     building a ship (since v29; before, no ship commissioned in the last two reviews could call it home), skipping a hull where a ship of it already sits idle or laid
     up (that is the existing fleet's work) and hulls no treasury can pay for.
   - **Valuation: sustained flow, not dispatch's score.** Dispatch's score is a one-shot rate: a 0.1-day
     hop with a one-off price gap scores thousands of credits per day, and annualising it bought ships
     for one-off trades that then lost money (first try: 6 ships, most unprofitable). A candidate is
     probed by dispatch in cargo-only mode (cargo runs from the yard, no follow-up forecast), which
     lists every feasible cargo run. Since v30 each run is valued over the route commitment, as whole
     round trips (at least one): out loaded, back empty, each hold bought and sold along the price
     curves of the stocks both stations would hold by then. The yard keeps producing and other ships
     keep taking their observed share (exports, and ship refuelling for fuel); the destination keeps
     consuming and other ships keep delivering (observed imports, at least the flow committed to the
     route). So a route that is already served, or that this ship's own flow saturates, sells near or
     below the base price instead of today's scarcity price. Fuel counts twice per round trip (the
     empty way back is lighter, but is often fuelled at the dearer port). Profit per day minus the
     ship's daily capital and crew cost, over the hull price, must reach `hurdle_return_per_year` (0.3).
     (v27-v29: margin per unit at the forecast arrival price × min(hold / round trip, open demand).)
   - Probes run in order of a cheap price-based upper bound and stop once the best return beats the
     next bound (the bound is loose at scarcity prices, so it mostly orders the probes). A review takes
     about 5 s, none when no treasury can afford a hull.
   - The winning hull is built with the tank variant dispatch's full score prefers (the refit
     criterion), and that choice holds for the refit payback period (180 days); otherwise new ships
     went straight back into the yard.
   - It is bought by the yard station's faction if its treasury holds the price plus the
     `working_capital` (25k), otherwise by the richest faction. At most one ship per review.
   - The treasury pays the hull to the outside economy and the working capital to the ship, and the
     money-supply target grows by that working capital. The ship spends `build_days` (30) in the yard
     (the `Refitting` phase) and then dispatches normally. Debug: `SPACETRAINS_TRACE_INVESTMENT=1`.

3. **Route commitment (v28).** A new ship works the route it was bought for until
   `route_commitment_days` (180) after it leaves the yard: from its yard it only flies to that
   destination, from anywhere else only home (with cargo if any pays, otherwise empty), and it does
   not reposition. In later reviews the flow it was bought for (units per day of that commodity to
   that destination) comes off the destination's consumption, so the treasuries stop buying a second
   ship for a route that is already served. Without it, ships took the run while the gap lasted and
   then left for long contracts, the gap reopened, and three Light Freighters were bought for one
   food route. The audit shows each new ship's route and commitment end.

Parameters: `data/economy/fleet_investment.csv`. The snapshot carries a `FleetInvestmentLedger`
(ships commissioned and sold, hulls bought, working capital, salvage), `money_supply_target` and
`sold_ships`. The ledger identities in `tests/economy_test.cpp` include it: outside = producers −
households + hulls − salvage; treasuries = taxes − subsidies − hulls − working capital + salvage.

## v27 result (730 days)

- Sold: IN Zephyr, IN Amphora, IN Borealis (never traded, day 211) and SS Endeavour (laid up from day
  498 after +59k of lifetime profit); salvage 141k.
- Commissioned 7 ships for 346k: a Plasma Courier and a Plasma Freighter at Mercury for metals to Low
  Earth Logistics, three Light Freighters at Earth L1 for food to Low Earth Logistics, an Orbital Tanker
  and a Fast Courier at Low Earth Logistics for machinery/electronics to Earth L1. Four of the six
  older than 10 days are profitable; MC Plasma Courier 1 made +189k.
- Fleet: 15/29 ships profitable (v26 11/22), cargo margin 1.39M, 0 laid up, 0 stranded, 34 CRITICAL
  lines. Low Earth Logistics food is no longer critical at day 730; residents there paid 0.90M (v26
  1.14M). Money supply +7.0% of the target (v26 +16.7%), drift 0. Run 3m54 (v26 2m57).
- Trajectory audit: 0 flagged.

**Open points.** Ships are free agents: a freighter built for the Earth L1 → Low Earth Logistics food
run takes that run while the price gap lasts, then leaves for long contracts (IC Light Freighter 2 left
for Mars after selling 50u of food for 690 cr), so the gap reopens and the treasuries order another
ship. Low Earth Logistics metals and reactor fuel stay critical (the Mercury ships deliver about 0.5
u/day against 9 u/day consumed). Treasury money limits the pace: one ship every 60 to 120 days.

## v28 result: route commitment (730 days)

- Bought 7 ships for 332k: Mercury metals to Low Earth Logistics (Plasma Courier, Plasma Freighter),
  Earth L1 food to Low Earth Logistics (two Light Freighters, the second at day 720 after the first's
  commitment ended), Low Earth Logistics electronics to Lunar Gateway and Earth L1, Earth L1 food to
  Lunar Gateway. Same 4 ships sold.
- Mostly neutral against v27: fleet lifetime profit 871k (v27 883k), cargo margin 1.39M, 13/29
  profitable (15/29), 33 CRITICAL lines (34), money supply +5.5% (+7.0%), treasuries 220k (203k).
  Run 4m02.
- The commitment works as designed: IC Light Freighter 2 shuttled food and electronics between Earth
  L1 and Low Earth Logistics for 180 days, and Low Earth Logistics food fell below base price (about
  15 cr against 50). But that is also why it lost money (−20k at day 730): once it fed the route,
  the margin it was bought for (271%/yr expected) was gone, and each 0.1-day hop earned about what its
  fuel cost.

## v30 result: sustained valuation (730 days)

Imports and exports per station and commodity are now tracked (60-day average of ship cargo trades),
and the valuation above replaced the one-trip margin. Against v29 (same seed, same code otherwise):

- Fleet lifetime profit 891k (v29 444k), 16/32 profitable (14/32), cargo margin 1.99M (1.54M), fuel
  699k (690k), money supply +8.8% (+2.2%), unmet demand 71.7% (72.3%), run 6m57 (5m41).
- 10 ships bought, as in v29, still almost all for the Earth cluster: metals and fuel to Low Earth
  Logistics, water to Earth L1, electronics to Lunar Gateway. A Plasma Bulk Tanker for Lunar fuel came
  at day 540 (+78k). Most of the profit gain is in the existing fleet (new ships 266k, v29 371k), so
  part of it is the different order of purchases, not the valuation itself.
- First try without the two fixes above: a 9000-day Titan -> Ganymede plan was valued as if it
  delivered within the window, and only the loaded leg's fuel was counted; an Orbital Tanker bought
  for Lunar fuel to Earth L1 earned 66k of cargo margin and burned 96k of fuel (fleet profit 394k).

**Open point: one ship per review.** The treasuries end with 542k they could not spend, while consumers
go without 72% of their demand (by base value; the audit's "Unmet Demand" table). Mars, Mercury,
Ceres, Ganymede and Titan get almost nothing; the production is there (the system makes 8-10x what
the stations consume and producers hold large stocks). Buying every candidate above the hurdle each
review, re-valued after each purchase with its committed flow, is the next lever.
