# Ship Technology Plausibility Review (planned)

## Goal

Put every ship class on one consistent, near-future technology level, so that which class wins on which route
comes out of physics and the cost model, not out of one class being decades more advanced than another. The
current class list is not final; the review may rename, retune or replace classes.

## Preliminary Findings (2026-10-06, from `data/ship_classes/ship_classes.csv`)

Implied specific impulse is `max_delta_v / ln(full mass / dry mass) / g0`.

| Class | Labelled | Dry | Propellant | Mass ratio | Δv | Implied Isp / power |
|---|---|---|---|---|---|---|
| light_freighter | chemical | 8 t | 56 t | 8 | 20.4 km/s | 999 s |
| tanker | chemical | 12 t | 60 t | 6 | 16.3 km/s | 925 s |
| fast_courier | chemical | 5 t | 45 t | 10 | 27.1 km/s | 1200 s |
| deep_space_freighter | chemical | 12 t | 132 t | 12 | 34.1 km/s | 1400 s |
| ntr_freighter | chemical | 10 t | 190 t | 20 | 41.1 km/s | 1400 s |
| ion_freighter | electric_ion | 10 t | 10 t | 2 | - | α = 300 W/kg |
| ion_courier | electric_ion | 5 t | 7 t | 2.4 | - | α = 800 W/kg |

Issues to check:

1. **"Chemical" ships are nuclear-thermal.** Chemical engines top out around 450-465 s (LOX/LH2). 900-1000 s
   is solid-core NTR (NERVA demonstrated ~850 s). 1200-1400 s needs liquid- or gas-core reactors, which are
   much further out. The Kepler classes span two or three technology generations.
2. **Dry masses are far too low for the tanks.** 190 t of hydrogen is about 2,700 m³; its tank structure,
   insulation and boil-off control alone would weigh well over the 10 t the NTR freighter has in total,
   before the reactor (NERVA-class: about 10 t), shielding, crew habitat and cargo hold. Mass ratios of 12-20
   are not credible for crewed cargo ships.
3. **Ion power density is about an order of magnitude ahead.** Today's solar-electric systems reach about
   10-20 W/kg; aggressive nuclear-electric studies target 30-100 W/kg. 300-800 W/kg is far-future.
4. **Cargo is tiny next to the ship.** Holds of 8-60 units (1-12 t) on ships of 12-200 t wet mass. Cargo mass
   is not in the rocket equation yet (part B of the operating-cost plan), which hides this.
5. **Crew habitats and life support** are not in the dry mass at all.

## Method

1. Choose a target era (proposal: about 2080-2100, i.e. mature solid-core NTR, megawatt-class nuclear-electric
   propulsion, no fusion) and write the reference numbers for it with sources: Isp, thrust-to-weight,
   engine and reactor specific mass, tank fraction per propellant, habitat mass per crew member.
2. Rebuild each class bottom-up from those numbers: engine + reactor + tankage + habitat + structure + hold.
   Derive dry mass, Δv with and without cargo, and acceleration; ion α from the power plant.
3. Price ships from the same breakdown, so `ship_value_cr` follows the hardware.
4. Re-check the trajectory and economy audits; record which class wins which route, and why.

## Dependencies

- Do it together with, or right before, part B of `ship_operating_costs.md` (cargo mass in the rocket
  equation, mission-sized fuelling), since both change the same numbers.
- The VariableISP atlas is dimensionless (ρ, κ, θ), so new α and masses should not need a new atlas, only
  checks that the new κ range is covered.
