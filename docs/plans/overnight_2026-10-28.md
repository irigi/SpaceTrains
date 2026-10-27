# Overnight 2026-10-27/28: ship failures, derelict salvage races, police inspections

Branch `feature/failures_salvage` (from `docs/roadmap_visuals`). Plan: `~/.claude/plans/i-like-random-failures-wild-plum.md`.
Switches: `data/economy/failures.csv` (and `patrols.csv` if step 6 is reached).

## Morning summary

(written at the end of the night)

## Baseline (before the branch)

`tools/benchmark.py --days 1460`, 4 starts: unmet 0.3 ± 0.4%, last fifth 0.0%, profitable 88.5 ± 1.3 of 130.8 ± 5.4
ships, boarded 4,793 ± 433, faction profit -1,199 ± 1,108 k, ~175 s a run.

## Decisions (for review)

1. **Weibull shape k = 3.** The hazard grows with age²: a 1-year-old hull is ~900× less likely to fail than a
   30-year-old one; lives spread about ±36% around the mean.
2. **Mean life = 30 y × 1.10 = 33 y**, computed: the capital charge amortises the hull over 30 years for as long as
   the ship exists, so the owner breaks even in expectation when mean life = 30 y × (1 + c), c = cargo at risk / hull
   value. Measured c = 0.098 (0.084-0.105 over 4 starts; time-average of what owners paid for the cargo of ships in
   flight over the fleet's hull value). Cargo counted at **purchase cost**, not sale value (sale value gave c = 0.69):
   the margin is a profit not made, which the charge need not cover. Fleet-wide, not per class.
3. **Starting fleet ages:** drawn from the steady-state age distribution of a fleet that replaces what fails (density
   ∝ survival), then the life conditional on that age. Otherwise nothing would fail for decades. Each ship draws from
   its own stream (data seed + ship id), so one ship's draw never moves another's.
4. **Failure rate is low at these numbers:** ~2 failures in 4 years (the starting fleet is ~20 old ships; the ~110
   bought in the run are young). That is what a 33-year mean life gives; see "Open points".
5. **Faults found in port show on the next leg**, at a point drawn from the ship's stream (failures happen under
   load; the calendar mean is kept).
6. **Near a planet (a leg within one planet's system) the hulk is towed in** at once: the destination station gets the
   cargo into its stock and the scrap value from Earth. No race.
7. **Derelicts can be on escape orbits:** plasma ships thrust all the way, so mid-flight they can be faster than solar
   escape speed (seen: 44 km/s at 2.1 AU). Such a wreck leaves; it is written off after the claim period like others.
8. **People aboard** (crew, passengers) leave in the lifeboat and are taken on to the leg's destination by rescue
   craft we do not simulate (the plan said "origin"; the destination reuses the landing code, so the people
   accounting stays exact). A timed crew rescue is a later extension.
9. **Money:** the owner loses the hull (capital charges stop), what it paid for the cargo and the delivery payment;
   the ship's cash goes to its home station as when a ship is sold. Scrap = 20% of ship value, paid by Earth to the
   salvager (journal category Salvage).
10. **Replacement right:** a loss gives the owner a claim for 180 days. While claims exist, a review's first purchase
    is the oldest claim's faction's (priced against its own money, any yard); if nothing passes its hurdle the claim
    waits for the next review and others may buy into places nobody holds. Claims count as taken places under the
    130-ship cap. Test with 1.5-year lives: 14 failures and 12 replacements in 400 days, drift 0.

## Open points

- Failures are rare with a 33-year mean life (~0.5 a year now, ~4 a year once the fleet ages). If you want more of
  them on screen, the lever that keeps the economics honest is a shorter amortisation (`lifetime_years`), which
  raises capital charges; a hazard multiplier alone would make ships unprofitable in expectation.
- A lost liner (scheduled loop) is replaced by an ordinary purchase; its loop is not resumed.

## Log
