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

## Deferred Work

- More detailed production chains
- Price signals and market scoring
- Station budget / faction budget systems
- Maintenance and service commodities

## Tests

- Production increases stock for positive-rate commodities.
- Consumption reduces stock but clamps at zero.
- Profile net-rate lookup matches loaded recipe data.
