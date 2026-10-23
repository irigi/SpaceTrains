# Economy overhaul: log (2026-10-22 night → 2026-10-23)

Plan: `economy_overhaul.md`. Decisions taken without the user (they are asleep) are marked **Decision** with
their reason.

## Morning summary (for the user)

All goals are met and committed on `feature/economy_overhaul` (not pushed). Read the decisions below and change
any you dislike. The main calls taken overnight:
- goods repriced to Earth's real prices plus launch;
- Earth buys only space goods;
- incomes sized to need, with tapering grants;
- purchase valuations capped at the landed cost;
- the money-supply controller off;
- a 3% travel budget;
- colonies with people;
- local fuel at 2.5-3.5 cr per 100 kg;
- Titan makes its own machinery.

| | Before (main) | After |
|---|---|---|
| Unmet demand, 4 starts × 4 years | 1.1% (last fifth 2.1%) | 0.2% (last fifth 0.0%) |
| Ships profitable | 76% | 69-72% (76% over 8 years) |
| Stations in the red at the end | (not tracked) | none; worst ~12 days in 4 years |
| Boardings, 4 years | ~16,000 | ~4,500 (mostly cislunar) |
| Speed, slowest 20-day stretch | 5.1 days/s | 6.0 days/s |
| Money drift, save/load, TSan | 0, identical, clean | 0, identical, clean |

## Goal checklist (updated as work goes)

| Goal | State (2026-10-23 02:20) |
|---|---|
| Real prices, Earth market | done: Earth sells at surface + launch, buys space goods; colonies' surface markets |
| Closed money, wages, households | done: exact identity; ships pay crews; residents spend wages; controller off |
| Station incomes | done: science (need-based, science stations pay a premium), relay, settlement, tourism, yards build from goods, microgravity, helium-3, LEO propellant (85,000 u in 4 yr), colonies' investment, migrants' savings |
| Venus | done: Venus Orbital Exchange, cloud science base, cheap scooped propellant |
| Labour, wage distributions, migration | done: jobs, wages from vacancies, lognormal gains and incomes, colonies' people move to their gateway's jobs, colonies recruit on Earth |
| Demand follows income and price | done; supply response built, off |
| Stations not bankrupt | done: no station in the red at the end; worst ~12 days in the red per 4 years |
| Old checks | unmet 0.2%, drift 0, tests pass, 6.0 days/s slowest 20-day stretch, trajectory flags 0.2/run (low-κ Mercury spirals) |
| Passengers still travel | ~4,600 boardings in 4 years, mostly cislunar; settlers and colony recruits interplanetary |

## Baseline (main 50d876c, 2026-10-22)

Start 0, 1,460 days:
- unmet 1.1%; passenger fares 18.0M cr; cargo margin 33.3M; residents' spending 27.2M (outside money);
- 112/137 ships profitable; money supply −13.4% against target; drift 0.

## Decisions

- **Decision (22:50):** no byte-identical switch-off for the overhaul as a whole. Base prices change in data, so
  an old run cannot be reproduced without the old data. The mechanisms still get switches where it is cheap, and
  tests that need the old economy use a data copy, as `passengers_off_data()` does.
- **Decision (23:00):** Earth buys only space-sourced goods (platinum, deuterium, samples, helium-3,
  microgravity products) and low-orbit propellant for its own traffic (0.9 × ask, 100 u/day at Low Earth
  Logistics). It does not buy ordinary goods: stations make them for free, so a bid would print money.
- **Decision (23:00):** the small colonies' imports are bid at 1.2 × the gateway's reference price. Their
  accounts may go down to minus their credit line (Mars 500k, Luna 150k, belt 100k) and receive Earth
  investment (Mars 300k, Luna 40k, belt 40k cr/yr). Without it Mars surface ran dry within three years.
- **Decision (23:30):** residents start with their savings buffer (90 days of wages), brought from Earth. The
  money-supply controller holds station + ship money, as before; residents' savings are their own. Counting
  them made the controller tax stations into the red as wages rose.
- **Decision (23:40):** credit band ceiling 250k → 500k and household spend time 60 → 30 days. Wage bills are
  5-10k cr/day, so the old band taxed stations on the swing of wages paid against spending returned.
- **Decision (23:40):** with households, a station's residents travel on their own savings: only what they
  hold beyond half their buffer. Mars households had paid 3M cr of emigrant fares from pooled savings.
- **Decision (23:20):** station incomes, set from each station's four-year trade deficit (cr/yr):
  - science: Titan 800k, Ganymede 250k, Mercury 100k, Venus cloud base 100k;
  - relay: Ganymede 50k, Mercury 50k, Earth L1 30k;
  - settlement: Mars 400k, Ceres 150k;
  - tourism and survey: Luna 60k.

  Luna's helium-3 output is 0.2 kg/day per 10,000 residents.
- **Decision (23:30):** labour market. Starting wages are set so that a job seeker at Earth's 30th wage
  percentile gains the fare over a 10-year stay (multiples of 0.5 cr/day):
  - 1.0 in cislunar space, at Venus and at Mercury;
  - 1.2 at Mars, 1.4 at Ceres, 2.0 at Ganymede, 3.0 at Titan.

  Jobs grow 3%/yr while a station is supplied and above its floor. They are cut 5%/yr below 70% supply,
  or when credits fall below minus the floor (credits below zero alone cut Mars jobs on wage swings).
  Visitor trips are scaled ×10 (0.05 → 0.5 per person-year) because only those whose budget covers the
  fare travel now.
- **Decision (23:40):** purchase valuations sell at most at the destination's landed cost (reference
  price). With Earth selling at its ask, a ship's margin is the carriage; at scarcity prices, cargo estimates
  were 89%/yr against 6% earned. After the fix: 44% expected, 62% earned. Carrier margin 1.2 → 1.5 and hurdle
  30% → 20%/yr, so freight earns a normal return. Ceres and Ganymede reactor-fuel output cut 10 → 2 per
  10,000 residents (5 t/day was unrealistic and glutted Earth orbit).

## Results

**Phase A** (surface markets, households, incomes; no labour market), 4 starts × 1,460 days:
unmet 0.4 ± 0.1%, last fifth 1.1 ± 0.3%, 95.8 of 140.5 ships profitable, money +0.6 ± 4%, drift 0,
2.0 ± 1.2 stations below their floor at the end, 10,500 boardings, 10.2M cr fares.

**Phase B** (labour market on, valuation cap, margin 1.5, hurdle 20%), start 0 × 1,460 days:
- unmet 0.8%; 69 of 114 ships profitable;
- cargo purchases: expected 36%/yr, earned 75%; passenger purchases: expected 26%, earned 4%;
- 2 stations below their floor (Mars, Ganymede);
- 2.2M cr fares.

### Phase C and fixes (2026-10-22 23:45 to 2026-10-23 00:25)

- **Decision:** supply following price is built (`supply_price_elasticity`) but off. Production is 8-10× use,
  so plants stood idle, the utilization rule cut jobs everywhere, and Venus throttled food on its own glut
  while Mercury went 23% short. The production cap (90 days of output) already stops gluts growing.
- **Decision:** demand follows income and price: `consume` goods (wage/start)^1 × (price/reference)^−1;
  upkeep goods (price/reference)^−0.1. Consumption by value rose 6.1M → 8.5M cr over 4 years as wages rose.
- **Decision:** the money-supply controller is off (`money_supply_days` 0). Its per-head taxes, levied
  when station + ship money ran above target, pushed Mars into job cuts. Money now moves only through named
  flows. Station money grows from Earth's incomes and settles near 4-5M cr in 4 years; the economy test now
  only requires it not to run away (below 3× the seed).
- **Decision:** a colony keeps its gateway stocked with at least 30 days of the station's own use. Mars, made
  a producer by its colony's food, held stock below its residents' 7-day buffer (upkeep 72%, jobs cut).
- **Decision:** residents spend savings above their buffer within 2 days (was 30). The float drained new
  stations: Earth L1 lost 195k in its first weeks and spent 63 days in the red.
- **Decision:** residents put 3% of their wages aside for long trips; fares and valuations stay within that.
- **Decision:** passenger valuations count travellers still to come after they give up waiting, over half a
  cycle at the queue's give-up rate. Passenger buys now expect 24%/yr and earn 11% (was 25 against 8).
- **Decision:** a yard builds a new hull from goods worth 20% of its price (metals half, machinery 30%,
  electronics 20%), out of its stock; what it lacks comes from Earth.

Benchmark (4 starts × 1,460 days, before the last two fixes): unmet 0.3 ± 0.1%, last fifth 0.6 ± 0.2%,
79.5 of 115 ships profitable (69%), 1.0 ± 0.8 stations in the red at the end, worst station 248 ± 106 days
in the red, 10,300 boardings, 3.2M cr fares, 0 trajectory flags in 3 of 4 runs.
Speed: 50.8 ms per simulated day, slowest 20-day stretch 6.3 days/s (limit 5).

### Long-run check and fixes (2026-10-23, 00:25-01:05)

The first 8-year run (start 0, before these fixes):
- unmet demand rose from 0.2% (year 2) to 1.4% (year 8, cumulative);
- Titan ran out of machinery and reactor fuel (35-38% unmet, upkeep 30%), and Mars and Titan cut jobs;
- treasuries piled up unspent money (Sol Federation 8.9M) once the fleet cap stopped purchases.

- **Decision:** ships pay their crews' wages and hazard pay to the home station's households (the user:
  "ships pay their employees"), not to the station.
- **Decision:** plants run with the workers employed. Outputs and the inputs feeding them scale with
  jobs ÷ workers (at most 1, saved per station in half-percent steps); consumption follows residents. Job
  cuts now cost output.
- **Decision:** Titan and Ganymede are science stations (`science_station` income). They centre their
  import prices 50% above landed cost, paid from their budget (the user: "willing to pay even high prices
  for being supplied, Earth is paying"). Mercury and Venus keep a plain `science` income.
- **Decision:** settlement subsidies Mars 400k → 700k, Ceres 150k → 300k cr/yr. Both are short of labour,
  so wages and imports rise.
- **Decision:** taxes only above 1.5M (was 500k). Remote stations take lumpy deliveries, and a faction that
  taxed their cash away left them in the red when a big delivery arrived: Ceres paid 1.3M in taxes and drew
  1.2M in subsidies, and spent up to 493 of 1,460 days in the red.

Benchmark (4 starts × 1,460 days):
- unmet 0.3 ± 0.1%, last fifth 0.5 ± 0.1%;
- 88 of 140 ships profitable (63%);
- stations in the red at the end 0.2 per run; worst station 126 ± 33 days in the red (was 291 ± 160);
- 5,000 boardings, 2.6M cr fares (the 3% travel budget halved residents' trips);
- 0 trajectory flags, drift 0; faction interest to Earth 1.0M (they now borrow for ships).

### Station money (2026-10-23, 00:45-01:10)

The second 8-year run (ceiling 1.5M) was stable: unmet 0.2% → 0.5% cumulative by year 8. But every station
ran a surplus in years 6-8 (Titan ~980k cr/yr, Low Earth Logistics 2.1M from yard sales) and sat at the
ceiling: 13.6M cr in stations, while Independent Consortium borrowed ~4M at interest.

- **Decision:** science and settlement grants are need-based (`station_incomes.csv` `need_based`): full
  while the station holds less than half the credit ceiling, falling to nothing at the ceiling. Relay and
  tourism are paid services and stay fixed.
- **Decision:** a faction covers a station in the red at once (within 2 days) while it can borrow the gap.
  The 30-day band left remote stations weeks in the red after each lumpy delivery: cash-flow timing, not
  insolvency.
- **Decision:** with that cover, taxes start again at 750k (was 1.5M).

Benchmark (4 starts × 1,460 days):
- unmet 0.3 ± 0.1%, last fifth 0.4 ± 0.1%;
- 89.8 of 139 ships profitable (65%);
- no station in the red or below its floor at the end; worst station 14.5 ± 4.7 days in the red;
- 5,150 boardings, 2.9M cr fares; faction interest to Earth 0.9M.

Trajectory audit: 0.5 ± 1.0 flags per run, all `many_revolutions` on Mercury departures at κ ≈ 0.005
(5.5 revolutions, 929 days). Those paths exist since the atlas was extended to low κ (main, PR #20), not
from the overhaul. They are costly, and the economy flies them rarely.

### Colonies' people and Titan (2026-10-23, 01:10-01:40)

- **Decision:** colonies have people with a lognormal wage: Mars surface 50,000 (median 0.3 cr/day,
  σ 0.4), Luna surface 10,000 (0.25), belt miners 5,000 (0.35, σ 0.6). Each year 5% of them look for a
  better job.
  - Those whose surface wage is low enough that the gateway's job, less hardship, repays the ascent over a
    10-year stay take its vacancies (within ~30 days), without a ship. The ascent is a 100 kg seat at the
    colony's launch price, about 3 cr at Mars.
  - The gateway's unemployed go down to the surface.
  - This is the user's original picture: workers move for better jobs between a surface and its station.
    Mars no longer carries 2,000+ unemployed.
- **Decision:** a colony recruits part of its growth on Earth (Mars 30%, Luna and belt 50%). It pays their
  passage up to the reference fare, out of its savings (not its credit line). They land at its gateway and go
  down to the surface. Paying up to twice the fare emptied Mars's account; the colonies' trade deficits now
  keep the recruiting modest.
- **Decision:** Titan runs its own machinery workshop (1.5 per 10,000 residents). It is 2,000 days from
  resupply, so its import demand alone never drew ships (machinery 26-60% unmet). A smaller workshop got
  stuck: a 21-unit start, the upkeep penalty, then output below use.

Benchmark (4 starts × 1,460 days):
- unmet 0.2 ± 0.0%, last fifth 0.0%;
- 87.8 of 139 ships profitable (63%);
- no station in the red or below its floor at the end; worst station 15 days in the red;
- 5,100 boardings, 2.6M cr fares; about 2,000 people hired up and 1,300 gone down per run;
- 0.5 trajectory flags per run (the low-κ Mercury spirals).

### Jobs and propellant (2026-10-23, 01:45-02:15)

- **Decision:** a station adds jobs only while fewer than 5% stand open. In the 8-year run Titan kept adding
  jobs nobody came to fill; its wage climbed from 1.5 to 3.75 cr/day.
- **Decision:** fuel factories price their propellant at local cost (`fuel_factories.csv` `price_cr`):
  - Luna and Ceres 2.5, Venus, Titan and Ganymede 3.0, Mercury 3.5 cr per 100 kg;
  - Earth's launched price is 7.9;
  - depots' reference prices start from the factory's price.

  At Earth's price, propellant from Luna earned nothing in low orbit (Earth's traffic buys at 0.9 × ask), and
  ships paid Earth prices everywhere. Now Luna's propellant is a low-orbit business: 85,000 units (674k cr)
  sold to Earth's traffic in 4 years. That is the case for space mining, and an income the user chose.

Benchmark (4 starts × 1,460 days):
- unmet 0.2%, last fifth 0.0%;
- 99 of 137 ships profitable (72%, was 63%);
- worst station 6 days in the red; no station in the red at the end;
- Earth exports 6.0M cr (was 5.3M); 5,100 boardings, 2.4M cr fares.

An 8-year run (start 90, before the propellant change) was stable: 0.1% unmet over the whole run, station
money steady at ~6.6M cr, the colonies grew (Mars surface 59,000) while sending ~3,500 people up to their
gateways.

### Speed and the purchase hurdle (2026-10-23, 02:15)

- **Finding:** the slowest 20-day stretch had fallen to 3.6 days/s (59.5 ms per simulated day). Fleet
  investment took 40 of 86 s: each review probed 100+ candidates, because the 20% hurdle let more of them
  through the bound.
- **Decision:** hurdle back to 30%/yr. Purchases now expect ~1.6× what they earn (cargo 41% against 25%),
  so 30% expected is ~19% earned. Slowest 20-day stretch 6.0 days/s (54.6 ms per day).

Benchmark (4 starts × 1,460 days):
- unmet 0.2 ± 0.1%, last fifth 0.0%;
- 91.8 of 129 ships profitable (71%);
- worst station 12 ± 7 days in the red, 0.2 stations below the floor at the end and none in the red;
- 4,600 boardings, 2.0M cr fares; Earth exports 5.9M cr.

The passenger test now checks that settlers are posted and passengers board, pay and land. Settlers
come only for vacancies, which are few early in a game.

## The 14 points of a realistic economy, re-scored (2026-10-23)

The user's question of 2026-10-22: scores before → after the overhaul (0-5).

| # | Point | Before | After | Why |
|---|---|---|---|---|
| 1 | Money is conserved | 5 | 5 | Every account named (Earth, colonies, households, stations, ships, factions); drift 0 |
| 2 | Prices from real costs | 4 | 5 | Goods at Earth's real prices plus launch; local propellant at local cost |
| 3 | Prices clear lasting shortages and gluts | 3 | 3.5 | Earth's ask and bid bound the gateways, colonies supply when the station pays; producers' gluts still only fill storage |
| 4 | Price gaps ≈ carriage | 4 | 4.5 | Landed cost at the reference, Earth caps the gateways |
| 5 | Supply follows price | 1 | 2 | Output follows employment; colonies sell more when the station pays more; price-driven output built but off |
| 6 | Transport follows profit | 4 | 4 | Purchases valued at landed cost; estimates ~1.6× earned |
| 7 | Producers earn from sales | 1 | 2.5 | A station pays wages for its output; industries are still not accounts of their own |
| 8 | Buyers spend what they earn | 2 | 4 | Residents live on wages, travel on a budget; colonies on their trade and Earth's investment; Earth itself unlimited |
| 9 | Demand follows income and price | 1 | 3.5 | `consume` goods follow wages and prices; travellers from income and gain distributions |
| 10 | Wages follow scarcity, pull workers | 1 | 4 | Wages from vacancies; lognormal gains; local hiring; recruiting; remote stations pay more |
| 11 | Failure is costly | 2 | 3 | Job cuts cost output; the unemployed leave; stations are bailed out by their faction |
| 12 | Forecasts and risk | 3 | 3 | Unchanged |
| 13 | Capital is costly and lasts | 3 | 3.5 | Yards in space build hulls from goods; factions borrow from Earth at interest |
| 14 | Few hard limits | 2 | 2.5 | Grants taper, jobs stop when unfilled; the fleet cap and price clamps remain |

## Verification (2026-10-23)

- **Save/load:** a game saved at day 300, loaded and run to day 600 saves byte-identical to one run straight
  to day 600 (476 KB, every new account, wage, job count, travel budget and colony population included).
- **Tests:** the five ctest suites pass. The economy tests add: Earth's ask and bid at a gateway, Earth's
  stock outside storage, the colony rates, a Monte Carlo check of the settlers' gain distribution, and a day's
  wages, goods and incomes.
- **ThreadSanitizer:** 800 days (RelWithDebInfo, `setarch -R`), no warnings, drift 0.
- **In the game** (shot tour): the station inspector shows residents, wage, jobs, incomes and what the
  surface sells and buys. The economy panel shows residents' savings and the background economies'
  accounts and people.

### Science stations' staffing (2026-10-23, 02:40)

- **Decision:** science stations (Titan, Ganymede) keep the crew their research needs: their jobs do not grow
  (they can still be cut). The user: they "would probably keep only crew that is needed for research +
  suppliers". Their wages hold at the starting level (1.0 and 1.5 cr/day).
- **New test** (economy tests, 180 days): no station spends 30 days in the red.

Benchmark (4 starts × 1,460 days): unmet 0.2%, last fifth 0.0%, 88.8 of 129 ships profitable (69%), worst
station 12 days in the red, 4,500 boardings.

## Final check: 8 years (2026-10-23, 02:50)

Two starts (0, 180) × 2,920 days, final state:
- unmet demand 0.1% over the whole run, last fifth 0.0-0.1%;
- 117.5 of 154 ships profitable (76%);
- no station below its floor or in the red at the end; worst station 17 days in the red over 8 years;
- 12,700 boardings, 6.4M cr fares;
- Earth exports 11.8M cr; helium-3 ~200 kg sold to Earth;
- colonies: Mars surface 59,000-60,000, Luna 10,500-11,000, belt 3,900 (its miners move up to Ceres jobs);
  3,300-4,000 people went up to gateways and 1,500-2,300 down;
- station money ~6.5M cr, steady from year 4;
- Sol Federation saves ~8.5-8.9M (the fleet cap stops its purchases); Independent Consortium borrows
  ~5M at interest from Earth.

## Statistics: accounts after 8 years (money journal, 2026-10-23)

Start day 0, 2,920 days (`--econ-audit` Accounts section; start 180 agrees within ~10%). Amounts in cr.

**Factions**

| Faction | Treasury | Taxes in | Operating result | Investment (hulls, working capital, salvage) | Its stations' profit before the faction |
|---|---|---|---|---|---|
| Sol Federation | +8.90M | 21.12M | +20.87M | −11.97M | +23.9M (Low Earth 13.0M, Luna 6.3M, L1 4.7M) |
| Independent Consortium | −5.12M | 5.39M | +2.16M (interest −1.57M) | −7.28M | +8.5M |
| Mars Corporation | +0.04M | 0.10M | −0.06M (insolvency cover 0.15M) | +0.10M | −0.5M |

**Stations**
- Every station lives mainly on its residents' spending, and its main cost is their wages.
- Next come cargo bought from ships and dividends from the ships it owns.
- The Earth-side hubs and Mercury pay most of the faction tax (Low Earth 12.0M, Luna 5.4M, Mercury 3.9M, L1 3.8M).

**Residents**
- Wages are 92-96% of their income; the rest is crew wages and starting savings.
- They spend 75-98% on services and 3-20% on goods, and save the rest.

**Colonies**

| Colony | Account | Bought at gateway | Sold at gateway | Earth investment |
|---|---|---|---|---|
| Mars surface | +35k | 2.74M | 0.68M | 2.40M |
| Luna surface | −150k (at its line) | 0.75M | 0.29M | 0.32M |
| Belt miners | −100k (at its line) | 0.55M | 0.19M | 0.32M |

Luna and the belt buy about 2.6-2.9× what they sell, and Earth's investment does not cover the gap. This is the
next fix.

**Earth**
- It paid 29.5M into space and received 16.3M: −13.2M over 8 years.
- Out:
  - space goods it buys (surface trade 8.75M);
  - grants (settlement 4.7M, science, relay);
  - travellers' fares (3.8M);
  - colony investment.
- In:
  - Earth's share of new hulls (8.4M);
  - goods it sells to space (6.1M);
  - faction interest (1.8M).

**Fleets**
- Sol Federation: cargo in 15.3M, fares 3.7M.
- Independent Consortium: cargo in 14.3M, fares 0.7M.
- Both send most of it home as dividends (8.6M and 8.3M), and pay capital charges, cargo, fuel and crews.

## Colonies fixed: the surface mines (2026-10-23)

Plan agreed with the user: the surface mines, the gateways trade; Earth-bound goods earn the colony about 70% of
Earth's price. Mechanics: `docs/modules/economy.md`, "Colonies".

**Decisions and why**
- **Extraction moved to the colonies.** Lunar Gateway (new profile `lunar_port`) and Ceres (`belt_port`) no
  longer make metals, water, helium-3 or platinum; Luna surface and the belt sell them at the gateway. Before,
  the stations made them for free, so the colonies almost never sold.
- **Colony goods are centred at ask × 1.1** at a gateway that doesn't make them. They were centred at the base
  price, which left ships no margin on Luna metals or Ceres platinum.
- **Gateway stock of colony goods.**
  - It was 14 days of net rate: 7 platinum units, too few for a ship to collect, so the price sat at the floor
    and the belt sold only 17k in 4 years.
  - Now 60 days, and a year for platinum and helium-3. Ceres holds about 800 platinum for ships to take home.
- **Belt platinum 0.5 → 3 units a day** (30 kg/day, about 11 t a year).
  - The belt's only real export: nobody needs Ceres' metals or water (the outer stations make their own, and
    Earth sells metals).
  - At 0.5 a day the belt could never pay for its imports.
- **Imports follow money and people:** affordability from the account between the credit line and a 90-day
  buffer, essentials (food, medicine) first. A colony no longer pins at its line.
- **Welfare:** growth × (2 × satisfaction − 1); the wage median follows income per head (√, clamped 0.5-2, one
  year).
- **Mars investment 300k → 400k cr/yr.**
  - The new price centre cut Mars surface's fuel income: the station had been paying 15 cr for fuel the
    colony sells at 3.35, and its account ran toward the line.
  - The extra 100k replaces that hidden subsidy, now as an open one.
- **Surface trade split** into surface sales and purchases in the money journal (save version 20).

**Steps (4 starts × 1,460 days)**

| Step | Unmet | Profitable | Red days (worst) | Belt / Luna / Mars surface account at day 1,460 |
|---|---|---|---|---|
| Before | 0.2% | 71% | ~12-18 | −100k (line) / −105k / −210k |
| Surface mines, ask × 1.1 | 0.1% | 71% | 8.5 | −100k / −105k / −320k |
| + 60-day gateway stock | 0.1% | 71% | 8.2 | −100k / +300k / −400k |
| + belt platinum 2.5, imports follow money | 0.3% | 67% | 10.0 | −24k / +290k / +16k |
| + growth and wages | 0.2% | 66% | 10.8 | −17k / +250k / +8k |
| + a year's stock of platinum and helium-3 | 0.2% | 68% | 14.8 | belt meets 64-78% of its imports |

**8 years, starts 0 and 180** (final data: Mars investment 400k, belt platinum 3/day)

| Colony | Account | Imports met | Income/yr | Wanted/yr | Wage (data) | People (start) |
|---|---|---|---|---|---|---|
| Luna surface | +0.82-0.85M | 100% | 230k | 115-120k | 0.35 (0.25) | 11,800-12,400 (10,000) |
| Belt miners | 0 to +18k | 65-82% | 110-180k | 130k | 0.33-0.36 (0.35) | 3,800-3,900 (5,000) |
| Mars surface | +10-26k | 42% | 470k | 437k | 0.31 (0.30) | 50,000-50,400 (50,000) |

Unmet 0.2-0.3%, 77-85% of ships profitable (76% before), drift 0, every station solvent (worst red streak 14
days). Lunar Gateway (+6.8-7.2M) and Ceres (+1.7-2.0M) stay profitable before their factions. Factions:
- Sol Federation +9.0-9.4M;
- Independent Consortium −3.9 to −5.3M (was −5.1M);
- Mars Corporation −0.4 to −0.9M (was ~0): Mars Transfer Port earns less on fuel now that Mars fuel sells near
  the colony's cost.

**Open**
- **Mars surface meets only 42% of its imports.** The cause is not money: Mars Transfer Port has never received
  reactor fuel, which is about 45% of Mars surface's imports by value.
  - Its order at the 452 reference price leaves ships about 23 cr a unit on a 400-day round trip.
  - This predates the colony work.
  - A gateway could order its colony's imports at the colony's bid. Not done here; next.
- **The belt still shrinks** (5,000 → 3,850) as miners take Ceres jobs at 2.5× their wage. This is the labour
  market working; the belt's wage now follows its income.
- **Luna surface accumulates money** (+0.85M in 8 years, from helium-3). It could import more as it gets richer
  (income-elastic colony demand), later.
