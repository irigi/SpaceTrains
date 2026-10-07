# Outer-System Exports (roadmap step 7, v32)

Before this step the outer stations (Ceres, Ganymede, Titan) made only goods the inner system also
makes, so a ship that flew supplies out came back empty and the trip rarely paid. They went without
about 94% of their demand (the audit's "Unmet Demand").

## Mechanisms

1. **Station resources** (`data/recipes/station_recipes.csv`): recipes of one station, on top of its
   profile's, per 10,000 inhabitants like the profile recipes. Ceres mines platinum-group metals;
   Ganymede and Titan extract deuterium. Their output is gated by the station's inputs like any
   recipe.
2. **Export goods** (`data/commodities/commodities.csv`): platinum (10 kg, 3000 cr) and deuterium
   (20 kg, 1500 cr): little mass, high value, no decay. Producers stockpile up to 720 days of output
   (other goods 90), since ships come by once in a year or two. Each producer starts with 200 units.
3. **Export markets** (`data/economy/export_markets.csv`): Low Earth Logistics and Earth L1 Terminal
   sell everything delivered to them on to Earth's economy (the outside account) at the base price,
   whatever the amount. The price does not move with the delivery and the stock is cleared every
   step; the payment is booked like a resident purchase, so the audit still reconciles. Planners
   see the market as a consumer of `units_per_day` (10), but never as a starving one (no urgency
   boost). The audit lists the units sold per market ("Export Markets").
4. **No other buyers.** Anywhere that neither makes nor exports an export good prices it at the
   floor (0.25x base). The "untraded good" rule (a 20-unit target) made Mars and the Moon bid 16x
   base for platinum, and ships carried it there.
5. **Open surplus for repositioning.** An empty ship relocating to a producer values only the surplus
   that the ships already docked there or on their way leave (the rule follow-up legs already used).
   Meant to stop several ships chasing the same stockpile; it made no difference (below).

## v32 result

| | v31, 730 d | v32, 730 d | v31, 1460 d | v32, 1460 d |
|---|---|---|---|---|
| Fleet profit | 981k | 768k | 2.39M | 2.79M |
| Profitable ships | 19/41 | 12/36 | 42/66 | 37/67 |
| Fuel bill | 662k | 473k | 1.67M | 1.19M |
| Unmet demand | 71.4% | 74.3% | 71.1% | 72.9% |
| Exports sold to Earth | - | 387k | - | 387k |
| Money supply | +6.2% | +0.3% | +5.6% | +7.6% |

- Exports work as a mechanism but do not yet start a trade. In four years one load reached Earth:
  IN Meridian (an NTR freighter) took the opening 129 units of Ceres platinum to Earth L1 for 387k
  and ended the 730-day run +479k (v31 +43k). A second load of 61 units was in transit at day 1460.
- The platinum drew ships away from the inner routes. MC Minerva left a food route at Mars that
  earned +309k in v31 to fly to Ceres (arrival day 1111); at day 1460 seven ships were on their way
  to Ceres, arriving between day 1467 and day 3139, for a stockpile of 77 units that grows by 0.45 a
  day. Ships bought at Ceres for platinum (days 960, 1080, 1440) lost money.
- Ceres, Ganymede and Titan still go without 94-99% of their demand. Supplies that the export trade
  pulls outward arrive after the four-year horizon: a round trip to Ceres takes about 1000 days.
- The open-surplus rule for repositioning changed nothing (the 730-day and 1460-day runs are identical
  to the runs without it): ships reach Ceres on cargo runs whose follow-up leg carries platinum, not
  by repositioning empty.

## Findings

- Ganymede has no feasible cargo run for any class, and every Titan run takes thousands of days
  (the nuclear-thermal plans of roadmap step 8). Their deuterium cannot move until the planners
  reach them.
- Ceres -> Earth takes a plasma freighter 480-590 days one way. With the first price (1000 cr,
  1 u/day per 10k, 90-day stockpile) a load was at most 81 units and no run cleared the hurdle.

## Open points

- The follow-up leg values a producer's stockpile at today's level minus the ships already inbound,
  but not against ships that will choose the same follow-up later, nor the stock it will hold after a
  500-day trip: the rush to Ceres comes from there.
- Whether exports should stay enabled while the outer system is out of reach in the benchmark
  horizon (they cost 3 points of unmet demand and 7 profitable ships at 730 days).
