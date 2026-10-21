# Tier 2 Log: Decisions and Measurements (from 2026-10-21)

Plan: `docs/plans/ship_structures_tier2.md`. Every decision is listed here for the user's review. Scorecard: 4
starts × 1460 days (`tools/benchmark.py --days 1460`), mean ± sd. "Price" is what stations paid on deliveries ÷
the landed reference price (new audit line, `Delivered price`).

## Baseline (main at `ea7fede`)

| Run | Unmet % | Last fifth % | Price ÷ ref | Profitable | Ships | Money % | Run s |
|---|---|---|---|---|---|---|---|
| baseline | 0.6 ± 0.5 | 1.2 ± 1.3 | 1.6 ± 0.1 | 98.8 / 133 | 133 | −15 ± 20 | 127 |

## Steps A-C: designs, modules, fits (no gameplay change)

Decisions:
1. **The tank variant stays the ship's class; modules are a separate fit.** The old design made tanks a slot;
   keeping the 36 class rows avoids rewriting part C and its tests. A design id is `class+module+…`.
2. **Port check instead of a tank-swap test:** `totals_from_parts` recomputes dry mass, value, Δv,
   acceleration, α, berths and hold of all 36 class rows from their parts alone, within CSV rounding.
3. **An unfitted ship flies its class object unchanged**, so floating-point results are bit-identical. A fitted
   design is derived (`derive_design`): the same exhaust speed and thrust (nuclear-thermal) or jet power
   (plasma) over the heavier ship.
4. **Modules are sized per hull by the builder** (`modules.csv`) and include their share of truss. Slots
   (`hull_slots.csv`): 3 habitat slots on freighters, 2 on couriers; one hold slot except on couriers; one drive
   slot on plasma ships, one tank slot on nuclear-thermal ones (game choice).
5. **Only modules with an effect switched on are offered** at refits and to new ships; an inert module only
   adds mass, and skipping it keeps run time.
6. **Refit options per yard visit:** the next tank size either way (with the current modules), or one module in
   or out. Same payback rule as part C. Fleet investment also tries the winning hull's standard tanks with each
   module.
7. **Engine sub-parts (reactor, shadow shield, nozzle …) postponed:** nothing needs them yet; damage acts on
   whole parts (step H).
8. **The duplicated time-cost formula was not merged:** a merge changes the summation order and therefore the
   results; hazard pay is added as a separate term instead, which is exactly 0 with radiation off.

Checks: a 365-day `--econ-audit` is byte-identical to the baseline after each of A, B and C (only the new
`Delivered price` line differs); save version 14 round-trips a fitted ship; bad fits are rejected on load.

## Step D: crew radiation

Model and sources: `docs/ship_technology.md`, "Crew radiation". Decisions:
1. **Hazard pay from the value of a statistical life, not from a dose-limited career.** Dividing a career's pay
   by its dose budget gives ~15 cr/mSv, about 13× today's crew cost in deep space. The compensating wage
   differential (ICRP 5.5%/Sv × ~$10M VSL) gives 0.43 cr/mSv: about +35% crew cost in deep space.
2. **The leg limit is the career limit (ESA, 1 Sv):** a crew is signed on per leg (as wages already work), so
   no leg may take its crew past the limit: about 540 days in deep space, less at solar minimum (+30% GCR).
3. **Docked crews live in the station's habitat**, with a location factor and shielding per station
   (`station_radiation.csv`, notes there).
4. **Particle events are a separate stream (`environment_rng_`), not rows in `events.csv`:** they are
   system-wide with a 1/r² dose, which the station-event format does not express. `--no-events` stops them too.
5. **The storm shelter protects the sleeping third of the day against cosmic rays** (−12% GCR) and the whole
   crew during events (1300 → 200 mSv for a 1972-class event).
6. **Planning:** hazard pay joins the time cost (at the origin's distance in dispatch; origin and destination in
   leg estimates, at the bucket start so cached estimates stay independent of the asking ship). The dose limit
   joins every endurance check and leg estimate, so contracts respect it too.
7. **Reference prices were not recomputed** with hazard pay: they stay the yardstick for every run.

Results (4 starts × 1460 days):

| Run | Unmet % | Last fifth % | Price ÷ ref | Profitable | Ships | Money % |
|---|---|---|---|---|---|---|
| baseline | 0.6 ± 0.5 | 1.2 ± 1.3 | 1.6 ± 0.1 | 98.8 / 133 | 133 | −15 ± 20 |
| D1: dose counted, no effect | 0.8 ± 0.7 | 1.5 ± 1.6 | 1.5 ± 0.1 | 104.5 / 134 | 134 | −20 ± 3 |
| D2: leg limit + hazard pay (kept) | 0.6 ± 0.2 | 1.8 ± 1.2 | 1.5 ± 0.0 | 107.0 / 139 | 139 | −16 ± 15 |

- D1 differs from the baseline only by chaos: the shelter is offered at refits, and the extra probes change the
  path of the run.
- One D2 run (start 0): 475,000 person-mSv over 4 years, hazard pay 204,000 cr, highest leg 975 mSv (just under
  the 1 Sv limit), 2 small particle events. **Storm shelters spread by themselves:** 9 ships by year 1, 37 by
  year 4. A 180 cr shelter saves ~0.8 cr/day of hazard pay (12% less cosmic-ray dose while asleep, 85% less
  event dose).
- Run time: a 730-day run takes 37 s with and without radiation (the 1460-day benchmark times were inflated by
  builds running alongside).

## Step E: cold hold (hydroponics, habitability and spin gravity not built)

Decisions:
1. **Hydroponics not built.** With sourced numbers it never pays on a transport ship. A bay growing a quarter
   of the food needs ~10 m² per crew member (a full diet takes ~40 m²), ~2.2 kW of LED light per crew (600
   µmol/m²/s at 2.7 µmol/J) and weighs ~0.9 t per crew. It saves 0.15 kg of food per crew-day, ~0.2 cr/day on a
   3-crew ship, against ~0.7 cr/day of capital alone. Bioregenerative life support pays on bases, not ships.
2. **Common room, habitability premium and spin gravity not built:** no sourced economic size for a comfort
   premium; inventing one would be a heuristic. They belong to the passenger plan, where fares can pay for
   volume and gravity.
3. **Cold hold built** (`cold_hold`, hold slot): the whole hold at 255 K (−18 °C), food and medicine spoil at a
   tenth of the ambient rate (estimate: frozen food keeps a year or more, cold-chain medicine its shelf life).
   0.6-3.1 t (MLI and a 2 kg/m² liner over the hold, a small cooler), 800-4,300 cr. The investment valuation
   keeps the ambient spoilage (it has no design in scope): conservative.

## Step F: liquid-hydrogen boil-off

Model: heat leak 0.2 W/m² at 1 AU through high-performance MLI (estimate), × r⁻¹, over 1.3 × a sphere of the
tank volume; boil-off = leak ÷ 446 kJ/kg. A Light Freighter's 90 t tank leaks ~150 W: 28 kg/day, ~7 t on a
260-day Mars Hohmann transfer. The Kepler planner loads burn + 10% reserve + boil-off, and its cost objective
pays for the boiled hydrogen. Decisions:
1. **Boil-off only in flight:** every station has a depot that already keeps its fuel cold (0.02%/day), and a
   docked ship plugs into it. (First version boiled in port too: unmet 1.0-1.1% with nobody buying coolers.)
2. **Two modules for the tank slot:** a 20 K zero-boil-off cryocooler (1.5-3.5 t, 10-23k cr: ~100 W of power
   per W lifted, from the engine's bimodal Brayton loop as in NASA's bimodal NTR studies), and a 90 K cooled
   tank shield that intercepts 60% of the leak (NASA Glenn broad-area cooling: up to 70%, theoretical; 0.4-0.9 t,
   1.3-2.9k cr). The shield sells; the full cryocooler, at about the cost of the hydrogen it saves, does not.
3. **Not modelled:** the arrival burn's lower mass after boil-off (conservative), LH2 slosh and stratification.

## Step G: Set B radiators (not built)

Two reasons: the tech-era decision keeps linopt's Set A hardware (Set B is "a later generation"), and the gain is
~2% of α for the freighters (their radiators are ~1 t of 34 t dry).

## Step H: wear and overhauls

Decisions:
1. **Drive life:** nuclear-thermal cores 10 h of full-power burn (NERVA's flight-engine goal, 10 h over 60
   restarts); a Light Freighter burns ~1.6 h a mission, so it needs a new core every 4-6 missions. Plasma drives
   50,000 h of thrust (estimate, NEXT ion thruster class): ~7 legs of 300 days. No leg may need more drive life
   than is left.
2. **Radiator punctures as a smooth expected rate, not random events:** Grün et al. 1985 give 0.146 grains of
   ≥ 10⁻⁵ g per m² per year at 1 AU (a 0.2 mm grain holes a 1 mm heat-pipe wall at ~20 km/s); 30% of a panel is
   pipe, a hit kills a 0.2 m² segment, about r^−1.3. That is ~1% of the pipes a year, so a 10% α step takes ~5½
   years; jet power is limited to what the remaining radiators reject (the design id carries `#r9` etc.).
3. **Overhauls at any port with the spares** (a home-yard rule could strand a ship with a dead engine far away):
   when 70% of the drive life is used or radiators fall below 85%. Spares bought from the port's stock at its
   price: the core's fuel elements as reactor fuel (40% of the engine mass × wear), plasma drives machinery and
   electronics (30%), radiators machinery (50% of the lost panels); labour at the refit share (30%) of the worn
   hardware's price; 20 days in the yard. A port without the spares lets the ship fly on within its remaining
   life; the stock drop raises the station's bids for the goods.
4. **The starting fleet is of mixed ages** (wear 0-50% by a hash of the ship id).
5. **ECLSS wear not modelled:** life support costs ~0.3 cr per crew-day, too little to matter; it would touch a
   dozen formulas.

## Step I: UI

The inspector's "Ship systems" section shows the fitted modules, the crew's dose this leg against the limit
and today's rate, hydrogen boil-off, drive life left and radiator condition (warnings in amber). The schematic
draws the shelter's water wall, the cold hold's liner, the tanks' cooling and the holed radiator segments; the
mass bar shows modules in violet. ShotTour takes `--shot-load=<save>` and prefers fitted ships.

## All switches on (the new default)

| Run | Unmet % | Last fifth % | Price ÷ ref | Profitable | Ships | Money % |
|---|---|---|---|---|---|---|
| baseline | 0.6 ± 0.5 | 1.2 ± 1.3 | 1.6 ± 0.1 | 98.8 / 133 | 133 | −15 ± 20 |
| D2 radiation | 0.6 ± 0.2 | 1.8 ± 1.2 | 1.5 ± 0.0 | 107.0 / 139 | 139 | −16 ± 15 |
| + boil-off, docked too (first version) | 1.0 ± 1.1 | 2.5 ± 3.3 | 1.5 ± 0.0 | 105.2 / 142 | 142 | −21 |
| + boil-off in flight, cooled shield (F2) | 1.0 ± 0.6 | 1.9 ± 1.0 | 1.6 ± 0.0 | 106.5 / 139 | 139 | −21 ± 5 |
| + cold holds (E) | 1.1 ± 0.7 | 2.0 ± 1.2 | 1.5 ± 0.1 | 104.5 / 139 | 139 | −14 ± 12 |
| + wear and overhauls (H, all on) | 0.9 ± 0.3 | 2.1 ± 1.0 | 1.6 ± 0.1 | 101.2 / 141 | 141 | −16 ± 11 |

A 730-day trial with everything on: 34 overhauls costing 375,000 cr (123 units of reactor fuel), 7 reviews
found no spares; by day 730 13-17 shelters, 5-12 cold holds and 2-3 cooled tank shields; money drift 0.
