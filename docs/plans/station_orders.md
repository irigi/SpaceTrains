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
