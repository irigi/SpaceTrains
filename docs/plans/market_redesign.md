# Market redesign (2026-10-19)

Why: after step 29 stations paid about 2.7× the reference (landed) price for what they import, while
ships' operating costs were about 35 cr per unit delivered against 220 cr of margin. The price was rent:
the station's bid follows only its stock ratio, ships take the posted price, and a ship's alternative is
another rent-priced delivery, so the rent sustains itself (scaling the bid down only lowers stocks until
the ratio brings the price back). The user asked for a system that covers demand at lower cost, with
ships that pay their way and stations that do not overpay, and asked explicitly for rotation contracts
with multi-stop loops.

## Decision (2026-10-19)

The game now runs **competitive pricing plus rotation contracts** ("y2"):

- **Pricing:** `competitive` 1, `ask_open_units` 1, urgency e = 1 capped at 3.
- **Contracts:** `contracts` 1, up to 4 stops, cover 1.0, priced cost-plus +100% (`contract_cost_plus` 1, at
  most the going rate), signed at ≥ 0.5 × the best spot score.

On 8 starts × 4 years against the reference:

| | Reference | Contracts alone | y2 (default) |
|---|---|---|---|
| Unmet demand | 1.3 ± 0.3% | 0.9 ± 0.3% | 1.3 ± 0.3% |
| Last fifth | 1.2 ± 0.6% | 1.1 ± 1.1% | 1.6 ± 1.4% |
| Price paid ÷ reference | 2.64 | 2.56 | 1.69 |
| Ship margin / cost per unit | 216 / 34 cr | 196 / 34 cr | 134 / 35 cr |
| Profitable ships | 85% | 83% | 75% |
| Ships on contracts | 0 | 12 | 7 |
| 4-year run, 14 threads | 69 s | 62 s | 74 s |
| Slowest 20 days | 6.2 days/s | 6.9 days/s | 5.3 days/s |

So y2 supplies as well as the reference while stations pay a third less, and ships still earn about
four times their operating cost on a unit. Contracts alone supply best and plan fastest, but leave the
rent. To prefer supply over price, set `competitive` to 0 and use `contract_accept` 0.7 and
`contract_cost_plus` 0, the contract settings tuned for posted prices.

Everything below is a switch in `data/economy/market.csv` (optional keys, defaults in
`domain::MarketDefinition`); with every switch off the economy is exactly the reference
(`econ-ref-2026-10-18`). The log of experiments is `docs/plans/overnight_2026-10-19.md`.

## Competitive pricing (`competitive`)

Once a day each priced order gets a market price from the ships that could serve it:

- **Bidders:** ships docked at a source of the good (a station that makes it, with at least
  `min_source_surplus` spare), or arriving at one within `available_days`.
- **A ship's ask:** the good's price at the source, plus the leg's cost (time at capital + crew cost,
  fuel at the source's price, from the cached leg estimate), times (1 + `return_share`) for the way back,
  spread over what the leg can carry. With `ask_open_units`, that is the open orders the source can fill
  at that station; otherwise `fill_ref` of the hold. Then times (1 + `ask_markup`).
- **Market price:** the second-lowest ask. With a single bidder, its ask × (1 + `solo_premium`).
- **What the order pays:** at most the market price × an urgency factor, clamp((S / projected stock at
  arrival)^`urgency_elasticity`, 1, `urgency_max`), within the stock-ratio bid. With
  `urgency_on_safety`, the factor is measured against the safety stock B instead of S.

Variants tried and left off: escalation while an order stays open (`escalation_rate`), binding tenders
to docked ships (`tender_awards`), and own-ask floors (`own_ask_floor`).

## Rotation contracts (`contracts`)

- **Reviews:** a ship without a contract (not a liner, not on its commissioning route) looks for one
  every `contract_review_days`. The first review is spread over that period by ship, so the fleet never
  plans all at once.
- **Loop search:** every loop from its port of up to `contract_max_stops` stops (port included), at
  most 400 loops over 9 stations. Each is valued from cached leg estimates; the best `contract_refine`
  are valued again with each leg dated from when the previous one arrives (launch windows).
- **Value:** profit per day after time and fuel. On each leg the hold fills by margin per unit with what
  the next stop still needs under no contract: `contract_cover` × uncontracted use × cycle days, capped by
  what the source can spare over the cycle.
- **Price:** the order's market price under competitive pricing, else its going rate. Optionally the
  going rate under competitive pricing (`contract_at_going_rate`), or cost-plus (`contract_cost_plus`).
- **Signing:** if the loop earns at least `contract_accept` × the best spot mission's score. The term
  is `contract_term_days`.
- **Execution:** the ship flies only its next stop. Contracted units load first, at the fixed price, as
  their own lot; spare hold space is filled at market. The deliveries still to come count as contracted
  arrivals at their estimated dates, so stations order only for the gaps. With `contract_reserve_days`, a
  source holds back from spot buyers what contract ships will load there soon.
- **End:** when the term runs out, or when the next leg cannot be flown (the contract lapses). The
  inspector shows the loop and its deliveries; `SPACETRAINS_TRACE_CONTRACTS=1` logs contract events.

## Lead-time cap (`lead_cap_factor`)

A delivery's lead sample counts at most this factor × the nearest source's lead, so slow far sources
(Ceres water at Earth L1) do not set the lead of a good that comes from next door. Left off: the long
lead works as a safety buffer for water that is short at its source.
