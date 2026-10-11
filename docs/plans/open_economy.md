# Open Economy (roadmap step 5, v26)

Before this step the simulated economy was closed: stations produced goods for free and their
residents consumed goods for free, so money only moved between stations and ships. Consumer stations
paid ships for every import and earned nothing back (Low Earth Logistics ended the v25 run at −458k cr),
while producers and home ports piled money up.

Now an explicit **external account** sits on the other side of every payment that leaves or enters the
simulated trade. The audit still checks that stations + ships + external = the seeded money exactly.

## Mechanisms

1. **Residents and local producers** (`Simulation::settle_local_economy`). After each economy step:
   - every unit used up at a station (recipe consumption, machinery wear) is bought by its residents and
     local industry, at the station's price curve;
   - every new unit (recipe output, depot fuel refills) is bought by the station from its local
     producers, along the same curve but **at most the base price**. Without the cap, a producer that
     starts short of its own output pays its producers up to 16× base for stock it never sells (Ceres's
     first 126 units of reactor fuel cost 243k).
   The other side is the *outside economy* balance.

   With every flow on one price curve, a station's balance changes only by its ship payments and the
   value of the stock it builds up. The audit's `Δstock` column shows that stock value.
2. **Dividends.** A ship keeps a working reserve (`ship_cash_reserve`, 50k). Cash above it goes to its
   owner, the home station, closing the gap in `dividend_days` (30). This is an internal transfer.
3. **Faction treasuries** keep each station's cash inside a band. Below `station_credit_floor` (25k)
   they pay subsidies; above `station_credit_ceiling` (250k) they take taxes. Either gap closes in
   `station_balance_days` (30).
4. **Money-supply controller.** The gap between the seeded money and the money in stations and ships
   is paid to the stations per head of population (or taxed, when there is too much), closing in
   `money_supply_days` (60). It never pushes a station out of the band, or it would fight the floor and
   ceiling.

Parameters: `data/economy/open_economy.csv`. Station ledgers record residents' payments, producer
payments, dividends, subsidies and taxes; ship ledgers record dividends (outside `lifetime_profit`,
since they are not a cost). The bridge snapshot carries `outside_economy_credits` and
`faction_treasuries`.

Station cash still does not gate any decision (stations buy whatever ships deliver); it is a measure.
Faction treasuries are where fleet investment (roadmap step 3) can draw the money for new ships.

## v26 result (730 days)

- Ship behaviour is unchanged from v25 (station money gates nothing): cargo margin 1.47M, 11/22
  profitable, 4 laid up, 36 CRITICAL lines. Trajectory audit: 0 flagged.
- Money drift 0. Money in stations and ships stays within −1% … +10% of the seeded 1.45M until day 630,
  and ends at +16.7%.
- Every station ends at or above the 25k floor (v25: Low Earth Logistics −458k, Lunar Gateway −126k).
  Venus (327k) and Mars (239k) sit near or above the ceiling because their ships pay large dividends.
- Over the run, residents paid 2.90M and producers received 2.08M; the faction treasuries collected 1.11M
  in taxes and paid 0.54M in subsidies; ships paid 0.81M of dividends.

**What drives money growth:** residents of a starving station pay scarcity prices. Low Earth Logistics
buys food at about 15× base although Earth L1, 0.1 days away, has a food glut. In the last 130 days its
residents paid about 600k, nearly all earned by one Fast Courier (IN Swift, 30-unit hold). The other
Earth-area ships are away on long contracts. This is a fleet-capacity gap, which fleet investment should
close; the controller is not tuned to absorb it.
