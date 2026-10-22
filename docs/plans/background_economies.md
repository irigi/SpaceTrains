# Planetary background economies (plan, 2026-10-22)

## Why

The simulated stations are small (5,000-22,000 people). Behind them, a few bodies have economies the game does
not simulate in detail: Earth's surface (very large), a Mars surface colony, Luna's surface, and the belt's
independent miners. Today they all appear as one **outside account** with no limit:
- residents spend from it;
- visitors and emigrants pay their fares from it (83% of all fares in a 4-year run);
- the export markets buy from it at fixed prices.

Nothing behind that account has an income, a price level, or jobs. So:
- demand does not depend on prices;
- migration has no gain to weigh against the fare;
- the outside money has no limit.

**Goal:** replace the outside account with a few **background economies**. Each is an effective model with a few
parameters:
- goods markets with supply and demand that respond to price;
- a population with **distributions** of income by occupation, not averages;
- jobs whose offers attract workers from elsewhere;
- its own money account, so its spending is limited by what it earns.

The audit's money identity stays exact (drift 0).

**Scope.** The background economies are not simulated industries. They are price and wage schedules seen from the
gateway stations, plus flows of people and money. Only some bodies get one. Small colonies stay stations as today.

## The economies

Starting set (sizes are estimates for the user to confirm; the colonies stay small):

| Economy | Gateway stations | People | Launch to orbit | Character |
|---|---|---|---|---|
| Earth surface | `earth_orbit`, `earth_l1` | ~10⁹ (only its space-facing demand matters) | high | sells everything; buys rare metals, deuterium, samples; huge, elastic |
| Mars surface | `mars_transfer` | ~50,000 | medium | farming and industry, pays well for skills, imports electronics and machinery |
| Luna surface | `luna_base` | ~10,000 | low | mining, regolith oxygen and metals, tourism |
| Belt miners | `ceres_depot` | ~5,000 | ~0 | scattered prospectors; buy machinery, fuel and food; sell platinum and metals |

Venus, Mercury, Ganymede and Titan have no background economy: their stations are the economy.

## Model

### Goods: prices at the gateway

For each economy and commodity there is a domestic market with constant-elasticity curves:
- supply S(p) = S₀ (p/p₀)^σ;
- demand D(p) = D₀ (p/p₀)^ε.

Net exports to orbit X shift the surface price p. Each day the price relaxes toward the price where
S(p) − D(p) = X (the averaged orbital flow):
- **Earth:** S₀ and D₀ are enormous, so p barely moves (fixed price, unlimited volume, at a cost);
- **Mars:** a few hundred tonnes a year moves its price noticeably.

At the gateway station the economy posts:
- an **ask** (it sells) = surface price + launch cost per kg × unit mass;
- a **bid** (it buys) = surface price − landing cost per kg × unit mass.

These go into the station's market like the existing export markets. Ships see them as a seller or buyer with a
deep book. The station's own curve still sets local prices, but it can never rise above the ask or fall below the
bid (arbitrage against the surface). This also delivers the roadmap item "anything can be bought at the Earth
stations for its surface price plus launch".

- Launch and landing costs are per kg per body (data), with a decision on how they relate to the game's own
  Kepler/plasma costs (a surface launcher is outside the simulation).
- `export_markets.csv` becomes a special case: platinum, deuterium and samples bought at Earth's bid.

### People: occupations and wage distributions

**Occupations** (a short list, for example): unskilled, technician, engineer, specialist, management. They map onto
today's `labour.csv` grades.

In each location (background economy or station) and occupation:
- **wages are lognormal**, with a median and a spread σ (dispersion, about 0.4-0.6 on Earth, data);
- **job offers to newcomers** form their own lognormal: their median, spread and number of openings per year.

**Distributions, not averages.** A Mars engineer job can pay well above the Earth median even if the Mars median
for engineers is lower:
- what matters for one worker is their own draw against the best offer they find;
- an Earth engineer at the 20th percentile may gain by moving to Mars, while one at the 80th does not.

### Job migration

A worker at A in occupation k considers an offer at B. They move if the present value of the wage gain over the
contract or expected stay T, less the hardship at B and the generalized trip cost (fare, time, dose, recovery:
`passenger_nonfare_cr` as today), is positive:

```
gain = (w_B − w_A) × T_eff − hardship_B × T − trip_cost > 0
```

**Flow per day:**
- **rate**: openings at B × P(the best applicant from A gains);
- the applicant pool is everyone in occupation k at A who searches (a search rate per year, data);
- computed by Gauss-Hermite quadrature over A's distribution (8 nodes) with the lognormal tail in closed form for B.

This is cheap: about 5 occupations × 13 locations × pairs, once a day.

- **Budget for the fare** = the traveller's own gain (surplus), not a multiple of the reference fare. The surplus
  quantiles become the passenger bands, so the existing queue and boarding code keeps working.
- **Who pays:**
  - the worker, from savings: the economy of origin's account;
  - or an employer who wants them: a station or a Mars firm. The sponsored settler becomes an employer paying a
    hiring bonus whose cap is the employer's own surplus (the vacancy's output value).
- **Return flows come from the same rule.** A worker whose station cut wages, or failed (vacancies cut, supply
  index low), finds Earth offers better and leaves. They pay from savings; a ruined station pays nothing. This
  replaces "emigrants are the best-paying passengers".
- **Rotation** stays for short contracts, but as an occupation-k job with a tour premium. The station chooses rotation
  or permanent hire by cost per position-day (both are known).

### Visitors and other discretionary travel

**Income distribution.** Each economy has a lognormal household income. A trip is taken by those whose travel
budget covers its generalized cost:
- budget = share of income × years saved;
- count = population × propensity × P(budget ≥ cost).

At today's fares (≈5 years of median income) only the top tail travels. So visitors become a small, wealthy group,
and how many there are depends on the fare.

**Purposes** (for the later population work):
- tourism;
- family visits: they scale with the migrants a place has received, a real feedback;
- business: it scales with trade volume between the pair.

### Money

Each background economy has an account:
- it **earns** from what ships buy from it (asks) and from its residents' wages paid in orbit, for those who work
  on stations;
- it **spends** on what it buys from ships (bids), on fares its people pay, and on hiring bonuses.

Income outside space trade grows exogenously (data); for Earth it is effectively unlimited, but recorded.
- Small economies (Mars, Luna, belt) can run out. Their bids then drop with their money, like the station budget gate.
- The outside account becomes the sum of these accounts plus the remaining true outside (ship purchases, salvage,
  interest), so the identity is unchanged.
- Station residents still spend as today, unless `docs/plans/economy_points_7_10.md` is built too. That plan's
  residents would be paid by their station's producers, and these economies would be the other side of their
  trade.

## Data

- `data/economy/background_economies.csv`: id, body, gateway stations, population, growth per year, launch and
  landing cost per kg, income median and σ, travel share, search rate.
- `data/economy/background_markets.csv`: economy, commodity, p₀, S₀, D₀, σ, ε, relaxation days.
- `data/economy/occupations.csv`: occupation, grade, and per location the wage median and σ, the offer median and
  σ, openings per year, hardship.
- Each row marked sourced / derived / estimate in the module doc, as for passengers.
- Switch `background_economies,0|1` (off ⇒ byte-identical audit), with sub-switches for goods, jobs and visitors.

## Build order (benchmark from step 2 on)

0. **Calibration (user decisions first).**
   - Today's base prices against Earth surface prices × 0.00078 cr/$ + launch: food is 50 cr per 100 kg, about
     $640/kg, while surface food plus a $200/kg launch is about 0.16 cr/kg. Either base prices fall a lot (the
     Earth stations become cheap for everything, and the fleet's revenue shifts), or the credit is not tied to the
     ship-price dollar for goods.
   - Wages: 0.5 cr/day is ≈ $234k/yr at the ships' exchange rate.
   - Write down the decision; size launch costs and the curves from it.
1. **Data and types,** switch off byte-identical, save version +1 with empty fields.
2. **Goods at the gateways:** asks and bids, surface price relaxation, Earth export markets folded in. Measure: unmet
   demand, the Earth stations' prices, fleet revenue.
3. **Accounts:** split the outside account, finite Mars/Luna/belt money, ledger lines in `--econ-audit`.
4. **Occupations and wage distributions,** station offers from today's grade wages × a hardship premium.
5. **Job migration** replaces the settler and emigrant formulas (and step 27's growth thresholds where people move
   by ship). Passenger bands from surplus quantiles.
6. **Visitors from income** (tourism, family, business).
7. **Rotation against permanent hire.**
8. **Audit and UI:** per economy prices, account, migration in and out by occupation, the gain distribution of
   people who moved; inspector panel for gateway stations. Docs and memory.

## Verification

- **Unit tests:**
  - the lognormal tail and expected gain against Monte Carlo;
  - quadrature flows against a brute-force sample;
  - ask/bid arbitrage (a station price never crosses them);
  - money moves exactly;
  - the switch-off identity.
- **Every step:** ctest, `--trajectory-audit` 0 flags, drift 0, save/load identity, `tools/benchmark.py` (4 starts),
  `tools/purchase_returns.py` (fare and price changes move valuations).
- **Plausibility checks (not inputs):**
  - migration rates of a few % of population per year;
  - movers mostly from the lower half of the origin's distribution for their occupation;
  - visitors' count falls when fares double.
- **Speed:** the slowest 20-day stretch stays above 5 days/s.

## Station income (user, 2026-10-22)

Earth imports and exports at real prices will upset today's balances, so stations get new income from Earth or
from a background economy. Chosen:
- science budgets: Earth agencies pay staff and supplies at high prices, within a yearly cap;
- propellant for Earth's own space traffic, delivered to low Earth orbit;
- shipyards and ship services in space;
- microgravity manufacturing for Earth;
- tourism and visitors' spending at stations;
- settlement subsidies (presence and claims);
- Earth investment in ventures;
- migrants' savings;
- platinum metals at real prices;
- relay, navigation and traffic services;
- helium-3 at high value where it is plausible.

Rejected: data centres in space, beamed solar power. Venus: under discussion (is a cloud city plausible with
reusable chemical ascent?).

## Open questions for the user

- Sizes and characters of the four economies, and whether Venus or Titan get one.
- The price-scale decision in step 0.
- Whether the user wants occupations here or waits for the population-composition work (EU2-style). Recommendation:
  build the occupation list here, small, so that work extends it.
