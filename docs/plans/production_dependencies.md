# Production dependencies and upkeep penalties (step 14, planned 2026-10-08, penalties confirmed, not implemented)

## Why

A station profile (`data/recipes/recipes.csv`) is one flat list: negative rows are consumed, positive rows are
produced. Nothing says which inputs an output needs, so every output is scaled by one number computed from all
inputs (`EconomySystem::step`, "Production gating").

- Until v37 that number was the **minimum** availability over the inputs: running out of 0.75 u/day of medicine
  cut the Mercury smelter's 100 u/day of metals to 10%, which starved its customers in turn (shortages cascaded).
- Since v37 it is the **value-weighted average**. That stops the cascade, but it is not physical: a smelter with
  plenty of food runs at about 90% with no ore. The user rejected it (2026-10-08): each good should have
  dependencies that must be met, and a shortage of something that is not a dependency (food) should cost output
  through explicit, per-condition penalties.

## Model

Every consumed good of a profile has one **role**:

1. **Material input of a named output** (hard dependency). Each output runs at the lowest availability among its
   own material inputs (the old minimum rule, but per output). An output with no material inputs (a mine, an
   ice extractor, a farm on local water) depends on none.
2. **Upkeep** of the whole station (life support and the crew's needs). A shortage applies an explicit penalty to
   every output of the station, per condition, and the penalties multiply.
3. **Consumption only** (residents' demand with no effect on output), if any good needs it.

Availability stays as it is now: stock over a 7-day buffer of the consumption rate, clamped to [0, 1].

### Upkeep penalties (confirmed by the user 2026-10-08)

| Shortage (availability 0) | Output multiplier | Note |
|---|---|---|
| medicine | 0.85 | sick days |
| food | 0.5 | rationing |
| fuel (station operations) | 0.7 | station keeping, local transport |
| oxygen or water | 0.1 | survival level; triggers emergency resupply (below) |

A partial shortage scales the penalty linearly: multiplier = 1 - (1 - full_penalty) x (1 - availability).
No penalty is total: a station never stops completely, so it cannot fall into a state it never leaves.

### Emergency resupply (user, 2026-10-08)

A station short of oxygen or water must not stay broken: if several stations sit at 10% for long, the economy
breaks. The owning faction then pays for an emergency delivery.

- **Trigger:** an upkeep good (oxygen and water; food too if the benchmark shows long famines) whose forecast stock,
  with the contracts already on their way, runs out before the soonest delivery a ship could make plus a margin,
  or whose availability is below 30%.
- **Emergency contract:** the faction treasury (not the station) offers a fixed price for 30 days of consumption,
  less the cargo already on its way: the station's reference price times an emergency premium (start at 3x). The
  premium rises every 10 days nobody takes it. The faction pays even when its treasury goes negative.
- **Sourcing:** a ship filling an emergency contract may buy below a station's sellable reserve (requisition), at
  that station's curve price.
- **Ships:** the contract is one more mission candidate with a fixed value, so the normal scoring decides who
  flies it. Idle ships and ships still waiting to depart can take it. A ship already in transit is not diverted.
- **End:** when the forecast covers 14 days again.
- **Observer:** each emergency is logged as an event (station, good, ship, cost to the faction) and shown in the UI.
  The benchmark counts emergencies per year and their cost; they should be rare.

### Data

- `data/recipes/recipes.csv` gets a `role` column: `input:<output>` (repeatable: one row per output it feeds,
  or `input:<output1>|<output2>`), `upkeep`, or empty for outputs.
- `data/economy/upkeep_penalties.csv`: `commodity_id,full_shortage_multiplier`.
- Every existing profile gets its rows classified, for example:
  - smelter_hub: metals <- (none, mined); machinery <- metals, electronics; upkeep food, water, oxygen,
    medicine, fuel.
  - industrial_hub: electronics <- metals; machinery <- metals; reactor_fuel and fuel probably input of both;
    upkeep food.
  - agri_hub: food <- water; oxygen <- water; medicine <- electronics? (to be decided per row with the user's
    taste in mind; the classification is the main design work).

### Code

- `EconomySystem::step`: compute upkeep multiplier once per station, then per output the minimum over its
  material inputs, times the upkeep multiplier. Consumption itself is unchanged (inputs are consumed at full
  rate whether or not the output runs, as now; revisit if it wastes scarce inputs).
- Loader and validation: every consumed row has a role; every `input:` names an output of the same profile.
- Station panel: show per output what limits it ("machinery 40%: metals short") and the upkeep penalties in force.
- Docs: `docs/modules/economy.md`.

## Checks

- Unit tests: an emergency contract opens below the trigger, closes at 14 days of cover, and the faction pays.
- Unit tests: an output stops without its material input and is unaffected by another output's input; upkeep
  penalties multiply; an output with no inputs runs at full rate under no shortage.
- `tools/benchmark.py` at 730 and 1460 days against v39d (730 d: 49.8 +- 1.0% unmet; 1460 d: 32.3 +- 1.2%, last
  ~290 days 10.5 +- 1.9%, 84.5/93 profitable). Watch for the cascade coming back (Mercury, Lunar Gateway, Earth L1)
  and record the result in `ship_operating_costs.md`.
