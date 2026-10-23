# Economy overhaul: log (2026-10-22 night → 2026-10-23)

Plan: `economy_overhaul.md`. Decisions taken without the user (they are asleep) are marked **Decision** with
their reason.

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
