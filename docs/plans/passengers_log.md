# Passengers: build log and decisions

Plan: `docs/plans/passengers.md`. User's choices (2026-10-21):
- **Demand:** rotation, migration and discretionary trips.
- **Growth:** only by arriving migrants.
- **Fares:** the employer pays rotation; migrants and visitors pay from the outside account.
- **Carriers:** cabin modules plus liner classes.

## Step 0: baseline

- **Code:** `main` at `5442d96` (tier 2 merged).
- **365-day `--econ-audit`:** kept in the scratchpad as the byte-identical reference for switch-off runs.
- **Benchmark (4 starts × 1460 days):** the tier 2 follow-up run of the same code: unmet 0.5 ± 0.2%, last fifth
  1.0 ± 0.3%, price 1.5×, 101/138 profitable, money −20 ± 10%.

linopt `passenger_ticket_optimizer.py fare` at 1.5 AU (its defaults):

| Passengers | Fare | Payload | Transfer | Jet power | Initial mass |
|---|---|---|---|---|---|
| 600 (README baseline) | $104k | 1,945 t | 140 d | | |
| 120 | $189k | | | | |
| 60 | $295k | 478 t | 128 d | 464 MW | 710 t |

Small ships pay dearly in the study because its 300 t constant payload (shared structure, controls,
common life support) is fixed.

## Step 1: data

Decisions:
1. **A berth is linopt's per-passenger term plus a per-berth share of its constant term:**
   - 2,500 kg at 1,000 $/kg plus 0.5 t (300 t / 600) at 1,500 $/kg: 3.0 t and 3.25 M$ = 2,535 cr per berth;
   - module cabins add 10% truss;
   - scaling the shared part per berth (instead of a fixed 300 t) is an estimate, so that small cabins and
     liners are not charged a 600-berth ship's core;
   - volume per berth from the Deep Space Habitat's pressurized volume per kg (~18 m³), heat as a crew member's
     3.2 kW (estimates).
2. **One cabin module per hull** in a new `cabin` slot (game sizes): couriers 12 berths, freighters 24, bulk
   hulls 60. One size per hull keeps the refit search as small as today's.
3. **Two liner classes, 40 berths each, small hold (20 u), crew of 3-4:**

   | Class | Drive | Δv / α | Price |
   |---|---|---|---|
   | Plasma Liner | 100 MW jet | α ≈ 500 W/kg (a courier's) | ≈ 268k cr |
   | Passenger Shuttle | liquid-core NTR, the NTR Freighter's tanks | Δv 10.3 km/s | ≈ 269k cr |

   Both cost more than any ship today (the dearest is 170k cr); fleet investment decides whether they pay.
4. **Gating:** with `passengers,0` the loader drops the liner classes and their hulls' modules and slots. The
   cabin modules stay in the data but are never offered: their `berths` effect is active only with
   passengers on. The 365-day audit is byte-identical to the baseline.
5. **Save and data:**
   - save version 15;
   - the data fingerprint now mixes crew, berths, module effects and `passengers.csv`.
