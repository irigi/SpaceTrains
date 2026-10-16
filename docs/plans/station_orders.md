# Step 29: station purchase orders (plan, 2026-10-15)

The user's direction (review of 2026-10-15, option A): demand should have a rational basis. A station
works out what it needs to last between ship visits and buys that; the price moves until ships are willing
to come; the mean price follows from need, not from where the station happens to be in its delivery cycle.
A station may buy a little extra when it is cheap. A good the station does not strictly need (an input
whose output may not pay) is worth at most the profit it makes possible.

This replaces the consumer side of today's price curve. Producers keep their curve for now (see Later).

## What is wrong today

Consumers price on current stock: `reference x (target / stock)^1.3`, clamped to [0.25, cap].

1. **The price is a sawtooth.** With rare deliveries the stock is a sawtooth, so the price says where the
   station is in its cycle, not what it needs.
2. **A big load lowers its own price.** A sale is the integral along the curve, so the last units of a full
   hold go cheap. Ships bring small loads to distant stations (Mars fuel, 60% unmet in year 4).
3. **Targets are formulas** (21 days, 1.2 x the reference round trip, 3000 u of depot fuel), not what the
   station observes about how often ships come and how late they are.
4. **The price never learns.** Nothing changes when ships do not come, except for oxygen and water
   (emergencies, step 14). The rest is patches: urgency weights in dispatch, input caps (step 24), the
   emergency premium, the depot buffer.

## Design

Each station keeps, per good it consumes (net consumer, export markets excepted), an **order**: how many
units it wants, by when, at what price. Ships see the orders, take slices of them as contracts at
departure (as they agree a contract price today), and are paid that flat unit price on arrival.

### 1. What the station observes (per station and good)

| Quantity | Meaning | Seed at start | Updated |
|---|---|---|---|
| `use` u | units/day used | recipe net rate | recipe rate + EMA of units sold to ships (crews' provisions, depot fuel: `ship_fuel_units_per_day`) |
| `lead` L | days from a contract to its delivery | reference one-way (round trip / 2), at least 1 | EMA over delivered contracts |
| `interval` I | days between deliveries | reference round trip for another planet's producer, 21 days locally | EMA over deliveries |
| `spread` σ | how much I and L vary | 0.5 x L | EMA of absolute deviation |
| `going rate` G | what the station has recently paid per unit | reference price (`reference_prices.csv`) | EMA over accepted contracts, weighted by units |

The EMAs use a window of a few deliveries (data). These are the station's own experience, so a station
that ships rarely visit plans for rare visits, without a formula about Hohmann times.

### 2. How much to buy (an (s, S) inventory policy over a stock profile)

**Contracts are binding** (user, 2026-10-15). Once a ship takes a slice, the station pays it in full on
arrival, however late, and plans around it. It does not order the same units twice; it orders only for the
windows its contracts leave uncovered.

- **Projected stock** p(τ): today's stock, falling at u per day, rising at each contracted arrival. This
  is today's `forecast_stock_on_arrival`, kept as a whole profile.
- **Safety stock** B = u x k x σ.
- **Reorder point** R = u x (L + k x σ) and **order-up-to level** S = R + u x I, as in an (s, S) policy:
  with no contracts on their way, a delivery tops the stock up to S, which lasts the usual interval plus
  the lead time of the next order. Capped by storage and by `max_cover_days`.
- **Open quantity for a delivery at time t:** what keeps the profile above B from t until the usual next
  order could arrive:

  N(t) = max over τ in [t, t + I + L] of (B − p(τ)), when positive.

  With no contracts this is exactly S − p(t). With contracts it is the gap they leave. A slow freighter
  due at t2 leaves a bridging order for deliveries before t2, sized to the use until t2; a ship that can
  only arrive after t2 sees just what the freighter does not cover. So a faster ship is worth sending, or
  buying (fleet investment uses the same orders), only for the window before the slow one arrives.
- **Extra buffer** up to S_max = S + u x E, where E is the number of extra days that pays: stock bought now
  instead of later costs interest (the faction's 5%/yr) plus decay. The station buys into it only at a
  discount d on the going rate, so E is where `holding cost x E` equals the discount. This is the "slightly
  overspend at a lower price" layer. The mean stays at S.

Quantity never depends on price, so a low price cannot produce a big stock: it produces no takers.

### 3. What to pay (price discovery)

The order's unit price P moves around the going rate:

- **Probing down:** while the profile stays above B for the next L + kσ days (no hurry), P = G x (1 − δ), a
  little below the going rate. If ships take that, G drifts down. This is how the price finds the cheapest
  willing carrier.
- **Raising:** while the profile, with every contract, falls below B within the next L + kσ days and no ship
  has taken the open quantity for `raise_days`, P rises by a factor ρ (for example 1.1), at most to the
  ceiling V. A contract taken resets the clock. A freighter due before the dip ends the raising; one due
  after the station has run dry does not, so the station keeps looking for a ship that can bridge the gap.
- **Accepted price:** a ship takes a slice at the P it saw; P is locked into the contract. Ships do not wait
  for higher prices, so the price stops one step above what the first willing ship needs (overshoot at
  most ρ).

The ceiling is not the price. With a ceiling of 40x the station still pays roughly the cost of carriage,
because it raises only until a ship comes.

### 4. What a good is worth: the ceiling V

| Good at this station | Ceiling V per unit |
|---|---|
| **Input** (feeds an output) | its marginal profit: value of the outputs one unit makes, less the going cost of the other inputs they need, times `(1 − keep_share)`. If G stays above V for `give_up_days`, the order is withdrawn and the plant runs on what stock it has: producing does not pay. It is tried again every `retry_days` at V. |
| **Upkeep** (spare parts, power, crew needs) | the output lost per unit short: `(1 − m) x station output value per day / u`, with m from `upkeep_penalties.csv`. |
| **Life-critical upkeep** (oxygen, water: today's emergency goods) | as above, but the faction tops the price up to `life_value x reference` (24x, today's emergency maximum). The part above the station's own V is paid by the faction, as emergency premiums are today (confirmed by the user, 2026-10-15). Headlines when it rises, as now. |
| **Consumed** (construction metals, science samples at Lunar Gateway and Mars) | `consume_value x reference` from data (4x for a start, today's cap). |
| **Depot fuel** (Mars, a depot without a factory) | 4x reference for a start; its sale price to ships is its import cost plus a margin (as residents pay import cost since step 17). |

### 5. How ships see it (dispatch)

`sale_value_on_arrival(station, good, units, days_ahead, ship)` stays the seam. It becomes piecewise flat:

1. units up to the open quantity for that arrival at P;
2. units into the extra buffer at G x (1 − d);
3. anything beyond at the floor price (0.25 x G, as today's minimum), so no ship is ever stuck with cargo.

**Open quantity for an arrival at time t** is N(t) from section 2. Contracts before t lower it (without
that, every ship sent to a starving port expects the same open quantity, fixed in v36). A contract after t
lowers it only for the stock it covers after its own arrival, so a freighter booked to arrive in two years
does not block a shuttle due tomorrow from bridging the gap (the step 17a failure).

### 5b. What a ship loads (the manifest)

Ships optimise for themselves and take the most lucrative business; if that means leaving with a small
load, so be it, but not because the ship sizes its load badly (user, 2026-10-15). There are no player
ships: every ship is autonomous.

Today a lot is one of 100%, 50% or 25% of the most the ship could carry (`LOAD_FRACTIONS`), and a mixed
hold is a separate code path. With flat order prices, a ship's revenue for a good is piecewise flat, so the
best load of that good is at a breakpoint, not at a fixed fraction: the open quantity, the end of the extra
buffer, the origin's surplus, the hold, or the heaviest load that still flies. One **manifest builder**
replaces both paths, per candidate destination:

1. List every good the origin has a surplus of and the destination will pay for (an open order, the extra
   buffer, the floor).
2. Each unit's **margin** = what the destination pays for it − what it costs here (marginal, along the
   producer's curve) − the **marginal propellant** for its mass. Marginal propellant comes from the plans
   already cached per tonne of cargo: the difference between the plan for this mass and the next tonne up.
3. Fill the hold greedily by margin per unit of hold, order slices first, down to the first unit with a
   negative margin, until the hold, the mass limit, the ship's credit or the surplus runs out.
4. Plan the transfer for the total mass (one plan per destination and tonne, cached as today) and score
   the mission over two legs as today.

**Filling the hold once the trip pays.** Step 3 is the fill: once an order slice has paid for the trip,
the next units cost only their purchase price and their propellant, so the ship tops up with anything whose
margin is still positive. At the destination such units land in that station's other orders or its extra
buffer at a discount. The extra buffer is exactly the slot for them: the station takes a little more than
it needs when it is cheap. Cargo that stays aboard to be sold one stop later (tramp trading) is left for
later; it needs cargo carried across legs and contracts at a station the ship has not chosen yet. The **urgency weight** (1–5x by days of stock left) is removed in a variant
and benchmarked: the order price now carries urgency.

The two-leg score is unchanged. Fleet investment uses the same seam (cargo-only probes), so open orders with their prices become the
demand that new ships are bought for. Liners take slices like everyone else.

### 6. What it replaces

| Today | With orders |
|---|---|
| Consumer price curve on stock, `cover_days`, step 23 cover rule | (s, S) quantity, flat order price |
| Step 24 input caps | input ceiling V |
| Emergencies (step 14) | the raise phase of an oxygen or water order, with the faction paying above V |
| Depot fuel buffer target at depots without a factory | order on measured fuel sales |
| Urgency weight in dispatch | removed (variant) |
| Reference prices (step 15) | the seed of the going rate; the table stays |

Unchanged: producers' curve (what ships pay at a source), export markets, residents paying import cost,
faction credit and treasuries, `import_unit_cost`.

## Implementation steps

Each step ends with tests passing and a four-start benchmark (`tools/benchmark.py --days 1460`).

1. **Observe (no behaviour change).** Per station and good: u with sales to ships, L, I, σ, G, plus the
   derived R, S, S_max, N and V. Shown in the station panel and the audit, saved. Check that they make
   sense: Titan electronics, Mars fuel, Earth L1 water, Low Earth Logistics food and metals.
2. **Orders replace the consumer curve.** New `data/economy/orders.csv` (k, δ, ρ, `raise_days`, d, EMA
   windows, `keep_share`, `life_value`, `consume_value`, `give_up_days`, `retry_days`). Order state in
   `StationState`, a `step_orders()` in the tick where `step_emergencies()` is, the piecewise-flat
   `sale_split` for consumers, contracts reserving their slice, and the manifest builder (5b) in place of
   `LOAD_FRACTIONS` and the separate mixed-cargo path. Ceilings all at today's caps (4x, inputs
   12x) at first, so this step measures the mechanism alone. Benchmark.
3. **Ceilings.** V as in section 4, withdrawal of inputs that do not pay, the faction paying above V for
   oxygen and water. Emergencies are folded into this; their events, counts and benchmark columns stay
   under the same names. Benchmark.
4. **Urgency weight off** (variant), then **clean up**: remove `cover_days`, the step 24 caps, the depot
   buffer for depots without a factory, the emergency code; save version bump; roadmap; log of decisions.

## Acceptance checks (1460 days, four starts, against the steps 23–28 final)

- Unmet no worse: 4.4 ± 0.4%, last fifth 2.3 ± 0.9%.
- Mars fuel under 20% unmet in year 4 (60% now).
- Mean unit price paid per (station, good) within 1.5x of its landed reference for routes that are served
  (orders find the carriage cost, not the ceiling); report the ones above.
- No station's stock above S_max except right after a delivery that was contracted below it.
- Larger lots on long routes (mean lot size to Mars, Titan and Ganymede up); share of hold filled per
  departure reported before and after.
- Trajectory flags 0, money drift 0, run time within +10%.

## Decisions (user, 2026-10-15)

1. **Contracts are binding and paid in full**, however late. The station plans around them: it orders only
   for the windows they leave uncovered (section 2), and a late contract does not stop it raising its price
   for a gap before that contract arrives (section 3).
2. **Oxygen and water above V:** the faction pays, as with emergencies.
3. **No minimum slice.** Ships size their own loads (section 5b); small loads are fine when they are the
   ship's best business, not when they come from coarse lot sizes.
4. **No player ships.** The player may later influence ships or stations (for example a station's orders),
   not fly them.

## Later (not in this step)

- Producers post **sell orders** the same way (an ask price that falls while stock piles up).
- **Quotes from ships** (option C): the station picks among offers by price and delivery date.
- Late-delivery pricing, if open point 1 turns out to matter.

## Progress

### Step 1: observe (done 2026-10-16)

Per station and good with an order (net consumers and depot fuel without a factory, export markets
excepted), `StationState::order_stats` keeps u's sales to ships, L, I, σ, G, the last delivery and a
count (saved, save version 11); `Simulation::order_view` derives B, R, S, S_max, N (for a delivery one
lead time from now), when the profile falls below B, and V. Parameters in `data/economy/orders.csv`
(k 1.5, EMA over 4 deliveries, d 0.1, keep_share 0.2, life 24x, consume 4x, depot fuel 4x). Shown in the
station panel ("ORDER PLAN") and in the audit ("Station Orders"). A 730-day run is identical to before
apart from the new table.

What the 1460-day run (start 0) shows, for steps 2 and 3:

- **Mars fuel:** u 18.9/day (ship sales), L 381 d, I 103 d, σ 100 d, so S ≈ 12,000 u, all of it open; the
  profile falls below B in 49 days. Paid 75 cr against a reference of 27 (2.8x).
- **Titan:** 3,350 u of electronics under contract against S = 856 (fifteen years of its use), and
  similar for machinery and metals: today's curve over-ships to Titan. Orders would stop at S_max.
- **Upkeep ceilings at stations with little output are too low:** Mars food V = 20 (reference 64),
  medicine 30, machinery 53. Its residents, not its output, need these. Step 3 needs a floor for
  upkeep (for example consume_value x reference) or a value for residents.
- **The extra buffer is two years for goods that do not spoil:** 10% discount / 5% a year interest
  = 730 days, so S_max = S + 2 years of use. Not "a little extra". Step 2 needs a storage cost, a smaller
  discount or a cap.
- **The lead time mixes sources:** Earth L1 water L = 130 d although Luna is days away; some deliveries
  come from the outer hubs. Per good is enough for now.
- **Going rates against the reference:** Earth L1 water 4.3x, Low Earth Logistics metals 3.2x, Mars fuel
  2.8x; electronics at Earth L1, Venus and Lunar Gateway well below (cheap local supply).

### Step 2: orders replace the consumer curve (in progress, 2026-10-16)

**Committed: the manifest builder (section 5b)**, with order pricing off (`order_pricing,0` in
`orders.csv`). Ships fill the hold by margin per unit, net of marginal propellant (the slope between the
empty plan and a laden one), and score the manifest whole and as its first half and quarter. This
replaces `LOAD_FRACTIONS` and the separate mixed-cargo path. Four starts × 1460 days, against the
step 1 build (which plays exactly like the steps 23–28 final):

| | unmet | last fifth | Mars fuel, last fifth | run |
|---|---|---|---|---|
| step 1 (baseline) | 4.5 ± 1.4% | 3.1 ± 2.3% | 34% | 95 s |
| manifest builder | 4.2 ± 1.0% | 2.9 ± 1.4% | 29% | 101 s |

Trajectory flags 0, money drift 0. Mars Transfer Port's balance still sinks to about −3.7M cr in some
starts (it did before: −3.1M at start 0 in step 1), so the money supply can end 70% under its target.

**Order pricing is written but off.** It covers the order price (it falls step by step while there is no
hurry, and rises ×ρ every `raise_days` while the projected stock falls below B within the lead time and
nobody takes the order, at most to today's caps), quotes that are piecewise flat, resale to ships at
import cost + `resale_margin`, and fleet investment valued on orders. With it on, unmet demand roughly
doubles. The variants tried, each over four starts:

| variant (order pricing on) | unmet | last fifth |
|---|---|---|
| as built | 8.7 ± 1.0% | 3.6 ± 2.2% |
| raise every L/4 days (0.5–5) | 8.4% | 4.5% |
| … and hurry within L + kσ | 8.9% | 6.9% |
| old resale price (curve) | 9.9% | 7.0% |
| bid = old curve price at the projected stock on arrival | 8.3% | 6.3% |
| extra up to 730 days | 8.6% | 4.3% |
| fleet investment on the old curve | 8.7% | 4.6% |
| k = 3 | 9.1% | 5.7% |
| lead, interval and spread not learned (seeds only) | 14.6% | 14.8% |

Bugs fixed on the way: the floor followed the going rate, so a shortage's prices made dumping pay (47,000 u
of food at Low Earth Logistics); a bid held at the going rate only ever rose; the hurry window
(L + kσ) was nearly always open at the outer stations.

What the delivery log shows with order pricing (start 0): 809 deliveries against 1,077, 175,000 u
against 195,000, paid 60.6M cr against 48.6M. Stations pay more for less, and the Earth cluster's short
hops lose the most (Low Earth Logistics fuel 39 deliveries against 81, Earth L1 water 71 against 108).
Because the curve-priced bid does no better, the problem is in the order quantities or the flat
structure of the quote, not in how the price moves. Mars is held back by its fuel in both: ships will not
fly to Mars without fuel there for the way back, so a fuel shortage there also stops its electronics,
machinery and food.

Debug aid: `SPACETRAINS_TRACE_DELIVERIES=1` logs every delivery.

**Investigation, continued (2026-10-16).** Bisecting with order pricing on (four starts each):

| order pricing on, except… | unmet | last fifth |
|---|---|---|
| nothing (as built) | 8.7 ± 1.0% | 3.6% |
| ships paid along the curve (no quote) | 6.5 ± 0.9% | 3.5% |
| … and resale along the curve | 7.4 ± 1.6% | 4.7% |
| … and fleet investment along the curve | 5.0 ± 1.3% | 4.5% |

So the quote and fleet investment's valuation each cost about two points. Found and fixed on the way:

- **Investment valued routes at today's bid**, which in a calm order probes toward the floor, so local
  routes looked worthless. It now uses the going rate (and the bound the higher of bid and going rate).
- **The bid fell on a timer while nobody came.** Earth L1's water bid fell from 21 to 12 cr over 40 days
  as its stock ran out. Now an open order nobody takes rises (every `raise_days`, daily in a hurry), and
  the bid falls a step only while ships take slices and there is no hurry.
- **Phantom follow-ups (also with pricing off):** the two-leg forecast did not check that the ship could
  leave the follow-up's destination again, so ships shuttled empty between Earth L1 and Low Earth
  Logistics, each time for a follow-up to Mars they could not fly. Follow-ups now need fuel to leave.
- **Tankering was a hard rule:** a ship that could not carry its return fuel to a port selling at more
  than twice the origin's price could not go there at all. It now buys there instead.

With pricing off these change nothing measurable (4.3 ± 0.8%, last fifth 2.9 ± 0.8%). With pricing on,
unmet stays at 8.5 ± 2.5% (last fifth 6.8%). Each order's price now behaves sensibly, so the likely cause is
systemic. The fleet is short everywhere, so most orders go untaken and every bid that rises on "nobody
came" ends at its cap. With many stations at their caps there is no relative signal left, and ships choose
by cap level, not by which station is closest to running out. The curve's price is a function of the
state (how depleted each station is), so it ranks them by need.

Debug aid: `SPACETRAINS_TRACE_ORDER="<station>:<good>"` logs an order once a day.

### State-based bid, pricing on (2026-10-16)

The user approved replacing time-based discovery with a bid on the state. An order's price for a delivery
landing at t is the curve's shape around the reference price, applied to the order's own levels:
P(t) = ref × clamp((S / p(t))^1.3, 0.25, price_cap). Here p(t) is the projected stock at arrival with every
earlier contract counted, and S is the order-up-to level. P is flat over the open quantity N(t); the extra
is paid at min(P, G)(1 − d) and anything past that at the floor. The quantities, lead, interval and spread
are still learned. The going rate G is now only observed, and is used for the extra and for ceilings. The
raise timers and `probe_discount`, `raise_factor` and `raise_days` are gone (save version 13).

Fleet investment values a route with the same bid on its projected destination stock. A station resells
an ordered good to ships at its bid on today's stock plus `resale_margin`. Before, it resold at its import
cost plus the margin, so a hub resold dear whatever it had once bought in a shortage.

| 4 starts × 1460 days | unmet | last fifth |
|---|---|---|
| pricing off (curve) | 4.3 ± 0.8% | 2.9 ± 0.8% |
| time-based bid | 8.5 ± 2.5% | 6.8% |
| state bid, investment at G, resale at import cost | 6.7 ± 0.3% | 5.3 ± 1.7% |
| + investment on the bid | 5.2 ± 2.0% | 2.0 ± 2.1% |
| + resale along the curve | 4.3 ± 0.3% | 2.2 ± 0.6% |
| + resale at the bid on today's stock (**on by default**) | 4.0 ± 1.1% | 1.4 ± 1.0% |

Tests changed with pricing on:
- **Emergency test:** the faction pays only above the station's bid, and the bid on an empty stock (4×)
  is already above the 3× premium. The test now checks that bid. Mercury's oxygen recovers by day 170
  (218 with pricing off).
- **Money test:** the 180-day band is now ±35%, up from ±25%. A delivery is paid one flat price, so
  imports, and what residents pay for them, start higher: +32% against +24%.

Still open:
- Mars fuel (about 37% unmet over the last fifth).
- Station money in one start: −79%, from Mars's balance, as before.
- Step 3: the ceilings V replace `price_cap` as the cap, and emergencies fold into orders.

### Step 3: ceilings (2026-10-16)

**V as the whole cap failed.** Under a state-based bid, the cap is what a station pays whenever its
projected stock dips, not a bound that discovery rarely reaches. Input ceilings run to 60–75× reference
(electronics at Mercury, Ceres, Ganymede and Titan: V = 16,000 cr against a reference of about 215; Earth
L1 water: 1,376 against 22). Going rates climbed to match (Titan electronics G = 10,400). Residents pay what
their station paid, so money grew 2–5× in four years in two of the four starts, and unmet rose to 7.8 ± 0.8%.

**What is built:**
- **The cap** is min(today's cap, V): 4×, inputs up to 12×. V only lowers a cap.
  - In today's data no V falls below its cap. Upkeep and consumed goods are floored at consume_value × ref =
    4×, and input V is far above 12×. A run without V is identical. The cap is in place for inputs that do
    not pay; explicit withdrawal (`give_up_days`, `retry_days`) is not needed while none exist.
- **Oxygen and water above the cap are the faction's.**
  - The emergency trigger and close rule stay as in step 14: it opens when the stock forecast 21 days out is
    empty, and closes at 14 days of cover.
  - Under orders the premium starts at the station's cap (its bid on an empty stock) instead of 3×. It rises
    ×1.5 every 15 days with no taker, up to `life_value` (24×).
  - The faction pays the premium above the order price on the open units.
- **Rejected:** a faction offer that follows the bid (up to 24× at once on an empty stock). It opened
  emergencies 3–4× as often (6–9 per run against 1–2) and gave 4.9 ± 1.4% unmet.

| 4 starts × 1460 days | unmet | last fifth | emergencies | money |
|---|---|---|---|---|
| step 2, state bid | 4.0 ± 1.1% | 1.4 ± 1.0% | 1–2 | +26, +15, −79, −12% |
| V as the cap, faction follows the bid | 7.8 ± 0.8% | 5.1 ± 2.4% | 2–4 | +192, +455, +2, −7% |
| cap min(today, V), faction follows the bid | 4.9 ± 1.4% | 1.1 ± 1.3% | 6–9 | +50, +40, −21, −82% |
| **cap min(today, V), emergency premium from the cap** | **4.0 ± 1.1%** | **1.2 ± 0.6%** | 1–2 | +24, +15, −79, −23% |

Emergency test: under orders a ship takes Mercury's oxygen at the station's own bid before the first raise,
so the faction pays nothing; the payment check stays for the curve.

### Step 4 (2026-10-16, one push; decisions are mine, for review)

**4a. Urgency weight off: rejected.** Urgency 1 for ordered goods gave 4.6 ± 1.2% unmet and 1.1 ± 1.0% in
the last fifth (c3: 4.0 ± 1.1%, 1.2 ± 0.6%). Urgency 1 everywhere gave the identical result, since
unordered consumer goods hardly matter. The order bid's ranking is not enough on its own, so the weight
stays.

**4b. Mars fuel: the depot fuel cap.** In a trace (start 0), Mars's fuel bid sits at its cap (4× its 26.8
reference, 107 cr) from day 1. The stock is empty from about day 300 to day 870. Only five tanker
deliveries came in four years (6,000 u against 12,500 u of use), 250–450 days after their contracts. The
order quantity is fine (S stays at 6,000–9,000 u), so the price is what holds supply back. Depot fuel now
takes `depot_fuel_value` × ref as its own cap, not min(today's cap, V):

| depot cap | unmet | last fifth | Mars fuel, last fifth | money by start |
|---|---|---|---|---|
| 4× (c3) | 4.0 ± 1.1% | 1.2 ± 0.6% | 40 ± 16% | +24, +15, −79, −23% |
| 6× | 4.1 ± 1.5% | 1.3 ± 1.1% | 21 ± 22% | +26, −27, −2, +39% |
| **8× (chosen)** | **3.3 ± 0.9%** | **1.7 ± 1.2%** | **8 ± 17%** | +13, −56, +12, +6% |
| 12× | 4.1 ± 0.9% | 0.7 ± 1.0% | 5 ± 10% | −11, −88, −91, −28% |

At 12×, Mars pays enough to sink its own balance. A fleet cap of 120 ships (from 90) at a 4× depot cap
gave 4.7 ± 1.2% unmet and 19% Mars fuel, so fleet size is not the constraint. Note for setting up
variants: a work dir needs the `tests` symlink as well as `build`; without it, plasma ships cannot plan
and the fleet collapses (27% unmet, 30 s runs).

**4c. Cleanup.** Three commits:
1. Orders are always on: the `order_pricing` switch and the curve's emergency split are gone. An emergency's
   premium starts at the station's cap. Depot fuel is ordered at every station without a fuel factory.
   Neutral (3.3 ± 0.9%).
2. The step 24 caps are folded into the orders. An input's cap is its ceiling V, kept within
   [`price_cap`, `input_cap` = 12]. With `keep_share` 0.75, V is step 24's quarter of the output value.
   - The result is identical (3.3%).
   - V at 80% of the margin, with no lower bound of 4×, gave 3.9% unmet and failed the money test, though its
     last fifth was better (0.7%).
3. Cover days, `cover_round_trip_factor` and `depot_buffer_units` are removed. A fuel factory keeps its buffer
   (`factory_buffer_days`). Stations start at their seeded order-up-to level S instead of the cover target,
   which gives distant stations about 1.9 reference round trips of stock instead of 1.2. The bridge's
   `target_stock` shows S for ordered goods.
   - Unmet 1.5% (1.1–1.6) against 3.3%; the last fifth is unchanged at 1.7%, so the gain comes from the early
     years.
   - No Mars fuel shortage in the last fifth.
   - Money ends +20 to +49% (the 180-day test stays inside ±35%).

The save format is unchanged (only data keys went), so the save version stays at 13. The data fingerprint
changes with the keys.

**4d. Acceptance checks** (final code, 4 starts × 1460 days; scratchpad `accept.py`), against the step-1
baseline (curve pricing, old load fractions):

| check | step 1 (curve) | final | verdict |
|---|---|---|---|
| unmet | 4.5 ± 1.4% | 1.5% | met (target ≤ 4.4%) |
| last fifth | 3.1 ± 2.3% | 1.7% | met (≤ 2.3%) |
| Mars fuel, last fifth | 34% | 0% | met (< 20%) |
| price paid within 1.5× of reference | 40% of station goods (median 2.0×) | 16% (median 2.4×) | **fails** |
| no stock above Smax at the end | 51 cases | 63 cases | **fails** (floor-price dumps) |
| larger lots on long routes | Mars 1,025 u, Ganymede 663 u, Titan 534 u | Mars 1,160 u, Ganymede 940 u, Titan 535 u | met (Titan flat) |
| hold share per arrival | 74% | 66% | lower (more big hulls on short hops) |
| flags, drift, run time | 0, 0, 95–104 s | 0, 0, 106 s | met |

**Prices** are high because the bid pays the reference only when a delivery lands on a full stock (S), and
the capacity-short fleet usually delivers near the safety stock. Bids centred lower lost supply:
- centre B: 5.7 ± 1.6% unmet;
- centre R = B + uL: 4.3 ± 1.3%;
- against 3.3% at the time.

High prices are what pull ships in here. Residents pay what their station paid, so money runs higher too.

**Stocks above Smax** are mostly floor-price dumps (Mercury food, Ganymede oxygen). The floor is 0.25 ×
reference beyond the extra, and that still pays when the cargo was cheap.

For review:
- Is the price level acceptable, given that unmet demand is the best so far?
- Should the floor beyond Smax be lower (or zero) to stop dumps?
- Should money growth be checked (+20 to +49%)?
