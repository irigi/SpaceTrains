# Science trade (step 16, planned 2026-10-08, not implemented)

The user's idea (2026-10-08): some stations produce science, which Earth and other rich colonies buy.

## Why

- **Gives the outposts an income.** Titan and Ganymede import nearly everything, and in step 17 their income has
  to come from somewhere. Deuterium alone is production-limited.
- **Fills the return leg.** Ships that supply the outer system mostly fly back empty. Light, valuable cargo makes
  the round trip pay. This is a direct lever on outpost supply.
- **Something to watch.** Fast plasma couriers carrying samples home is a story the observer can follow.

## Model

- New commodity **`science_samples`**: returned samples, instrument cassettes and prototypes. It has to be
  physical, since data can be sent by radio. Each unit is light, with a high base price (start at about 10x
  electronics per unit). Calibrate so that a full courier hold matters but science stays a backhaul, not the
  main business.
- **Producers** (rates to calibrate, small): Titan (prebiotic chemistry), Ganymede (subsurface ocean), Ceres
  (asteroid samples), Mercury (solar physics). In step 14 terms, science has electronics as its material input,
  and the station's upkeep penalties apply to it like any other output.
- **Buyers:**
  - Earth L1 and Low Earth Logistics, through `export_markets.csv` (outside money, a flat price like platinum).
  - Lunar Gateway and Mars Transfer Port as station consumers with their own price curve and a small demand
    (universities and labs), so some science money stays in the simulated economy.
- **Revenue:** the producing station sells to ships along its curve as usual. In step 17 the faction takes its
  tax share.

## Checks

- Each producing station's science income per year against its import bill, to see how much of the outposts'
  deficit it covers.
- Share of return legs from the outer system that carry cargo, before and after.
- Benchmark: unmet demand at the outposts and the number of ships that are profitable.

## Later (not in this step)

- Science that loses value with age, which would favour fast couriers.
- Research stations as a separate station type.
