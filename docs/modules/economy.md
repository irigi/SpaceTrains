# economy

How goods are made, priced, ordered, carried and paid for, and how money moves. Ships' own costs, dispatch and
fleet investment are in `ships_and_fleet.md`.

**Code:**
- `src/economy/EconomySystem.{hpp,cpp}`: production, price curves, surface markets, reference prices;
- `src/simulation/Market.cpp`: station orders, emergencies, the daily market among ships, reference prices;
- `src/simulation/Households.cpp`: residents, colonies, labour, station incomes, settling the local economy;
- `src/simulation/Contracts.cpp`: liners and rotation contracts;
- `src/simulation/FleetInvestment.cpp`: treasuries, credit, taxes (and fleet investment);
- `src/simulation/MoneyJournal.{hpp,cpp}`, `Statistics.cpp`: the money journal and statistics.

**Data:** `data/commodities/`, `data/recipes/`, `data/economy/` (one CSV per topic, `key,value` where it is a list
of settings). Optional keys default to the values in `domain::*Definition` (`src/domain/Definitions.hpp`).

**Target:** physical and economic feasibility. Uneconomic choices (long waits, year-long hauls for little) must lose
on cost, not be cut off by rules.

## Goods and prices

- **Base prices** are Earth's surface price plus launch, at the ships' scale of 0.00078 cr/$
  (`commodities.csv`; surface $/kg in `surface_markets.csv`). Launch costs $100/kg up and $50/kg down at Earth,
  $40/$20 at Mars, $20/$10 at Luna, $5/$5 at Ceres (`background_economies.csv`). Examples: food 8.4 cr per 100 kg,
  electronics 64 cr per 20 kg, platinum 249 cr per 10 kg, helium-3 2,340 cr/kg.
- **Producers' price curve.** What a ship pays at a producer: `reference × clamp((target / stock)^1.3, 0.25,
  price_cap)` (`pricing.csv`: `price_cap` 4). Trades integrate the curve over the stock they move
  (`get_trade_value`), so a big lot moves along it. A producer's target stock is 14 days of output.
- **Reference prices** (`reference_prices.csv`): for each consumer and good, the base price plus `carrier_margin`
  (1.5) × the cheapest cost per unit of carrying it from one of its two nearest producers (a full hold out in a
  standard hull of at least 150 u, back empty, median over 6 departure dates). They seed each order's going rate.
  - *Why precomputed:* thousands of plans per start would slow every start and test. Regenerate with
    `spacetrains_headless . --write-reference-prices data/economy/reference_prices.csv` after changing stations,
    recipes or ship classes (it reproduces the file exactly).
  - Depot fuel at stations without a factory is centred on the factory's price plus the carriage.
- **Fuel factories** (`fuel_factories.csv`) price their propellant at local cost (`price_cr`: Luna and Ceres 2.5,
  Venus, Titan and Ganymede 3.0, Mercury 3.5 cr per 100 kg, against Earth's launched 7.9) and fill their depot up to
  `factory_buffer_days` of output. Fuel stands in the depot, outside cargo storage. Ships' fuel purchases (60-day
  average) count as demand when a fuel run is valued. *Why:* fuel is made only at a few places, and lifting it from a
  planet costs extra (user, 2026-10-07).

## Production

- **Recipes** (`recipes/recipes.csv` by economy profile, `station_recipes.csv` per station) scale with population
  ÷ 10,000. A consumed row has a **role**:
  - `input:<outputs>`: feedstock; the outputs stop without it;
  - `upkeep`: life support, maintenance, power; a shortage penalises every output of the station;
  - `consume`: demand with no effect on output (construction metals, samples at Luna and Mars).

  *Why maintenance goods are upkeep:* machinery, electronics and reactor fuel are spare parts and power, so a
  shortage costs output gradually instead of stopping it. Chemical hubs (Venus, Titan) process the local atmosphere
  and have no hard input.
- **Availability** of a consumed good: stock over a 7-day buffer, in [0, 1]. **Upkeep multiplier:** the product of
  `1 − (1 − m)(1 − availability)` over the upkeep goods, `m` from `upkeep_penalties.csv`. The penalties multiply, so
  a station short of several goods falls very low but never stops (user's choice).
- **Each output** runs at the lowest availability of its inputs × the upkeep multiplier × the share of jobs filled
  (`employed_share`, below), and stops while storage is 85% full or the stock is above 90 days of output (720 for
  export goods). A plant that is not running uses no feedstock; upkeep and consume goods are used at the full rate.
  `StationState::output_factor` and `upkeep_multiplier` show the result.
- **Production is 8-10× use.** *Why:* cutting it (to 3× or 5×, 2026-10-08) removed the slack that covers goods in
  transit, spoilage and imperfect dispatch, and the Earth cluster spiralled into shortages. `tools/rebalance_production.py`
  can scale it if that is wanted later.
- **Supply following price** is built but off (`households.csv` `supply_price_elasticity` 0). With production at
  8-10× use it idled plants and cut jobs everywhere; it needs capacity sized to use first.
- **Machinery** wears at 0.05% a day in stock.
- **Unmet demand:** stock-outs of upkeep and consume goods, and for an input the output it failed to feed.
- **Stations start stocked** (`Simulation::starting_inventory`): every ordered good at its order-up-to level S
  (below), within 85% of storage. *Why:* starting at the target stock was the largest single gain in supply
  (2026-10-08).

## Station orders

Each station keeps, for every good it consumes, an **order**: how many units it wants, by when, at what price.
Ships take slices of it as contracts at departure and are paid that flat unit price on arrival. Settings:
`orders.csv`.

- **What the station observes** (its own experience, so a station that ships rarely visit plans for rare visits):
  use per day (recipes plus what ships buy there), lead time from contract to delivery, interval between
  deliveries, their spread, and the going rate it has paid. Each is an average over the last `ema_deliveries`.
- **Quantity: an (s, S) policy over the projected stock.** The projected stock is today's stock falling at its use and
  rising at each contracted arrival.
  - Safety stock B = use × `safety_factor` × spread; reorder point R = use × (lead + k × spread); order-up-to
    level S = R + use × interval, capped by storage and `max_cover_days` (`pricing.csv`).
  - The open quantity for an arrival at time t is what keeps the projected stock above B until the next usual order
    could arrive. With no contracts that is S − stock. A slow freighter due later leaves a bridging order for the
    gap before it, and a ship that can only arrive after it sees what the freighter does not cover.
  - An **extra buffer** up to S + use × E, bought only at a discount (`extra_discount`); E is where holding cost
    (interest plus spoilage) equals the discount, at most `max_extra_days`.
  - Quantity never depends on price, so a low price cannot produce a big stock: it produces no takers.
- **Contracts are binding** (user, 2026-10-15): once a ship takes a slice, the station pays it in full on arrival,
  however late, and orders only for the windows its contracts leave uncovered.
- **Price discovery.** The bid moves around the going rate: a little below it while there is no hurry (probing for the
  cheapest willing carrier), raised while the projected stock dips below B and nobody takes the open quantity, at
  most to the **ceiling V**. A taken slice resets the clock. The ceiling is not the price: the station raises only
  until a ship comes.
- **Ceilings V:**
  - input: the value of the outputs one unit makes less the other inputs, × (1 − `keep_share`), within
    [`price_cap`, `input_cap`] × reference;
  - upkeep: the output lost per unit short;
  - oxygen and water (`emergency` in `upkeep_penalties.csv`): up to `life_value` × reference, the faction paying the
    part above the station's own ceiling;
  - consumed: `consume_value` × reference;
  - depot fuel at a station without a factory: `depot_fuel_value` (8) × reference. *Why 8:* at 4× Mars ran out of
    fuel for most of two years; at 12× it paid enough to sink its own balance.
- **What a ship is paid** (`sale_split`): up to the open quantity at the bid, then into the extra buffer at a
  discount, beyond that at the floor (0.25 × reference), so no ship is ever stuck with cargo. Contracted arrivals,
  and a rotation contract's deliveries still to come, count in the projected stock (each lot decays at its carrier's
  hold rate).
- **Emergencies** (`Simulation::step_emergencies`): when oxygen or water's projected stock runs out within 21 days,
  the faction offers a premium above the station's cap for 30 days of use, raised 1.5× every 15 days nobody takes
  it, up to `life_value`. It closes when 14 days are covered.

## The market among ships

Switches in `market.csv`; the design and its measurements were decided on 2026-10-19 and confirmed by the user.
*Why:* with posted bids alone, stations paid about 2.7× the landed cost. The price was rent: a ship's alternative
was another rent-priced delivery, so the rent sustained itself. Competitive pricing with rotation contracts supplies
as well while stations pay about a third less, and ships still earn several times their operating cost per unit.

- **Competitive pricing** (`competitive`): once a day each order gets a market price from the ships that could serve
  it, the second-lowest ask (a single bidder's ask × (1 + `solo_premium`)).
  - Bidders: ships docked at a source of the good (`min_source_surplus` spare) or arriving at one within
    `available_days`.
  - A ship's ask: the price at the source plus the leg's time and fuel cost, × (1 + `return_share`) for the way back,
    spread over `fill_ref` of the hold or the open orders the source can fill if fewer, × (1 + `ask_markup`).
  - The order pays at most the market price × an urgency factor, clamp((S ÷ projected stock at arrival)^
    `urgency_elasticity`, 1, `urgency_max`), within its own bid.
- **Rotation contracts** (`contracts`): a ship looks for a loop of up to `contract_max_stops` stops from its port
  every `contract_review_days` (staggered by ship), values each from cached leg estimates (the best
  `contract_refine` again with real launch windows), fills each leg by margin with what the next stop still needs
  under no contract (× `contract_cover`), prices deliveries cost-plus (`contract_cost_plus`, at most the going rate),
  and signs if the loop earns at least `contract_accept` × its best spot mission. It then flies only its next stop;
  contracted units load first as their own lot. The contract ends after `contract_term_days`, or when a leg cannot
  be flown.
- **Follow-up horizon** (`follow_up_horizon_days`, off): discounts a mission's forecast second leg by its start.
- **Tried and removed** (measured, no gain or worse): a lead-time cap, scaling the stock-ratio bid down (stocks fall
  until the price comes back), escalation, binding tenders, own-ask floors, anchoring the market price on the
  reference, urgency against the safety stock, holding source stock for contract ships.
- **Scheduled liners** (`liners.csv`): a seed ship flies its loop forever, carrying whatever pays for the next stop
  (or nothing); never laid up, sold or refitted; outside the fleet cap; its faction tops its cash up.

## Background economies and colonies

`background_economies.csv`, `surface_markets.csv`, the `colony_*` keys in `households.csv`.

- **Accounts:** Earth's surface (the old outside account), the Mars and Luna surfaces, the belt miners at Ceres.
- **Earth** keeps its gateways (Low Earth Logistics, Earth L1) stocked with `surface_depth_units` of every good it
  sells, outside the station's storage, and is there the curve's centre and its cap: Earth never sells dearer than
  its ask. It **buys only space goods** (platinum, deuterium, samples, helium-3, microgravity products) at a flat bid,
  plus propellant for its own traffic at Low Earth Logistics (0.9 × ask, 100 u/day). *Why:* stations make ordinary
  goods for free, so an Earth bid for them would print money.
- **The colonies mine; the gateway trades.** Lunar Gateway and Ceres make no metals, water, helium-3 or platinum;
  Luna surface and the belt sell them there (Earth-bound goods at about 70% of Earth's price). A good the colony sells
  and the station does not make is centred at ask × `colony_centre_markup`, and the station stocks `colony_stock_days`
  of the colony's sales so a ship can collect a worthwhile lot. *Why:* when the stations made them, the colonies
  almost never sold.
- **Colony imports:** bid at 1.2 × the gateway's reference price, worth that bid to the gateway's order even where
  the station uses none itself (Mars Transfer's reactor fuel sat at the floor and was never delivered before).
  Imports follow money: affordability f = (account + credit line) ÷ (90 days of imports + credit line); essentials
  (food, medicine) buy at min(1, 2f), the rest at max(0, 2f − 1), so a colony slows down instead of stopping dead.
  Colonies may go down to minus their credit line and get yearly Earth investment (Mars ran dry within three years
  without it). A colony keeps its gateway stocked with 30 days of the station's own use.
- **Welfare:** satisfaction is the value bought ÷ the value its people want (90-day average); growth and recruiting
  × (2 × satisfaction − 1); the wage median follows income per head (× √(income ÷ base imports), clamped 0.5-2,
  relaxing over a year). Sales and imports scale with the colony's people.

## Households and labour

`households.csv`, `labour_market.csv`, `labour.csv`.

- **Residents are an account of their own.** The station pays wages (employed × wage, a benefit to the unemployed);
  residents pay for the goods they use up at the station's **import cost** (deliveries averaged over about 30 days of
  use) and spend savings above a 90-day buffer back at the station within about 2 days. *Why import cost:* at the
  reference price every chronically short station lost money on each import; at import cost it breaks even on
  average.
- **Demand follows income and price:** `consume` goods (wage ÷ start wage)^1 × (price ÷ reference)^−1; upkeep goods
  barely respond (−0.1).
- **Jobs and wages:** every station off Earth has jobs and a wage. Jobs grow 3%/yr while the station is supplied,
  above its floor and fewer than 5% stand open; they are cut 5%/yr below 70% supply or below the floor. Wages follow
  the vacancy rate. Science stations (Titan, Ganymede) keep only the crew their research needs (no growth, wages held).
  Starting wages (multiples of 0.5 cr/day: 1.0 cislunar, Venus, Mercury; 1.2 Mars; 1.4 Ceres; 2.0 Ganymede; 3.0 Titan)
  are set so that a job seeker at Earth's 30th wage percentile gains the fare over a 10-year stay.
- **Output follows employment:** outputs and their inputs scale with jobs ÷ workers (at most 1, in half-percent
  steps); consumption follows residents.
- **Who moves:** settlers come for vacancies when (station wage × (1 − hardship) − their Earth wage) × a 10-year stay
  pays the passage, Earth wages lognormal (median 0.107 cr/day, σ 0.9), so a low-paid worker may move where a well-
  paid one would not; the unemployed go home; visitors travel when half a year's income covers the trip. Colonies'
  people (Mars 50,000, Luna 10,000, belt 5,000) take a gateway's vacancies when the job repays the ascent, and the
  gateway's unemployed go down. A colony recruits part of its growth on Earth and pays their passage. Travellers pay
  from their own accounts within a 3% travel budget. Passenger traffic itself: `ships_and_fleet.md`.
- **Shipyards** (Mercury Sunward Yard, Low Earth Logistics): a new ship pays its nearest yard half its price; the yard
  builds it from metals, machinery and electronics worth 20% of the price out of its stock, Earth supplies what it
  lacks, and the rest of the price goes to Earth.

## Station incomes

`station_incomes.csv`, paid by Earth, sized from each station's trade deficit:
- science budgets: Titan and Ganymede as science stations (they centre import prices 50% above landed cost,
  `science_price_premium`: "Earth is paying"), Mercury, the Venus cloud base;
- relay services: Ganymede, Mercury, Earth L1; settlement: Mars, Ceres; tourism and survey: Luna. Visitors also
  spend during their stay.
- Science and settlement grants are **need-based**: full below half the credit ceiling, nothing at it. Services are
  paid in full.
- Helium-3 (Luna), microgravity products (Earth orbit, Earth L1) and the science samples Titan, Ganymede, Ceres and
  Mercury make from electronics sell to Earth: light, valuable goods for the outposts' return legs.

## Money

`open_economy.csv`.

- **Exact accounting:** every transfer has a payer and a payee. Stations + residents + ships + treasuries +
  background accounts always equal the seeded money (the audit's drift is 0). The open economy is explicit: Earth and
  the colonies are the accounts on the other side of incomes, launches, hulls and travel.
- **Faction treasuries:** tax station cash above 750k (375k while the faction is in debt); top up stations below 25k
  (by at most the core crew's goods and wages per day while the faction borrows); cover a station in the red within
  2 days while they can borrow the gap; borrow down to minus (50% of their fleet's hull value + stations' stock at
  reference + 2 years of taxes) at 5%/yr; buy ships with cash plus credit. *Why the quick cover:* remote stations
  take lumpy deliveries, and a slower band left them weeks in the red after each one: cash-flow timing, not
  insolvency.
- **The money-supply controller is off** (`money_supply_days` 0). *Why:* its per-head taxes pushed Mars into job cuts;
  money now moves only through named flows.
- **Money journal** (`MoneyJournal.{hpp,cpp}`): every money move books itself with its account pair and one of 37
  categories (trade, residents, Earth, travel, owners, factions, investment); balances stay where they were, the
  journal only records (the game is byte-identical without it). Accounts in data order: Earth, each colony, each
  faction, each station, its residents, each faction's fleet. Each account's balance = seed + received − paid, and
  each category's total in equals its total out (`tests/economy_test.cpp`).
- **History:** every 30 game days, balances, the month's flows by account and category, a 6 × 6 sector matrix
  ("who pays whom"), each station's people, workers, jobs, wage and lifetime profit, the colonies' people. Saved with
  the game.
- **Profit definitions:** a station's is cash gained + taxes − subsidies + its stock's value gain against the stock
  it started with (before the faction's transfers); a faction's operating result is everything but investment
  (hulls, working capital, salvage); a ship's is `lifetime_profit` (dividends and liner top-ups are owner transfers).

## Growth and events

- **Growth** (`growth.csv`): population grows `growth_per_year` while the worst upkeep good's availability (90-day
  average) is at least `grow_above` and the station is above its floor; shrinks `decline_per_year` below
  `decline_below`; bounded by the core crew and `max_population_factor` × the seed. With passengers on, growth off
  Earth happens as settlers land. Slow on purpose: growth needs supply to keep up, so it cannot run away.
- **Events** (`data/events/`): outages, booms, demand spikes, settler migrations, with a rate per year and a duration
  range, from a seeded generator (saved). They act only through production. `--no-events` switches them off.

## Reports and tools

- `--econ-audit`: stations (stock against S, cover, shortages), ships, residents, colonies, passengers, the
  transport-capacity balance and the Accounts section (each account's balance, flows and top categories).
- `--stats-json <path>`: the statistics JSON; the bridge writes the same as `<snapshot>.stats` while the UI's
  Statistics screen is open.
- `tools/benchmark.py`: 4 starts × 1,460 days; judge economy changes with it, not with single runs (they are chaotic).
- `tools/purchase_returns.py`: purchases' expected against earned returns by ship group.
- `tools/rebalance_production.py`: scales profile industries to a multiple of system use (not applied in data).
- Debug: `SPACETRAINS_TRACE_ORDER`, `SPACETRAINS_TRACE_CONTRACTS=1`.

## Open points

- Luna surface's money piles up (later: imports that rise with income).
- Industries as accounts of their own; supply following price (needs capacity sized to use).
- Producers posting sell orders; quotes from ships; late-delivery pricing.

## Tests

`tests/economy_test.cpp`: production and consumption, roles and penalties, orders, the money identity and
journal, no station 30 days in the red in 180 days, money not running away.
