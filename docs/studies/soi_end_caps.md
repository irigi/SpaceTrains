# Gravity wells and moons without slowing the planners: end caps

Proposal for roadmap item 3 (patched conics), from an outside review, 2026-11-12. Not built and not measured:
every number below is an estimate from the data files and has to be confirmed by the item's first step
(measure what the missing wells cost per route). Companion: `sunward_terminal_position.md`.

## The idea in one paragraph

Do not make the planners solve the flight inside a planet's or moon's sphere of influence (SOI). Treat each
well as an **end cap**: a piece with a closed-form cost in time, propellant and radiation, attached to the ends
of the heliocentric leg the planners already compute. Dispatch scores thousands of candidate plans, and those
get only arithmetic and table lookups on top of today's work. The one plan a ship actually flies gets one exact
solve that joins the pieces properly. This is what keeps the game at 5 days/s with the same fleet.

## Why not solve it properly everywhere

The Long Burn does (`~/Projects/TheLongBurn/docs/notes/solver_brief.md`, `solver_review.md`): each plan is a
chain of phases joined at SOI boundaries, and each table cell searches the spiral acceleration and two exit
turns together. That search, not the leg solver, is where its time goes (about 13 evaluations of 35
propagations per cell; for a near pair 99% of the time in the moons' SOI checks) and where its wrong answers
come from (jumps between branches). It is needed there: 3D, inclined and polar orbits, one player ship whose
plan must be right. SpaceTrains has 130 ships, a planar world, and nobody checking one ship's plan to the
percent.

## Nuclear-thermal ships: a formula

The Lambert search already yields, at each end, the difference between the arc's velocity and the planet's.
Today that difference is charged as the burn. Under patched conics it is the hyperbolic excess speed v∞, and
the burn from a circular parking orbit of radius r around a body with gravitational parameter μ is

    Δv = √(v∞² + 2μ/r) − √(μ/r)

- **Station at a planet:** apply it once per end.
- **Station at a moon:** apply it twice. Leaving the planet from the moon's distance r_m with excess v∞ needs
  the speed √(v∞² + 2μ_p/r_m) there; the moon already moves at v_m, so the excess needed relative to the moon
  is v∞,moon = √(v∞² + 2μ_p/r_m) − v_m, and the burn in the moon's parking orbit is the formula above with
  v∞,moon and the moon's μ. (A second candidate, diving to a low pass of the planet for a larger Oberth gain,
  is also closed form; add it only if the measurements show it matters, e.g. at Ganymede.)
- **Within one planet's system** (LEO ↔ Luna): the planet-centred Hohmann arc stays; the moon end's burn becomes
  the same formula with the arc's speed relative to the moon as v∞.

The Lambert target stays the planet's position: an impulsive arc matches position only, and whatever velocity
mismatch remains *is* v∞. So the search is unchanged; only the cost of each cell changes. Because the new cost
is not linear in v∞, the cheapest cell of the grid may be a different one than today. Time inside the wells
(hours to a few days of hyperbola) can be added in closed form or ignored at first. Extra solver time: none.

## Plasma ships: three separable pieces

### 1. Spirals are constants per station

A low-thrust ship leaves a circular orbit on a slow spiral under tangential thrust. To escape, it must shed
about the orbit's circular speed: Δv_well ≈ v_circ. At thrust acceleration a,

    time  T = Δv_well / a          effort  J = ∫a² dt = a · Δv_well

(the effort is what the fuel budget is measured in: J_max = 2P(1/m_dry − 1/m₀); it does not depend on the mass,
as for the heliocentric leg). The Long Burn measured that such constant tangential spirals lose 1-3.5% against
the true optimum, so they need no optimising. Each station therefore has a fixed **ladder** of wells, computed
once at load from `bodies.csv` and `stations.csv`:

| Station | Parking orbit (km/s) | Planet's well from the moon's distance (km/s) | Total (km/s) |
|---|---|---|---|
| Low Earth Logistics (500 km) | 7.6 | | 7.6 |
| Earth Sunward (1,500 km today) | 7.1 | | 7.1 |
| Venus (900 km) | 6.8 | | 6.8 |
| Mars (1,200 km) | 3.1 | | 3.1 |
| Mercury (600 km) | 2.7 | | 2.7 |
| Ceres (400 km) | 0.3 | | 0.3 |
| Luna (200 km) | 1.6 | 1.0 (Earth) | 2.6 |
| Titan (800 km) | 1.6 | 5.6 (Saturn) | 7.2 |
| Ganymede (500 km) | 1.8 | 10.9 (Jupiter) | 12.7 |

Within one planet's system, a plasma hop between two circular orbits costs |v_circ,1 − v_circ,2| the same way.

Size of the effect (rough): a plasma freighter has about 10 MW and a full standard tank gives a budget near
275 m²/s³ without cargo. At a = 0.005 m/s², LEO costs 38 m²/s³ and 18 days, Ganymede 64 m²/s³ and 29 days. One
way LEO → Ganymede would spend over a third of the budget and about seven weeks in the wells. If the
measurements confirm this, the wells will reshape the plasma fleet's routes, and where stations sit (the
Sunward study; perhaps high orbits for plasma ports everywhere) becomes the main question of item 3.

### 2. The acceleration needs no search

For the fastest trip on a given budget, every phase must trade effort for time at the same rate. A spiral's
rate is −dJ/dT = a². The heliocentric leg's is `effort_time_slope` (|p(T)|² for a target on its circle), which
the planner already computes for the fuel fit. So a = the leg's thrust acceleration at its end. In practice:
take the leg for the budget left after the spirals, read its slope, set a, repeat once or twice. With the
cost-weighted objective (propellant price and daily cost) the same rule holds, the common rate being the price
ratio. One acceleration serves every spiral of a plan.

### 3. The heliocentric leg, and what changes about its target

Today the leg starts co-moving with the origin planet and ends co-moving with the destination: the atlas
refinement targets v_r = 0 and v_θ = v_circ. With spirals this is no longer exactly right:

- the escape spiral hands the ship over 1-2 km/s faster than the planet (it reaches escape energy at roughly
  200,000 km from Earth for a = 0.01 m/s², well inside the SOI, and is by then part of the heliocentric flight);
- the capture spiral, computed backwards from the parking orbit, needs the ship to arrive with a similar
  velocity offset relative to the destination planet;
- a station at L1 moves 0.45 km/s slower than a circular orbit at its radius (Sunward study).

So the true leg has a **different start state and a different target velocity**. Two levels:

- **Scoring plans keep today's leg** (co-moving at both ends) and add the spirals' time and effort. The error
  is what a 1-2 km/s offset at the ends costs when spread over the leg: estimated at a few percent of the
  budget for 100-day legs, more for short ones (it scales as Δv²/T), and partly a gain rather than a loss when
  the offset points along the flight. In 2D the direction of the offset is free: a low parking orbit passes
  every angle within hours, so the ship leaves the spiral pointing where the leg wants to go.
- **The chosen mission's plan is solved exactly**: start = the escape spiral's end state, target = the capture
  spiral's entry state (moving with the destination planet), through the general solver
  (`variable_isp/GeneralTransfer`, which takes any boundary states; median 0.4-0.8 ms on its bench sets), warm
  started from the atlas leg's primer. `refine_seed_stm` already goes through that solver, so the natural place
  is to give it an end state with an offset instead of v_circ. If the exact solve needs more effort than the
  budget allows, the existing larger-budget and fuel-fit steps apply.

The first step must measure the gap between the scoring estimate and the exact plan per route. If it is large
somewhere (short hops, the Jupiter system), add a per-route correction factor to the scoring rather than a
search.

## Display

Do not send spiral samples through the bridge: tens to hundreds of turns per ship would swamp the paths file.
A slow tangential spiral has a closed form (v₀ the starting circular speed):

    v(t) = v₀ − a t        r(t) = μ / v(t)²        θ(t) = θ₀ + (v₀⁴ − v(t)⁴) / (4 a μ)

valid until near escape, where a short tabulated tail (the family has one parameter, ε = a r²/μ; The Long Burn's
`spiral_table.rs`) or a few integrated samples take over. The bridge sends the parameters (body, r₀, a, start
time, direction); Godot draws the spiral in the planet's frame, as it draws planet-system paths now, and places
the ship on it. The simulation needs the ship's position during a spiral only as "at this planet, at radius
r(t)" (radiation zone, eclipses).

## What else it touches

- **Radiation:** spiral days inside the belts count for the crew (`station_radiation.csv` has the zones).
- **Chases:** police and salvage follow a ship's path samples (`PathMotion`). Suggested rule: a ship inside a
  spiral cannot be met; chases target the heliocentric phase or wait. To be decided by the user.
- **Engine-failure rescue (item 2):** a failure during a spiral leaves the ship on a bound orbit near a
  station; treat it like today's local recoveries.
- **Economy:** reference prices and fares, fuel sizing, purchase valuations and the fleet mix all move. Run
  `--write-reference-prices`, `--write-reference-fares`, the benchmark and `purchase_returns.py`.
- **Launch windows:** the heliocentric leg departs one escape-spiral time after the ship leaves the station;
  the window search gets that prefix, as it has wait prefixes now.

## Steps

1. **Measure** (no behaviour change): for every route and drive, log what the end caps would add (Δv, effort,
   days, belt days) next to today's plan, in a headless report. Decide station positions from it (Sunward
   study).
2. **Charge the costs** in both planners, paths unchanged (ships are drawn as now). Recalibrate and benchmark.
   The speed check: `--profile` before and after; scoring cost should be unchanged.
3. **Exact hand-off** for the chosen plan (plasma) and the measured gap against the scoring estimate.
4. **Draw** the spirals and hyperbolas; ship positions inside wells.
5. Optional refinements only where step 1 or 3 shows a need: the Oberth dive for moon stations, per-route
   scoring corrections, hyperbola times.

## What carries over from The Long Burn, and what does not

- **Carries over:** the one-parameter spiral family and its table; the 1-3.5% measurement for constant
  tangential spirals; one acceleration per plan with the equal-rate rule; the canonical solver frame (already
  taken back, `docs/bugs/spacetrains_upstream.md`); judging solver changes against a reference set rather than
  against the previous answers.
- **Does not:** the joint search over acceleration and turns, the route tables of J*(date, time) and the SOI
  checks along the path. They pay for exactness one ship needs and a fleet does not.
- **Easier here:** 2D removes the plane changes and makes the exit direction free.
