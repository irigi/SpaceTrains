# Economy overhaul: log (2026-10-22 night → 2026-10-23)

Plan: `economy_overhaul.md`. Decisions taken without the user (they are asleep) are marked **Decision** with
their reason.

## Goal checklist (updated as work goes)

| Goal | State (2026-10-23 00:25) |
|---|---|
| Real prices, Earth market | done (phase A) |
| Closed money, wages, households | done (A); money identity exact; controller off (C) |
| Station incomes | done: science, relay, settlement, tourism, yards, microgravity, helium-3, LEO propellant, colonies' investment, migrants' savings |
| Venus | done: Venus Orbital Exchange, cloud science base (samples, budget) |
| Labour, wage distributions, migration | done (B): jobs, wages from vacancies, lognormal gains and incomes |
| Demand follows income and price | done (C); supply response built, off |
| Stations not bankrupt | improving: about 1 station in the red at the end; worst station ~250 of 1,460 days in the red |
| Old checks (unmet, drift, save/load, speed) | unmet 0.3%, drift 0, tests pass, 6.3 days/s slowest 20-day stretch |
| Passengers still travel | ~10,000 boardings in 4 years (was ~16,000), mostly cislunar |

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
