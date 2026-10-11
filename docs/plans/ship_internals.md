# Ship Internals: Structure, Modules, Condition

Goal (user, 2026-10-11): ships, and later stations, show their internal structure. At minimum:
- size, mass, power and temperature of the engine, radiators, tanks and payload;
- the payload split into crew quarters, common rooms and so on.

It should be doable and interesting, not a ship editor.

Three tiers:
1. **Show the structure.** Done 2026-10-11 and described below.
2. **Modules as refit choices**, with component condition and damage. Designed below; queued in `docs/roadmap.md`.
3. **Stations** get the same treatment. Sketch at the end.

The decisions are logged in `ship_structures_log.md`. Graphics are proposed separately in
`ship_visuals_proposal.md`.

## Tier 1: The Structure, Shown (built 2026-10-11)

Principle: **decompose, don't change.** The parts add up exactly to each class's existing dry mass and value, so:
- the simulation, economy, saves and benchmarks are untouched;
- a 365-day `--econ-audit` is byte-identical to the build before.

### Data
`data/ship_classes/build_ship_classes.py` already built every class from parts. It now also writes
`ship_components.csv`, one row per part per class variant (36 classes, 480 rows). Columns:

| Column | Meaning |
|---|---|
| `class_id, component_id, parent_id, kind, name` | `parent_id` is `habitat` for the habitat's own parts |
| `mass_kg, price_cr` | whole kg and credits; top-level parts sum exactly to the class row, habitat parts to the habitat |
| `power_w` | engines: jet power; habitat: electrical draw |
| `heat_w` | waste heat produced (engine, habitat) or rejected (radiators, cabin thermal control) |
| `temperature_k` | reactor core, radiator panel, tank contents, cabin |
| `volume_m3, area_m2, capacity_kg, crew_capacity` | tank and hold volume, radiator panel area, propellant or rated cargo, berths |
| `isp_s, thrust_n` | nuclear-thermal engines (plasma Isp varies along the path) |

Parts by drive:

| Part | Nuclear-thermal | Plasma (variable Isp) |
|---|---|---|
| Engine | solid core (950 s, core 2,700 K) or liquid core (1400 s, ~5,900 K); thrust, jet power (330-460 MW) | magnetic-nozzle drive; jet power (10-40 MW), waste heat (30% of the total power) |
| Radiators | none (the propellant carries the heat away) | panels at 594 K, 3 kg/m²; Plasma Freighter 357 m² |
| Tanks | LH2 at 20 K, 71 kg/m³ (90 t = 1,270 m³) | water at 293 K (30 t = 30 m³) |
| Habitat | NASA Deep Space Habitat split (below); 48 m³ pressurized and 3.2 kW per crew member | same |
| Hold, structure | rated cargo, 1 m³ per unit; truss | same |

The habitat (8 t per crew member) is split in the proportions of NASA's 500-day Deep Space Habitat
(NTRS 20120014530):
- pressure hull 44%
- life support (ECLSS) 22%
- meteoroid and thermal shielding 15%
- cabin thermal control 9%
- power and avionics 7%
- crew quarters 1%
- galley, wardroom, medical and exercise 1.5%
- airlock and EVA suits 1%

Sources and status (sourced, derived, estimate, game) are listed in `docs/ship_technology.md`, under
"Component details".

Checks:
- `--check` in the builder;
- `tests/test_build_ship_classes.py`, run by ctest as `ship_class_builder`, which also fails if the CSVs are stale;
- the C++ loader validates the sums;
- `tests/test_main.cpp` covers both the sums and a rejected broken file.

### Code
- `domain::ShipComponentDefinition`, with `ShipClassDefinition::components`, loaded and validated by
  `DataLoader`. The file is required.
- **Seam for tier 2:** `Simulation::ship_design(const ShipState&)`. Every ship-level lookup (14 call sites) asks
  it instead of `get_ship_class(ship.class_id)`. Today it returns the class.
- **Bridge:** a static `<snapshot>.designs` file (classes and parts), written like `.paths` with a sequence file
  and a signature, so tier 2 can rewrite it when fits change.
- **Live per-ship fields:**
  - `engine_state`: off, burning or coasting for nuclear-thermal ships; thrusting for plasma ships.
    Nuclear-thermal burns are impulsive in the planners; for display each lasts half the mission's propellant
    at the engine's mass flow.
  - `radiator_temperature_k`: the design temperature while thrusting, otherwise sunlit equilibrium,
    278.6 K/√r.
  - `cargo_mass_kg`, `provisions_mass_kg`.
- **Godot:**
  - The inspector has a collapsible "Ship systems" section (`InspectorPanel._add_ship_systems`).
  - Ship models now follow `hull_id`; tank variants used to fall back to the default model.
  - ShotTour shoots the section for one ship of each drive.

## Tier 2: Modules as Refits (designed, not built)

Constraints: keep the simulation deterministic and headless-testable, keep refits economic decisions (the part C
rule), and avoid a free-form editor.

### 2a. Designs derived in C++ (no gameplay change)
- `ship_design(ship)` returns a **derived design**: the hull's base class plus the ship's fit, cached by a design
  id (`class_id+module+module…`, sorted). The cache is a `std::map`, so references stay stable.
- Derivation uses the formulas the builder already uses, from the component fields tier 1 added (that is why the
  engine carries Isp, thrust and jet power):
  - dry mass and value: sums of the parts;
  - nuclear-thermal: Δv = Isp g₀ ln((dry + propellant)/dry) and acceleration = thrust/(dry + propellant);
  - plasma: α = jet power/dry mass;
  - crew = berths; hold = the sum of hold parts.
- Validation test: derive every tank variant in C++ from its nominal hull plus a tank module, and match the
  builder's CSV rows. That proves the port before any gameplay uses it.
- The leg-estimate cache key (`Simulation.cpp`, `ship_class.id|origin|...`) becomes the design id
  automatically.

### 2b. Module catalogue and fits
- `data/ship_classes/modules.csv`, generated by the builder from the reference numbers. Columns:
  - the component columns, plus `slot`, `hulls` (which hulls accept it) and `effects` (`key=value;...`).
- Each hull gets slots: habitat extensions, hold bays and drive mounts. Small hulls get one or two, freighters
  three or four.
- `ShipState::fit`: module ids. Tank variants become the `tanks` slot, so part C refits keep working.
- Save version 11; the data fingerprint includes the modules.

| Module | Slot | Effect (existing quantity it changes) | Gameplay |
|---|---|---|---|
| Common room | habitat | crew endurance (now a flat 730 d) grows with habitable volume per crew | long outer-system runs without a crew swap |
| Hydroponics bay | habitat | food make-up × 0.3, adds power and heat | less provisions buying far from farms |
| Storm shelter (water wall, ~20 g/cm²) | habitat | survives the solar-storm event without a crew health hit; the water can double as reserve | insurance for inner-system and long routes |
| Med bay | habitat | shorter recovery after events; slightly higher crew wages accepted | |
| Passenger cabins (linopt: 1,600 kg per passenger + 1.2 kg/passenger-day) | habitat | carries passengers, a new trade good with fares | liners and couriers earn on people |
| Cryo / pressurized bay | hold | lower `decay_fraction_per_day` for food and medicine in that bay | premium perishable trade |
| Set B drive radiators (747 K, 10 kW/kg) | drive | lighter radiators raise α and κ | expensive upgrade for plasma ships |
| Extra radiator area | drive | headroom for radiator damage (below) | redundancy vs mass |

### 2c. Refits
`consider_refit` already scores the next smaller and larger tank variant. Generalise it to "fit options": add,
remove or swap one module, scored the same way. The rule stays:
`(score − current) × REFIT_PAYBACK_DAYS − bill − current × refit_days > 0`.

Fleet investment picks a hull and then its best fit, as it now picks the best tank variant.

### 2d. Condition and ship events
- `ShipState::condition`: component id → 0..1. Wear grows with operating time; major refits at the home yard
  repair parts at price × damage × repair factor.
- **Ship events** in `events.csv` get kind `ship_damage` (target: ship, by hull or drive). For example:
  - a micrometeoroid hit on drive radiators: −20 to −50% of the heat rejection;
  - a reactor scram: the engine is out until repaired;
  - a solar storm (crew health, see storm shelter).
- **Damage is felt through physics.** A plasma ship with less radiator capacity must run at lower power:
  jet power ≤ heat capacity × (1 − φ)/φ. That gives a lower α, a lower κ and a slower atlas trajectory. The
  design id includes the condition in 10% buckets, so caches stay bounded.
- **Determinism:** ship events draw from their own RNG stream (`event_rng_` seed + 1), so the station-event
  sequence and today's saves stay reproducible.
- UI: condition bars per part in the systems section; damage and repairs in the comms log.

### 2e. Order
1. 2a design derivation, with the tank-variant equivalence test.
2. 2b catalogue, fits and save.
3. 2c refits.
4. Effects one at a time, each checked with `tools/benchmark.py`: endurance → hydroponics → cryo bay →
   radiators → passengers (largest, last).
5. 2d condition and ship events.

## Tier 3: Stations (sketch)
Station modules map onto quantities the economy already has:
- habitat rings → population
- factories → recipes
- storage → storage capacity
- depot tanks → fuel buffer
- docks → throughput
- power plant and radiators → a power budget that the factories draw from

Step 27's growth would then build modules, with a cost and a visible result. Do it after tier 2, together with
an economy rebalance.
