# Fuel Factories and Bulk Tankers (roadmap step 5, v29)

Until v28 every station's depot refilled itself at 200 u (20 t) of fuel a day, so fuel cost the base
price everywhere and nobody hauled it. The fleet bought about 4 t a day, almost all of it around Earth
(Low Earth Logistics, Earth L1, Lunar Gateway). Now only a few stations make fuel, and the others hold
what ships bring.

## Mechanisms

1. **Fuel factories** (`data/economy/fuel_factories.csv`): Venus Aerostat Exchange 360 u/day, Titan Atmo
   Works 210, Lunar Gateway 250 (lunar ice), Ceres Deep Depot 100, Ganymede Deep Mine 100. None in Earth
   orbit. A factory fills its depot at its output rate up to `factory_buffer_days` (30) of output, at
   least the depot buffer (`data/economy/fuel_supply.csv`, 3000 u). The fuel lines left the recipes
   (Earth L1's 20 u/day and the chem hubs' 300), and a factory's output counts in its station's net
   rates, so dispatch sees it as a producer. Every depot starts full (3000 u).
2. **Other depots** keep the 3000 u buffer outside cargo storage but never refill on their own. Their
   fuel prices along the usual curve against that buffer (up to 16x base when empty), which is what
   pays tankers.
3. **Bulk tankers** (`build_ship_classes.py`): Bulk Tanker (solid-core NTR, 2000-unit hold, 300 t of
   propellant, 77k cr) and Plasma Bulk Tanker (40 MW, 2000 units, 100 t, 90k cr), with tank variants.
   2000 units of fuel are 200 t. Fleet investment buys them like any hull.
4. **Fuel demand for investment.** Each station averages the fuel ships buy there (`ship_fuel_units_per_day`,
   60-day exponential window). The treasuries count it with the station's own consumption when they
   value a fuel run.
5. **Fuel-aware planning** (`Simulation::choose_mission`):
   - *Carried return fuel.* Where the destination's depot cannot refuel a ship for a leg as long as this
     one, the ship loads the missing fuel here on top of the plan's load. That fuel rides as payload (the
     leg is re-planned with it, up to three times), must fit in the tanks and in what this port sells,
     and is added back to the transit propellant samples. The trip is rejected only when even that is
     impossible. Fuel cargo runs do the same: a tanker that bought its return fuel out of the cargo it
     just delivered drained the depot it supplied.
   - *Economic tankering.* Where the destination sells fuel at more than twice this port's price
     (`TANKERING_PRICE_RATIO`), the ship carries its return fuel rather than buying it there.
   - *Fuel at the port's price.* Dispatch still values the burn at base price (fuel aboard is sunk) but
     adds the scarcity premium on fuel it must buy here. The planners trade propellant against time at
     this port's price, so ships fly slower, cheaper transfers where fuel is scarce.
   - *Cost-aware cis-lunar spirals.* The plasma planet-system spiral used 1.5x the shortest transfer its
     full load allowed, whatever the costs: a Plasma Freighter burned 36 t to carry 15 t of fuel from Luna
     to Low Earth Logistics. It now takes the cheapest of 1.5x to 10x that time.
6. **Fleet investment:** the two-review settle rule is gone (committed flow discounts a served route, and
   the rule held Lunar Gateway to one tanker every 180 days). A yard builds one ship at a time.

## v29 result (730 days)

- Fleet lifetime profit 444k (v28 871k): fuel is now a real cost, 690k (v28 135k). Cargo margin 1.54M
  (v28 1.39M), which includes fuel sales by tankers. 14/32 ships profitable, 0 laid up, 0 stranded,
  32 CRITICAL lines (v28 33). Money supply +2.2% of the target, drift 0. Trajectory audit: 0 of 325
  plans flagged. Run 5m32 (v28 4m02); the re-plans for carried fuel cost the extra time.
- Ships bought 2330 t of fuel at Lunar Gateway at 8.7 cr/u; carried return fuel on 97 trips. Fuel
  reached Low Earth Logistics (8471 u) and Earth L1 (2308 u) by tanker. The treasuries bought a Plasma
  Bulk Tanker at Lunar Gateway (day 240), which made +206k: four 1000 u runs to Low Earth Logistics in
  its commitment, then fuel to Mercury and metals back.
- Earth-orbit fuel costs about 35-60 cr/u from day 180 on (Earth L1 averaged 12 cr/u, Low Earth
  Logistics 49): the shuttles leave once their commitment ends and only one more tanker was bought.

**Open points.**
- Mars Transfer Port runs dry around day 350 (its station uses 8.5 u/day) and Mercury is marginal: no
  treasury chose a Ceres-Mars or Venus-Mercury tanker, because Earth routes valued higher every review.
- The investment valuation prices the margin at today's scarcity price (see `fleet_investment.md`);
  for fuel that is up to 16x base.
- Tankers leave their route after the 180-day commitment, as other new ships do.
