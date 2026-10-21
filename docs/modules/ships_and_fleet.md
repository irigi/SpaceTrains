# ships and fleet

How ships pay their way, choose missions, refit and are bought and sold. All of it lives in `Simulation`
(`src/simulation/Simulation.{hpp,cpp}`). Parameters are in `data/economy/` (`ship_operations.csv`,
`fleet_investment.csv`, `open_economy.csv`, `fuel_supply.csv`, `fuel_factories.csv`). This file condenses the
finished plans for operating costs (parts A-C, v12-v25), fleet investment (v27-v31), fuel factories (v29) and
outer exports (v32-v33). Their full texts, with measurements, are in git history before 2026-10-11 under
`docs/plans/`.

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

Wages and capital go to the ship's home station; provisions are ordinary trades. A ship whose cash falls below
zero **lays up**: crew discharged, capital still charged. It reactivates when a mission's profit covers rehiring
and provisioning.

## Dispatch (`choose_mission`)

- **Two-leg scoring.** Each candidate leg (cargo, or empty toward a pickup) is scored together with the best
  follow-up leg from its destination: `(urgency × leg profit + follow-up profit) / (leg days + follow-up days)`.
  Profit is revenue − cargo cost − fuel − provisions − daily cost × (wait + transit). Follow-ups come from the
  forecast stock at arrival, less what other ships have already planned to pick up. Without one, the ship is
  charged an empty trip back. They are cached per class, 5-day departure bucket and 10% fuel bucket
  (`estimate_leg`).
- **Mixed cargo, forecasts and contracts:** see `economy.md`, "Prices and target stocks".
- **Mission-sized fuelling (part B).** Plans carry the payload (cargo and provisions) in the rocket equation and
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

## Tank refits (part C)

- Every class is built with 50/100/150/200% of its nominal propellant (`build_ship_classes.py`). Variants share a
  `hull_id`; the nominal one keeps the class id.
- Every 90 days, a ship docked at its home base compares the next smaller and next larger tank. Each comparison
  is a full dispatch pass as that variant, planned from the day the yard would finish (`consider_refit`).
- **Refit rule:** refit when (variant score − current score) × 180 days > yard bill + current score × 20 days.
  - Cost: `refit_days` in the yard, crew discharged, and a bill of `refit_cost_fraction` of the hardware value
    added or removed, paid to the home station. Propellant that no longer fits is sold to the depot.
  - Afterwards the ship is locked out of further refits for the yard time plus 180 days.

## Modules, crew radiation, boil-off and wear (tier 2, 2026-10-21)

Plan `docs/plans/ship_structures_tier2.md`; decisions and measurements `docs/plans/tier2_log.md`; sources
`docs/ship_technology.md` ("Crew radiation", "Boil-off and wear"). Every mechanism is a switch in
`data/economy/ship_environment.csv` (all on by default); with all off the economy is the pre-tier-2 one.
- **Designs.** A ship flies `ship_design(ship)`: its class (tank variant) plus its fit (`ShipState::fit`, modules
  from `data/ship_classes/modules.csv`, sized per hull by the builder) plus its radiator damage in 10% steps.
  Fitted designs are derived from the parts (`src/domain/ShipDesign.cpp`) and cached by design id
  (`class+module…#r9`); the cache is pure and never saved. Slots per hull: `hull_slots.csv`.
- **Refits** offer the next tank size either way or one module in or out, by the part C rule; only modules
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

## Fleet investment (`step_fleet_investment`)

- **Selling:** a ship laid up for `layup_sale_days` without a break is sold for `salvage_fraction` of its value
  to its faction's treasury. Its cash or debt goes to its home station. It stays in the audit as `(sold)`.
- **Commissioning:** every `review_days` the treasuries consider every hull in standard tanks, at every station
  whose yard is free. They skip hulls where a ship of that hull already sits idle, and hulls no one can pay for.
  - **Valuation:** a cargo-only dispatch probe lists the runs from the yard. Each run is valued as whole round
    trips over the route commitment, along the price curves of the stocks both stations would hold by then.
    Production, consumption, other ships' observed imports and exports, and flows already committed all count,
    so a served route sells near base price.
  - **Hurdle:** profit per day, minus capital and crew, over the hull price, must reach
    `hurdle_return_per_year`. Near `max_fleet_size`, candidates are ranked by profit per day instead.
  - **Buying:** a review buys until no candidate clears the hurdle, at most `max_ships_per_review`. Each purchase
    counts at once against the destination's open demand.
  - **Paying:** the yard station's faction pays if it can (cash plus credit, see `economy.md` "Station money"),
    otherwise the richest faction. The hull is paid to the outside economy and `working_capital` to the ship.
    The ship is built in `build_days`, with the tank variant dispatch prefers.
- **Route commitment:** for `route_commitment_days` after the yard, a new ship flies only between its yard and
  the destination it was bought for. Its flow comes off that destination's demand in later reviews.
- **Liners** (`data/economy/liners.csv`) are outside all of this: see `economy.md`, steps 18-22.
- Debug: `SPACETRAINS_TRACE_INVESTMENT=1`, `SPACETRAINS_TRACE_SHIP=<id>`.

## Money around ships

- **Dividends:** cash above `ship_cash_reserve` goes to the home station over `dividend_days`. These are internal
  transfers, outside `lifetime_profit`.
- **The outside account** sits on the other side of every payment that leaves the simulated trade: residents,
  hulls, salvage, export markets. The audit checks that stations + ships + outside equal the seeded money
  (drift 0).
- **The money-supply controller** pays or taxes stations per head to keep stations and ships near the target
  over `money_supply_days`. The target grows by each new ship's working capital. It never pushes a station out
  of the treasury band.

## Fuel supply and exports

- **Fuel factories** (`fuel_factories.csv`) fill their depot up to `factory_buffer_days` of output. Other depots
  hold `depot_buffer_units` outside cargo storage and only gain what ships deliver, priced along the usual curve.
  Bulk Tanker and Plasma Bulk Tanker (2,000 units) are ordinary hulls for investment. Each station's ship fuel
  purchases (60-day average) count as demand when a fuel run is valued.
- **Outer exports:**
  - Station recipes (`station_recipes.csv`): Ceres makes platinum; Ganymede and Titan make deuterium. These are
    light, valuable goods, stockpiled up to 720 days.
  - Export markets (`export_markets.csv`): Low Earth Logistics and Earth L1 sell what is delivered on to Earth at
    base price, in any amount. Planners see them as steady consumers, never as starving ones.
  - Anywhere else, export goods price at the floor, so nobody hauls them elsewhere.
