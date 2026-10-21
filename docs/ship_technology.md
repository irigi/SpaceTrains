# Ship Technology: Reference Numbers and Sources

Status: the review is complete (2026-10-07). It remains the source of the era's numbers for
`data/ship_classes/build_ship_classes.py`, which builds every class from them.

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

## Reference Range for Variable-Isp Ships (user's `~/Projects/VariableISPRocketTrajectories/linopt/`)

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

linopt (`~/Projects/VariableISPRocketTrajectories/linopt/`) has two parameter sets. Hardware mass per jet watt is
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

### Component details (added 2026-10-11, ship structures tier 1)

These describe the parts in `ship_components.csv`. They do not change any mass or price above (see
`docs/plans/ship_internals.md`).

| Quantity | Value | Status |
|---|---|---|
| Radiator areal density (two-sided heat-pipe panel) | 3 kg/m² (designs reach 2-3.6) | sourced: [NASA Glenn carbon-carbon radiator, NTRS 20080048181](https://ntrs.nasa.gov/citations/20080048181), [UNM ISNPS Tech Report 99](https://isnps.unm.edu/reports/ISNPS_Tech_Report_99.pdf); 5.2 kg/m² for a ceramic design: [TFAWS 2024](https://tfaws.nasa.gov/wp-content/uploads/TFAWS2024-PT-23-Paper.pdf) |
| Radiator emissivity | 0.85 | estimate |
| Drive radiator temperature, Set A (4 kW/kg) | **594 K**; Set B (10 kW/kg) would need 747 K | derived: 2εσT⁴ / (3 kg/m²) = heat per kg |
| Plasma Freighter radiator area | 357 m² (1.07 t, 4.3 MW) | derived |
| Solid-core reactor core temperature | 2,700 K | estimate within the NERVA range |
| Liquid-core reactor core temperature | ~5,900 K | derived: Isp ∝ √T, (1400/950)² × 2,700 K |
| Nuclear-thermal jet power | ½ F vₑ: Light Freighter ~330 MW, Fast Courier ~460 MW | derived |
| LH2 density, tank temperature | 71 kg/m³, 20 K | sourced (UMD MERs above) |
| Plasma propellant | water, 1,000 kg/m³, 293 K | game (linopt allows water or argon) |
| Habitat split, 4 crew / 500 days, dry 32,022 kg | structures 14,116 · environment protection 4,826 · ECLSS 6,890 · thermal 2,868 · power 924 · avionics 1,321 · crew systems 807 · EVA 272 kg | sourced: [NASA Deep Space Habitat, NTRS 20120014530](https://ntrs.nasa.gov/api/citations/20120014530/downloads/20120014530.pdf), "Mass Summary" |
| Crew systems split | crew quarters 260, galley 154, wardroom 52, medical 178, exercise 94 kg (+ restraints, laptops, lights) | sourced: same, "Crew Systems Mass by Mission" |
| Habitat volume | 193 m³ pressurized, 90 m³ habitable for 4 crew (48 and 22.5 m³ per crew) | sourced: same |
| Habitat heat load and power | 12.9 kW for 4 crew (3.2 kW per crew), ECLSS 6.4 kW of it | sourced: same, "DSH Thermal Control Heat Rejection" |
| Cabin and cabin-radiator temperatures | 295 K, 277 K | estimate (ISS-style ammonia loop at 2-6 °C) |
| Hold volume | 1 m³ per cargo unit | game |

### Crew radiation (added 2026-10-21, ship structures tier 2, step D)

Values in `data/economy/ship_environment.csv` (defaults in `domain::ShipEnvironmentDefinition`) and
`data/economy/station_radiation.csv`; the model is `src/simulation/CrewEnvironment.cpp`. Doses are effective
dose, mSv.

| Quantity | Value | Status, source |
|---|---|---|
| Cosmic rays (GCR) in deep space behind a habitat hull | 1.84 mSv/day, cycle mean | sourced: Curiosity RAD in cruise, Zeitlin et al. 2013 (*Science* 340) |
| Solar-cycle modulation | ±30%, highest at solar minimum | estimate (GCR dose varies about 2× minimum to maximum) |
| Radial gradient | +3%/AU | estimate (Pioneer/Voyager gradients 2-4%/AU) |
| Extra water against GCR | dose × exp(−x / 46 g/cm²) | estimate: ~35% less behind 20 g/cm²; Monte Carlo studies give 38-50% for 20 g/cm² CFRP ([Naito & Kodaira 2022](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC9365775/)) |
| Storm shelter occupancy | 1/3 of the day (the crew sleeps there) | game choice |
| August 1972 event, blood-forming organs | ~1300 mSv inside a pressure vessel | sourced: ~1.3 Gy BFO inside an aluminium pressure vessel, 0.5 Gy in an equipment room ([NTRS 20040100724](https://ntrs.nasa.gov/citations/20040100724)) |
| … behind 20 g/cm² of water | ~200 mSv | sourced: 20 g/cm² of water keeps the 1972 event under the 25 rem 30-day limit ([NTRS 19880016043](https://ntrs.nasa.gov/api/citations/19880016043/downloads/19880016043.pdf)); 200 is an estimate below that bound |
| Event sizes | power law N(>M) ∝ M^−0.5, 0.01-2 × the 1972 event, 0.5 a year above 0.01 on cycle average (none at minimum, twice at maximum) | estimate: a 1972-class event about once in 20 years (1956, 1972, 1989) |
| Event dose with distance | 1/r² | estimate (fluence falls about as r⁻², peak intensity faster) |
| Career limit (per leg: a crew is signed on per leg) | 1000 mSv | sourced: ESA career limit 1 Sv; NASA-STD-3001 (2022) uses 600 mSv |
| Hazard pay | 0.43 cr/mSv | derived: ICRP 103 nominal risk 5.5%/Sv × ~$10M value of a statistical life = $550/mSv, at 0.00078 cr/$ |

Docked crews live in the station's habitat: dose = GCR × location factor × exp(−station shielding / 46 g/cm²).
Anchors: ISS crews get ~0.5-0.7 mSv/day (LEO factor 0.3 → 0.55); Chang'e-4 measured 1.37 mSv/day on the lunar
surface (airless surfaces 0.74); Venus aerostats and Titan sit under atmospheres like or thicker than Earth's
(~0.001 mSv/day at sea level). Ganymede's mine sits at the equator inside the moon's own magnetic field, where
the particle flux is like Callisto's (Cooper et al. 2001, *Icarus*); the crossing of Jupiter's radiation belts on
approach is not modelled.

Not modelled: crew dose histories across legs (each leg signs on a fresh crew), shielding of the whole habitat
against cosmic rays (about 20 t of water per 10 g/cm² on a Deep Space Habitat: never worth it), and
radiation-hardened ship parts.

### Boil-off and wear (added 2026-10-21, ship structures tier 2, steps F and H)

| Quantity | Value | Status, source |
|---|---|---|
| MLI heat leak into LH2 tanks | 0.2 W/m² at 1 AU, × r⁻¹ | estimate (high-performance MLI in vacuum: 0.1-0.4 W/m²) |
| Tank area | 1.3 × a sphere of the tank volume | game geometry (several cylinders) |
| LH2 latent heat | 446 kJ/kg | sourced (handbook value) |
| 90 K broad-area-cooled shield | intercepts 60% of the leak | sourced bound: up to 70%, theoretical (NASA Glenn, Christie et al. 2011, NTRS 20110016177) |
| 20 K cryocooler | ~100 W input per W lifted, ~5 kg per W lifted | estimate (Carnot COP 20/280; ~14% of Carnot) |
| 90 K cryocooler | ~12 W per W lifted, ~1 kg per W lifted | estimate (Carnot COP 90/210; ~20% of Carnot) |
| Cooler power | engine's bimodal Brayton loop, ~10 kg/kWe | estimate (NASA bimodal NTR studies, Borowski: ~25 kWe per stage) |
| Nuclear-thermal core life | 10 h of full-power burn | sourced: NERVA flight-engine goal, 10 h over 60 restarts |
| Plasma drive life | 50,000 h of thrust | estimate (NASA NEXT ion thruster class) |
| Meteoroids holing a heat pipe | ≥ 10⁻⁵ g: 0.146 per m² per year at 1 AU, × r^−1.3 | sourced flux model: Grün et al. 1985 (Icarus 62); 0.2 mm grain through a 1 mm wall at ~20 km/s (estimate); r-scaling estimate |
| Radiator pipe share, segment size | 30% of the panel, 0.2 m² | estimates |
| Overhaul spares | core fuel elements 40% of the engine mass (reactor fuel); plasma drive 30% (machinery, electronics); radiators 50% of the lost panels (machinery) | estimates |
| Cold hold | 255 K, spoilage × 0.1; MLI + 2 kg/m² liner, ~0.5 W/m² leak | estimates |

### Open questions for the user

1. Variable-Isp hardware: Set A (recommended) or Set B?
2. *Resolved:* `specific_engine_power_w_per_kg` is **jet power** per kg of dry mass
   (`VariableIspTrajectoryPlanner.cpp:109` feeds it straight into κ and the I-invariant). So α = jet power /
   (hardware + tanks + habitat + structure + hold), and the hardware rows above are per jet watt.
3. Sources still needed for the liquid-core Isp and T/W and the in-space LH2 tank fraction. Both are
   currently estimates.

## Rebuilt Classes (method step 2, 2026-10-07)

Generated by `data/ship_classes/build_ship_classes.py` (writes `ship_classes.csv`; `--markdown` prints this
table). Set A hardware, LH2 tanks 0.18, long-endurance habitats 8 t/crew, structure 10%, hold 10% of rated
cargo at 200 kg/unit. Choices made in this step:

- Crews trimmed for cargo work: freighters 3-4, couriers 2 (were up to 6).
- Cargo holds keep their old unit counts. Dispatch reserves whole holds when it scores follow-up loads,
  so bigger holds are a separate economy change.
- Nuclear-thermal engines are sized for 0.5 m/s² at full tanks (0.3 for the two big liquid-core ships).
- Plasma ships keep their old α and κ (same trip performance); the jet power grows with the heavier ship.
- Ship values are priced from the breakdown (see Pricing below). Display names: ion → Plasma.
  Renamed afterwards (2026-10-07): `propulsion_type` is `nuclear_thermal` or `variable_isp` (the loader
  rejects anything else), and `ion_freighter` / `ion_courier` are `plasma_freighter` / `plasma_courier`.
- Starting propellant in `ships.csv` keeps each ship's old fraction of a full tank.

| Class | Drive | Crew | Engine | Radiators | Tanks | Habitat | Hold | Structure | Dry | Propellant | Performance | Price |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Light Freighter | solid core | 3 | 2.3 t | - | 16.2 t | 24.0 t | 0.8 t | 4.3 t | 47.7 t | 90.0 t | 950 s, Δv 9.9 km/s (9.0 laden), 0.50 m/s² | 59 M$ = 46k cr |
| Orbital Tanker | solid core | 3 | 2.3 t | - | 16.2 t | 24.0 t | 1.2 t | 4.4 t | 48.1 t | 90.0 t | 950 s, Δv 9.8 km/s (8.5 laden), 0.50 m/s² | 59 M$ = 46k cr |
| Fast Courier | liquid core | 2 | 6.8 t | - | 16.2 t | 16.0 t | 0.3 t | 3.9 t | 43.2 t | 90.0 t | 1400 s, Δv 15.5 km/s (14.8 laden), 0.50 m/s² | 103 M$ = 80k cr |
| Plasma Freighter | plasma | 3 | 1.3 t | 1.0 t | 1.5 t | 24.0 t | 0.4 t | 2.8 t | 31.0 t | 30.0 t | 9 MW jet, α = 291 W/kg, κ = 1.6 | 52 M$ = 40k cr |
| Plasma Courier | plasma | 2 | 2.6 t | 1.9 t | 1.5 t | 16.0 t | 0.2 t | 2.2 t | 24.4 t | 30.0 t | 18 MW jet, α = 738 W/kg, κ = 4.6 | 54 M$ = 42k cr |
| Deep Space Freighter | liquid core | 4 | 8.0 t | - | 32.4 t | 32.0 t | 0.4 t | 7.3 t | 80.0 t | 180.0 t | 1400 s, Δv 16.2 km/s (15.7 laden), 0.30 m/s² | 149 M$ = 116k cr |
| NTR Freighter | liquid core | 4 | 10.6 t | - | 45.0 t | 32.0 t | 0.6 t | 8.8 t | 97.0 t | 250.0 t | 1400 s, Δv 17.5 km/s (16.9 laden), 0.30 m/s² | 184 M$ = 144k cr |

Nuclear-thermal Δv roughly halves (Light Freighter 20.4 → 9.9 km/s, NTR Freighter 41.1 → 17.5 km/s). Mass
ratios of 3-3.6 replace 6-20, because tanks, habitat and engine now count.

Audit (730 days, `--econ-audit`): cargo margin 324k, fuel 3.6k, wages 38k, 7/22 profitable, 13 laid up,
3 stranded, 43 CRITICAL (v16: 690k, 10/22, 9 laid up, 0 stranded, 34). Causes found by tracing:

1. **Fuel volume.** A rebuilt Light Freighter burns ~44 t for Mars -> Earth; Earth L1 sells 17.5 t, so the
   return-fuel check rejects the trip. The whole system produces ~6 t of fuel per day, most of it at Venus.
   Ships strand at Low Earth Logistics (consumes fuel, produces none) until stock arrives. A scratch run
   with fuel production x5, starting fuel x3 and storage x2 gave 384k margin, 12 laid up, 2 stranded.
2. **Reach.** Solid-core ships (≤ 10 km/s) cannot reach Mercury, Venus or the outer system from Mars.
3. **Revenue per trip.** Holds of 1-12 t on ships of 25-100 t dry; cargo pays the same as before while
   the ship is heavier.

Follow-up (v18, fuel depots): every station refills its fuel toward 3000 units at up to 200 units/day
(`data/economy/fuel_supply.csv`), in tanks outside its cargo storage, and dispatch counts the refills a
destination makes before the ship arrives. Cargo margin 648k, fuel 168k (fuel now sells at base price
instead of the old 0.25x glut price, and the ships are heavier), 8/22 profitable, 7 laid up, 0 stranded,
35 CRITICAL. Five of the six plasma ships are laid up: three times heavier than before with the same small
holds and their old prices. Pricing (step 3) and hold sizes come next.

## Pricing (method step 3, 2026-10-07)

`build_ship_classes.py` prices each part per kg: plasma engine 10,000 $/kg, radiators 1,500, dense-propellant
tanks 300, habitat 1,500, structure and hold 300 (all linopt); solid-core NTR engine 5,000, liquid-core
10,000 and LH2 tanks 600 $/kg (estimates). Dollars convert at 0.00078 cr/$, chosen once so the starting
fleet's total value stayed at the old 1.65 M cr: relative prices follow the hardware, the total capital
burden does not move. Cross-check: linopt's 20 $/kg propellant becomes 0.016 cr/kg; the game's fuel base
price is 0.08 cr/kg.

Habitat dominates every price (24-36 M$ of 52-184 M$), so plasma ships get cheaper (90k -> 40k cr): a 9-18 MW
drive is only 1-3 t of hardware. Liquid-core ships get dearer (NTR Freighter 120k -> 144k, Deep Space
Freighter 70k -> 116k) for their big engines and LH2 tanks.

Audit (v19, 730 days): cargo margin 790k, fuel 171k, wages 71k, capital 274k, 9/22 profitable, 6 laid up,
0 stranded, 31 CRITICAL. Seven ships never move. Two Mars light freighters (≤ 10 km/s; Earth trips don't pay
for 46 t ships carrying 8 t), the NTR Freighter at Ganymede (needs 229-260 t + 10% reserve for any inner
destination, holds 250 t), and the Plasma Freighter at Ganymede and Titan (κ 1.6: no atlas window inward
from Titan; Ceres trips take 3,000-5,000 days). The Plasma Courier (κ 4.6) does leave the outer system.
These are class sizing questions for fleet investment.

## Cargo Holds and Market Scale (2026-10-07)

Holds now rate about each freighter's dry mass of cargo (Light Freighter 200 u, Tanker 300, Plasma Freighter
150, Deep Space 300, NTR Freighter 400; couriers 30). The plasma freighter's jet power went 9 -> 10 MW to keep
α ≈ 290 W/kg with the heavier hold.

Big holds exposed two economy problems (see the v20-v24 calibration log, in git history as `docs/plans/ship_operating_costs.md`):

1. **Trade pricing.** A delivery was paid entirely at the station's pre-trade price, so 150 units into a
   starving station all sold at the 16x scarcity price (Lunar Gateway -650k in v20). Trades now settle
   along the price curve (exact integral), and dispatch scores part loads (1, 1/2, 1/4 of the hold).
2. **Market size.** Total consumption was ~8 units/day for the whole system, so a 200-unit hold was 25 days
   of everyone's demand. Recipe rates are now per 10,000 inhabitants and x10; storage and starting stocks
   scale by population / 1000. System demand (~80 u/day) is now about the fleet's carrying capacity.
3. **Forecasting.** Dispatch values a sale on the destination's stock at arrival (today's stock, cargo
   already inbound, consumption meanwhile), so ships stop converging on the same starving port.

Result (v24): cargo margin 929k against 565k of costs, 8/22 profitable, 4 laid up, 0 stranded, 36 CRITICAL;
run time 2m07. Consumer stations still lose money (Lunar Gateway -115k): that is the open-economy step.
Ships that never move are the outer-system and Mars classes noted under Pricing.

## Method

1. Fix the era from the linopt range above (advanced non-torch fusion or plasma drives; liquid- or gas-core
   thermal rockets) and write the reference numbers for it with sources: Isp, thrust-to-weight, engine and
   reactor specific mass, radiator specific mass, tank fraction per propellant, habitat mass per crew member.
2. Rebuild each class bottom-up from those numbers: engine + reactor + tankage + habitat + structure + hold.
   Derive dry mass, Δv with and without cargo, and acceleration; ion α from the power plant.
3. Price ships from the same breakdown, so `ship_value_cr` follows the hardware.
4. Re-check the trajectory and economy audits; record which class wins which route, and why.

## Dependencies

- Do it together with, or right before, part B of the operating-cost plan (`docs/modules/ships_and_fleet.md`) (cargo mass in the rocket
  equation, mission-sized fuelling), since both change the same numbers.
- The VariableISP atlas is dimensionless (ρ, κ, θ), so new α and masses should not need a new atlas, only
  checks that the new κ range is covered.
