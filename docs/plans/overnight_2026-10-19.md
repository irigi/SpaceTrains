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
| c2 + contracts (k2) | 1.9 ± 0.5 | 1.3 ± 1.2 | 1.64 ± 0.22 | 123 ± 17 | 35 ± 4 | 38 ± 2 | 21 / 19 | 70 ± 9 | −25 ± 19 | — |
| c1 + urgency max 3 (c3) | 1.7 ± 0.5 | 1.3 ± 1.0 | 1.86 ± 0.15 | 135 ± 9 | 34 ± 1 | 38 ± 1 | 20 / 19 | 75 ± 1 | −35 ± 9 | — |
| c1 + markup 0.6 (c4) | 2.7 ± 0.5 | 3.1 ± 1.8 | 1.43 ± 0.21 | 138 ± 32 | 42 ± 3 | 36 ± 1 | 20 / 17 | 66 ± 3 | −27 ± 21 | — |
| c1 + 60-day pool, solo +100% (c5) | 3.9 ± 0.9 | 6.1 ± 2.2 | 1.20 ± 0.15 | 107 ± 6 | 41 ± 3 | 35 ± 1 | 22 / 15 | 59 ± 4 | −42 ± 5 | — |
| c2 + asks over open orders (c6) | 1.6 ± 0.2 | 2.3 ± 1.3 | 1.48 ± 0.08 | 122 ± 6 | 33 ± 2 | 37 ± 2 | 20 / 20 | 73 ± 5 | −6 ± 10 | — |
| c6 + lead 3× (c7) | 1.9 ± 0.4 | 2.4 ± 0.5 | 1.53 ± 0.10 | 142 ± 7 | 35 ± 1 | 38 ± 1 | 19 / 20 | 77 ± 2 | −13 ± 9 | — |
| contracts (k1, legs from today) | 1.1 ± 0.2 | 0.9 ± 0.7 | 2.64 ± 0.17 | 209 ± 11 | 34 ± 0 | 40 ± 2 | 20 / 19 | 82 ± 3 | −6 ± 20 | — |
| contracts, best 6 loops re-dated (k3) | 1.2 ± 0.3 | 0.7 ± 0.2 | 2.56 ± 0.08 | 200 ± 14 | 31 ± 1 | 41 ± 1 | 19 / 19 | 83 ± 3 | +2 ± 12 | — |
| c6 + contracts (k4) | 2.0 ± 0.2 | 1.7 ± 1.6 | 1.48 ± 0.25 | 119 ± 23 | 35 ± 3 | 37 ± 1 | 21 / 18 | 73 ± 1 | −21 ± 23 | — |
| c6, urgency max 3 (c8) | 1.7 ± 0.4 | 1.8 ± 0.8 | 1.74 ± 0.13 | 144 ± 6 | 33 ± 1 | 39 ± 3 | 20 / 19 | 77 ± 5 | −16 ± 11 | — |
| c6 + markup 0.6 (c9) | 2.0 ± 0.5 | 1.3 ± 0.6 | 1.66 ± 0.10 | 139 ± 8 | 35 ± 3 | 39 ± 1 | 18 / 19 | 77 ± 3 | −19 ± 6 | — |
| c6 + return share 1.0 (c10) | 2.1 ± 0.3 | 1.0 ± 0.8 | 1.60 ± 0.12 | 136 ± 8 | 36 ± 1 | 39 ± 1 | 18 / 20 | 75 ± 4 | −26 ± 10 | — |
| c6 + binding tenders (t1) | 2.8 ± 0.8 | 2.3 ± 1.0 | 1.68 ± 0.10 | 141 ± 6 | 35 ± 2 | 39 ± 2 | 19 / 19 | 73 ± 6 | −20 ± 19 | — |
| c6, urgency on safety stock, max 4 (c11) | 1.8 ± 0.5 | 1.4 ± 0.6 | 1.71 ± 0.18 | 143 ± 13 | 34 ± 2 | 39 ± 1 | 19 / 19 | 77 ± 3 | −12 ± 12 | — |
| competitive + escalation 5%/day (e1) | 1.5 ± 0.4 | 0.6 ± 0.7 | 2.38 ± 0.09 | 199 ± 14 | 32 ± 2 | 41 ± 1 | 17 / 19 | 82 ± 3 | −2 ± 16 | — |
| escalation 10%/day (e2) | 1.5 ± 0.4 | 1.0 ± 0.8 | 2.52 ± 0.10 | 206 ± 11 | 31 ± 3 | 41 ± 2 | 18 / 18 | 86 ± 2 | −14 ± 8 | — |
| e1 + urgency (e3) | 1.4 ± 0.4 | 1.4 ± 1.2 | 2.63 ± 0.22 | 209 ± 12 | 31 ± 2 | 42 ± 2 | 17 / 20 | 86 ± 3 | −8 ± 21 | — |
| contracts, staggered reviews (k3b) | 1.1 ± 0.2 | 0.5 ± 0.7 | 2.57 ± 0.15 | 204 ± 8 | 32 ± 2 | 42 ± 1 | 20 / 18 | 85 ± 3 | +2 ± 21 | — |
| k3b, 2-stop shuttles only (k7) | 1.5 ± 0.2 | 1.4 ± 0.7 | 2.57 ± 0.21 | 209 ± 8 | 32 ± 1 | 39 ± 1 | 21 / 19 | 83 ± 4 | −4 ± 28 | — |
| k3b, at most 3 stops (k8) | 1.2 ± 0.2 | 0.7 ± 1.1 | 2.56 ± 0.17 | 203 ± 12 | 33 ± 2 | 40 ± 1 | 20 / 19 | 84 ± 3 | −13 ± 20 | — |
| k3b, accept 0.5 (k5) | 1.6 ± 0.3 | 1.6 ± 1.0 | 2.39 ± 0.01 | 194 ± 10 | 30 ± 2 | 39 ± 2 | 21 / 18 | 84 ± 4 | +4 ± 5 | — |
| k3b, cover 1.0 (k6) | 1.0 ± 0.2 | 0.7 ± 0.4 | 2.62 ± 0.27 | 197 ± 14 | 34 ± 2 | 40 ± 2 | 20 / 18 | 83 ± 2 | −19 ± 13 | — |
| c6 + contracts at market, cover 1 (x3) | 1.8 ± 0.5 | 1.7 ± 1.2 | 1.55 ± 0.16 | 125 ± 9 | 35 ± 3 | 39 ± 1 | 21 / 18 | 72 ± 1 | −19 ± 5 | — |
| c6 + cost-plus contracts +50% (x1) | 1.9 ± 0.4 | 2.1 ± 0.5 | 1.39 ± 0.09 | 116 ± 2 | 36 ± 2 | 38 ± 2 | 20 / 19 | 74 ± 4 | −28 ± 9 | — |
| c6 + cost-plus contracts +100% (x2) | 1.6 ± 0.3 | 1.2 ± 0.3 | 1.57 ± 0.20 | 129 ± 15 | 35 ± 2 | 39 ± 1 | 19 / 19 | 73 ± 3 | −30 ± 13 | — |
| c8 + cost-plus contracts +50% (x4) | 1.4 ± 0.2 | 1.8 ± 0.5 | 1.74 ± 0.17 | 139 ± 10 | 34 ± 1 | 40 ± 2 | 19 / 19 | 80 ± 2 | −19 ± 5 | — |
| c6 + contracts at the going rate (x6) | 2.4 ± 0.9 | 1.9 ± 0.3 | 1.49 ± 0.21 | 118 ± 12 | 34 ± 5 | 38 ± 1 | 21 / 19 | 75 ± 1 | −21 ± 9 | — |
| c6 + own-ask floor (f1) | 1.7 ± 0.3 | 1.6 ± 1.4 | 1.52 ± 0.07 | 124 ± 3 | 33 ± 1 | 39 ± 0 | 19 / 19 | 78 ± 5 | −18 ± 11 | — |
| f1 + contracts (f2) | 1.8 ± 0.5 | 2.4 ± 1.0 | 1.52 ± 0.23 | 125 ± 19 | 36 ± 6 | 38 ± 3 | 21 / 19 | 73 ± 4 | −26 ± 7 | — |
| f1 without urgency (f3) | 3.0 ± 0.4 | 3.3 ± 1.4 | 1.43 ± 0.03 | 128 ± 4 | 42 ± 3 | 35 ± 1 | 19 / 20 | 70 ± 5 | −30 ± 21 | — |
| c6, fleet cap 170 (x5) | 1.6 ± 0.2 | 2.6 ± 1.0 | 1.49 ± 0.24 | 113 ± 15 | 35 ± 3 | 38 ± 1 | 18 / 19 | 65 ± 1 | −43 ± 11 | — |
| c6, urgency on safety, max 8 (c12) | 1.4 ± 0.4 | 1.0 ± 0.7 | 2.10 ± 0.30 | 173 ± 22 | 37 ± 1 | 38 ± 1 | 21 / 20 | 75 ± 3 | −5 ± 16 | — |
| k6, accept 1.0 (k9) | 1.0 ± 0.4 | 0.5 ± 0.8 | 2.64 ± 0.32 | 201 ± 18 | 31 ± 3 | 40 ± 2 | 20 / 19 | 84 ± 4 | −3 ± 13 | — |
| k6, review every 15 days (k10) | 1.4 ± 0.2 | 1.7 ± 1.7 | 2.68 ± 0.21 | 214 ± 15 | 33 ± 2 | 40 ± 2 | 19 / 20 | 84 ± 3 | −4 ± 15 | — |

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
6. Asks spread over what the leg can carry to open orders (`ask_open_units`), not 65% of the hold:
   unmet 2.0 → 1.6%, price 1.60 → 1.48× (c6). Kept.
7. The lead-time cap hurts in every combination (+0.2–0.5 points of unmet): the long observed lead
   at Earth L1 is a bug in principle but works as a safety buffer for water, which is short anyway.
   Dropped (switch stays, off).
8. Persistent shortages in every variant are geographic, not the market: LEO metals (10–13% unmet)
   and Earth L1 water (7–11%). Lunar Gateway is the only nearby source (metals ~15 u/day net against
   ~10 u/day at LEO alone, also shipped to Venus and Mars); LEO got 7.7 u/day of metals even at the
   price cap. A data question for the user (Luna output), not changed here.
9. Rotation contracts work: about 15 ships on loops at a time, 70+ contracts signed per run (many
   3–4-stop loops of 250–360 days); unmet 1.4 → 1.1–1.2%, last fifth 0.7%, units delivered +13%,
   cost per unit delivered lowest so far. Under competitive prices fewer ships sign (8).
10. Contract reviews are staggered per ship over the 30-day review period (hash of the id), so the
   fleet never plans loops all at once.
11. Multi-stop loops earn their complexity: 2-stop shuttles 1.5% unmet (last fifth 1.4%), up to 3 stops
   1.2% (0.7%), up to 4 stops 1.1% (0.5%). In the contract runs, Earth L1 water and LEO food are fully
   supplied by cislunar loops; under competitive prices those short hauls are 9–13% unmet.
12. Binding tenders (cheapest docked ship at a source gets the order as a one-leg contract): rarely
   fire (ships seldom sit docked at a source) and supply worse (2.8%). Rejected.
13. Escalation (the market price x a factor that rises while the order stays open): supply recovers,
   but the price climbs back to 2.4–2.6×, the rent level. With posted or escalating prices the
   price settles where ships come, and it is the same rent. Rejected.
14. Under competitive prices every job pays about the same profit per day (markup x the ship's daily
   cost), so ships are near-indifferent and short cheap hauls lose to anything with a little more.
   Next: cost-plus contracts under competitive spot prices: loops chosen by the value they create at
   market prices, deliveries priced at purchase + the loop's cost share x (1 + markup), at most the
   station's going rate.
15. The Earth L1 water shortage under competitive prices is at the source: Lunar Gateway makes ~22
   water a day and L1, LEO and crews all draw on it; its stock is nearly drained and loads to L1 are
   ~130 u every 25–30 days (6.2 u/day used). Prices there are not low (45–125 cr against 22). Contracts
   fix it in the posted world because a loop's deliveries load first and so reserve Luna's water for
   L1; under competitive prices few ships sign.
16. Own-ask floors (a ship is paid at least its own cost plus markup, within the stock bid) do not help
   (1.7%); without the urgency factor competitive pricing fails (3.0%). Cost-plus contracts are rarely
   signed (3–5 ships): their markup on the loop's cost loses to urgency-weighted spot work.
17. More ships do not help under competitive prices (cap 170: same unmet, 65% profitable, money −43%):
   capacity is not the limit, so cap 130 stays.
18. With 4 starts the noise is ±0.2–0.4 points of unmet; finalists are judged on 8 starts (0, 45, ..., 315).
