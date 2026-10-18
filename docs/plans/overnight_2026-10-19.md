# Overnight 2026-10-18/19: market redesign

Goal (user, 2026-10-18 evening): stations supplied, ships used and paying back their price, stations not
overpaying. Try a landed-cost bid, tenders and rotation contracts with multi-stop loops; keep only
what improves the economy. Work until 17:00 at the latest, no questions; decisions are logged here.

Reference: tag `econ-ref-2026-10-18` (fleet cap 130, the user's decision). Every redesign is a switch
in `data/economy/market.csv`, whose defaults reproduce the reference. Scorecard: 4 starts × 1460 days,
mean ± sd (`sweep.py` in the session scratchpad).

## Scorecard

| Run | Unmet % | Last fifth % | Price ÷ ref | Margin cr/u | Cost cr/u | Hold use % | Empty / wait % | Profitable % | Money % | Run s |
|---|---|---|---|---|---|---|---|---|---|---|
| ref (cap 130) | 1.4 ± 0.1 | 1.1 ± 0.6 | 2.70 ± 0.18 | 221 ± 9 | 34 ± 2 | 42 ± 1 | 17 / 19 | 83 ± 3 | −8 ± 26 | 124 |
| ref, cap 90 | 1.5 ± 0.2 | 1.7 ± 0.8 | 2.94 ± 0.13 | 236 ± 12 | 35 ± 3 | 43 ± 3 | 17 / 21 | 91 ± 3 | +30 ± 13 | — |
| lead cap 3× | 1.6 ± 0.1 | 1.1 ± 1.0 | 2.53 ± 0.09 | 236 ± 20 | 36 ± 2 | 38 ± 1 | 20 / 19 | 85 ± 3 | +4 ± 11 | — |
| lead 3× + bid × 0.7 | 2.1 ± 0.6 | 2.2 ± 0.8 | 2.69 ± 0.29 | 239 ± 10 | 36 ± 1 | 39 ± 2 | 19 / 21 | 83 ± 2 | +2 ± 13 | — |
| lead 3× + bid × 0.5 | 1.8 ± 0.5 | 2.6 ± 0.3 | 2.46 ± 0.27 | 214 ± 16 | 32 ± 2 | 39 ± 2 | 20 / 20 | 81 ± 3 | −17 ± 36 | — |
| competitive (c1) | 3.6 ± 0.2 | 3.2 ± 0.7 | 1.35 ± 0.08 | 127 ± 15 | 44 ± 3 | 34 ± 2 | 22 / 16 | 62 ± 3 | −45 ± 12 | — |
| c1 + lead 3× | 3.1 ± 0.5 | 3.2 ± 1.4 | 1.25 ± 0.29 | 121 ± 23 | 41 ± 5 | 32 ± 1 | 24 / 17 | 63 ± 4 | −40 ± 17 | — |
| c1 + urgency e=1, max 2 (c2) | 2.0 ± 0.6 | 1.8 ± 1.0 | 1.60 ± 0.10 | 121 ± 8 | 36 ± 2 | 37 ± 2 | 20 / 19 | 71 ± 1 | −25 ± 14 | — |

Run times are left out while several sweeps share the machine; speed is measured separately at the end.

## Decisions and findings

1. Reference prices already are a landed cost: base price plus 1.2× the transport cost of the best
   bulk hull over a round trip (`compute_reference_prices`). So the rent is the stock-ratio multiplier
   alone (2.7× at delivery), and "idea 3" becomes the lead-time fix plus re-centring the bid.
2. Lead-time fix (`lead_cap_factor` 3: a lead sample counts at most 3× the nearest source's lead):
   prices 2.70 → 2.53×, but fewer units delivered and unmet 1.4 → 1.6%. Not a clear win on its own;
   kept as a switch and judged again in combination.
3. Scaling the posted bid down (bid × 0.7, × 0.5) does not lower what stations pay: stocks fall until
   the stock ratio brings the price back (2.69×, 2.46×) and unmet rises. With posted prices the price
   is where ships are willing to come, and a ship's alternative is another rent-priced delivery, so
   the rent sustains itself. Only competition between ships' asks breaks it. Bid re-centring dropped.
4. Competitive pricing (`competitive` 1): once a day each order's market price is the second-lowest
   ask of the ships free at a source within 30 days (their leg cost incl. a 50% return share over 65%
   of the hold, plus a 30% markup; one bidder: its ask + 50%). The order pays at most that. Price
   halves (1.35×), margin 221 → 127 cr/u, but unmet 3.6%: unmet sits on short cheap hauls (LEO food
   and metals, L1 water), where a big hold's ask spread over 65% of its hold priced a 100 u order
   below the reference. An urgency factor (market price × clamp((S/stock)^1, 1, 2)) gives the ranking
   by need back: unmet 2.0%, price 1.60×.
5. Background thread for loop planning (user's question): not needed so far. A loop search from one
   port over 9 stations is at most 400 cycles of up to 4 legs, valued from cached leg estimates (72 per
   class and 5-day bucket, computed in parallel by the existing pass mechanism). If it shows up in the
   profile, the plan is a worker thread whose result applies at a fixed later game time (deterministic).
