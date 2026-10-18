# Session Log 2026-10-11: Moon Paths, Ship Structures Tier 1, Visuals Proposal

The user asked for this as one long autonomous session (until ~21:00) and asked me to make design choices myself.
Every choice is listed here for review.

## Step 0: Branch
- `origin/main` (2f8a825, the merge of PR #12) contains `c67522a`, the old branch tip; the trees are identical.
- New branch `feature/bugfixes_ship_structures_and_trajectories` from `origin/main`.
- Upstream tracking removed: `git switch -c` had set it to `origin/main`, so a bare `git push` would have gone to
  main. Nothing pushed.
- "origin/master" in the request: GitHub has no master, so I took `main`.

## Step 1: Wobbly moon paths (diagnosed, not fixed)
(the diagnosis doc was removed once the wobble was resolved).
- **Cause:** moon legs co-move with the planet (0.4-1.4 km/s relative to Jupiter) but meet the moon at its
  orbital speed (10.9 km/s at Ganymede). In the moon's frame the approach loops at the moon's period.
- **Latent second cause:** Kepler wait samples at moon stations alias.
- The hooked orange line in the Jupiter screenshot is Jupiter's own orbit ring in perspective, not a path.
- Recommended fix F1, an in-system leg at moon stations, needs one decision from you: charge only time
  (recommended first) or also Δv.

## Step 2: Ship structures, tier 1
Decisions:
1. **Decompose, don't change.** Parts add up exactly to the existing classes. The rounding remainder goes to the
   largest part (≤ 1 kg, ≤ 50 cr). The economy is untouched; a 365-day audit is byte-identical to `origin/main`.
2. **Habitat split from NASA's Deep Space Habitat** (500 days, 4 crew), not invented. Crew systems are split
   into "crew quarters" and "galley, wardroom, medical, exercise" by the DSH item list. The parts are:
   - pressure hull
   - shielding
   - life support
   - cabin thermal control
   - power and avionics
   - crew quarters
   - common rooms
   - airlock and EVA
3. **Radiator temperature is derived, not chosen:** 594 K for the Set A drive, from 3 kg/m² panels and
   emissivity 0.85.
4. **Liquid-core core temperature is derived** from Isp ∝ √T (about 5,900 K). Solid core is 2,700 K.
5. **Plasma propellant is water** (293 K, 1,000 kg/m³), one of linopt's options. Argon would need cold storage.
6. **Hold volume is 1 m³ per unit** (game choice).
7. **The engine's `power_w` is jet power** (what α uses). Waste heat goes in `heat_w`. A habitat's `power_w` is
   its electrical draw (DSH: 3.2 kW per crew member).
8. **`ship_components.csv` is required** and validated by the loader, like every other content file. A
   ctest (`ship_class_builder`) fails when the CSVs drift from the builder.
9. **Seam for tier 2:** `Simulation::ship_design(ship)`, a pure refactor of 14 lookups.
10. **The designs go to their own bridge file** (`.designs` with a sequence file), not into every snapshot.
    That keeps snapshots small, and tier 2 can rewrite it when fits change.
11. **Live engine state:**
    - Nuclear-thermal burns: for display, each lasts half the mission's propellant at the engine's mass flow.
      The planners stay impulsive.
    - Plasma ships "thrust" the whole transit.
    - Idle radiators show the sunlit equilibrium temperature, 278.6 K/√r.
12. **UI is text only.** It is a collapsible "Ship systems" section at the end of the ship inspector, open by
    default, remembered across selections. Graphics are the step 4 proposal.
13. **Bug fixed on the way:** ship models compared `class_id`, so tank variants (`tanker_tank150` …) lost
    their hull's model. They now use `hull_id`.
14. **ShotTour** now shoots the systems section for one plasma and one nuclear-thermal ship
    (`ship_systems_*.png`).

Checks:
- all four test suites pass (`ship_class_builder` is new);
- the 365-day `--econ-audit` and snapshot match `origin/main` except for the four new snapshot fields;
- the Godot screenshots render correctly.

## Step 3: Tier 2 design
`docs/plans/ship_internals.md`: designs derived in C++ (validated by re-deriving the tank variants), a
module catalogue with slots, refits through `consider_refit`, condition, and ship events on their own RNG stream.
Damage acts through physics: less radiator capacity → less drive power → lower κ.

## Step 4: Visuals proposal
Proposals A-D. The user's answers (2026-10-11): top view and false colour; A (inspector schematic) and D (map
hints) are built, described in `docs/modules/ui_observer.md`. B (shipyard panel) and C (3D close-up) are
archived, not planned: `docs/archive/ship_visuals_shelved.md`.

## Follow-up the same day (user feedback)
- **Moon paths:** the user sees the wobble in the heliocentric frame. Cause: Godot's linear r/θ interpolation
  between samples. Fixed with a cubic Hermite; the user confirmed it, so the
  diagnosis doc and the moon-frame ideas (F1, F2) were dropped.
- **Plans cleanup:** 11 finished plans were removed; their mechanisms are condensed in
  `docs/modules/ships_and_fleet.md` and `economy.md`. The tech review is now `docs/ship_technology.md`.
- **A built (decisions):**
  - The schematic sits under the engine line. The parts list is folded below it, closed by default.
  - Habitat systems are drawn as bands by mass inside a frame for the pressure hull and shielding.
  - Hover shows the spec line in the widget, not a popup tooltip, because the inspector rebuilds twice a second.
    For the same reason one schematic instance is kept across rebuilds.
  - Cargo uses one colour per commodity (`UiTheme.COMMODITY_COLORS`).
- **D built (decisions):**
  - The bridge sends `engine_burn_s`, so the map glow follows each burn's real length (the old window was a
    fixed 6 h).
  - Glow size is 0.45 · log₁₀(jet power / 100 kW) model units, clamped to 0.5-2.4.
  - Wings show false colour only while thrusting, and the old blue panels otherwise.
- **Bug fixed on the way:** both radiator wings of the plasma map model were named "wing". Godot renamed the
  second, which then lost its material and drew untextured. They are now `wing_left` and `wing_right`.
