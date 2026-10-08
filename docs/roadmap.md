# SpaceTrains Roadmap

## Done

### Phase 1 — VariableISP Integration and Headless Verification (2026-05)

- Electric ion ship classes with tech-level parameters (α, ε).
- `VariableIspTrajectoryPlanner` backed by the precomputed trajectory atlas; per-sample propellant via the I-invariant scaling.
- Multi-planner dispatch by propulsion type; Lambert solver and NTR ships for the Kepler side.
- Headless executable with `--verbose`, `--econ-audit`, and periodic reports.

### Phase 2 — Godot Observer Frontend (2026-06)

- Bridge snapshots carry `trajectory_path`, destination ghost, and snapshot timing for smooth interpolation.
- Procedural sci-fi HUD (top bar, entity browser, inspector, market panel, event ticker).
- Starfield, Sun bloom/halo, planet textures, orbit rings, procedural ship/station meshes, trails, engine glow.
- Economy: dynamic prices, credits, trade settlement, storage caps, profit-based dispatch; fleet-collapse fix (730-day audit: 0 stranded). Fleet investment (v27-v28): treasuries buy ships for routes they commit to for 180 days, long-laid-up ships are sold. Fuel factories at five stations, bulk tankers, fuel-aware planning (v29).

## Current Plan (updated 2026-10-08) — start here

Branch `feature/opus5_5_return` (solo repo: commit on the branch, no PRs, push only when asked).
Last commits: `773443c` part C tank refits (v25), `9a5afaa` open economy (v26), fleet investment (v27), route commitment (v28), fuel factories (v29), sustained valuation (v30), investment rate (v31), outer exports (v32), bridge start-up fix, stockpile forecast (v33a), plasma reach (v33); overnight 2026-10-07/08 (steps 9-13 below, log in `docs/plans/overnight_2026-10-08.md`): speed, smooth display, map look, panels, save/load, unmet-demand findings (v36), mixed cargo (v37).

### Decisions so far

- **Economy target:** physical and economic feasibility. Uneconomic trajectories (multi-revolution, long
  waits) must lose on cost (capital, wages, provisions, fuel), not on heuristics.
- **Operating costs (part A, done):** wages and capital paid to the home station; provisions are real
  food/water/oxygen; ships with no money or no work lay up. Two-leg mission scoring.
- **Fuelling (part B, done):** missions load burn + 10% reserve; cargo and provisions are in the rocket
  equation; ion ships choose their fuel load by fuel + time cost.
- **Perihelion limit** 0.1 AU for every planner.
- **Unviable ships are acceptable:** the starting fleet is not final. A class that cannot earn on a route
  should simply not be used there; the economy should deploy other ships (fleet investment, below).
- **Technology era:** variable-Isp ships stay at the level of the user's `~/VariableISPRocketTrajectories/linopt/`
  studies (exhaust 50-250 km/s, about 540 W/kg at ship level, radiators, 15% waste heat). No torchships;
  keep radiators and efficiency limits, but do not cut ship efficiency much.
- **Naming (confirmed by the user 2026-10-06):** variable-Isp classes are advanced plasma or fusion
  drives with radiators, not "ion"; the "chemical" Kepler classes are nuclear-thermal (solid core about
  900-1000 s, liquid/gas core up to about 1400 s). Rename in data and UI during the tech review.
- **Money:** keep exact accounting. Prefer an open economy with an explicit external account (Earth's
  economy, faction treasuries) over a strictly closed one; the audit must still reconcile to zero drift.
- **Outer-system exports:** no exotic matter. Candidates: platinum-group metals (Ceres belt), deuterium
  (Ganymede, Titan), nitrogen/ammonia (Titan), science samples.
- **Fuel supply (user, 2026-10-07):** fuel comes from **fuel factories** at a few particular places
  (not Earth: lifting from a planet surface costs extra) and very large freighters (or many of them) carry it
  to the stations that buy it; the planner sees that a destination will be short of fuel. Done in v29
  (`docs/plans/fuel_factories.md`): ships carry their return fuel and fly cheaper transfers where fuel is
  scarce, and reject a trip only when even that is impossible.

### Next steps, in order

1. **Tech plausibility review** (`docs/plans/tech_plausibility_review.md`): fix reference numbers for the
   era, rebuild every class bottom-up (engine, reactor, radiators, tankage, habitat, hold), rename classes,
   price ships from the breakdown (naming already confirmed). Done so far: reference numbers, class
   rebuild (`7cdb236`), fuel depots (`dad132c`), pricing (`1808fe6`), renaming (propulsion types
   `nuclear_thermal` / `variable_isp`, classes `plasma_freighter` / `plasma_courier`), cargo holds with
   price-curve trades and an economy scaled with population (v24). The tech review is complete.
2. **Part C: modular tanks and refits** (`docs/plans/ship_operating_costs.md`): done (v25). Every class
   has 50/100/150/200% tank variants on one hull; ships refit at their home base when the missions the
   neighbouring tank size opens repay a 20-day yard stay and a bill of 30% of the hardware change.
   Mostly inner-system ships shrink their tanks. Consumer stations bled faster (LEO −458k), so the open
   economy went next.
3. **Open economy** (`docs/plans/open_economy.md`): done (v26), moved ahead of fleet investment. Residents
   pay for consumed goods and stations pay local producers (at most the base price) against an external
   account; ships pay cash above a 50k reserve to their home station; faction treasuries keep station
   cash between 25k and 250k; a 60-day controller holds the money in stations and ships near the seeded
   amount (v26: +16.7% at day 730, driven by food scarcity at Low Earth Logistics). Money drift 0.
4. **Fleet investment** (`docs/plans/fleet_investment.md`): done (v27). Ships laid up for 180 days are
   sold for 40% salvage to their treasury; every 60 days the treasuries buy the one ship (hull, tank
   variant, yard) with the best sustained return (margin per unit × min(hold per round trip, the
   destination's consumption), minus running costs; hurdle 30%/yr). v27: 7 bought, 4 sold, 15/29
   profitable, Low Earth Logistics food no longer critical, money +7.0%. v28: a new ship shuttles
   between its yard and the destination it was bought for for 180 days, and that flow counts against
   the destination's demand in later reviews (13/29 profitable, fleet profit −1.4%, money +5.5%).
   v30: each candidate run is valued over the commitment at the stocks both stations would hold,
   with other ships' observed imports and exports (fleet profit 891k vs 444k in v29, 16/32 profitable).
   Open: treasuries buy only one ship per review and end with 542k unspent while 72% of demand goes
   unmet.
5. **Fuel factories and bulk tankers** (`docs/plans/fuel_factories.md`): done (v29). Fuel factories at
   Venus, Titan, Lunar Gateway, Ceres and Ganymede (none in Earth orbit); other depots only hold what
   ships deliver and price against their buffer. Bulk Tanker and Plasma Bulk Tanker classes (2000 units)
   bought by fleet investment. Fuel-aware planning: ships carry return fuel into ports that cannot refuel
   them (or sell at more than 2x), pay the local scarcity premium, and the planners trade propellant at
   the port's price (cis-lunar plasma spirals are now cost-aware). v29: fuel 690k cr (v28 135k), fleet
   profit 444k (871k), 14/32 profitable, 0 laid up, 0 stranded. Open: Mars runs dry by day 350 and
   Mercury is marginal (no tanker bought for them); Earth-orbit fuel costs 35-60 cr/u.
6. **Investment rate**: done (v31). A review buys until no candidate clears the hurdle (at most 8),
   each valued against the flow of the ships already ordered. 19 ships bought (v30 10), fleet profit
   981k, 19/41 profitable, treasuries end with 69k (v30 542k); unmet demand 71.4% (71.7%).
7. **Outer-system exports** (`docs/plans/outer_exports.md`): done (v32, v33). Ceres mines platinum,
   Ganymede and Titan deuterium; Earth's two stations buy any amount at the base price for the outside
   economy. v32 started a rush to Ceres; v33a: follow-ups forecast the producer's stock at arrival and
   leave other ships' planned pickups to them. v33 (with step 8): 1.02M cr exported by day 730, fleet
   profit 1.035M, 17/43 profitable, unmet demand 72.9% (v31 71.4%). Open: export volumes are small
   (production-limited after the starting stockpiles), and Ganymede and Titan still get few supplies.
8. **Trajectory reach** (`docs/modules/trajectory.md`): done (v33). The variable-Isp planner now takes κ
   at the origin's radius (it used 1 AU for every origin: ships leaving Jupiter or Saturn were planned
   60-280x too weak, those leaving Mercury 10x too strong), and a refined window that just passed is
   flown at once instead of a whole synodic period later. Plasma ships reach every outer station in
   250-550 days in both directions. Nuclear-thermal Jupiter <-> Saturn plans of 2500-4500 days are
   physical (Hohmann about 3700 days) and lose on cost. Left: theta branches, atlas time-optimality
   (needs a regenerated atlas).

Steps 9-13 (plan `docs/plans/speed_gui_orders.md`, the user's 2026-10-07 priorities):

9. **Speed:** done. Profiling timers (`--profile`), double-precision integration, exact parallel mission
   planning (passes over a thread pool), a memoised Lambert grid, candidates planned without paths, fleet reviews
   spread over ticks, a fixed 0.1-day tick for headless and UI. 730 days: 11m10 -> 52 s with 90 ships.
10. **GUI:** done, the user reviews the look. Smooth display clock (no stalls at 1 day/s; short ones at 5 days/s
    during fleet reviews), orbit rings exact up close, KSP-like map (true-size bodies, icon labels, brighter Sun
    and planets), mission in words and richer ship/station panels, economy overview (M).
11. **Save/load:** done (F5/F9, `--save-at`/`--load`; exact continuation, tested).
12. **Unmet demand:** investigated. Main cause transport capacity (steady supply needs ~45,000 u of holds in
    transit, the fleet had 5,000-11,000 u) and targets too small for distant supply; one forecast bug. Open for the
    user: local life-support production at the outposts (design decision).
13. **Orders and mixed cargo:** done. Mixed cargo by marginal value (v37), investment values mixed holds and
    ranks by profit per day near the fleet limit (v37d, v39a), contracts with prices agreed at departure (v39c);
    station panels show orders and contracts.
14. **Production dependencies and upkeep penalties** (planned, user-approved 2026-10-08, not started;
    `docs/plans/production_dependencies.md`): replaces v37's weighted-average production gating, which the user
    rejected as unphysical. Each output depends only on its own material inputs (hard, minimum rule); life-support
    and crew goods are upkeep with explicit per-condition penalties on the whole station (confirmed: medicine
    -15%, fuel -30%, food -50%, oxygen or water -90%). Oxygen and water shortages trigger emergency resupply paid by
    the faction, so no station stays broken. Needs a role column in `recipes.csv`.
15. **Landed-cost reference prices** (planned 2026-10-08, `docs/plans/station_finances.md`): each consumer's curve
    is centred on base price plus the transport cost from its nearest producer, and the cap drops from 16x to about
    4x. Aimed at the outposts' chronic shortage and the windfalls that pile money in ships.
16. **Science trade** (planned 2026-10-08, `docs/plans/science_trade.md`): light, valuable `science_samples` from
    Titan, Ganymede, Ceres and Mercury, bought by Earth (export) and Lunar Gateway and Mars (curve). Gives the
    outposts an income and the return legs a cargo.
17. **Station money** (planned 2026-10-08, `docs/plans/station_finances.md`): station and residents are one
    account; local consumption is no payment; Earth stations are backed by the outside account; station cash limits
    orders; faction core-crew subsidies, budgets and loans; the money-supply controller is set to measure only.
    Fixes the source of new money (residents paying scarcity prices) and the faction deficits.

Then tune the economy (money in ships, Mars Corporation, outposts) against steps 14-17.

Deferred (user, 2026-10-08):
- **Station growth and decline:** wanted, but needs limits first: a subsidised core crew (step 17 provides it) and
  a guard against a few efficient stations producing everything while the rest wither.
- **Piracy and police:** later. A first version needs no chase game: on a ballistic coast a ship's path is known,
  so an intercept is a rendezvous problem (Lambert), costly in delta-v and likeliest near departure and arrival.
  It could start as an expected-loss cost per route that police presence lowers.
- **Random events and small stories:** later. The player only watches, so events are what makes the economy
  interesting to watch. Step 14's per-station multipliers and step 17's emergency log are the hooks: an event is
  a timed multiplier on production, consumption or price, defined as data.

Checks after each step: the three test suites, `tools/benchmark.py` (four starting dates, mean and spread; one
run is too noisy to judge a change) and `--trajectory-audit`. v39d (2026-10-08, four starts): 730 days unmet
demand 49.8 +- 1.0%, 32.5 +- 1.6% in the last fifth, 50.8/91 profitable, holds 99,000 u, money +1.3 +- 7.4%;
**1460 days 32.3 +- 1.2%, the last ~290 days 10.5 +- 1.9%, 84.5/93 profitable**, exports 1.84M, money +10.5 +- 12%;
0 trajectory flags, drift 0; a single 730-day run takes about a minute. Earlier: v37b 63.3 +- 2.2% at 730 days. Older single-run figures, for history: compare with the calibration log in `ship_operating_costs.md`
(v33: 17/43 profitable, fleet profit 1.035M, cargo margin 2.06M, fuel 599k cr, exports 1.02M, unmet demand
72.9%, 34 CRITICAL lines, money supply +6.7%, drift 0, run 11m10; trajectory audit 0 of 277 plans flagged;
v31 without exports: 19/41, 981k, 71.4%). "Unmet demand" is the share of the stations'
consumption (by base value, over the whole run) that found no stock; CRITICAL lines only count the
station goods with under 7 days of stock at the last day. Debug idle ships with
`SPACETRAINS_TRACE_SHIP="<ship name>"`.

## Trajectory Correctness Over Long Runs (mostly done)

Unphysical trajectories appear after months of self-play:

- A VariableISP path ends with a sharp dent: the integrated trajectory misses the destination and the final sample is snapped onto the station.
- A path loops around the Sun many times with only ~10 samples per revolution.

Audit tooling is in place (`--trajectory-audit`, `--trajectory-sweep`; see `docs/modules/trajectory.md`).
Findings from the first 2-year sweep (192k plans, 2026-10-06):

- **Bug 2 = Kepler wait prefix — FIXED.** Paths for awaiting ships prepended only 11 samples of the origin orbit; a
  573-day wait at Venus drew 2.5 turns as a star polygon (83° per step). Now sampled at ≤ 5° of heliocentric sweep
  (11–360 segments); `coarse_wait` went from 11,982 plans to 0.
- **Bug 1 = VariableISP endpoint snap — FIXED.** 96% of nearest-cell seeds and 52% of interpolated seeds missed the
  target by > 0.01 AU (worst: Mars -> Titan overshoots to 22 AU); the snap drew a dent. The planner now refines a real
  atlas cell's seed by minimum-norm Newton shooting onto the destination station's actual position at arrival (and
  starts from the origin station's position at departure), trying up to 4 windows. Sweep: max miss 13.3 AU -> 0.0013 AU,
  `end_dent`/`endpoint_miss`/`start_miss` -> 0. Cost: ion plan p50 ~8 ms, p99 ~94 ms; 3-year headless run 1m36s -> 3m24s.
- **Integrator hang.** Some ion -> Titan plans got seeds that dive into the Sun and RK45 never finished (would
  freeze the simulation). Now guarded by a step budget; the planner treats failure as no window.
- **Theta wraparound.** Atlas theta spans ±1.1 rev; planner accepts retrograde/near-full-loop targets, and the
  integrated theta can land 2-4π away from the target.
- **Lambert path to Titan — FIXED.** A full-tank NTR Ceres -> Titan picks a hyperbolic Lambert arc; the path sampler
  only handled ellipses, so all samples sat at Ceres' radius with the departure time and the snap drew a 6.8 AU line.
  Now samples ellipses, hyperbolas, and near-parabolas (Barker). Sweep: Lambert max miss 6.8 AU -> 0.0001 AU.

- **VariableISP κ at the origin's radius — FIXED (v33).** κ was taken at 1 AU while the atlas path was scaled to
  the origin's orbit: no plasma ship could fly home from Titan, Ganymede -> Earth took 995 days against 241 out.
  Missed windows no longer wrap to a whole synodic period (Ganymede -> Ceres 3133 -> 432 days).

Next fixes:

- Decide which theta branches (retrograde, >1 rev) are acceptable; atlas θ labels are only valid mod 2π
  (~25% of cells actually fly θ ± 2πn).
- Atlas time-optimality: ~55% of solved cells end with > 2% fuel left (the generator only penalised overuse), so
  transfer times are not minimal for the ship's kappa. Consider enforcing m_end = m_dry in the shooting.
- Planning cost: 3-year headless run 1m36s (before) -> 4m22s now (~0.24 s per simulated day). Cache refined plans
  (path depends on (ρ, θ, T) only, not κ) if the live bridge stutters.
- **Hohmann fallback dents — FIXED.** The half-ellipse is now drawn through the actual stations with Kepler timing,
  and elliptic arcs (Hohmann and Lambert) are sampled evenly in eccentric anomaly so the sharply curved far end of
  eccentric ellipses has no kinks. Sweep: Hohmann flagged 1,647 -> 0; Lambert end dents 3 -> 0.
- **Perihelion limit — FIXED.** No planner bounded perihelion: 8 Lambert and 4 ion plans passed *through the Sun*.
  Both planners now enforce 0.1 AU (`kMinPerihelionM`); ion feasible plans -5%.
- **Path sampling — FIXED.** Rendered paths are thinned by curvature (≤ 6° per point, ≥ 40 segments) from a dense
  generation. Sweep: chemical plans 0 flagged; ion 1 `coarse_sampling` left (was 3,034).
- Regression tests for each fixed case.

## Economy Follow-ups

- Re-run the 730-day audit; check whether goods still pile up at producers while consumers starve.
- Check whether short-hop clustering around Earth L1 survived the profit-based dispatch.
- **Ship operating costs**, parts A (done, calibrating) -> B mission-sized fuelling with cargo mass in the
  rocket equation -> C modular tanks and refits. See `docs/plans/ship_operating_costs.md`.
- **Technology plausibility review** of all ship classes (one consistent near-future tech level). See
  `docs/plans/tech_plausibility_review.md`. Do it with or before part B.
- **Fleet investment:** done (v27, route commitment v28), see `docs/plans/fleet_investment.md`.
- **Open economy with exact accounting:** done (v26), see `docs/plans/open_economy.md`.
- **Outer-system exports.** Give outer stations something worth shipping inward, not exotic matter.
  Candidates: platinum-group metals and other high-value metals from the Ceres belt; deuterium (heavy water)
  from Ganymede and Titan ice for fusion research and reactors; nitrogen and ammonia from Titan for the
  nitrogen-poor Moon and Mars; physical science samples (low mass, high value, limited demand). Earth's
  economy (the external account) buys them through the Earth stations at stable prices.

## Phase 3 — Persistence and Scenario Control

- Add save/load snapshots.
- Add seeded scenario setup and reproducible simulation runs (also needed to replay bad trajectories deterministically).
- Add scenario-level data for alternative starting economies or factions.

## Phase 4 — Richer Orbital and Mission Fidelity

- Improve Kepler planner with patched-conic SOI handoffs.
- Replace local direct-transfer approximation with proper planet-SOI arc (also fixes ~100% propellant use on Earth–Moon VariableISP-style hops).
- Add stranded-ship handling, support/refuel behavior, clearer mission state transitions.
- Consider outer Solar System bodies (Jupiter and beyond).

## Phase 5 — Ships with Individual Variation

- Add per-ship power multiplier (±α variation) and size multiplier.
- Size scaling preserves κ; power multiplier changes κ slightly per ship.
- Reflect individual ship specs in UI selection panel.

## Phase 6 — Post-v1 Systems

- Add pirates, police, inspections, rescue, and later combat resolution.
- Expand content: outer-system stations, more factions, science mission chains.
- Support custom star systems through data-only content changes.
