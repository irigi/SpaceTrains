# Where Earth Sunward Terminal should sit

Advice for roadmap item 3 (patched conics), 2026-11-07. Not built; decide with the item 3 measurements.

## Today

`earth_l1` (Earth Sunward Terminal) orbits Earth at 1,500 km. That is inside the inner Van Allen belt, where
trapped protons are densest, so no one would put 22,000 people there. In the game it does the same job as Low
Earth Logistics (500 km), only higher. Planners charge only heliocentric burns, so leaving Earth costs nothing
from either station.

## Recommendation

Move it during the item 3 recalibration, preferably to the **Sun-Earth L1 point**. A high Earth orbit above the
belts is the cheap fallback. Do it only together with item 3: until the gravity wells cost something, L1 is just
1% closer to the Sun than Earth and the move changes nothing in play.

## Why: the gain is not mainly propellant

- **Plasma ships** (low thrust, exhaust 50-250 km/s) cannot use a deep, quick burn (the Oberth effect). A spiral
  out of a circular orbit costs about its orbital speed: ~7.6 km/s from LEO, ~7.1 km/s from 1,500 km, against
  ~3.2 km/s impulsive. That is only 3-14% of the ship's mass in propellant. The cost is **time and radiation**: at
  ~0.01-0.02 m/s² the spiral takes roughly 4-10 days, much of it inside the belts. Crew days and crew radiation
  are already modelled, so both would count.
- **Nuclear-thermal ships** (900-1400 s, high thrust) burn deep and quick, where escape is cheap for them, but
  ~3 km/s is a large part of their Δv. For them the gain is **propellant**.

## What makes sense for people and for the game

- **Two jobs, two places:** LEO receives cargo launched from Earth, and Earth sells there. L1 is the deep-space
  port where plasma ships start and end their trips without ever entering the well. Nuclear-thermal ships work
  the LEO↔L1 shuttle, which suits them.
- **New trade:** a real LEO→L1 run replaces Earth stocking both gateways equally. Earth's goods at L1 carry the
  cost of the climb.
- **It fits the design:** at L1 the Sun is always in the same direction and there are no eclipses, so the fixed
  sun shield makes sense. The id `earth_l1` and the name "Sunward" become literal again.
- **Downsides:** L1 is outside Earth's magnetic field, so the habitat needs mass shielding, like every station
  except LEO. Holding position (a halo orbit) takes a few m/s per year, which can be ignored.

## What the simulation would need for L1

- **New station kind:** stations are placed as altitude over a body on a circular orbit. L1 needs a point fixed in
  the rotating Sun-Earth frame (planar: on the Sun-Earth line, ~1.5 million km sunward of Earth).
- **Arrival speed:** L1 moves at Earth's angular rate, ~0.45 km/s slower than a circular orbit at its radius
  (29.48 against 29.93 km/s). `VariableIspIntegrator::refine_seed()` targets v_θ = v_circ and has to accept this.
  The general chase solver already handles any arrival state.
- **The LEO↔L1 leg:** 1.5 million km is at the edge of Earth's Hill sphere, beyond the bounded local direct arc
  used between stations around the same planet. That leg belongs in the item 3 escape and capture model.
- **Economy:** Earth's surface sales at L1 (`surface_depth_units`) should carry the climb cost, or Earth sells only
  at LEO and ships haul the rest.

## Fallback: a high Earth orbit

Roughly 70,000 km altitude, above the outer belt. A spiral out costs ~2.3 km/s. Only the `altitude_m` in
`stations.csv` changes and the circular-orbit model stays, so you get most of the time and radiation benefit.
You lose the constant sunlight and the L1 identity.

## Order

1. Measure what the missing wells cost per route (item 3, first step).
2. Choose L1 or a high orbit using those numbers. L1 is likely to win if item 3 builds the frame handling anyway.
