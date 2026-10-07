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
   - Candidates: every hull in its standard tanks, built at every station that no ship commissioned
     in the last two reviews calls home, skipping a hull where a ship of it already sits idle or laid
     up (that is the existing fleet's work) and hulls no treasury can pay for.
   - **Valuation: sustained flow, not dispatch's score.** Dispatch's score is a one-shot rate: a 0.1-day
     hop with a one-off price gap scores thousands of credits per day, and annualising it bought ships
     for one-off trades that then lost money (first try: 6 ships, most unprofitable). A candidate is
     probed by dispatch in cargo-only mode (cargo runs from the yard, no follow-up forecast), and its
     best run is valued as margin per unit (sale value minus purchase and fuel) × units per day, where
     units per day = min(hold / round trip, destination's consumption rate), minus the ship's daily
     capital and crew cost. Annual profit / price must reach `hurdle_return_per_year` (0.3).
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
