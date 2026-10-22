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
