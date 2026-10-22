# Economy overhaul: log (2026-10-22 night → 2026-10-23)

Plan: `economy_overhaul.md`. Decisions taken without the user (they are asleep) are marked **Decision** with
their reason.

## Goal checklist (updated as work goes)

| Goal | State |
|---|---|
| Real prices, Earth market | not started |
| Closed money, wages, households | not started |
| Station incomes | not started |
| Venus | not started |
| Labour, wage distributions, migration | not started |
| Stations not bankrupt | not checked |
| Old checks (unmet, drift, save/load, speed) | not checked |
| Passengers still travel | not checked |

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
