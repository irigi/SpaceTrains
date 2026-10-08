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

- A station's price for a good is `base x clamp((target / stock)^1.3, 0.25, 16)`; trades integrate it exactly over
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

## Deferred Work

- More detailed production chains
- Station budget / faction budget systems
- Maintenance and service commodities

## Tests

- Production increases stock for positive-rate commodities.
- Consumption reduces stock but clamps at zero.
- Profile net-rate lookup matches loaded recipe data.
