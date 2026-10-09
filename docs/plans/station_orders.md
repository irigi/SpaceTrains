# Station orders, mixed cargo and contracts (step 13, 2026-10-08)

The user's idea (2026-10-07): with a limited fleet, stations should order a *mix* of goods in advance; the price
offered for each good falls as more of it is ordered, so a supply ship loads fuel until the next unit of food pays
more, then food; the order is a contract made before the ship sets off. Done overnight in three parts; see
`docs/plans/overnight_2026-10-08.md` for the measurements.

## What a station wants (orders)

- A consumer's **target stock** (where its price is the base price) covers its resupply time: three weeks, or 1.4 x
  the one-way transfer from the nearest producer, at most a year (`EconomySystem::cover_days`). The price is
  `base x clamp((target / stock)^1.3, 0.25, 16)`.
- The **forecast at arrival** (`Simulation::forecast_stock_on_arrival`) runs today's stock forward at the net rate and
  adds the same good other ships deliver *before* this ship.
- The price schedule along that curve, from the forecast stock, *is* the order: each further unit is worth less. The
  station panel lists, per consumed good, what is still wanted up to the target after the stock and the cargo on its
  way, and what the station pays now.

## Filling a hold (mixed cargo)

`Simulation::choose_mission_pass`, per destination: the hold is filled chunk by chunk (1/40 of it); each chunk goes to
the good whose next units earn the most, `urgency x (marginal sale along the destination's curve - marginal purchase
along the origin's curve)`, until the hold, the destination's storage or the ship's credit is used up or no good
earns. The whole manifest, its first half and its first quarter are planned (cargo mass in the rocket equation) and
scored like single-good runs, with the two-leg follow-up. Single-good runs remain candidates.

## Contracts

At departure each lot's price is agreed: the destination's forecast value for the units that will survive the trip
(`CargoLot::contract_value`). On arrival the station pays it whatever the market did meanwhile (pro rata if storage
forced a jettison). Ships earn what they planned; stations bear the forecast's error. The station panel lists inbound
contracts (cargo, arrival, agreed price).

## Fleet investment

A new ship is valued by its best run from the yard over the route commitment; a mixed hold is valued lot by lot (each
along its own good's stocks) less running costs once. Near the fleet limit (`max_fleet_size`, 90) candidates are
ranked by profit per day rather than return on price, so a place in the fleet goes to a big hold.

## Results (four starting dates each)

| Version | 730 days unmet | last fifth | 1460 days unmet | last ~290 days | profitable (1460 d) |
|---|---|---|---|---|---|
| v36e (single good per trip) | ~66% (one run) | ~61% | - | - | - |
| v37b (mixed cargo) | 63.3 +- 2.2% | 49.8 +- 5.3% | - | - | - |
| v39d (+ map changes, investment, contracts, fixes) | 49.8 +- 1.0% | 32.5 +- 1.6% | 32.3 +- 1.2% | 10.5 +- 1.9% | 84.5/93 |

## Open

- Remote outposts (Titan, Ganymede) stay the worst supplied: little demand, very long trips. A higher price cap for
  them made things worse (ships chased the premiums); faction subsidies for outpost supply are the next idea.
- Investment does not yet size hulls to a destination's order volume explicitly (profit per day near the limit does
  it indirectly).
- Contracts have no deadline or penalty: arrivals happen at the planned time in this simulation.
