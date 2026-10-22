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

### A2 fixes built (2026-10-22, user approved all three)

- **1. No Moon wait** (`TrajectoryPlanner.cpp`, Kepler planet-system branch): when both stations' parking
  orbits take under a day (LEO 90 min; every Earth and Moon station), a ship leaves at once. Earth ↔ Luna by
  NTR: 5.0 days instead of 5.0 + 15.8 waiting.
- **2. Riders' time in the probe:** fleet investment's passenger plans add `berths × fill_ref × person-day`
  (as the reference fares).
- **3. Full tank in the probe:** those plans may buy a whole tank (`full_tank` in `plan_to` /
  `plan_with_return_fuel`), return fuel included, instead of today's fuel on sale.
- **Contract loops wait in port.** Fix 1 stopped rotation contracts: a loop carries `need per day × cycle` per
  lap and pays fuel per lap, and the Moon wait had been stretching Earth–Moon loops to ~29 days. Without it a
  9.8-day loop was worth −20 cr/day (the 28.8-day one +249), and no contract was signed in 400 days. A loop may
  now wait at its first stop before each lap (0, 7, 14, 28 or 56 days, the best value; the wait costs the ship's
  time). Saved as before: the wait is the cycle minus the legs (no save version change). Loops are worth
  100-390 cr/day again, but are signed less often: they must earn half the best spot score, and quick Luna spot
  trips score in the thousands. First contract on day 180 (was 71); the contract save/load test now runs
  200 + 90 days.
- Reference fares do not change (the reference carriers are plasma ships). `reference_fares.csv` differs from
  what the current code writes in 53 of 68 rows (interplanetary, not Earth ↔ Luna), both before and after these
  fixes: an older drift, left for the user.

**Benchmark, 8 starts × 1,460 days** (baseline = this branch before the fixes, same tool and machine):

| Run | Unmet | Last fifth | Profitable | Ships | Money |
|---|---|---|---|---|---|
| baseline | 1.15 ± 0.49% | 1.79 ± 0.76% | 110.3 | 142.9 | −17 ± 8% |
| fixes 1-3 only (4 starts) | 1.8 ± 1.1% | 2.6 ± 3.1% | 115.2 | 144.8 | −15 ± 18% |
| fixes 1-3 + loop waits | 1.07 ± 0.76% | 2.33 ± 2.24% | 113.5 | 144.9 | −21 ± 14% |

The last fifth's mean comes from start 0 (7.6%; without it 1.57% against 1.76%): Earth-system shortages (LEO
food, Earth L1 electronics and machinery), not Luna. Earlier versions also had single starts at 5-9%
(passengers_log). Luna passengers do not change (start 0: Earth L1 → Luna 3,344 gave up, was 2,998): the times
were not the limit; the fare is (plan point 1).

### Reference fares regenerated (2026-10-22, user)

`reference_fares.csv` rewritten from the current code: 53 of 68 fares changed (most 0-5%, Mercury +8-12%),
Earth ↔ Luna unchanged; Mercury → Ceres and Mercury → Ganymede dropped out (no feasible reference trip: the
Mercury atlas gap, point D). Benchmark, 8 starts × 1,460 days, with the A2 fixes: unmet 0.86 ± 0.34%, last
fifth 1.98 ± 0.76% (start 0: 3.8%), 112.3 profitable of 145.8 ships, money −23 ± 20% (before the A2 fixes:
1.15%, 1.79%, 110.3 / 142.9, −17%).

### Point 3 checked: a larger liner does not make Luna pay (2026-10-22)

Purchase valuations of the Luna passenger runs (730 days, start 0, temporary trace of each part; per cycle):

| Ship (yard → destination) | Cycle | People out / back | Fares | Fuel (2 legs) | Profit/d − running/d | Return |
|---|---|---|---|---|---|---|
| Large Plasma Liner, Earth L1 → Luna | 35.9 d | 186 / 89 | 14,183 cr | 61,518 cr | −1,341 − 326 | −50%/yr |
| Large Plasma Liner, Luna → Earth L1 | 16.9 d | 38 / 84 | 8,078 cr | 9,840 cr | −118 − 327 | −13%/yr |
| Plasma Liner, Earth L1 → Luna | 44.2 d | 40 / 40 | 17,226 cr | 12,066 cr | 110 − 78 | +4%/yr |
| Plasma Liner, Luna → Earth L1 | 19.9 d | 40 / 40 | 10,096 cr | 1,620 cr | 417 − 78 | **+46%/yr** |
| Passenger Shuttle (NTR), Earth L1 → Luna | 10.0 d | 40 / 25 | 4,937 cr | 136,020 cr | −13,122 − 70 | −1,793%/yr |

- **Fuel decides it, not size.** Earth makes no fuel: its order price runs 28-77 cr/u and the going rate 40-45,
  against a reference of 9.7 (base 8). A probe ship starts with empty tanks, so it buys its load at Earth's
  price, and the valuation charges the way back at the same price (`2 × fuel_cost`), although a ship refuels
  at Luna, a fuel factory. From Luna the same liner pays a sixth of the fuel.
- **The fare ignores it too.** The reference fare (86 cr Earth L1 ↔ Luna) is planned at the base fuel price,
  so it covers a fraction of what an Earth departure costs.
- **A bigger ship does not help.** Fuel per seat is about the same (885 t dry for 200 berths against 198 t
  for 40), the Large Liner's capital is 280 cr/day, and the flow (about 3.2 people a day out, 1.2 back) fills
  200 berths only one way.
- **NTR is out on this hop:** 68,000 cr of hydrogen per leg (73 t at Earth prices) for 40 seats.
- **The Plasma Liner based at Luna clears the 30% hurdle (+46%/yr) and is still not bought:** above half the
  fleet cap, candidates are ranked by profit per day (339 cr/day), and cargo runs earn more.

Options (user to choose):
- **A. Fuel in the valuation and the fare (recommended):** price the way back at the fuel the ship would buy at
  the destination (Luna: cheap), and plan reference fares at each origin's fuel price (the going rate) rather
  than the base price. Earth departures then cost what they cost, and Luna-based liners show their advantage.
- **B. A scheduled Luna liner** like the IC Outer Loop liners (`liners.csv`, outside the fleet cap): a Plasma
  Liner based at Luna on Luna → Earth L1 → Luna (or with LEO). Quick to add, but at today's fares it is
  a service someone subsidises.
- **C. Let passenger carriers compete by return, not profit per day,** under the cap, so the +46% Luna liner
  is bought. Changes fleet composition across the board.

### Option A built: fuel at real prices (2026-10-22, user)

- **Fares follow fuel prices.** `reference_fares.csv` gains `fuel_out_kg` and `fuel_back_kg`, the propellant
  per seat in the fare (with the carrier margin; the return share burned leaving the destination).
  `live_fare_cr` adds them at today's port prices above base (`port_fuel_price` = max(base, station price)).
  Every fare reader uses it (traveller budgets, hedonic fares, rotation tours, the sponsor's credit check),
  except the choice of Earth gateway, which stays on the static fare. Earth L1 → Luna at Earth fuel ~42 cr/u:
  86 → ~215 cr; Luna → Earth L1 ~150 cr.
- **The way back at the destination's fuel price.** Probe passenger runs record `return_fuel_kg` (−1 when the
  ship carries its return fuel); the valuation charges it at the destination's price instead of
  `2 × fuel_cost`. Save version 17.

**Benchmark, 8 starts × 1,460 days** (same machine, runs one after another):

| Run | Unmet | Last fifth | Profitable | Ships | Money | Run time |
|---|---|---|---|---|---|---|
| fares regenerated (before A) | 0.86 ± 0.34% | 1.98 ± 0.76% | 112.3 | 145.8 | −23 ± 20% | 116 s |
| option A | 1.47 ± 0.60% | 2.51 ± 0.66% | 118.6 | 146.1 | −6 ± 24% | 118 s |
| option A, contracts off | 3.06 ± 1.29% | 3.70 ± 1.09% | 119.1 | 147.1 | +4 ± 17% | 110 s |

- **Luna passengers board** (route audit, starts 0 / 90): Earth L1 → Luna gave up 2,942 → 1,304 and
  2,976 → 851, boarded 1,560 → 1,787 and 1,352 → 1,606. Fewer post (4,874 → 3,252): the fare is higher.
- **Cargo pays for it under the cap.** Passenger carriers now pay, so the fleet review buys more of them
  (start 90: legs by ships with berths 268 → 800). The shortages that grow are Mars fuel, food and oxygen and
  Earth L1 water. Unmet 0.86 → 1.47%.
- **Contracts stay on** (user's rule): without them unmet doubles (3.06%) for a 7% faster game.

### Luna ferry: design study and trial (2026-10-22, user)

**Study** (scratchpad model on the builder's masses and prices and the game's spiral and cost rules; fuel at
Earth 42 cr/u, Luna 10; flows 3.2 people/day out, 1.2 back; each leg at its cheapest trip time):

| Design | Dry | Price | W/kg | Out / back | Fuel per person | Cost per person |
|---|---|---|---|---|---|---|
| Plasma Liner (40 berths) | 198 t | 268k | 504 | 16.7 / 8.7 d | 46 cr | 96 cr |
| Large Plasma Liner, 50% tanks (200) | 877 t | 1.22 M | 570 | 17.8 / 9.7 d | 91 cr | 190 cr (cannot fill) |
| 30 berths, 2 crew, 65-70 MW, 2.5 t cabins | 126-128 t | 169-175k | 514-548 | 16 / 8 d | 36 cr | 75-76 cr |
| same, 1.5 t cabins (estimate) | 88 t | 120k | 512 | 15 / 7.5 d | 30 cr | 61 cr |
| same, 1.0 t cabins (estimate) | 69 t | 95k | 509 | 14 / 7 d | 26 cr | 54 cr |

- **Slower is not cheaper beyond the optimum.** Passengers' time is cheap (0.5 cr/day wage, ~0.8 cr/day dose),
  so the cheapest trip is set by fuel against the ship's capital and crew. Out of Earth it is about twice the
  reference trip (16-17 days against 9), back from Luna about the same (8 days). Lower-power ferries (260 W/kg)
  fly 24 days and cost about as much: the fuel saved goes to capital.
- **Small ships fit the flow** (3 people a day): 30-40 berths; the 200-berth liner flies half empty.
- **The big lever is cabin mass:** 2.5 t per berth is the long-haul cabin (linopt); a short-hop cabin would
  cut the cost per person by 20-30% more. Not built: a new mass assumption for the user to decide.
- Power-to-mass capped at the liners' level (≤ 570 W/kg, tech era); faster designs above 900 W/kg were dropped.

**Trial:** `luna_ferry` (Luna Ferry: 30 berths, 2 crew, long habitat like every class, 65 MW, 12 t tanks;
135 t, 178k cr, 481 W/kg) added through `build_ship_classes.py`. With the short habitat it became the cheapest
reference carrier on 150-200-day routes (Earth → Venus, Mars → Earth), which a two-week habitat should not fly,
so it got the long one; it is then no route's reference carrier. Reference prices and fares regenerated
(prices had drifted from the code in 33 rows; the ferry changes none).

| Run (8 starts × 1,460 days) | Unmet | Last fifth | Profitable | Ships | Money |
|---|---|---|---|---|---|
| option A | 1.47 ± 0.60% | 2.51 ± 0.66% | 118.6 | 146.1 | −6 ± 24% |
| A + regenerated prices | 1.68 ± 0.40% | 2.41 ± 0.59% | 117.3 | 143.6 | +8 ± 21% |
| A + prices + Luna Ferry | 1.80 ± 0.37% | 2.92 ± 1.03% | 119.5 | 146.4 | −1 ± 26% |

(Delivered price ÷ reference is not comparable across the price regeneration.) The ferry is bought (4-6 per
run, valued 90-117%/yr on Luna → Earth L1 at Luna) but Luna service does not measurably change: Earth L1 → Luna
gave up 1,304 / 851 without it, 1,433 / 1,181 with it (starts 0 / 90). Within noise overall.

**What the runs show instead:** about 45 of ~145 ships now carry passengers (12-15 Large Plasma Liners on
interplanetary routes, where few berths against long queues clear near the top band's budget), and Mars fuel
is the largest shortage (25-47% unmet at Mars). Under the 130-ship cap, passengers and cargo compete for hulls.

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
