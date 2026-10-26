# Tier 3: Station Structures (concept, 2026-10-26)

Status: **shelved after the concept** (user, 2026-10-26). The structures would mean recalibrating most of the
economy, and growth by building has no economic basis today (tensions 9 and 10). Only a read-only **light view**
is built: see "Light view (built instead)" at the end. The concept and its tensions stay here for a later
revisit; the decisions listed are not needed now.

Tier 1 and tier 2 (ships) are in `ship_internals.md`, `ship_structures_tier2.md` and `tier2_log.md`. The sketch
this replaces is the "Tier 3: Stations (sketch)" section of `ship_internals.md`.

## Goal

A station is made of modules you can see and count:
- habitat sections;
- life support and farms;
- production lines;
- fuel works and depot tanks;
- power and radiators;
- storage;
- docks;
- a yard (at the shipyards).

Each module maps onto a quantity the economy already has. Later, stations grow by building modules: the build has a
cost in goods and time, the goods arrive by ship, and the result shows on the map. As for ships, this is not an
editor: the simulation decides what gets built.

## What the economy has today (measured on `main`, 2026-10-26)

Code: `EconomySystem::step`, `Households.cpp`. A 730-day `--econ-audit` gave the money figures below.

- **Everything scales with residents.** Each profile's recipe rate is per 10,000 residents
  (`RATE_REFERENCE_POPULATION`).
  - Outputs and their inputs are also multiplied by `employed_share` = min(1, jobs ÷ workers), where
    workers = residents × 0.6.
  - So output ∝ min(residents, jobs ÷ 0.6): **jobs already act as installed plant capacity**, but as one pool.
    All the station's outputs grow and shrink together.
  - Upkeep and consume rows scale with residents only.
- **Growth** (step 27): residents grow 3%/yr while the station is supplied, up to 1.5 × the seeded population. Jobs
  grow 3%/yr while the station is supplied and its vacancies are filled. **Nothing is built and nothing is paid.**
- **Storage:** one number of units per station (1 unit of helium-3 takes the same space as 1 unit of reactor fuel).
  Plants stop at 85% full. A second, hidden limit caps each output's stock at a number of days of its output.
- **Fuel:** a fixed factory output at six stations (`fuel_factories.csv`), and a 30-day buffer. Fuel stands outside
  storage, and a depot without a factory has no tank size.
- **No power, no docks, no capital.** Local production is free apart from wages and inputs. Stations pay no capital
  charges; ships pay interest and depreciate over 30 years. There is no limit on docked ships and no time spent
  in port.
- **Machinery stock decays** 0.05%/day (`MACHINERY_WEAR_PER_DAY`): crude depreciation.
- **Money scale** (2 years):
  - each station pays 1-2.5M cr/yr in wages, which mostly come back as the residents' spending;
  - station profit before the faction is 7k-1.65M cr/yr;
  - faction taxes are about 1.5M cr/yr each for Sol Federation and the Independent Consortium, and go into ships.
- **Supply is far above use:**
  - food: about 195 units/day made against about 22 used by the stations;
  - metals: about 183 units/day (Mercury, Ganymede, the Luna and belt colonies) against about 12;
  - electronics: about 72 against about 16.

  (The colonies' imports add to the use, but not by a factor of ten.)
- **Station residents** total about 104,000 at the seed.

## The concept

### Principle: decompose first, change later (as in tier 1)

**Step 3a.** Every station gets a module list whose sums reproduce today's numbers exactly:
- habitat capacity = 1.5 × the seed (today's growth cap);
- production lines carry today's jobs;
- storage = `storage_capacity_units`;
- fuel works = `fuel_factories.csv`;
- upkeep rows are split between modules.

A 730-day `--econ-audit` must stay byte-identical. Only then do the modules start to drive anything, one effect at a
time, each checked with `tools/benchmark.py`.

### Module kinds and what each one drives

| Module | Physical basis | Drives (existing quantity) |
|---|---|---|
| Habitat section (ring sector, buried habitat, aerostat envelope) | pressurized volume, shielding g/cm², gravity | population cap (replaces `max_population_factor`), radiation shield (`station_radiation.csv`) |
| Life support and farms | closure of the food, water and oxygen loops | the residents' food, water and oxygen imports (see tension 2) |
| Production line, one per output (smelter line, fab, chemical plant, bioreactor, extraction rig, lab) | throughput per worker, power, inputs | its output's capacity and its own jobs (see tension 1) |
| Fuel works and depot tanks | electrolysis and liquefaction power, tank volume, cooling | `fuel_factories.csv` output, the fuel buffer, depot size |
| Power (solar arrays scaled by 1/r², or reactors) and radiators | W per m², W per kg, heat rejected | a power budget for the lines (see tension 6) |
| Warehouse | container slots, 1 unit = 1 m³ as in ship holds | `storage_capacity_units` |
| Docks | berths, handling | cosmetic at first (see tension 8) |
| Yard | slips | shipyard role (`yards` in `households.csv`), repairs (tier 2) |

### Building (after 3a)

- **A build is a list of goods plus time:** metals, machinery and electronics, plus the station's own labour.
  The goods arrive through the existing station purchase orders (step 29), so construction becomes ordinary
  cargo, and money flows to the producing stations.
- **Small increments,** so a 4-8 year run shows change: a ring sector for about 500 people, one more smelter line,
  one more array.
- **Construction absorbs today's overcapacity.** Metals made are about 15 × used today. That surplus is exactly the
  slack that kept "supply that follows price" switched off (`economy_overhaul_log.md`). Building demand would give
  the surplus a use and give prices a reason to leave the floor.
- **Faction money that piles up** once the fleet cap is reached (Sol Federation about 9M by year 8) gets an
  outlet.

### Show it

- An inspector section "Station systems" like the ships' (modules, capacity, staffing, condition, power).
- `StationModels.gd` builds the model from the module list instead of from the population and the profile
  flavour. The file already notes that tier 3 may replace it.
- New modules appear on the map as they are finished.

### Later: Victoria 2 style populations

If each production line has job slots by kind (for example labourers, technicians, engineers, scientists), the
population work can attach its kinds of people to them directly. Tier 3 should keep that seam, even if it fills
only one kind at first.

## Tensions: where the structures do not fit the current economy

Each item gives the conflict, its size, the options, and a recommendation.

### 1. Production is per resident; modules make it capital

- **Conflict.** Today a station that gains people gains output in all its goods at once, for free. With production
  lines, output is the lines' capacity × staffing. More people without lines means unemployment; more lines
  without people means vacancies.
- **Compatibility.** Step 3a can be exact. Output ∝ min(residents, jobs ÷ 0.6) is already "lines staffed by
  workers" as long as every line grows by the same factor. The break comes only when lines are built one product
  at a time. That is the point of the feature, and it changes behaviour.
- **Unknown.** The jobs split between lines has no data behind it today. It is a game choice: by output value, or
  by real labour intensity.
- **Upkeep rows** (machinery, electronics, reactor fuel) mix the habitat's needs with the plants' needs. Each row
  must be split between residents and lines. That is also a game choice, and it changes who runs short when a line
  stops.
- **Recommendation:** split jobs by output value at base price (neutral and reproducible). Leave upkeep per
  resident in 3a and move the plant part of it to the lines in step 3d.

### 2. Stations import 10-150 × less food, water and oxygen per head than ships

- **Conflict.** Per resident per day:

  | | Stations | Ships (`ship_operations.csv`) | Real need |
  |---|---|---|---|
  | Food | 0.025-0.05 kg | 0.6 kg | about 0.6 kg dry |
  | Oxygen | about 0.006 kg | 0.05 kg | about 0.84 kg |
  | Water | about 0.025 kg | 0.1 kg | about 3 kg with hygiene |

- **Implication.** The stations hide farms and recycling that close the food loop 92-96% and the oxygen and water
  loops about 99%. As soon as the habitat is shown, those farms must exist as modules. They are large: tens of m²
  of growing area per person.
- **Make-or-buy risk.** Once farms are modules, "build a farm or import food" becomes a decision. Earth L1 lives on
  exporting food. If stations close their loops further, the food trade, which is the base of the network, can
  collapse.
- **Options:**
  - (a) declare today's closure as fixed data per station (the farms are shown but never built);
  - (b) let closure be built and score it like any other investment;
  - (c) make farms cost power and labour so that importing stays cheaper near a producer.
- **Recommendation:** (a) now; (b) or (c) only with a benchmark showing the trade survives.

### 3. Capital has no price: the existing stations are free, so who pays for new ones?

- **Conflict.** Ships carry capital (price, interest, depreciation). Stations carry none. If modules are priced the
  way ships are priced, the stations' value dwarfs all the money in the game:
  - a Light Freighter costs about 0.9 cr per kg of hardware;
  - Low Earth Logistics' habitat alone would be more than 100M cr at that rate;
  - all the money flows above are a few M cr per year.
- **Options:**
  - (a) the existing stations are sunk: no capital charge, only maintenance;
  - (b) every station pays capital charges (station profits would turn deeply negative, and the whole economy
    would need recalibrating);
  - (c) new modules are paid as goods at commodity prices plus labour, not at an aerospace price.
- **Price gap.** Metals cost 0.084 cr/kg, about ten times less per kg than a ship's hardware. Building with goods
  makes space construction cheap per tonne, but heavy.
- **Recommendation:** (a) together with (c). Book modules as station assets at the cost of their goods, so a build
  does not look like a loss in `station_lifetime_profit` and in the Statistics screen.

### 4. Real habitat masses exceed the metals supply

- **Numbers.** Stanford torus (NASA SP-413, 1975), roughly:
  - about 15 t of structure per person;
  - about 1,000 t of passive shielding per person (lunar slag).

  NASA's Deep Space Habitat, used for the ships, is 8 t per person. Today's 3% growth on about 104,000 residents is
  about 3,100 people a year:
  - at 15 t of metals each, that is about 640 units/day of metals, 3.5 × all the metals made in the system;
  - at 3 t each, it is about 130 units/day, which fits in today's surplus of about 170.
- **Shielding cannot be a traded good.** 1,000 t per person is about 5,000 metal units. It has to come from local,
  untraded material: regolith, slag, ice or water. Earth L1 and Low Earth Logistics have none of that locally;
  their shielding values in `station_radiation.csv` are 20 g/cm² and 0.
- **Recommendation:**
  - traded construction goods cover the pressure structure and systems only (about 3-5 t per person);
  - shielding is local mass with a per-station source (surface, ice, imported water), and it sets the radiation
    value;
  - growth slows to what the metals can feed, or construction demand drives the metals price up (both are
    acceptable outcomes; the second is the interesting one).

### 5. Most "stations" are on the surface in the data, yet in orbit on the map

- **Conflict.** `stations.csv` puts every station in orbit (200-1,500 km), and the 3D models are orbiting rings.
  The radiation data describes five of them as surface or atmosphere bases:
  - Mercury and Ceres: regolith-bermed surface;
  - Lunar Gateway: surface;
  - Ganymede: underground mine;
  - Titan: surface under its atmosphere.

  Venus is an aerostat at 50 km. Only Earth L1, Low Earth Logistics and Mars Transfer are truly orbital.
- **Why it matters for structures.** A surface base has gravity, local shielding and mines. An orbital port has
  docks and depots. Showing them as one rotating ring is wrong for five of nine stations. Lifting goods between the
  surface and the port costs delta-v. The colonies already pay for that ascent ($20/kg at Luna, $5/kg at Ceres);
  station goods do not.
- **Options:**
  - (a) every station = an orbital port plus a site (orbit, surface or aerostat), modules tagged by site, no ascent
    cost (only visual and physical consistency);
  - (b) the same with an ascent cost per unit between site and port;
  - (c) move the radiation notes to orbit and keep rings everywhere.
- **Recommendation:** (a). (b) changes the export prices of five stations, and it should be a separate economy
  step.

### 6. Power: none today, and reactor fuel use implies thousands of times too much

- **Conflict.** Low Earth Logistics uses 2.25 units of reactor fuel a day, which is 1,125 kg. At a light-water
  reactor's burnup (about 50 GW-days per tonne), that is about 56 GW of heat for 18,000 people. Real needs are far
  smaller:
  - Mercury's smelters make 80 units of metals a day (16 t). At aluminium-electrolysis energy (about 15 MWh/t),
    that is about 10 MW.
  - Venus's fuel works make 36 t of propellant a day. As liquid hydrogen from electrolysis and liquefaction, that
    is about 90 MW.

  So stations need tens to hundreds of MW, and reactor fuel use is about 1,000 × the physical rate.
- **Effect of a physical power budget.**
  - Reactor fuel demand would fall about 1,000 ×.
  - The reactor fuel trade would nearly vanish. It carries Ceres and Ganymede exports and about 45% of Mars
    surface's import value.
- **Options:**
  - (a) reactor fuel stays an upkeep good with no physical power link, and the power budget uses solar and
    reactors with no fuel accounting;
  - (b) redefine "reactor fuel" as reactor consumables (fuel assemblies, moderator and coolant make-up, shielding
    replacement), sized to keep today's trade;
  - (c) recalibrate to physics and accept the loss of that trade.
- **Recommendation:** (b), stated openly in `ship_technology.md`. Add the power budget only as a later step
  (3f), because it adds a constraint that nothing binds today.

### 7. "Fuel" is one good, but two different propellants

- **Conflict.** Nuclear-thermal ships burn liquid hydrogen at 20 K. Plasma ships use water. The economy has one
  "fuel" good at 100 kg per unit. Fuel works and depot tanks are physically different for the two:
  - hydrogen needs electrolysis, liquefaction and cryogenic tanks;
  - water needs mining and plain tanks.
- **Recommendation:** keep one good for now, with fuel works modelled as hydrogen works (the costlier case) and
  depot tanks as cryogenic tanks. List the split as a later economy step, not as part of tier 3.

### 8. Docks would need queues that dispatch cannot forecast

- **Conflict.** There is no port time and no berth limit. Contracts assume arrival equals delivery. A berth limit
  needs queues. Dispatch and the forecasts on arrival do not know about queues, so contracts would be paid late or
  wrong, and stations would bear the error.
- **Size.** About 130 ships and 9 stations. The docked count per station has not been measured yet.
- **Recommendation:** docks are cosmetic in tier 3 (the model shows berths matching the measured peak). Port time
  and queues come later, if the measurement shows berths would ever be full.

### 9. Growth has no return for anyone, so the decision to build has no economic basis

- **Conflict.** Residents pay no rent. A new ring brings workers whose output is mostly glutted (prices at the
  floor) and whose wages and spending circulate inside the station. So a station's profit hardly rises with growth.
  Only Earth grants (settlement grants are need-based) and Earth exports (platinum, deuterium, helium-3) pay for
  growth. An investment rule like fleet investment's payback would build almost nothing. A rule without payback is
  a policy, not an economy.
- **Options:**
  - (a) faction development policy: treasuries above a level fund builds where vacancies, crowding or shortages
    are greatest (simple and visible; does not follow the "uneconomic must lose on cost" principle);
  - (b) rent, so housing earns a return: residents pay the station per person, which changes the household
    balances calibrated in the overhaul;
  - (c) Earth pays per settler housed: a settlement grant tied to capacity.
- **Recommendation:** (a) for habitat and (c) for the colonies' gateways. Production lines use a payback rule
  (value of added output at the forecast price − upkeep − wages), which builds little today because of tension
  10. That is acceptable, since today's overcapacity means nothing needs to be built.

### 10. Today's production overcapacity makes production lines pointless at first

- **Conflict.** If 3a reproduces today's capacity (food about 9 ×, metals about 15 × use), no production line pays
  for years. If lines are sized to use instead, 3a is no longer byte-identical, and the economy loses the slack
  that the benchmarks showed it needs (the 3 × production rebalance of steps 18-22 was reverted).
- **Options:**
  - (a) keep the capacity; let construction demand (tension 4) and growth absorb it over the years;
  - (b) mothball idle lines (no wages or upkeep) and restart them when the price returns; this is "supply that
    follows price", now with a physical meaning.
- **Recommendation:** (a) first, then (b) as its own measured step. It is the natural home of the parked
  `supply_price_elasticity`.

### 11. Maintenance would be counted twice

- **Conflict.** Station machinery and electronics upkeep, the machinery stock's decay, and tier 2's condition and
  repair model for ships all stand for wear. A module condition on top of these double-counts it.
- **Recommendation:**
  - the per-module upkeep is today's upkeep rows split between modules (sums unchanged), with no new consumption;
  - module condition comes from events only (outages, hull breach) and is repaired from the existing upkeep
    goods;
  - remove `MACHINERY_WEAR_PER_DAY` when module upkeep replaces it (a measured change).

### 12. Labour is almost free next to materials

- **Numbers.** A station worker earns about 0.11 cr a day, about 40 cr a year. That is about 2.4 units of metals,
  roughly 480 kg. Construction cost is therefore almost all goods, and build time is set by deliveries, not by
  workers.
- **Is it consistent?** Yes: it is Earth's wage against Earth's price plus launch. It does mean construction
  jobs barely matter.
- **Recommendation:** count construction workers as jobs (so a build gives a visible employment bump and a reason
  for settlers), but price builds in goods.

### 13. Ship habitat and station habitat are different technologies

- **Conflict.** Ships use the Deep Space Habitat split: 8 t per person, 48 m³, no food closure. Stations need about
  15 t per person of structure, heavy shielding, about 95% food closure and spin gravity. Reusing the ship
  numbers would make stations light and import-hungry. Using the station numbers for ships would break tier 1's
  exact sums.
- **Recommendation:** a separate station-habitat reference (SP-413 and NASA's lunar surface habitat studies),
  sourced in `ship_technology.md` the same way.

### 14. Storage in units, a hidden per-good cap, and depot fuel with no tanks

- **Conflict.** Storage counts units regardless of mass. Each output also has a hidden cap of a number of days of
  output (`production_cap_units`). Depot fuel has no tank limit.
- **Recommendation:** define a unit as 1 m³, as in ship holds, so warehouse modules equal `storage_capacity_units`.
  Keep the per-output cap in 3a; it is behaviour, not structure. Depot tanks become the fuel limit only in a later
  measured step.

### 15. Fleet cap against yard capacity

- **Conflict.** The fleet cap of 130 is the user's choice, for performance. Yard slips would make shipbuilding
  capacity physical, and could emerge as a lower or higher cap.
- **Recommendation:** slips limit the build rate only. The cap stays at 130.

### Smaller points (fit without conflict)

- **Events map onto modules:**
  - an outage damages a line;
  - a hull breach damages a habitat section;
  - a settler migration needs free housing (otherwise people wait or turn back).
- **Saves** need a new version; the data fingerprint includes the modules.
- **The money journal** needs construction categories (goods for builds, assets). Its identity is unaffected,
  because builds are ordinary trades.
- **The colonies** (Mars, Luna and belt surfaces) stay abstract background economies, with no modules.
- **Science stations** (Titan, Ganymede) get labs as their lines. Their jobs do not grow today, which is fine as
  long as no lab gets built.
- **Low Earth Logistics and Earth L1** buy construction goods from Earth at Earth's ask (surface price plus
  launch). Space metals compete on price, as they do today.

## Order (proposal)

| Step | What | Effect on the simulation |
|---|---|---|
| 3a | Modules decompose today's numbers (data builder like `build_ship_classes.py`, C++ loader, validation of sums) | none; byte-identical audit |
| 3b | Show them: inspector "Station systems", models built from modules | none |
| 3c | Habitat capacity and housing builds (faction development policy, goods through purchase orders) | growth needs housing, construction trade |
| 3d | Production lines with their own jobs and upkeep; builds by payback; mothballing | per-product capacity |
| 3e | Module condition from events, repair from upkeep goods; remove machinery stock decay | events become visible damage |
| 3f | Power budget (solar by 1/r², reactors), reactor fuel as consumables | power-limited lines |
| later | Docks with port time, ascent cost from surface sites, separate hydrogen and water fuel | economy steps of their own |

Each step from 3c on is checked with `tools/benchmark.py`, 4 starts × 1,460 days, against the previous step.

## Decisions for the user

1. **Food, water and oxygen closure** (tension 2): fixed per station (recommended), or built?
2. **Capital** (tension 3): existing stations sunk, new modules paid in goods (recommended)?
3. **Who decides to build habitat** (tension 9): faction development policy (recommended), rent, or Earth paying
   per settler?
4. **Surface or orbit** (tension 5): port plus site with no ascent cost (recommended)?
5. **Reactor fuel** (tension 6): redefine it as reactor consumables to keep today's trade (recommended), or
   recalibrate to physics?
6. **Job split between lines** (tension 1): by output value (recommended), or by labour intensity?
7. Are docks cosmetic and is one fuel good acceptable for tier 3 (tensions 7 and 8)?

## Light view (built instead, 2026-10-26)

A read-only "Station systems" section in the station inspector. It shows what a station is made of, derived from the
numbers the economy already has. **The simulation does not change:** the snapshot gains fields, and a 365-day
`--econ-audit` stays byte-identical.

It is designed as the seam for the Victoria 2 style populations (roadmap "Later"). Nothing it shows should have to
be withdrawn when populations arrive. Their model replaces the typical figures with simulated ones.

### Victoria 2, and what to borrow

In Victoria 2:
- **Pops** are groups of people of one type (farmers, labourers, craftsmen, clerks, and so on), culture and
  province. Each has a size, cash and an employer.
- **Pops buy their needs in three tiers:** life, everyday and luxury. How well they are met drives growth,
  promotion and migration.
- **Workplaces have job slots by type.** Resource gatherers (farms, mines) employ farmers and labourers.
  Factories employ craftsmen, and clerks raise their output.
- **Wages and unemployment are per type,** and pops promote, demote and migrate toward better pay and needs.

What SpaceTrains already has, in that language:

| Victoria 2 | SpaceTrains today |
|---|---|
| a pop | one household per station (residents), plus ship crews paid to their home station, visitors, and the colonies' people (lognormal wages) |
| pop size, cash | `population`, `household_credits` |
| life needs | upkeep goods the residents use (food, water, oxygen, medicine): price-inelastic (−0.1) |
| everyday needs | `consume` goods: follow wage (+1) and price (−1) |
| luxury needs | travel (3% of wages), visits |
| workplaces | one pool of jobs per station; every output runs on min(1, jobs ÷ workers) |
| promotion and migration | settlers come for vacancies on their wage gain; the unemployed go home |

### Pop kinds, tuned to space (proposal for the population work)

- **Labourers:** miners, farm and dock hands.
- **Technicians:** plant operators, life support, maintenance (Victoria 2's craftsmen).
- **Engineers:** fabs, yards, reactors; they raise a works' output (Victoria 2's clerks).
- **Scientists:** labs.
- **Service and administration:** shops, schools, clinics, the port, governance.
- **Spacers:** ship crews. They live aboard and are paid to their home station (already modelled).
- **Dependants:** children and the retired. Today they are the 40% of residents outside the workforce
  (participation 0.6).

Later dimensions, not in the view:
- **Origin:** Earth-born or station-born. Station-born people have no Earth wage to go back to, so they stay
  longer and accept more hardship. The settler-gain formula already uses Earth's wage distribution.
- **Wages by kind,** where today there is one wage per station.

### What the view shows

1. **People** (exact today):
   - residents against housing (housing = the growth cap, `max_population_factor` × the seed);
   - workers employed and out of work;
   - dependants;
   - spacers based here (the crews of ships whose home is this station);
   - visitors, and passengers waiting.
2. **Works:** one per output line, grouped by name (for example, farms make food and oxygen). For each works:
   - capacity, today's output and what limits it;
   - its inputs;
   - its jobs.

   The station's jobs are split:
   - a services share (life support, port, administration) of 40%;
   - the rest between the works, by output value at base price × a labour share for the works' kind.

   Each works' jobs are split by kind with a **typical staffing** mix. This is a decomposition, like the
   ship habitat split of tier 1: the kinds add up exactly to the jobs. The population work later fills these slots
   with real pops.
3. **Needs, in Victoria 2's tiers:**
   - **Life needs** (food, water, oxygen, medicine): the residents' use against a reference need per person.
     For food, water and oxygen this gives the share their own farms and recycling cover (tension 2: shown as
     fixed, never built).
   - **Station upkeep** (machinery, electronics, reactor consumables, fuel): availability.
   - **Everyday goods** (`consume`): use and availability.
4. **Site:**
   - where the habitat is (orbit, surface, underground or aerostat; tension 5, as data only);
   - its gravity (spin rings at 1 g in orbit, the body's surface gravity on the ground);
   - its shielding (`station_radiation.csv`).
5. **Storage and depot:** the warehouse fill, the fuel stock against the factory's 30-day buffer.

Data: `data/stations/works.csv` (works names, kinds, labour shares and the typical staffing), and
`data/stations/station_systems.csv` (the services share, the reference needs, the need tiers). A `site` column is
added to `station_radiation.csv`.

**Reference needs:** NASA's Baseline Values and Assumptions Document (BVAD, 2018), rounded:
- food 0.62 kg dry per person per day;
- oxygen 0.82 kg;
- drinking and food water 2.5 kg (hygiene excluded).

**Decisions taken without the user** (to review):
- the 40% services share;
- the labour shares by kind;
- the staffing mixes;
- the pop kind names.

All are display-only.
