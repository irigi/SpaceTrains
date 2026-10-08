# economy

## Purpose

`economy` advances station inventories over time and produces the supply/demand shape that mission generation uses. It is the source of commodity production and consumption, not the owner of ship decisions.

## Responsibilities

- Apply production/consumption rates to station inventories.
- Expose per-profile net rates for mission scoring.
- Keep inventory math deterministic and simple.

## Non-responsibilities

- Choosing which ship flies where
- Reserving cargo on missions
- Rendering inventory state
- Loading economy data files

## Public Interfaces

```cpp
class EconomySystem {
public:
    explicit EconomySystem(const UniverseDefinition& universe);

    void step(std::vector<StationState>& stations, double dt_s) const;
    std::unordered_map<std::string, double> get_profile_net_rates(
        const std::string& profile_id) const;
};
```

## Data Flow

```
UniverseDefinition recipes + StationState inventories -> EconomySystem -> updated StationState inventories
```

## Invariants

- Inventory values never go below zero.
- Economy stepping is independent of Godot or UI frame rate.
- Profile rate lookup is derived from loaded recipe data, not hardcoded.

## Prices and target stocks (v36-v37)

- A station's price for a good is `reference x clamp((target / stock)^1.3, 0.25, 4)` (16x base before step 15); trades integrate it exactly over
  the stock they move (`get_trade_value`), so big lots move along the curve.
- **Target stock.** A producer's is 14 days of output. A consumer's covers its *resupply time*: three weeks, or
  1.4 x the one-way transfer from the nearest producer (Hohmann half-orbit between the parent planets, a few days
  within one planet's system), at most a year (`cover_days`). With three weeks everywhere a hold sized for a
  200-day route flooded a Mars price to a quarter of base and nobody supplied the distant stations.
- **Forecast at arrival** (`Simulation::forecast_stock_on_arrival`): today's stock run forward at the net rate,
  never below empty, plus the same good other ships deliver *before* this ship arrives. (Before v36 every inbound
  cargo counted, so a hop of hours to a starving station looked worthless while a slow ship was months out.)
- **Mixed cargo** (dispatch, `Simulation::choose_mission_pass`): per destination, the hold is filled chunk by chunk
  (1/40 of it) with the good whose next units earn the most along both stations' curves, weighted by how short the
  destination is of it; the whole manifest, its half and its quarter are scored like single-good runs. Fleet
  investment values a new ship by its best run, a mixed hold valued lot by lot (v39a).
- **Contracts** (v39c): at departure each lot's price is agreed from the destination's forecast on arrival (the
  forecast dispatch planned with, for the units that survive the trip); the station pays it on arrival whatever the
  market did meanwhile (pro rata if storage forced a jettison). Ships earn what they planned; stations bear the
  forecast error. The station panel lists inbound contracts and the goods it still wants (orders).
- **Transport capacity** (`--econ-audit`): steady supply of every consumer from its nearest producer needs about
  45,000 u of holds in transit (104,000 with the export markets); the fleet is capped at `max_fleet_size` (90).

## Production dependencies and emergencies (step 14)

- Every consumed recipe row has a **role** (`data/recipes/*.csv`): `input:<outputs>` (feedstock: the outputs stop
  without it), `upkeep` (life support, maintenance, power: a shortage penalises every output of the station) or
  `consume` (demand with no effect on output).
- **Availability** of a consumed good: stock over a 7-day buffer, in [0, 1]. **Upkeep multiplier**: the product of
  `1 - (1 - m)(1 - availability)` over the upkeep goods, `m` from `data/economy/upkeep_penalties.csv` (no penalty is
  total). Each **output** runs at the lowest availability of its own inputs times the upkeep multiplier (zero while
  storage is 85% full). A plant that is not running uses no feedstock; upkeep and consume goods are used at the full
  rate. `StationState::output_factor` and `upkeep_multiplier` show the result (station panel, `--econ-audit`).
- **Unmet demand**: stock-outs of upkeep and consume goods, and for an input the output it failed to feed (where
  it is the limiting input).
- **Emergencies** (`Simulation::step_emergencies`, goods flagged in `upkeep_penalties.csv`: oxygen and water):
  opened when the forecast stock with inbound cargo runs out within 21 days, closed when it covers 14 days of use.
  One contract for 30 days of use: while units are open, the station's price below 30 days of stock is at least
  the premium times its reference price; the premium starts at 3x and grows 1.5x every 15 days nobody takes it, up
  to 24x. The faction pays the part above the station's curve (`CargoLot::emergency_premium`), on arrival.

## Reference prices (step 15)

- A consumer's curve is centred on its **reference price** (`EconomySystem::reference_price`), read from
  `data/economy/reference_prices.csv`: base price plus `carrier_margin` x the cheapest cost per unit of carrying the
  good from one of its two nearest producers (planned round trips, full or partial hold out, back empty). Regenerate
  with `spacetrains_headless --write-reference-prices data/economy/reference_prices.csv`. Producers, fuel and export
  markets keep the base price. The curve is clamped to [0.25, `price_cap`] x its centre (`pricing.csv`, 4).

## Station money (step 17)

- A station and its residents are one account: local production is free; residents pay for what they use up at
  the station's **import cost** (`StationState::import_unit_cost`, deliveries averaged over about 30 days of use;
  the reference price before any delivery), with money from the outside account (`Simulation::settle_local_economy`).
- Faction treasuries tax station cash above 250k, top up stations below 25k by at most the upkeep of 30% of
  their population per day, may **borrow** down to minus (50% of their fleet's hull value + 2 years of taxes),
  pay 5%/yr interest on debt, and buy ships with cash plus credit (`open_economy.csv`).
- A station budget gate (`affordability_days`, off) can scale what a station offers ships by what it can pay.
- Details and the variants tried: `docs/plans/steps_14_17_log.md`.

## Steps 18-22 (economy improvements)

- **Starting stocks** (`Simulation::starting_inventory`): every consumed good starts at least at its target stock,
  within 85% of storage.
- **Production scale** (`tools/rebalance_production.py --multiple 3`): a tool that scales profile industries to a
  multiple of system consumption. Tried and reverted in data (it removed the slack the supply chains need).
- **Depot fuel** at stations without a factory is centred on base plus the carriage from the nearest factory
  (part of `reference_prices.csv`).
- **Fleet cap** (`fleet_investment.csv`): `max_fleet_hold_units` (built, 0 = off) with `max_fleet_size` (90); while
  a hold cap is set, candidates near it rank by profit per day per hold unit.
- **Scheduled liners** (`data/economy/liners.csv`): a seed ship flies its loop of stops forever, carrying whatever
  pays for the next stop (or nothing), never laid up, sold or refitted, outside the fleet cap; its faction tops its
  cash up when it runs out (`ShipLedger::subsidies`).
- Results and decisions: `docs/plans/steps_18_22_log.md`.

## Steps 23-28

- **Cover from the round trip** (step 23, `pricing.csv`): a consumer whose nearest producer is on another planet
  wants at least `cover_round_trip_factor` (1.2) x the reference round trip of stock (`round_trip_days` in
  `reference_prices.csv`), at most `max_cover_days` (1460). Titan's electronics cover went from 365 to ~1,460 days.
- **Input value** (step 24): a good that is an `input:` at a station may rise past `price_cap` when short, up to
  `input_value_share` (25%) of the value of the outputs one unit makes, at most `input_price_cap` (12) x its
  reference (`EconomySystem::price_cap(station, good)`).
- **Faction money** (step 26): a faction may borrow against its stations' stock at reference prices as well as its
  fleet; a faction with savings covers its stations' whole deficit (the core-crew cap applies only while it
  borrows).
- **Growth** (step 27, `growth.csv`): population grows `growth_per_year` while the worst upkeep good's availability,
  averaged over `window_days`, is at least `grow_above` and the station is above its credit floor; it shrinks
  `decline_per_year` below `decline_below`; bounds are the core crew and `max_population_factor` x the seed. Rates,
  targets and the money controller's per-capita share follow the current population (`StationState::population`).
- **Events** (step 28, `data/events/`): outages, booms, demand spikes and settler migrations at given stations,
  with a rate per year and a duration range, drawn from a seeded generator (saved). They act only through the
  production step. `--no-events` switches them off.
- Results and decisions: `docs/plans/steps_23_28_log.md`.

## Deferred Work

- More detailed production chains
- Piracy and police (user, 2026-10-08: later)

## Tests

- Production increases stock for positive-rate commodities.
- Consumption reduces stock but clamps at zero.
- Profile net-rate lookup matches loaded recipe data.
