# Passenger routes: findings and plan (2026-10-22)

The fleet cap is settled at 130 (user, 2026-10-22). These are the open passenger questions, with the route data
found while checking the cap sweep (passengers_log, "Cap findings checked").

## Route data (cap 130, start 0, 1,460 days, `--econ-audit` "Routes by people who gave up")

| Route | Posted | Boarded | Gave up | Legs flown | By ships with berths | Left empty while people waited | Problem |
|---|---|---|---|---|---|---|---|
| Earth L1 → Luna | 4,713 | 1,344 | 2,998 | 165 | 30 | 3 | Luna fare (A) |
| Earth orbit → Luna | 3,176 | 600 | 2,270 | 119 | 10 | 1 | Luna fare (A) |
| Luna → Earth L1 | 1,812 | 742 | 966 | 144 | 24 | 2 | Luna fare (A) |
| Luna → Earth orbit | 1,620 | 631 | 833 | 163 | 17 | 1 | Luna fare (A) |
| Earth orbit → Venus | 2,290 | 932 | 650 | 26 | 11 | 3 | few legs (B) |
| Earth orbit → Mars | 1,653 | 475 | 511 | 16 | 8 | 0 | few legs (B) |
| Earth orbit → Mercury | 1,350 | 424 | 431 | 14 | 8 | 0 | few legs (B) |
| Mars → Earth L1 | 2,370 | 931 | 377 | 16 | 11 | 1 | few legs (B) |
| Mercury → Earth L1 | 1,081 | 200 | 308 | 15 | 5 | 0 | few legs (B) |
| Earth orbit → Ceres | 1,042 | 332 | 238 | 14 | 6 | 1 | few legs (B) |

From other runs:
- Titan → Earth (8-year runs): 2,428-2,684 posted, 40-211 landed. Few ships leave Titan (C).
- Ceres → Titan: 6-15 posted, none boarded (too small to matter).
- Mercury → Mars, Mercury → Titan: no reference fares, so no passengers (atlas gap, D).

Problems:
- **A. Luna fare.** The reference fare is 86 cr (Earth L1) and 93 cr (Earth orbit), the cost per seat of a
  200-berth plasma liner (`data/economy/reference_fares.csv`, 9.1-10.6 days). Every design's purchase probe
  values a Luna passenger run below the hurdle, from the purchase trace (`SPACETRAINS_TRACE_INVESTMENT=1`):
  - Passenger Shuttle (ntr_shuttle) −48%/yr at 1.89 passengers/day, 16.1 days;
  - light freighter or tanker with a cabin −14 to −16%/yr;
  - plasma courier −13%/yr;
  - plasma liner from Luna +25%/yr (the hurdle was not met).
  Only 2 Luna passenger ships were bought in 4 years (days 1,087 and 1,150, both at Luna).
- **A2. Long Earth → Luna probe times** for a 5-day hop: 14-22 days for plasma ships, 16.1 for the NTR
  shuttle, and some probes at 158-692 days (plasma liner 410 d, plasma courier 692 d). These may be waits for a
  window counted as travel, or planner trouble near Earth. Check first: it changes the economics of A.
- **B. Few legs on the interplanetary routes.** There are 14-26 legs in 4 years, about half by ships with
  berths. People wait 100-300 days and give up. Ships that leave rarely go out empty while people wait, so
  boarding works. The question is whether a route with steady travellers should get a scheduled liner.
- **C. Titan return leg:** demand for the trip home with little ship traffic away from Titan.
- **D. Mercury atlas gap.**
- **E. Unexplained:** boardings fall 20% at cap 220 against cap 130 with the same berth legs. Lower faction
  money explains part of it (sponsored settlers −300). Low priority, because the cap stays 130.

## A2 findings: Earth ↔ Luna probe times (2026-10-22)

Measured with a temporary trace of every purchase probe's Earth ↔ Luna passenger leg (400 days, start 0;
the trace code was removed again). There are three causes, all outside the fare model:

1. **Nuclear and chemical ships wait for the Moon.** The Kepler planet-system branch
   (`TrajectoryPlanner.cpp`, `planet_system_primary` block) flies a 5.0-day Hohmann transfer but first waits
   until the Moon's phase lines up: **15.8 days on average, up to 26** (275 legs). That is because stations
   sit at a fixed angle around their planet (`CelestialMechanics::get_station_position`, `theta_rad` is
   constant), so the Earth L1 Terminal at 1,500 km never goes round Earth and the only clock is the Moon's
   27-day period. A real station in that orbit comes round every ~2 hours, so a departure window is at most
   one parking orbit away. This also delays the real ships that fly the hop, not only the probes. NTR
   shuttle: 16.1 days = 5.0 flight + 11.1 wait.
2. **Plasma probes plan without the riders' time.** The probe's passenger options call
   `plan_with_return_fuel(destination, load_kg)` with no rider time, so the spiral planner trades time
   against fuel at the ship's own cost and picks slow spirals (factor up to 10× the shortest). The reference
   fares are planned with `berths × fill_ref × person-day` added, and dispatch adds the riders' time too.
   With the same term in the probe (experiment), mean plasma times fell 15-40% (plasma freighter 67 → 46 d,
   bulk tanker 60 → 36 d, courier 32 → 27 d).
3. **Plasma probes are limited by the yard's fuel on sale today.** A probe ship starts with empty tanks and
   can only buy what the yard sells now; the spiral then stretches to fit that budget: 384 kg on sale →
   231 days, 11-15 t → 146-179 days, the 410-day plasma liner burned 222 kg. Shielded cabins (water wall) are
   also slow at 30 t on sale, since the hop needs no shielding but carries it.

**What it changes.** In the 400-day experiment, fixing 2 alone changed no purchase. The times are
not what keeps Luna below the hurdle: at a 5.6-day trip (no wait) the Fast Courier with a cabin was valued at
−1414%/yr, because one way burns about 27 t of propellant for 12 berths at an 86 cr fare. So points 1-3 are
needed for correct times, but the fare and the shuttle economics (plan points 1-2) decide Luna.

Proposed fixes (for the user to choose):
- 1: in the Kepler planet-system branch, when an end is a station in low orbit (period well below a day),
  drop the Moon-phase wait (or limit it to one parking orbit). Station drawing stays at its fixed angle.
- 2: pass the riders' time (`berths × fill_ref × person-day`, as the reference fares) in the probe's
  passenger plans.
- 3: value a probe's passenger legs on a full tank budget (the sustained route), not today's fuel on sale.

## Plan (the user's points, in order of work)

1. **Why fares do not rise when people give up (A).** The traveller's budget is `reference cost × band
   multiple` (`passenger_bands`, Passengers.cpp:138). The fare is the lower of the budget and the larger of
   the hedonic fare, the clearing fare and the carrying cost. A clearing fare above the hedonic one happens only
   when more people wait than there are berths, and the purchase valuation (`expected_passenger_revenue`) uses
   expected fares. So a long queue never raises the fare that the purchase review sees, and no ship flies to let
   the clearing fare show. Look at:
   - the band multiples for short hops (an 86 cr budget leaves no room);
   - letting the purchase valuation count the queue that gave up (willing-to-pay people, at their budgets);
   - whether the reference fare for short hops should come from a ship that suits the hop.
2. **Tech review of the Luna shuttle.** Check A2 first. Then compare the costs of a cheap short-hop ship:
   chemical or NTR, small crew, no shielding, cheap hull, quick turnaround. Its capital per seat-day against
   the Passenger Shuttle (268,600 cr, crew 3, 40 berths, 250 t propellant, 10.3 km/s; tank variants 50-200%).
   The hop needs about 4 km/s each way, so 50% tanks or a smaller hull may do.
3. **Would a larger liner make Luna pay?** Run the probe for `large_plasma_liner` (200 berths, 1.24 M cr) on
   Earth L1 ↔ Luna. The flow is about 3.2 people a day out and 1.2 back, so a 200-berth ship on a 30-day
   cycle fills about half its seats one way. Use the trace above, or a forced purchase in a test run.
4. **The other routes (B, C).** After 1-3, rerun the route audit. For B, test a scheduled liner on Earth ↔
   Mars and Earth ↔ Venus: do those routes repay one? For C, check whether the cheaper ships from point 2
   apply.
5. **Mercury (D):** the npz atlas update, which adds reference fares Mercury → Mars and Mercury → Titan.

Tools: `--econ-audit` (route lines), `SPACETRAINS_TRACE_INVESTMENT=1` (every purchase candidate's valuation),
`tools/benchmark.py` (4 starts) for any change to the economy.
