# Steps 23–28 (2026-10-08): log and decisions to review

Plan: cover from the real round trip (23), inputs priced by what they make (24), smooth drawing of outer-system paths
(25), liners, Mars Corporation's debt and Mercury after a benchmark (26), station growth (27), random events and
stories (28). The user asked for all five follow-ups in one push.

Benchmarks: `tools/benchmark.py --days 1460`, four starting days, mean ± sd.

## Summary

| Configuration (1460 days) | unmet | last fifth (~290 d) | profitable | money | emergencies | over limit | events | run |
|---|---|---|---|---|---|---|---|---|
| after step 22 (baseline) | 17.4 ± 2.2% | 7.0 ± 4.3% | 76.8/93 | −21.8 ± 12% | 15.0 | 0.8 | – | 331 s |
| + step 23 (cover from round trip) | **5.2 ± 0.4%** | **2.4 ± 0.9%** | 75.2/94 | −28.2 ± 14% | 8.0 | 1.0 | – | 321 s |
| + step 24 (input caps) | 4.5 ± 1.4% | 2.6 ± 1.1% | 79.8/94 | −30.1 ± 19% | 10.0 | 0.5 | – | 420 s* |
| + step 27 (growth) | 4.7 ± 1.2% | 2.5 ± 1.0% | 78.2/93 | −61.0 ± 18% | 14.2 | 0.8 | – | 390 s* |
| + step 26 (faction money), events off | 4.8 ± 1.4% | 3.1 ± 1.4% | 78.2/93 | −30.7 ± 17% | 14.0 | **0** | – | 491 s* |
| **final, events on** | **4.4 ± 0.4%** | **2.3 ± 0.9%** | 77.2/94 | −18.2 ± 11% | 12.8 | **0** | 18.5 | 358 s |

\* run times are inflated where builds and tests ran alongside.

- **Step 23 is the big win:** unmet demand falls from 17.4% to about 5%, the last fifth from 7.0% to about 2.5%, and
  emergencies from 15 to 8. Titan and Ganymede leave the shortage list.
- Steps 24 and 27 are about neutral on supply. Step 26 removes the over-limit faction. Events cost nothing
  measurable.
- Trajectory flags 0 throughout.
- Exports rise from 3.4M to about 4.5–4.8M cr. The science outposts now get the electronics they make samples from.

Year 4 of the final run with events (start day 0): Mars fuel 60% unmet, Low Earth Logistics metals 16% and food 12%,
Earth L1 water 11%, everything else under 6%. Earth-pair hops are 58, 56, 62 and 25 a year (were 28 → 10). Eight
emergencies in four years.

## Step 23: cover from the real round trip

- **Before:** a consumer's target stock covered 1.4× the Hohmann one-way time to its nearest producer, at most 365
  days. Titan's electronics target was ~200 u against a ~1,250-day plasma round trip. Above the target the price
  falls below reference, so no delivery brought more than a year's use, and Titan ran dry for two years or more
  between calls.
- **Now:** the target covers at least `cover_round_trip_factor` (1.2) × `round_trip_days` from
  `reference_prices.csv` (the reference trip's median round trip), at most `max_cover_days` (1460). Both keys are in
  `pricing.csv`.
- **Decision to review:** only when the producer is on another planet. Within a planet's system the reference
  plasma ship spirals slowly (Earth L1 ↔ Lunar Gateway: 55 days), which would have overstated what a local shuttle
  needs.
- **Decision to review:** fuel at depots keeps its own target (the depot buffer). Mars fuel is therefore not
  covered by this rule; see open items.
- **Test change:** the liner test now runs 800 days instead of 700. The West liner's first leg to Titan takes ~715
  days with the larger load it now carries.

## Step 24: inputs priced by what they make

- **24a, diagnosis:** with step 23 in, Earth-pair hops recovered by themselves (117 in year 4 of the step 24 run).
  The crowding-out was largely the outposts' shortages pulling the fleet out. No bug found.
- **24b:** a good that is an `input:` at a station may rise past `price_cap` when short, up to `input_value_share`
  (25%) of the value of the outputs one unit makes, at most `input_price_cap` (12) × its reference
  (`EconomySystem::price_cap(station, good)`). At the target stock the price is still the reference.
  - Earth L1 water: ~12× instead of 4×.
  - Low Earth Logistics metals: ~8×.
  - The science outposts' electronics (an input of their samples): 12×.
- **Result:** about neutral (4.5 vs 5.2%, within noise), with slightly more profitable ships. Earth L1 water went
  from 32% unmet (step 22) to 11–24% across runs.
- **Decision to review:** whether 12× is too steep for the outposts' electronics. It helps Titan, but a single
  late delivery can sell for a lot.

## Step 25: smooth outer-system paths (Godot)

- **Cause:**
  - the drawn path was an offset from an anchor that blended the origin planet into the destination over the
    whole flight;
  - VariableISP paths are thinned by curvature, so flat spirals keep samples weeks apart;
  - between samples, the planet's yearly circle leaked into the line as a sawtooth.
- **Fix:** interplanetary flights are drawn from their heliocentric samples, with radius and angle interpolated
  between them. Hops within one planet's system keep the rail. `--snapshot-json` now also writes the planned paths
  next to the snapshot, to make this measurable.
- **Measured** on a day-200 snapshot (largest distance of the drawn point from the planned path): up to 0.31 AU
  before, at most 0.012 AU after (the arc between samples).

## Step 26: liners, Mars Corporation, Mercury

- **Liners:** no extra liners. With step 23, each call carries enough that Titan and Ganymede are under 5% unmet in
  year 4.
- **Mercury:** no longer short (food was the suspect), so nothing changed.
- **Mars Corporation's debt** was stock, not running losses. After step 23 Mars holds a year or more of imports,
  bought on station debt, while the faction could only borrow against its few ships. Independent Consortium meanwhile
  kept 15M of taxes while Ganymede sat at −1.3M cr. Two changes (`Simulation::faction_credit_limit`,
  `step_treasuries`):
  - a faction may borrow against its stations' stock at reference prices (×`faction_loan_to_value`, as for ships);
  - a faction with savings covers a station's whole deficit. The core-crew cap of step 17 applies only while the
    faction borrows.
  - Factions over their limit: 0 in every run (was 0.5–1). The money supply recovers from −61% (after growth) to
    about −20 to −30%.
- **Decision to review:** the second change weakens step 17's rule "a station that cannot pay its way gets less
  than it spends" for factions in surplus. I think that's right while the money sits idle in the treasury.

## Step 27: station growth

- **Data:** `data/economy/growth.csv`:
  - +3%/year while the worst upkeep good's availability, averaged over 90 days, is at least 95% and the station is
    above its credit floor;
  - −5%/year below 70% (emigration);
  - between the core crew (30%) and 1.5× the seeded population.
- **Mechanics:** rates, targets and the money controller's per-capita share follow `StationState::population`.
  Changes of 5% are announced ("news" in the ticker). The station panel shows the population change and supply.
  Save v9.
- **Result over four years:** Ganymede, Titan, Ceres and Earth L1 grow about 10%; Mars shrinks about 10%. Supply is
  unchanged.
- **Decision to review:** the rates are slow on purpose. Growth needs supply to keep up, so it can't run away, and
  the 1.5× cap answers the "two efficient stations" worry.

## Step 28: random events and stories

- **Data:** `data/events/events.csv`, 11 events, about 4.6 a year in all:
  - outages: Earth L1 crop blight, Low Earth Logistics fab contamination, Mercury solar storm, Venus bioreactor;
  - booms: Ceres platinum seam, Titan cryovolcano for science samples;
  - demand spikes: Mars and Ganymede epidemics, Titan hull breach;
  - settler migrations: Mars and Lunar Gateway.
- **Seed:** `data/events/settings.csv`, mixed with the start date. The generator state is saved (save v10).
- **Mechanics:** events act only through the production step, so prices, emergencies and ships respond through the
  market. Headlines go to the ticker, and the station panel lists running events with days left.
  `--no-events` / `benchmark.py --no-events` switch them off for comparisons.
- **Result:** with events the economy is no worse (4.4 ± 0.4% vs 4.8 ± 1.4%).
- **Decision to review:** the event list and rates. Fuel factories (Venus, Mercury) can't have outages yet, because
  fuel comes from `fuel_factories.csv`, not recipes.

## Open items

- **Mars fuel** (60% unmet in year 4 of both final audit runs). Mars fuel's target is the depot buffer, not the step
  23 cover, so tankers bring little at a time from 250+ days away. Next: give depot fuel the round-trip cover too.
- **Low Earth Logistics** metals and food at 11–16%, and Earth L1 water at 11–17%: the Earth cluster's local
  shuttles.
- The money supply sits at −20 to −30% of target (stock on station shelves is not counted as money).
- Fuel-factory events; piracy (deferred by the user).
