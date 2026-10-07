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
3. **Ion power density is about an order of magnitude ahead of electric propulsion.** Today's solar-electric
   systems reach about 10-20 W/kg; aggressive nuclear-electric studies target 30-100 W/kg. 300-800 W/kg is
   intended, though: it matches the user's reference range below, which is a fusion/plasma-drive level.
   Cargo mass now counts in the planners (part B), so the class names should say what the engine is.
4. **Cargo is tiny next to the ship.** Holds of 8-60 units (1-12 t) on ships of 12-200 t wet mass. Cargo mass
   is in the rocket equation since part B, so larger holds now cost propellant honestly.
5. **Crew habitats and life support** are not in the dry mass at all.

## Reference Range for Variable-Isp Ships (user's `~/VariableISPRocketTrajectories/linopt/`)

The user's design studies (`passenger_ticket_optimizer.py`, `time_optimal_transfer_solver.py`) set the intended
range for the variable-specific-impulse classes:

| Parameter | linopt default | Meaning |
|---|---|---|
| Max exhaust speed | 50-250 km/s | Isp about 5,100-25,500 s |
| Engine core | 20 kW/kg | total power per kg of engine (fare optimizer default) |
| Radiators | 10 kW/kg | waste heat rejected per kg of radiator |
| Waste heat | 15% of total power | 85% of the power ends up in the jet |
| Aggregate propulsion hardware | 0.25 kg/kW (4 kW/kg) | solver/spreadsheet default (10 kW/kg engine, 30% heat, 4 kW/kg radiators), not the set above; see Reference Numbers |
| Tank mass | 5% of propellant | |
| Reference ship | 500 t payload, ~240 t hardware, ~1 GW | whole-ship power/dry mass about 540 W/kg |

Assessment:
- The game's ion classes (alpha 300 and 800 W/kg of dry mass) sit inside this range: a linopt reference
  ship has about 540 W/kg at the whole-ship level. So the VariableISP classes already match the intended era.
- 15% waste heat is beyond any nuclear-electric chain (reactor to electricity at 25-40%, thruster at 60-80%:
  70-85% of reactor power is waste heat, and radiators dominate the mass at 10s of kg/kW). It fits a
  non-torch fusion drive that exhausts plasma directly through a magnetic nozzle, with radiators for the
  remaining losses. Accelerations stay around 0.01-0.02 m/s², so it is not a torchship.
- Decision (user, 2026-10-06): no torchships; radiators and efficiency limits must stay in the model, but
  do not cut ship efficiency much. So the review keeps the variable-Isp classes at this level and labels
  them honestly (advanced fusion or plasma drive, with radiators), rather than downgrading them to
  present-day ion engines.
- Consequence for the Kepler classes: in the same era, liquid- or gas-core thermal rockets at 1000-1400 s
  are a consistent pairing; the real problems are the "chemical" label, dry masses and tankage, and the
  tiny holds (issues 1, 2 and 4 above).

## Reference Numbers (method step 1, drafted 2026-10-07)

Status per row: **sourced** (a published number), **derived** (computed from sourced rows), **estimate** (my
engineering judgement, still needs a source), **game** (a modelling choice, not a physical claim).

### Variable-Isp drive (advanced fusion/plasma, magnetic nozzle, radiators)

linopt (`~/VariableISPRocketTrajectories/linopt/`) has two parameter sets. Hardware mass per jet watt is
`1 / ((1 - φ) α_eng) + φ / ((1 - φ) ρ_rad)`.

| Quantity | Set A: solver/spreadsheet | Set B: fare optimizer | Status |
|---|---|---|---|
| Engine core α_eng (total power / engine kg) | 10 kW/kg | 20 kW/kg | game (linopt) |
| Waste heat φ (heat / total power) | 30% | 15% | game (linopt) |
| Radiators ρ_rad (heat / radiator kg) | 4 kW/kg | 10 kW/kg | game (linopt) |
| Propulsion hardware (engine + radiators) | 0.25 kg/kW_jet = 4.0 kW_jet/kg | 0.077 kg/kW_jet = 13.1 kW_jet/kg | derived |
| Split engine / radiators | 57% / 43% | 77% / 23% | derived |
| Exhaust speed | 50-250 km/s (Isp 5,100-25,500 s) | same | game (linopt) |
| Tank mass | 5% of propellant | same | game (linopt); plausible for dense propellant (water, argon), not for LH2 |
| Hardware cost | engine 10,000 $/kg, radiators 1,500 $/kg, tanks 300 $/kg | same | game (linopt) |

The "≈540 W/kg whole-ship" reference ship (500 t payload, ~240 t hardware, ~1 GW) is built on **Set A**.
With Set B the same ship needs only ~76 t of hardware for 1 GW. Recommendation: use **Set A** as the
baseline. It is the one the 540 W/kg decision was made on, and its radiators stay a large share of the mass,
in line with "keep radiators". Set B can be a later "improved drive" generation.

### Nuclear-thermal rockets (Kepler classes, LH2 propellant)

| Generation | Isp | Engine thrust/weight | Status / source |
|---|---|---|---|
| NERVA XE-Prime (tested 1969) | 841 s vac | 247 kN, 18.1 t → ~1.4 | sourced: [NERVA](https://en.wikipedia.org/wiki/NERVA) |
| Solid core, NERVA-derived (NASA DRA 5.0, 25 klbf) | 900-910 s | ~3.4 (111 kN, ~3.3 t) | sourced: [Borowski et al., NTRS 20120012928](https://ntrs.nasa.gov/archive/nasa/casi.ntrs.nasa.gov/20120012928.pdf) |
| Advanced solid core (CERMET / composite) | ~950-1000 s | ~3 | estimate |
| Liquid core | ~1300-1500 s | ~1 | estimate (needs a source) |
| Closed gas core ("nuclear light bulb") | 1100-3200 s | 0.4-6.9 across the design range | sourced: [NTRS 19710028762](https://ntrs.nasa.gov/api/citations/19710028762/downloads/19710028762.pdf) |
| Open-cycle gas core | 2500-6500 s (4400 s at 6000 MW) | ~10⁻² powerplant T/W at 3000 s | sourced: [NTRS gas-core studies](https://ntrs.nasa.gov/api/citations/19700016142/downloads/19700016142.pdf); out of scope (user cap ~1400 s) |

Proposal for the game: two generations. **Solid core 950 s, T/W 3** (workhorse) and **liquid core / light bulb
1400 s, T/W 1** (premium, heavier engine). DRA 5.0's Copernicus Mars ship carries about 190 t of LH2 with three
engines: the same propellant load as our `ntr_freighter`, but on a ship that also needs drop tanks, a truss and
a ~40 t habitat.

### Tankage

| Propellant | Density | Tank mass / propellant mass | Status |
|---|---|---|---|
| LH2, launch-vehicle tank | 71 kg/m³ | 0.128 | sourced: [UMD ENAE 483 mass estimating relations](https://spacecraft.ssl.umd.edu/academics/483F24/483F24L08.MERs/483F24L08.MERsx.pdf) |
| LH2, long-duration in space (MLI + zero-boil-off cryocoolers, meteoroid shield) | 71 kg/m³ | **0.18** | estimate (0.128 + insulation and cooling) |
| Dense propellant for variable-Isp (water, argon) | 1000-1400 kg/m³ | 0.05 | game (linopt) |

### Crew

| Quantity | Value | Status |
|---|---|---|
| Habitat, long endurance (500 days, 4 crew, ISS-derived systems) | 32 t dry / 41.4 t wet → **8 t dry per crew** | sourced: [NASA Deep Space Habitat, NTRS 20120014530](https://ntrs.nasa.gov/api/citations/20120014530/downloads/20120014530.pdf) |
| Habitat, short hops (weeks; cislunar shuttles) | ~4 t per crew | estimate |
| Metabolic need, open loop | ~5 kg/person-day (0.84 O₂, ~0.6 food dry, rest water) | sourced: [eclss-assessment](https://skills.cat/skills/luncosim/space-engineering-skills/eclss-assessment), [NTRS 19900019017](https://ntrs.nasa.gov/api/citations/19900019017/downloads/19900019017.pdf) |
| Make-up with closed water/air loops | food 0.6, water 0.1, oxygen 0.05 kg/crew-day | game (already in `ship_operations.csv`), consistent with the row above |

### Structure and hold (game choices, no physical source needed)

| Quantity | Value | Status |
|---|---|---|
| Primary structure and truss | 10% of the other dry mass | game |
| Cargo hold (racks, containers, handling) | 10% of the rated cargo mass | game |

### Open questions for the user

1. Variable-Isp hardware: Set A (recommended) or Set B?
2. *Resolved:* `specific_engine_power_w_per_kg` is **jet power** per kg of dry mass
   (`VariableIspTrajectoryPlanner.cpp:109` feeds it straight into κ and the I-invariant). So α = jet power /
   (hardware + tanks + habitat + structure + hold), and the hardware rows above are per jet watt.
3. Sources still needed for the liquid-core Isp and T/W and the in-space LH2 tank fraction. Both are
   currently estimates.

## Method

1. Fix the era from the linopt range above (advanced non-torch fusion or plasma drives; liquid- or gas-core
   thermal rockets) and write the reference numbers for it with sources: Isp, thrust-to-weight, engine and
   reactor specific mass, radiator specific mass, tank fraction per propellant, habitat mass per crew member.
2. Rebuild each class bottom-up from those numbers: engine + reactor + tankage + habitat + structure + hold.
   Derive dry mass, Δv with and without cargo, and acceleration; ion α from the power plant.
3. Price ships from the same breakdown, so `ship_value_cr` follows the hardware.
4. Re-check the trajectory and economy audits; record which class wins which route, and why.

## Dependencies

- Do it together with, or right before, part B of `ship_operating_costs.md` (cargo mass in the rocket
  equation, mission-sized fuelling), since both change the same numbers.
- The VariableISP atlas is dimensionless (ρ, κ, θ), so new α and masses should not need a new atlas, only
  checks that the new κ range is covered.
