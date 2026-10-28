# Overnight 2026-10-27/28: ship failures, derelict salvage races, police inspections

Branch `feature/failures_salvage` (from `docs/roadmap_visuals`). Plan: `~/.claude/plans/i-like-random-failures-wild-plum.md`.
Switches: `data/economy/failures.csv` (and `patrols.csv` if step 6 is reached).

## Morning summary

All planned steps are built, tested and committed on `feature/failures_salvage` (not pushed, not merged):
a75290f failures and salvage, 6901bb4 UI and docs, bcf09c7 police inspections, plus this log.

- **Failures:** Weibull hull lives (shape 3, mean 33 y = 30 y amortisation × 1.10, computed so capital charges cover
  hull and cargo losses in expectation). A ship that fails in flight becomes a derelict drifting on its orbit;
  near a planet it is towed in. The owner loses hull, cargo and delivery and may replace the ship first (180 days).
- **Salvage races:** ships weigh a wreck (cargo + 20% scrap) against their missions; the first to reach it wins.
  Seen: 5 racers for one wreck, 3 losing.
- **Police:** 4 faction cutters meet ships bound for their stations at the cheapest point of the path (a new
  meeting solver) and ride alongside 3 days; the inspected ship keeps its course. ~110 inspections per 4 years,
  ~950 cr each to the treasuries.
- **Final benchmark** (4 × 1,460 d, everything on): unmet 0.4 ± 0.6% (baseline 0.3 ± 0.4), profitable 86.2 ± 6.1
  (88.5 ± 1.3), boarded 4,608 ± 405 (4,793 ± 433), run 173 s (175): all within noise; drift 0 in every run.
  Emergency premiums 953 ± 1,171 cr (baseline 0) — one run, likely noise.
- **Checks:** tests pass (new: Kepler propagation, failures, meeting solver, patrols); `failures,0` and `patrols,0`
  reproduce the old game exactly; save/load byte-identical mid-race and with cutters mid-sortie; Godot shot tours
  (`--shot-derelict` with `--shot-load`) show the wreck card, a racer and a cutter.
- **To review first:** decisions 2 (mean life factor, purchase cost), 4 (failures are rare: ~2 per 4 years now),
  11 (salvage in dispatch), 15-17 (cutters, jurisdiction, meeting point). Open points below.

## Long benchmark 2026-10-28 (8 starts × 2,920 days)

Configs: off (`failures,0`, `patrols,0`), on (as committed), stress (`lifetime_years` 10: mean life 11 y).

| | off | on | stress |
|---|---|---|---|
| Unmet % | 0.3 ± 0.4 | 0.5 ± 0.3 | 0.6 ± 0.6 |
| Ships profitable % | 75.6 ± 2.5 | 74.8 ± 2.5 | 65.1 ± 3.4 |
| Plasma fleet profit M cr | 13.2 ± 1.2 | 12.6 ± 1.3 | 8.0 ± 1.2 |
| Capital charges M cr | 7.9 ± 0.2 | 8.2 ± 0.3 | 10.0 ± 0.6 |
| Sol Federation net M cr | 9.7 ± 1.3 | 9.7 ± 1.2 | 11.5 ± 1.1 |
| Independents net M cr | 0.8 ± 0.4 | 0.4 ± 0.2 | 0.4 ± 0.4 |
| Mars Corporation net M cr | 0.0 ± 0.1 | 0.0 ± 0.1 | -0.3 ± 0.4 |
| Failures | – | 4.6 ± 0.7 (mean age 30 y) | 22.4 ± 1.1 (8.9 y) |
| Salvaged / written off | – | 3.5 / 0.6 | 15.5 / 3.5 |
| Inspections, police cost | – | 336, 224 k cr | 323, 329 k cr |

"On" is within noise of "off" everywhere (Independents' net −0.4 M at ~1.3 sd). Stress: the economy still
delivers; profits fall mainly through the higher capital charge of the shorter amortisation. The benchmark's
"MONEY DRIFT" flag fires in all three, also "off": −0.0001 cr after 8 years, float rounding.

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
11. **Salvage competes in dispatch** (not only idle ships): plasma ships are almost never idle, so at each docked
    review a ship weighs its best salvage run per day against its best mission (looking at most every 5 days, saved,
    so planning stays cheap). Ships with nothing to do (idle, laid up) also look every half day; a laid-up ship
    rehires its crew for it.
12. **Rendezvous is dear:** matching a wreck's velocity costs ~8 km/s from Mars for a wreck on a transfer arc, so
    nuclear-thermal ships rarely can; plasma ships do the salvage. Plasma way out: half the load, then 35%, 20% if no
    way home is left.
13. **The race:** a new racer must meet the wreck ≥5% sooner than the current claimant; at most 3 racers. Losers fly
    on to their port empty (no mid-flight replanning yet). The salvager sells the cargo at market and gets the scrap
    from Earth at the rendezvous.
14. **Salvage value is modest:** wreck cargo + 20% scrap (8-30 k) usually pays less per day than trade, so salvage
    happens mainly when ships have little else. At real settings 7 of 8 wrecks were salvaged in 4 starts.
15. **Police cutters are plasma couriers** in faction service (no new class: `ship_classes.csv` is written by
    `build_ship_classes.py`), outside the trading fleet and its cap: Sol Federation 2 at Low Earth Logistics, Mars
    Corporation 1 at Mars Transfer Port, Independents 1 at Ceres. The faction pays hull, crew and fuel to the home
    port (new journal category `Patrols`).
16. **Jurisdiction:** ships bound for the faction's stations, interplanetary legs, not inspected in 120 days.
    **Legal assumption** (in the module doc): the inspected ship complies but need not change course; the cutter
    matches it for 3 days.
17. **Meeting point:** least propellant + time (the cutter's daily cost at the port's fuel price) over 8 fixed-time
    rendezvous along the target's remaining path; a way home after the inspection must exist. Late meetings are
    cheapest, so cutters mostly meet ships on their approach (customs at the border).
18. **Inspections have no economic effect yet** (no contraband): the seam for customs and smuggling.

## Open points

- Failures are rare with a 33-year mean life (~0.5 a year now, ~4 a year once the fleet ages). If you want more of
  them on screen, the lever that keeps the economics honest is a shorter amortisation (`lifetime_years`), which
  raises capital charges; a hazard multiplier alone would make ships unprofitable in expectation.
- A lost liner (scheduled loop) is replaced by an ordinary purchase; its loop is not resumed.
- ~~The wreck's drift is not drawn on the map~~ Done 2026-10-28: wrecks' coasts in dark grey, racers' and cutters'
  paths in bright yellow.
- Racers that lose keep flying to the rendezvous point and on to their port; turning back needs planning from a
  mid-flight state (the plasma `plan_chase` can do it; Kepler legs would need a Lambert search from a state).
- Cutters pick from the 3 nearest eligible ships: a cheap, local choice; a faction-wide schedule could do better.

## Log

- 23:10 branch, baseline benchmark.
- 23:20 step 1 (clocks): c measured 0.098 → factor 1.10. Tests pass. Steps 2-3 (derelicts, replacement).
- 23:35 step 4 (salvage): idle-only racing found almost no racers (plasma ships never idle; NTR cannot afford the
  rendezvous) → salvage weighed in dispatch. Committed a75290f. Benchmark at real settings within noise.
- 23:45 save/load round trip mid-race byte-identical; derelict shot tour checked. UI + docs committed 6901bb4.
- 00:05 final benchmark (above); summary.
- 23:55 step 6 (police): meeting solver, cutters; round trip with cutters mid-sortie byte-identical; shot tour
  checked (a first version wrote cutters into the bodies list; fixed).
