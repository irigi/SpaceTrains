# Steps 18–22 (2026-10-08): log and decisions to review

Plan: start stocked (18), production at 3x consumption (19), landed-cost fuel at depots without a factory (20),
fleet cap by hold capacity (21), scheduled liners (22). The user chose the options for 19–22 on 2026-10-08.
Benchmarks: `tools/benchmark.py`, four starting days, mean ± sd. Baseline (after step 16), 730 days: unmet
48.5 ± 2.9%, last fifth 29.9 ± 5.2%, 50.5/90 profitable, money −23.9 ± 8.2%, exports 0.79M, 13 emergencies.

## Step 18: start stocked

`Simulation::starting_inventory`: every good a station consumes starts at least at its target stock (its
resupply cover: three weeks, or 1.4x the transfer from the nearest producer, at most a year), scaled down to fit
85% of storage. The data files keep their inventories; the rule follows the recipes.

| | unmet | last fifth | profitable | holds | money | exports | emergencies | interest |
|---|---|---|---|---|---|---|---|---|
| after step 16 | 48.5 ± 2.9% | 29.9 ± 5.2% | 50.5/90 | 94,600 u | −23.9 ± 8.2% | 0.79M | 13 | 123k |
| step 18 | **24.0 ± 2.9%** | 25.0 ± 4.2% | 50.2/90 | 110,200 u | −30.3 ± 16% | 1.15M | 14 | 36k |

Most of the old two-year ramp-up was the stations starting nearly empty.

### Decisions to review

1. **Stations start at their target stock**, not at the data files' few weeks. For the outposts that is up to a
   year of consumption, the stock they would hold if they had been supplied all along.
