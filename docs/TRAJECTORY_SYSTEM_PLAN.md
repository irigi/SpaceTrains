# Trajectory System Design Plan

## Status: PLANNING (not yet implemented)

---

## 1. Current System Summary

Ships currently move in **straight lines** with smoothstep interpolation:

- **State machine:** `docked → launching → traveling → arriving → docking → docked`
- **Position during travel:** `lerp(origin, destination, smoothstep(t))` — a straight line with ease-in/out
- **Destination tracking:** Recalculated every tick (destination station orbits with its planet)
- **Fuel:** Consumed linearly: `dt * 0.01 * base_speed` — no physics basis
- **Velocity field:** Exists in `ShipData` but is **unused**
- **Key files:**
  - `sim/systems/ShipMovementSystem.gd` — 5-state movement logic (129 lines)
  - `sim/WorldState.gd:199-305` — `ShipData` class
  - `sim/Simulation.gd` — Tick loop, AU_SCALE=50, TICK_DELTA=0.2 sim-minutes

---

## 2. Design Goals

1. **Generic trajectory abstraction** — Different trajectory types (linear, Keplerian, future low-thrust) share a common interface
2. **Keplerian ellipses** as the first real trajectory type — ships follow heliocentric transfer orbits
3. **Moving target interception** — Periodically recompute trajectory when target position changes
4. **Fuel tracking with physical basis** — Delta-v budget, departure/arrival burns visible in UI
5. **Performance** — All computations must be fast enough for 40+ ships at 50× speed (400 ticks/frame)

---

## 3. Architecture Overview

### 3.1 New Files

```
sim/trajectory/
    Trajectory.gd            # Base class (abstract interface)
    KeplerianTrajectory.gd   # Heliocentric elliptical transfer orbit
    LinearTrajectory.gd      # Current straight-line movement (fallback/legacy)
    TrajectoryPlanner.gd     # Solves for trajectory given endpoints + constraints
```

### 3.2 Modified Files

| File | Changes |
|------|---------|
| `sim/WorldState.gd` | Add trajectory fields to `ShipData`; add fuel/delta-v fields |
| `sim/systems/ShipMovementSystem.gd` | Delegate to trajectory objects instead of lerp |
| `sim/systems/MissionSystem.gd` | Check fuel feasibility before dispatching |
| `ui/SelectionPanel.gd` | Show delta-v budget, burn costs, trajectory type |
| `view/SolarSystemRenderer.gd` | Draw elliptical trajectory paths for ships |
| `sim/Simulation.gd` | Minor: expose Sun body reference for trajectory computation |

### 3.3 Unchanged State Machine

Keep the existing 5 states: `docked → launching → traveling → arriving → docking → docked`.
The "traveling" state now delegates position computation to a `Trajectory` object.
Fuel burns happen at state transitions (departure during `launching→traveling`, arrival during `arriving→docking`).

---

## 4. Trajectory Base Class

```
class Trajectory (RefCounted):
    # Core interface
    get_position_at_time(t: float) -> Vector3    # World position at sim_time t
    get_velocity_at_time(t: float) -> Vector3    # Velocity vector at sim_time t
    get_start_time() -> float                     # Sim time when trajectory begins
    get_end_time() -> float                       # Sim time when trajectory ends
    get_duration() -> float                       # end_time - start_time
    is_complete(current_time: float) -> bool      # Has the ship arrived?
    get_progress(current_time: float) -> float    # 0.0 to 1.0

    # Fuel/delta-v info
    get_departure_delta_v() -> float              # Delta-v for departure burn
    get_arrival_delta_v() -> float                # Delta-v for arrival burn
    get_total_delta_v() -> float                  # Sum of all burns

    # Serialization
    to_dict() -> Dictionary
    static from_dict(d: Dictionary) -> Trajectory
```

**Key design choice:** Trajectories are **immutable after construction**. When a moving target shifts enough to require replanning, a **new** trajectory object is created. This avoids complex state mutation and makes serialization simple.

---

## 5. Keplerian Trajectory Details

### 5.1 The Physics

A Keplerian transfer orbit is a heliocentric ellipse connecting the departure point to the arrival point. This is **Lambert's problem**: given two position vectors r₁, r₂ and a transfer time Δt, find the orbit.

**Orbital elements stored:**
- `semi_major_axis` (a) — Size of the ellipse
- `eccentricity` (e) — Shape of the ellipse
- `argument_of_periapsis` (ω) — Orientation in the orbital plane
- `true_anomaly_start` (ν₁) — Angle at departure
- `true_anomaly_end` (ν₂) — Angle at arrival
- `epoch` — Sim time at departure
- `direction` — Prograde (+1) or retrograde (-1) transfer

All orbits are coplanar (Y=0) since all planets currently orbit in the XZ plane, so inclination and longitude of ascending node are not needed.

### 5.2 Lambert Solver (Simplified)

For a game, we use a **universal variable Lambert solver** or a simplified approach:

**Approach: Iterative Lambert via Stumpff functions**
1. Given r₁, r₂ (heliocentric, in AU), and desired transfer time Δt
2. Use universal variable formulation with Stumpff functions c₂(ψ) and c₃(ψ)
3. Newton iteration on ψ to match the desired transfer time
4. Typically converges in 5-8 iterations
5. Extract v₁, v₂ (departure and arrival velocities on the transfer orbit)

**Convergence guarantee:** Use bounded bisection as fallback if Newton diverges (should be rare for reasonable transfers).

**Performance:** One Lambert solve = ~8 iterations × simple trig/sqrt per iteration. With 15 ships traveling, and replanning every ~100 ticks: negligible.

### 5.3 Position Evaluation (Every Tick)

Once the ellipse is defined, getting position at time t:
1. Compute mean anomaly: `M = M₀ + n * (t - epoch)` where `n = sqrt(μ/a³)`
2. Solve Kepler's equation: `M = E - e * sin(E)` for E (eccentric anomaly)
   - Newton's method: 3-4 iterations for e < 0.97
3. Compute true anomaly: `ν = 2 * atan2(sqrt(1+e) * sin(E/2), sqrt(1-e) * cos(E/2))`
4. Compute radius: `r = a * (1 - e * cos(E))`
5. Position in orbital frame, then rotate by ω to heliocentric XZ plane

**Cost per tick per ship: ~10-15 floating point operations** after Kepler solve. Trivially fast.

### 5.4 Delta-V Computation

At departure:
- Ship is co-moving with origin planet (circular orbit velocity v_planet₁)
- Transfer orbit departure velocity v₁ (from Lambert solve)
- `departure_Δv = |v₁ - v_planet₁|`

At arrival:
- Transfer orbit arrival velocity v₂ (from Lambert solve)
- Destination planet circular orbit velocity v_planet₂
- `arrival_Δv = |v₂ - v_planet₂|`

**Planet circular velocity:** `v_circular = sqrt(μ_sun / r)` where r is the planet's orbital radius.

Note: We are NOT simulating escape from a planet's gravity well (no SOI/patched conics). The departure and arrival burns are instantaneous impulses that are assumed to cover both the planet escape/capture and the heliocentric velocity change. This keeps things simple while being visually and physically more realistic than straight lines.

### 5.5 Transfer Time Selection

How to choose Δt (transfer time) for a given journey?

**Simple heuristic for gameplay:**
1. Compute Hohmann transfer time as baseline: `t_hohmann = π * sqrt(a_h³ / μ)` where `a_h = (r₁ + r₂) / 2`
2. Allow the game/AI to choose faster transfers at the cost of more delta-v
3. Ship's `base_speed` stat maps to a "transfer aggressiveness" factor:
   - `transfer_time = t_hohmann * speed_factor` where speed_factor < 1 means faster but costlier

**For initial implementation:** Use Hohmann-like transfer times. Ships with higher `base_speed` get a lower `speed_factor`.

---

## 6. Moving Target Interception

### 6.1 Problem

When ship A targets ship B (or a station on a moving planet), the destination moves during transit. The ship needs to aim where the target **will be**, not where it **is**.

### 6.2 Algorithm

**Iterative convergence (runs at launch + periodically during travel):**

```
function plan_intercept(origin_pos, target_entity, current_time):
    # Initial guess: target's current position
    estimated_arrival_pos = target.get_position_at_time(current_time)

    for iteration in range(5):  # Usually converges in 2-3
        # Solve Lambert for transfer to estimated position
        trajectory = lambert_solve(origin_pos, estimated_arrival_pos, ...)
        estimated_arrival_time = current_time + trajectory.duration

        # Where will target actually be at that time?
        new_estimated_pos = target.get_position_at_time(estimated_arrival_time)

        if distance(new_estimated_pos, estimated_arrival_pos) < threshold:
            break  # Converged
        estimated_arrival_pos = new_estimated_pos

    return trajectory
```

### 6.3 Replanning Policy

- **Check every N ticks** (e.g., every 50 ticks = 10 sim-minutes)
- **Replan only if** the destination has moved more than a threshold from the trajectory endpoint
- **Fuel cost:** Each replanning "costs" a small correction delta-v (deducted from fuel)
- **Smooth transition:** When replanning, the new trajectory starts from the ship's current position and velocity

### 6.4 Target Types

- **Station on planet:** Use `CelestialBodyData.get_position_at_time()` to predict planet position at arrival. Planets are on known analytic orbits so prediction is exact.
- **Another ship:** Use the target ship's current trajectory to predict its future position. Less accurate since the target might also replan, but good enough for a game.

---

## 7. Fuel System Redesign

### 7.1 New Fields on ShipData

```gdscript
# Existing (keep)
var fuel: float = 100.0
var fuel_max: float = 100.0

# New fields
var delta_v_capacity: float = 10.0     # km/s total delta-v when fully fueled
var delta_v_remaining: float = 10.0    # km/s remaining
var specific_impulse: float = 3000.0   # seconds (Isp) — engine efficiency
var dry_mass: float = 50.0             # tonnes (ship without fuel)
var wet_mass: float = 100.0            # tonnes (ship with full fuel)
```

### 7.2 Fuel ↔ Delta-V Relationship

Using the Tsiolkovsky equation: `Δv = Isp * g₀ * ln(m_wet / m_dry)`

For gameplay, we **simplify** to a linear relationship:
- `fuel_fraction = fuel / fuel_max`
- `delta_v_remaining = delta_v_capacity * fuel_fraction`
- When a burn of Δv is performed: `fuel -= (Δv / delta_v_capacity) * fuel_max`

This is not exactly Tsiolkovsky but is close enough for a game and avoids the logarithmic math on every burn calculation. The visual effect is the same: ships have a budget, burns deplete it, and running out means you're stranded.

### 7.3 Fuel Checks

- **Before launch:** `MissionSystem` checks that `delta_v_remaining >= departure_Δv + arrival_Δv + margin`
- **If insufficient:** Ship waits for refueling, or the mission is not assigned
- **During travel:** Course corrections consume small amounts of delta-v
- **On arrival:** Deduct arrival burn fuel
- **At station:** Refueling restores fuel/delta-v to max (as currently implemented)

### 7.4 Stranded Ships

If a ship runs out of fuel mid-transit:
- It continues on its current Keplerian orbit (no more burns possible)
- It becomes "stranded" — a new state or a flag
- Could be rescued by another ship (future feature)
- For now: let it coast; if it happens to pass near a station, dock automatically

---

## 8. UI Changes (SelectionPanel)

### 8.1 Ship Panel Additions

```
[b]Fuel:[/b] 73.2 / 100.0  (73%)
[b]Delta-V:[/b] 7.3 / 10.0 km/s

[b]Current Trajectory:[/b] Keplerian Transfer
  Departure Δv: 1.2 km/s (burned)
  Arrival Δv: 0.8 km/s (pending)
  Transfer time: 4.2 hours
  ETA: 2.1 hours
  Progress: 50%
```

### 8.2 Insufficient Fuel Warning

When a ship can't take a mission due to fuel:
```
[color=red]Insufficient fuel for transfer to Mars Prime
  Required: 3.2 km/s  |  Available: 2.1 km/s[/color]
```

---

## 9. Rendering Changes (SolarSystemRenderer)

### 9.1 Trajectory Visualization

For traveling ships (or the selected ship), render the planned trajectory path:

```gdscript
# Sample the trajectory at 64-128 points
# Draw as LINE_STRIP using ImmediateMesh (same technique as orbit lines)
# Use a distinct color (e.g., ship's faction color with higher alpha)
# Only draw the remaining portion (from current position to destination)
```

**Performance:** Drawing 15 trajectories × 128 vertices = 1920 vertices. Negligible.

### 9.2 Optional Enhancements (Later)

- Draw departure/arrival burn markers (small circles at the burn points)
- Show the "predicted target position" marker at the arrival point
- Fade out completed portions of the trajectory

---

## 10. Gravitational Constant (μ)

We need a solar gravitational parameter μ in game units.

**Game units:**
- Distance: 1 AU = 50 Godot units (AU_SCALE)
- Time: sim-minutes (TICK_DELTA = 0.2 sim-minutes)

**Real solar μ:** μ_sun = 1.327×10²⁰ m³/s²

**In AU and years:** μ = 4π² AU³/yr²

**In game units:** We need μ in (Godot units)³ / (sim-minutes)².

Derivation:
- 1 Godot unit = 1/50 AU → 1 AU = 50 GU
- Earth's orbital period in the game: 21900 sim-minutes (from Simulation.gd:135)
- Earth's orbital radius: 1.0 AU = 50 GU

From circular orbit: `T = 2π * sqrt(r³/μ)` → `μ = (2π)² * r³ / T²`

`μ_game = (2π)² * 50³ / 21900² = 39.478 * 125000 / 479610000 ≈ 0.01029 GU³/min²`

**Verification:** Mercury's period should be: `T = 2π * sqrt((0.39*50)³ / μ_game)`
= `2π * sqrt(19.5³ / 0.01029)` = `2π * sqrt(7414.875 / 0.01029)` = `2π * sqrt(720590)` = `2π * 849` ≈ `5332 min`
Game value: 5280 min. Close enough (difference from rounding).

**Store as constant:** `const MU_SUN: float = 0.01029` (in GU³/sim-min²). Or better, compute it dynamically from Earth's data to stay consistent with any future period changes.

---

## 11. Implementation Steps (Ordered)

### Phase 1: Trajectory Abstraction (Foundation)

1. **Create `Trajectory.gd` base class** with the interface defined in §4
2. **Create `LinearTrajectory.gd`** that reimplements the current smoothstep behavior as a Trajectory subclass
3. **Add `trajectory` field to `ShipData`** (nullable reference)
4. **Modify `ShipMovementSystem.gd`** to use `LinearTrajectory` objects — behavior should be identical to current
5. **Update serialization** in `ShipData.to_dict()` / `from_dict()` to handle trajectory objects
6. **Test:** Game should play identically to before

### Phase 2: Keplerian Transfer Orbits

7. **Implement Lambert solver** in `TrajectoryPlanner.gd`
   - Universal variable formulation with Stumpff functions
   - Compute departure and arrival velocities
8. **Implement `KeplerianTrajectory.gd`**
   - Store orbital elements
   - `get_position_at_time()` via Kepler equation solver (Newton iteration)
   - `get_velocity_at_time()` via vis-viva and angular momentum
9. **Implement Hohmann-like transfer time selection** in `TrajectoryPlanner`
10. **Wire up `ShipMovementSystem`** to create `KeplerianTrajectory` when launching
11. **Compute μ_game** as a constant from the game's Earth orbit parameters
12. **Test:** Ships should follow elliptical paths between planets

### Phase 3: Fuel System

13. **Add delta-v fields** to `ShipData` (see §7.1)
14. **Set delta-v values** for initial ships in `_spawn_initial_ships()`
15. **Compute and deduct departure burn fuel** during launch
16. **Compute and deduct arrival burn fuel** during arrival
17. **Add fuel feasibility check** in `MissionSystem` before dispatch
18. **Refueling at stations** restores delta-v
19. **Update `SelectionPanel`** to show delta-v budget and burn costs
20. **Test:** Ships should consume realistic fuel amounts; underfueled ships should wait

### Phase 4: Moving Target & Replanning

21. **Implement `plan_intercept()`** iterative algorithm (§6.2) in `TrajectoryPlanner`
22. **For station targets:** Use exact planet position prediction (trivial — analytic orbits)
23. **Add replanning check** in `ShipMovementSystem._update_traveling()` every N ticks
24. **Deduct correction delta-v** for replanning maneuvers
25. **Test:** Ships targeting distant planets should curve naturally and replan as needed

### Phase 5: Rendering

26. **Draw trajectory curves** for traveling ships in `SolarSystemRenderer`
27. **Use ImmediateMesh LINE_STRIP**, sample trajectory at ~96 points
28. **Show remaining trajectory only** (from current position to arrival)
29. **Color by faction** with distinct alpha
30. **Test:** Visual verification that ellipses look correct

### Phase 6: Polish

31. **Handle edge cases:** Very short transfers (same planet), nearly circular transfers, high-eccentricity transfers
32. **Stranded ship handling** — coast on current orbit if out of fuel
33. **Save/load** — Verify trajectory serialization round-trips correctly
34. **Performance profiling** — Verify no frame drops at 50× speed with 40+ ships

---

## 12. Performance Budget

| Operation | Per-tick cost | Ships | Total per tick |
|-----------|-------------|-------|----------------|
| Kepler equation solve | ~15 FLOPs | 15 traveling | 225 FLOPs |
| Lambert solve (replan) | ~200 FLOPs | 1-2 replanning | 400 FLOPs |
| Trajectory rendering | 0 (per-frame, not per-tick) | — | — |

At 400 ticks/frame (max): 400 × 625 = 250,000 FLOPs/frame. **Negligible** — GDScript can handle millions of FLOPs per frame.

Trajectory rendering: 15 ships × 96 vertices = 1,440 vertices per frame. **Negligible.**

---

## 13. Key Design Decisions & Rationale

| Decision | Rationale |
|----------|-----------|
| Trajectories are immutable | Simpler code, easier serialization, no state bugs |
| No patched conics / SOI | Adds complexity without visual payoff at this stage; planetary gravity wells are handled as instantaneous burns |
| Linear delta-v ↔ fuel mapping | Avoids Tsiolkovsky logarithm while maintaining the core gameplay constraint |
| Compute μ from game's Earth orbit | Stays self-consistent with the game's time scale; no magic numbers |
| Keep same 5-state machine | Minimizes changes to MissionSystem, docking, cargo delivery, UI |
| Replan every ~100 ticks | Balances accuracy vs. computational cost; stations on analytic orbits means planet position prediction is exact anyway |

---

## 14. Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Lambert solver divergence for extreme geometries | Fallback to bisection; clamp eccentricity; fall back to LinearTrajectory for very short distances |
| Ships taking absurdly long Hohmann transfers between distant planets | Cap transfer time; allow "faster" transfers at higher delta-v cost (base_speed → aggressiveness factor) |
| Save file format change breaks old saves | Add version field; support loading legacy ships without trajectory data (assign LinearTrajectory) |
| Visual glitches from floating origin + trajectory rendering | Trajectory positions go through `floating_origin.world_to_render()` like everything else |
| GDScript precision for orbital math | Use `float` (64-bit in Godot 4); sufficient for game-scale AU distances |

---

## 15. Future Extensions (Not For Now)

These are deliberately **out of scope** but the architecture should not prevent them:

- **Low-thrust trajectories** (gigawatt-class engines): New `LowThrustTrajectory` subclass. Would need numerical integration or pre-computed spiral lookup tables.
- **Gravity assists / swing-bys**: Extend `TrajectoryPlanner` with multi-segment trajectories.
- **Inclination**: Add i, Ω fields to `KeplerianTrajectory`; planets get nonzero inclinations.
- **N-body perturbations**: Periodically correct the Keplerian trajectory with perturbation terms from Jupiter etc.
- **Atmospheric aerobraking**: Special arrival trajectory type for planets with atmospheres.
- **Rendezvous with ships**: `plan_intercept()` already supports this in principle.
- **Orbital station-keeping**: Ships in orbit around a planet, not just "docked".

---

## 16. Notation Reference

| Symbol | Meaning | Game units |
|--------|---------|------------|
| μ | Solar gravitational parameter | GU³/min² |
| a | Semi-major axis | GU (Godot units, 1 AU = 50 GU) |
| e | Eccentricity | dimensionless |
| ω | Argument of periapsis | radians |
| ν | True anomaly | radians |
| E | Eccentric anomaly | radians |
| M | Mean anomaly | radians |
| n | Mean motion = sqrt(μ/a³) | rad/min |
| Δv | Change in velocity (burn magnitude) | GU/min |
| Isp | Specific impulse | sim-minutes (game-scaled) |
| r | Orbital radius | GU |
| T | Orbital period | sim-minutes |
