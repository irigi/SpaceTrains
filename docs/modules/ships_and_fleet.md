# ships and fleet

How ships pay their way, choose missions, refit and are bought and sold, and how passengers travel. Prices,
orders and money: `economy.md`.

**Code** (all members of `Simulation`):
- `src/simulation/ShipOperations.cpp`: capital and crew costs, provisions, radiation, wear and repair, boil-off,
  propellant purchases;
- `src/simulation/MissionPlanning.cpp`: leg estimates and dispatch (`Simulation::MissionPass`);
- `src/simulation/ShipPhases.cpp`: a ship's phases (idle, refitting, awaiting departure, in transit, arrival) and
  refits;
- `src/simulation/FleetInvestment.cpp`: selling and buying ships (`Simulation::InvestmentReview`);
- `src/simulation/Failures.cpp`: failure clocks, derelicts, salvage races, replacement claims;
- `src/simulation/Passengers.cpp`, `CrewEnvironment.cpp`, `src/domain/ShipDesign.cpp` (designs from parts).

**Data:** `data/economy/` (`ship_operations.csv`, `fleet_investment.csv`, `open_economy.csv`, `fuel_supply.csv`,
`ship_environment.csv`, `passengers.csv`, `failures.csv`), `data/ship_classes/` (classes, components, modules, hull slots, spares;
written by `build_ship_classes.py`). Sources for the physics: `docs/ship_technology.md`.

## Operating costs

Time costs money, so feasible but uneconomic trips (multi-revolution spirals, year-long waits, empty loops)
lose on their own instead of being cut off by rules. Each ship accrues, prorated per tick:
- **Capital:** `ship_value_cr × (interest_rate_per_year + 1 / lifetime_years) / 365` a day, in every phase,
  laid up included.
- **Wages:** `crew_size × wage_cr_per_crew_day`, not while laid up. The wage is the crew grade of the labour
  scale (`data/economy/labour.csv`, which also prices yard work).
- **Life support:** food, water and oxygen per crew-day, bought as real goods. Ships start with 730 days aboard
  (`PROVISION_ENDURANCE_DAYS`) and top up where a port can spare it. A departing crew must hold the mission
  × 1.1 plus a 120-day return reserve; a docked crew doesn't eat into that reserve, it lays up instead.

Capital goes to the ship's home station, wages and hazard pay to its residents; provisions are ordinary trades. A ship whose cash falls below
zero **lays up**: crew discharged, capital still charged. It reactivates when a mission's profit covers rehiring
and provisioning.

## Dispatch (`choose_mission`)

A dispatch pass (`Simulation::MissionPass`) plans one ship's options from its port and picks the best by profit
per day. Plans it has not computed yet are queued and computed in parallel, then the pass is repeated, so the last
pass is exactly the sequential result (`choose_mission`, `choose_missions` for several ships at once). Its steps,
in order:
1. **Cargo runs:** per destination, the manifest (below), whole and as its first half and quarter, with the
   passengers who board;
2. **Investment probes** only: berth runs for ships with berths, and each cabin module on a bare hull;
3. **Empty legs** toward a better pickup, on the same two-leg terms;
4. a liner flies its next stop empty, and a ship on its commissioning route flies home empty, if nothing pays;
5. **Repositioning** toward a producer whose surplus the system needs, if nothing else is worth flying.

- **Two-leg scoring.** Each candidate leg (cargo, or empty toward a pickup) is scored together with the best
  follow-up leg from its destination: `(urgency × leg profit + follow-up profit) / (leg days + follow-up days)`.
  Profit is revenue − cargo cost − fuel − provisions − daily cost × (wait + transit). Follow-ups come from the
  forecast stock at arrival, less what other ships have already planned to pick up; without one, the ship is
  charged an empty trip back. A follow-up counts only if the ship could leave its next port again.
  - Follow-ups are cached per destination, arrival time and the fuel the ship can leave with (a lighter load
    arrives earlier with more fuel; until 2026-10-26 one set per destination was reused for every load).
  - Follow-up legs are cheap estimates (`estimate_leg`), cached per class, 5-day departure bucket, 10%-of-tank
    fuel bucket and 2 t payload bucket, planned from the bucket's start so an estimate does not depend on which
    ship asked first (or on a save and load).
- **Urgency weight:** a lot's margin counts 1-5× by how few days of stock the destination has left. Turning it off
  was measured and rejected: the order price alone does not rank deliveries well enough.
- **The manifest:** for each destination the hold fills chunk by chunk (1/40 of it) with the units that earn the
  most each: what the destination pays for the next unit (open order, extra buffer, floor; along its curve where it
  posts no order), less the cost here along the producer's curve and the propellant its mass burns, until the next
  unit would lose money or the hold, the credit or the surplus runs out. A rotation contract's deliveries load first.
  Ships size their own loads: small loads are fine when they are the ship's best business (user, 2026-10-15).
- **Contracts:** each lot's price is agreed at departure from the destination's order for the units that survive
  the trip in this ship's hold; the station pays it on arrival whatever the market did meanwhile (pro rata if
  storage forced a jettison). Ships earn what they planned; stations bear the forecast error.
- **Mission-sized fuelling.** Plans carry the payload (cargo and provisions) in the rocket equation and
  report `propellant_load_kg`, what is aboard at departure.
  - Nuclear-thermal: the smallest load L with L = 1.1 × burn for the Δv, never less than already aboard.
  - Variable-Isp: fuel budgets of 100/75/55/40% are ranked by atlas estimate and the two cheapest are refined.
  - The Lambert search minimises fuel plus time cost (`PlanningOptions`).
  - Ships buy the plan's load when a mission is assigned, not by a refuel threshold.
- **Fuel-aware planning.**
  - *Carried return fuel:* where the destination can't refuel the ship for a leg as long as this one, the ship
    loads the missing fuel here as payload (re-planned up to three times). It also carries its return fuel where
    the destination sells fuel at more than `TANKERING_PRICE_RATIO` (2) × this port's price.
  - Fuel aboard is valued at base price (it is sunk), but fuel bought here pays this port's scarcity premium, and
    the planners trade propellant against time at that price.
  - Cis-lunar plasma spirals take the cheapest of 1.5-10 × the shortest transfer time.
- **Stranded:** no mission, under 1% fuel, and none for sale.

## Tank refits

- Every class is built with 50/100/150/200% of its nominal propellant (`build_ship_classes.py`). Variants share a
  `hull_id`; the nominal one keeps the class id.
- Every 90 days, a ship docked at its home base compares the next smaller and next larger tank. Each comparison
  is a full dispatch pass as that variant, planned from the day the yard would finish (`consider_refit`).
- **Refit rule:** refit when (variant score − current score) × 180 days > yard bill + current score × 20 days.
  - Cost: `refit_days` in the yard, crew discharged, and a bill of `refit_cost_fraction` of the hardware value
    added or removed, paid to the home station. Propellant that no longer fits is sold to the depot.
  - Afterwards the ship is locked out of further refits for the yard time plus 180 days.

## Modules, crew radiation, boil-off and wear

Sources: `docs/ship_technology.md` ("Crew radiation", "Boil-off and wear"). Every mechanism is a switch in
`data/economy/ship_environment.csv` (all on by default); with all off the economy is the one without them.
- **Parts.** Every class is built from its components (`ship_components.csv`: habitat and its rooms, hold, tanks,
  drive, reactor, radiators, shields), which add up exactly to the class's dry mass and value; the inspector's
  schematic and the 3D models draw them.
- **Designs.** A ship flies `ship_design(ship)`: its class (tank variant) plus its fit (`ShipState::fit`, modules
  from `data/ship_classes/modules.csv`, sized per hull by the builder) plus its radiator damage in 10% steps.
  Fitted designs are derived from the parts (`src/domain/ShipDesign.cpp`) and cached by design id
  (`class+module…#r9`); the cache is pure and never saved. Slots per hull: `hull_slots.csv`.
- **Refits** offer the next tank size either way or one module in or out, by the tank-refit rule; only modules
  whose effect is switched on are offered. Fleet investment also tries the winning hull with each module.
- **Crew radiation** (`src/simulation/CrewEnvironment.cpp`): cosmic rays and solar particle events dose each
  crew; docked crews get their station's dose (`station_radiation.csv`). A crew signs on per leg, and no leg may
  take it past the 1 Sv career limit; hazard pay (0.43 cr/mSv) is a wage, and the planners' time costs include
  it. Module: storm shelter.
- **Boil-off:** nuclear-thermal ships lose liquid hydrogen in flight; the Kepler planner loads and pays for it.
  Modules: 90 K cooled tank shield, 20 K zero-boil-off cryocooler.
- **Cold hold:** perishables spoil at a tenth of the ambient rate.
- **Boil-off in the planners:** a plan carries the hydrogen it expects to boil (`propellant_boiled_kg`, at the
  leg's mean rate, `leg_boiloff_kg_per_day`); fuel on arrival, the return-fuel check and the mission's fuel cost
  all count it. The audit's `Boil-off:` line counts nuclear-thermal arrivals short of the arrival burn or under
  half the reserve.
- **Wear:** drives use their rated life (nuclear-thermal cores 100 burn hours, plasma drives 10 full-power
  years; a leg must fit in what is left) and meteoroids hole plasma radiators, cutting jet power.
  `consider_repair` overhauls at any port with the spares: the wearing parts (`spares.csv`: fuel elements,
  reactor fuel, power electronics, heat pipes) in goods at the port's price plus hours at the labour scale
  (`overhaul_bill`), a few days in the yard. The planners price wear in (`wear_cr_per_propellant_kg` for
  nuclear-thermal burns, `wear_cr_per_flight_day` for plasma thrust), so asks and contracts cover it.

## Passengers

Switches and numbers: `data/economy/passengers.csv` (master switch `passengers`, on; sub-switches `rotation`,
`migration`, `discretionary`). With `passengers,0` the economy runs without them: the liner classes do not load and
cabins are never offered. Open points on routes and cabins: `docs/plans/passenger_routes.md`. A berth is costed as
linopt's per-passenger mass (2.5 t) plus a share of the habitat's constant part: about 3 t and 2,535 cr. `--econ-audit` reports passengers by kind, by station
on every route to or from Titan, the routes where most people gave up (with legs flown, legs by ships with
berths, and those that left empty while people waited), and the legs flown in all.

**Who travels.** People wait at stations in groups (kind, destination, employer, demand band):
- **Settlers:**
  - posted at the cheapest Earth gateway for a thriving station, at the growth rate;
  - off Earth, growth now happens only as they land, and a failing station loses emigrants by ship;
  - migration events post a convoy of settlers at Earth.
- **Rotation:**
  - a share of each off-Earth station's people is relieved once a tour;
  - the tour minimises the cost per day on site from the reference fares;
  - the station pays both fares, and sends no relief while it is too poor.
- **Visitors:** a gravity model deterred by the trip's cost against a year's income. They stay `visit_days`
  and go home.

**Prices.**
- **Reference fares** (`data/economy/reference_fares.csv`, `--write-reference-fares`): the cheapest cabin
  carrier's cost per berth, planned with the passengers' time.
- **What a group pays:** the reference fare adjusted for the trip's time and dose. When more wait than fit,
  everyone pays the fare that clears the berths (up to 4× the reference cost, the top demand band).
- **Giving up:** groups give up as waiting uses up their budget.
- **Who pays** (economy overhaul, households and labour market on):
  - Earth's people pay from Earth's account;
  - a station's residents pay from their savings, within their travel budget (3% of wages);
  - the work station pays for rotation;
  - a colony pays for the settlers it recruits.

  Settlers pay their own passage out of what the job gains them, and visitors travel when half a year's
  income covers the trip (`economy.md`, "Households and labour").
- **Without the labour market:** fares come from the outside account (emigrants, visitors) or from the work
  station (rotation, and settlers with `settlers_sponsored`). A station recovers what it paid for passages
  (`passage_debt_cr`) at 1/`passage_recovery_days` a day, at most `passage_max_markup` × its residents'
  spending on goods. Settler demand falls with the ticket's payback time T (reference generalized cost over a
  year's wage): 1 / (1 + (T / `settler_payback_half_years`)^`settler_payback_exponent`) of the growth rate.

**Ships.**
- **Carriers:**
  - two cabin modules for the new `cabin` slot (12/24/60 berths), plain or shielded (a 20 g/cm² water wall);
  - two 40-berth liners and the 200-berth Large Plasma Liner;
  - the liners' habitat slots take a cabin water wall and, on the plasma liners, a tank wrap: the cabin sits
    inside the water tank. Its shield is the water aboard ÷ the cabin's wall area, so it thins as the tank
    empties.
  - Cosmic rays are cut by exp(−shield ÷ 46 g/cm²); particle events see the storm shelter plus the shield.
  - Berths are apart from the hold; passengers' mass and their 2 kg/day are payload.
- **Dispatch** fills berths alongside the hold:
  - each destination's manifest adds the boarding passengers' fares less their provisions;
  - follow-ups count passengers waiting at the next port;
  - the planner adds their time to the ship's.
- **Fleet investment** values cabin designs and liners at the sustained passenger flow, filled by band at
  each group's fare or the clearing fare, and commits them to their route like cargo ships.
  - A hull fitted with a cabin module is a candidate for its berths only (the bare hull is the cargo
    candidate): its bound is the passenger bound, its probe plans no cargo, and only its passenger runs count.
  - The passenger bound assumes a ship twice as fast as the reference carrier, filled the same way, and is
    taken as the larger of the cargo and passenger bounds (a passenger run is valued on its own).

## Fleet investment (`step_fleet_investment`, `Simulation::InvestmentReview`)

- **Selling:** a ship laid up for `layup_sale_days` without a break is sold for `salvage_fraction` of its value
  to its faction's treasury. Its cash or debt goes to its home station. It stays in the audit as `(sold)`.
- **Commissioning:** every `review_days` the treasuries consider every hull in standard tanks, at every station
  whose yard is free. They skip hulls where a ship of that hull already sits idle, and hulls no one can pay for.
  - **Order of work:** candidates are ranked by a cheap bound on their return (the yard's surplus goods against
    their consumers' prices, or berths filled both ways) and probed best first, two at a time; the review stops
    once the best value found beats every remaining bound, and a purchase waits for a later tick while probes run.
  - **Valuation:** a cargo-only dispatch probe lists the runs from the yard. Each run is valued as whole round
    trips over the route commitment, along the price curves of the stocks both stations would hold by then.
    Production, consumption, other ships' observed imports and exports, and flows already committed all count,
    so a served route sells near base price.
  - **Hurdle:** profit per day, minus capital and crew, over the hull price, must reach
    `hurdle_return_per_year` (30%: purchases expect about 1.6× what they earn, so that is ~19% earned). Sustained
    sales are valued at most at the destination's landed cost (or its going rate if higher): at scarcity prices,
    estimates ran 15× the earned returns. Near `max_fleet_size`, candidates are ranked by profit per day instead.
  - **Buying:** a review buys until no candidate clears the hurdle, at most `max_ships_per_review`. Each purchase
    counts at once against the destination's open demand.
  - **Paying:** the yard station's faction pays if it can (cash plus credit, see `economy.md` "Money"),
    otherwise the richest faction. The hull is paid to the outside economy and `working_capital` to the ship.
    The ship is built in `build_days`, with the tank variant dispatch prefers.
- **Route commitment:** for `route_commitment_days` after the yard, a new ship flies only between its yard and
  the destination it was bought for. Its flow comes off that destination's demand in later reviews.
- **Liners** (`data/economy/liners.csv`) are outside all of this: see `economy.md`, "The market among ships".
- **Fleet cap:** `max_fleet_size` 130 (user, 2026-10-22). A hold cap (`max_fleet_hold_units`) is built but off: it
  filled the fleet with small ships and was slower.
- Debug: `SPACETRAINS_TRACE_INVESTMENT=1`, `SPACETRAINS_TRACE_SHIP=<id>`.

## Failures and salvage (`Failures.cpp`, `data/economy/failures.csv`)

Switches: `failures` (the clocks) and `salvage` (races for derelicts); with `failures` 0 the game is exactly as
before. Debug: `SPACETRAINS_TRACE_FAILURES=1` (clocks at start, each derelict's state), `SPACETRAINS_TRACE_SALVAGE=1`.

- **Failure clock:** every hull has a life drawn from a Weibull distribution, shape `weibull_shape` (3: the hazard
  grows with age², so a new hull rarely fails), mean = `lifetime_years` (`ship_operations.csv`, the capital charge's
  amortisation) × `mean_life_factor`. Each ship draws from its own stream (data seed + ship id), so no draw moves
  another. The starting fleet's ages come from the steady state of a fleet that replaces what fails (density ∝
  survival), and each life is drawn conditional on the age reached; new ships start at age 0.
- **Why the mean is 1.10 × 30 years:** the capital charge recovers the hull's value at V / L a year for as long as
  the ship exists, so the owner breaks even in expectation when the mean life is L × (1 + c), c = cargo lost per
  failure / V. Measured c ≈ 0.098: the time average of what owners paid for the cargo of ships in flight over the
  fleet's hull value (the audit prints it). Purchase cost, not sale value: the margin is a profit not made. There
  is no insurance (maybe later): it would only smooth the owner's losses, not change their mean.
- **When the clock runs out:** in flight, the ship fails now; in port (or laid up, or in the yard), the fault shows
  on its next leg at a fraction of the flight drawn from its stream. On a leg within one planet's system the hulk is
  towed in at once: the destination gets its cargo into stock and the scrap value from Earth.
- **Derelict:** on an interplanetary leg the ship becomes a derelict (`Simulation::derelicts()`, saved) coasting on
  the two-body orbit it had (its velocity from the planned path; plasma ships thrust all the way, so a wreck can be
  on an escape orbit). It keeps its cargo for `claim_days` (365), then is written off. The owner loses the hull (its
  capital charge stops), what it paid for the cargo and the delivery; the ship's cash goes to its home station as in
  a sale; the ship is listed `(sold)`. People aboard leave in the lifeboat and are taken on to the leg's destination
  by rescue craft that are not simulated (assumption; a timed rescue is a later extension).
- **Replacement claim:** the owner holds a claim on the lost ship's place under the fleet cap for
  `replacement_days` (180). While claims exist, a review's first purchase is the oldest claim's faction's, priced
  against its own money at any yard; if nothing clears its hurdle the claim waits for the next review, and others
  may buy only into places nobody holds.
- **Salvage run:** out from the ship's port to a rendezvous with the derelict, then on to the first port it can
  reach of its home, the derelict's destination and its origin (earliest arrival).
  - Plasma ships: `trajectory::plan_chase` against the derelict's Kepler orbit (`kepler_target`), with half the
    load for the way out (then 35%, 20% if no way home is left), and again to the port's planet (`path_target` on
    the planet's sampled motion), arriving with the cargo aboard and the 10% reserve.
  - Nuclear-thermal ships: Lambert grids (waits 0-40 days, flights 10-400 days out and 10-500 home), Δv against the
    station's circular velocity, the derelict's velocity and the port planet's; the burns are booked at the start.
    Rendezvous is dear (~8 km/s from Mars to a wreck on a transfer arc), so plasma ships do most of the salvage.
  - Worth: the cargo that fits the salvager's hold at the port's reference prices plus the scrap
    (`scrap_fraction` × ship value, paid by Earth at the rendezvous); cost: fuel at the port's price and the days at
    capital, crew and wear.
- **Who goes:** at dispatch a docked ship (idle or laid up, not under a contract, a liner loop or a new ship's route)
  weighs its best salvage run per day against its best mission (`consider_salvage`), at most every 5 days; ships that
  found nothing look every `salvage_review_days`. A ship sets out only if it would meet the wreck sooner than the
  current claimant by `racer_margin` (5%), and at most `max_racers` (3) set out for one wreck.
- **The race:** the first to reach the wreck takes the cargo that fits its hold (sold at market at its port) and the
  scrap; a racer that gets there later finds it gone and flies on to its port empty (it does not turn back: that
  would need planning from mid-flight, a later extension).
- **At the real settings** (4 starts × 1,460 days, 2026-10-28): 2 failures a run (the starting fleet's oldest), 7 of
  8 wrecks salvaged, up to 5 racers for one wreck, every loss replaced by its owner; unmet demand and profits within
  the benchmark's noise.
- **UI:** derelicts ride in the snapshot's ship list with phase `derelict` (map icon orange, a wreck card in the
  inspector); racers carry `salvage_target_name`; the top bar counts wrecks adrift.

## Money around ships

- **Dividends:** cash above `ship_cash_reserve` goes to the home station over `dividend_days`. These are internal
  transfers, outside `lifetime_profit`.
- **The background economies** (Earth, the colonies) sit on the other side of every payment that leaves the
  simulated trade (economy overhaul; Earth's account is the old outside account):
  - hulls: the yard's share goes to a yard in space;
  - salvage, interest, Earth's surface trade, station incomes;
  - travellers from Earth.

  The audit checks that stations + households + ships + treasuries + background accounts equal the seeded
  money (drift 0). Ships pay their crews' wages to the home station's households.
- **The money-supply controller** (off since the overhaul, `money_supply_days` 0) paid or taxed stations per
  head to keep stations and ships near a target that grew by each new ship's working capital.

## Fuel supply and exports

- **Fuel factories** (`fuel_factories.csv`) fill their depot up to `factory_buffer_days` of output, priced at
  their own cost (`price_cr`, 2.5-3.5 cr per 100 kg since the overhaul, against Earth's 7.9). Other depots
  hold `depot_buffer_units` outside cargo storage and only gain what ships deliver, priced along the usual curve.
  Bulk Tanker and Plasma Bulk Tanker (2,000 units) are ordinary hulls for investment. Each station's ship fuel
  purchases (60-day average) count as demand when a fuel run is valued.
- **Outer exports:** Ceres (platinum, from the belt miners), Ganymede and Titan (deuterium) and the science
  stations make light, valuable goods, stockpiled up to 720 days. Low Earth Logistics and Earth L1 sell them on to
  Earth at Earth's bid (`surface_markets.csv`; the older `export_markets.csv` still works) in any amount; planners
  see them as steady consumers, never as starving ones. Anywhere else export goods price at the floor, so nobody
  hauls them elsewhere.
