# Plan: statistics views, population charts, inspector redesign, porkchop plots

**Status 2026-10-27: steps 0-5 built on `feature/statistics_views`, for the user's review.** The design as built is in
`docs/modules/ui_observer.md` (inspector, People, Flows, Routes, overlay), `economy.md` (flow record) and
`trajectory.md` (porkchop plots). Departures from the plan: no `origin_station_id` on cargo lots (every lot is sold at
the end of its mission, so the mission's origin is the lot's); the goods balance derives "made net of use" from stock
snapshots instead of counting production in the economy step; lanes are judged by propellant per unit delivered; the
"who pays whom" drill-down to accounts is left (the journal keeps sectors only); no disk cache for porkchops.

## Context

The roadmap's "Later" block (docs/roadmap.md) asks for (a) more views in the statistics screen, first the flows of
goods from station to station, and (b) porkchop plots for both ship classes. The user adds: (c) visualise the
population data the economy already produces (pies etc.), and (d) assess the right-hand inspector panels for ships
and stations and unify/improve them. Porkchops must be cheap: precomputed / incrementally updated, never recomputed
from scratch on every request.

User choices (2026-10-27): porkchops in a new Statistics "Routes" tab **and** a small "next windows" line in the
station inspector; inspector redesign = shared sections + KPI strip (one scrolling panel, no tabs); build order
widgets → inspector → people → flows → porkchops.

Everything here is display-only. Invariant for every step: a 365-day `--econ-audit` stays byte-identical, the
save JSON diff stays clean (except new saved fields for the delivery record), drift 0.

## Step 0 — branch

`git switch -c feature/statistics_views` from local `main` at `cdf087b` (1 ahead of origin; not pushed).

## Assessment of the inspector today (`godot/scripts/ui/InspectorPanel.gd`, 927 lines)

Findings from the code (to confirm with screenshots before the redesign, see Verification):

1. **One long undifferentiated scroll.** A station stacks ~14 blocks (header, growth, passengers, events, factory,
   cash, economy lines, emergencies, storage, market, production, systems, orders, inbound, docked, money). Section
   titles are 12 px caps in ACCENT, the same size as the body text, so they don't separate the blocks.
2. **Packed number strings.** Orders print two lines per good like
   `kind · 0.42 u/day · lead 31 d · every 40 ± 9 d · up to 120 u (extra 30) · worth ≤ 88 · 5 deliveries`; the ship
   ledger is one `sales +… · cargo −… · fuel −…` line. These have no column headers and can't be compared row to row.
3. **Three ways to show money:** ship "Cash · lifetime profit" + ledger line; station "MONEY SINCE START" with other
   keys; the statistics screen's flow tables. Credits are formatted with both `UiTheme.format_credits` and `%+.0f cr`.
4. **Two ways to draw rows:** ship gauges (`_add_gauge`), station market rows as hand-built HBoxes with fixed min
   widths; the target stock is only in a tooltip; the cover column mixes days and a `%✗` share.
5. **Inconsistent folding:** ship systems and station systems fold; orders, inbound ships and the market don't.
   Fold state is a few booleans, not per section.
6. **No at-a-glance state.** The ship's key facts (where/ETA, fuel %, hold %, profit) and the station's (people,
   cash, unmet share, storage %) are spread through the scroll.
7. Most text is TEXT_DIM at 11–12 px; dim dominates, which makes the warnings (WARN/ALERT) the only things that
   stand out.

## Step 1 — shared UI widgets (`godot/scripts/ui/`)

New, reused by every later step:
- `UiWidgets.gd` (static helpers, moved out of `StatisticsPanel.gd`/`InspectorPanel.gd`): `table(headers, rows,
  widths)` (from `StatisticsPanel._table`), `section(title, id, summary)` (a heading with a fold arrow and a one-line
  summary when folded; fold state kept in a dictionary by `kind/id`), `kpi_strip([{label, value, color}])`, `gauge`
  (from `InspectorPanel._add_gauge`), `credits()` (one formatter, wraps `UiTheme.format_credits`).
- `DonutChart.gd`: `set_data(title, slices[{name, value, color}])`, legend with values and shares, hover highlights
  a slice and its legend row. Drawn with `draw_polygon` arcs like `LineChart.gd`.
- `StackedBar.gd`: horizontal bars of parts per row (comparisons across stations, where pies are poor), hover tip.
- `Heatmap.gd`: a grid of coloured cells with axes, a colour legend, a hover readout, optional contour lines and
  point markers (used by the porkchop and by the flow matrix as a coloured table option).
- Colours: categorical palette in `UiTheme.gd` (extend `SERIES_COLORS`; keep `COMMODITY_COLORS` for goods);
  sequential scale for heatmaps (reuse `ThermalPalette.gd`'s approach, a separate perceptual palette).

## Step 2 — inspector redesign (`InspectorPanel.gd`)

Same panel, same data from the snapshot; only presentation changes.
- **KPI strip** at the top under the header. Ship: status + ETA, propellant %, hold %, lifetime profit.
  Station: people, cash, unmet share (30 d), storage %.
- **Same section order and widgets for both kinds:** Status → Cargo/Market → People (crew / residents) →
  Systems → Money → Ships (station only). Every section folds and remembers it per kind.
- **Market** as a table (good, stock bar to target with target shown, price + trend, rate/day, cover days,
  short %) via `UiWidgets.table` with a bar cell.
- **Orders** as one table row per good (bid, open units, ref, going, short in) with the learnt details (use,
  lead, interval ± spread, up-to, ceiling, deliveries) in the row's tooltip or a folded sub-table.
- **Money block** shared by ship and station: income vs expense by category since the start as two small
  stacked bars + a net line, from the existing `ledger` dictionaries (ship `cargo_revenue/fuel/wages/...`, station
  `household_sales/dividends/subsidies/taxes`) and the station's profit before the faction.
- Inbound/docked ships as a small table (ship, cargo, ETA, contract value); click selects the ship (as the fleet
  table in `StatisticsPanel._ship_button` already does).
- Font: section titles 13 px bold, body 12 px TEXT_PRIMARY, dim only for secondary notes.
- Keep: the schematic instance kept across refreshes (`_park_schematic`), no rebuild while the mouse is held
  (`_pointer_held_here`), wheel capture, no keyboard focus.
- Body inspector gets the same header/KPI/sections (orbit, moons, stations).

## Step 3 — population charts

Data already exists: station systems (`Simulation::station_systems_json`, `StationSystems.cpp`: housing,
residents, workers, employed, dependants, spacers, visitors, waiting, jobs_by_kind, works, needs by tier), stats
(`Statistics.cpp`: population, workers, jobs, wage per station; colony people, people_history, wage p10/p50/p90).
- Stats file: add `systems` for every station (display-only, cheap: 9 stations) so the Statistics screen can show
  all of them; written only while the screen is open, as today.
- Statistics: rename tab "Residents" → **"People"** and add:
  - a donut of people across the system by station + colonies (Earth noted separately, it dwarfs the rest);
  - per-station stacked bars: employed workers / idle / dependants / spacers / visitors, housing as a marker;
  - jobs by kind (stacked bar per station; donut for the selected one);
  - needs coverage per station (life / upkeep / everyday: made here vs imported);
  - population over time (stacked area via `LineChart` extended, from `JournalSample.stations` and
    `colony_people`), wage p10/p50/p90 for colonies as a small range plot.
- Station inspector, People section: donut of the station's residents and a jobs-by-kind bar (replaces the text
  lists in `_add_station_systems`).

## Step 4 — goods flows from station to station

- **Delivery record** (C++): add `origin_station_id` to `domain::CargoLot` (`src/domain/State.hpp`), set where
  the lot is loaded; at sale (`Contracts.cpp` / `ShipOperations.cpp`, where the Cargo money is booked) record
  `{from, to, good, units, value, day}`. Earth and colonies' surface trade (`SurfaceSales/SurfacePurchases`) is
  recorded with the colony/Earth as from/to. Passengers by kind (settlers, crew rotation, visitors) recorded the
  same way at disembarkation (`Passengers.cpp`).
- Keep it like the money journal (`MoneyJournal.hpp`): lifetime and running-period matrices
  (nodes × nodes × goods, nodes = stations + colonies + Earth ≈ 13), closed into `JournalSample` each 30 days.
  Saved in `SimulationPersistence.cpp`; older saves load with empty history.
- Stats JSON: the matrices for the chosen period; a per-station goods balance (made, used, imported, exported,
  stock change) from existing production/consumption counters plus the record; transport work per lane
  (tonne-days and propellant) from missions at arrival.
- Statistics: new **"Flows"** tab — from × to table (units or value, one good or all, period switch), coloured
  cells (Heatmap), the passenger table, the per-station balance, lane costs; and a "who pays whom" drill-down from
  the existing 6 × 6 sector matrix to accounts. Map overlay (toggle key, e.g. G): lines between stations, width
  by flow over the chosen period.

## Step 5 — porkchop plots (cheap by construction)

**What is plotted.** For an ordered pair of primaries (heliocentric routes only; stations around the same planet
have no windows), departure date (x) against flight time (y).
- Kepler (nuclear-thermal): Δv of each cell = exactly what the planner charges, from
  `KeplerTrajectoryPlanner::lambert_row` (make a public pure entry point). Coloured per class at display time:
  propellant fraction by the rocket equation with the class's exhaust velocity (`thermal_exhaust_velocity_mps`),
  with contours where a full hold / empty hull runs out of Δv.
- Variable-Isp: the effort J = ∫a²dt of the minimum-effort fixed-time rendezvous, from
  `variable_isp::general::solve_transfer` (GeneralTransfer.hpp; median 0.4–0.7 ms), cells diving under
  `kMinPerihelionM` marked out. J does not depend on the ship, so one grid serves every class; per class at display
  the propellant comes from J and the class's power and masses with the I = 1/m_f − 1/m0 formula
  (memory: feedback_variableisp_propellant), with a contour where it exceeds the tank.
- Overlay: ships now flying or waiting on the route as dots at (their departure, their transit time), coloured by
  class kind.

**Why it is cheap.**
- Both grids are geometry only: one per body pair, shared by all classes, payloads and fuel loads.
- **Aligned columns, computed once:** departure columns sit at absolute multiples of a step per pair (window
  span / 96, span = min(synodic period, 730 d) as the planner uses); flight times on a fixed per-pair axis
  (e.g. 0.3–2.0 × Hohmann time, 48 rows). A column is a pure function of (pair, departure); as game time advances
  only the new columns at the right edge are computed, the rest are reused (the same scheme as the planner's
  `lambert_rows_` cache).
- **Lazy and background:** the bridge computes only pairs that were requested (the open Routes tab, the selected
  station's destinations), on one low-priority worker thread separate from the simulation's ThreadPool, so it
  never slows a tick; columns are posted to the UI as they finish (the plot fills in).
- **Warm starts:** variable-Isp cells are solved along a column from the previous flight time
  (`continue_in_time`), and a new column from its left neighbour, so most solves are a few Newton steps.
- Kepler: 96 × 48 Lambert solves per pair, well under 0.1 s. Variable-Isp: ~4,600 cells per pair ≈ 2–3 s of one
  core the first time, then ~50 cells (one column) per step of game time.
- **Disk cache** (optional, measured first): columns keyed by pair + departure in a file next to the atlas cache
  dir, so a restart or a load does not recompute. Not part of the save.
- Bounded memory: LRU over pairs.

**Plumbing.** Command file gets `porkchop_request: <origin_body>,<dest_body>` (like `stats_open`); the bridge
writes `<snapshot>.porkchop` (+ `.seq`) with the columns it has. Pure: never read by the simulation (audit
byte-identical).

**UI.** Statistics **"Routes"** tab: origin and destination pickers, class picker (or "Kepler Δv" / "variable-Isp
effort" raw), the heatmap with "now" marked, hover readout (departure date, flight time, Δv or propellant, arrival
date), ships overlaid. Station inspector: one line per heliocentric destination "next good window in N d
(T d flight)" from the cached grid (the best cell per departure), button opens the Routes tab on that pair.

## Files

- New: `godot/scripts/ui/UiWidgets.gd`, `DonutChart.gd`, `StackedBar.gd`, `Heatmap.gd`;
  `src/trajectory/Porkchop.{hpp,cpp}` (grid, column cache, worker); tests `tests/porkchop_test.cpp`,
  delivery-record tests in `tests/economy_test`.
- Changed: `InspectorPanel.gd`, `StatisticsPanel.gd`, `LineChart.gd` (stacked area), `UiTheme.gd`, `Main.gd`
  (read `.porkchop`, map flow lines); `src/bridge/main.cpp` (request + side file);
  `src/simulation/Statistics.cpp` (systems, flows, balances); `MoneyJournal.hpp` / a new `DeliveryRecord` beside it;
  `State.hpp` (`CargoLot::origin_station_id`); `Contracts.cpp`, `ShipOperations.cpp`, `Passengers.cpp` (record);
  `SimulationPersistence.cpp`; `TrajectoryPlanner.hpp` (public `lambert_row`).
- Docs: `docs/modules/ui_observer.md`, `economy.md` (delivery record), `trajectory.md` (porkchop), roadmap
  "Current Plan" updated (and its stale clean-up status line fixed).

## Verification

- Before step 2: screenshots of the current ship and station inspector with a shot tour (warn the user first, a
  window opens; parse-check scripts; short timeout) to confirm the assessment; again after, side by side.
- Each GDScript change: `godot4 --headless --path godot --check-only --script res://scripts/<file>.gd`.
- C++: build + ctest; new porkchop tests (a Kepler cell equals the planner's Lambert cell; a column computed
  twice, or after time advanced, is identical; variable-Isp J of a cell matches `spacetrains_visp_bench` set A
  within tolerance; propellant from J matches the planner's fit for the same transfer).
- Economy invariance: 365-day `--econ-audit` byte-identical before/after each C++ step; save → load → save JSON
  diff clean (delivery record included); `tools/benchmark.py` not needed (no economy change) but run once at the
  end to confirm.
- Cost: time the porkchop worker per pair and per new column; check `--shot-smooth` shows no extra stalled frames
  at 5 d/s with the Routes tab open; `pgrep` the bridge after Godot runs.
- Commit each step on the branch; no push unless asked.
