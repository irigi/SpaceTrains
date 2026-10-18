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

## Decisions and findings

1. Reference prices already are a landed cost: base price plus 1.2× the transport cost of the best
   bulk hull over a round trip (`compute_reference_prices`). So the rent is the stock-ratio multiplier
   alone (2.7× at delivery), and "idea 3" becomes the lead-time fix plus re-centring the bid.
