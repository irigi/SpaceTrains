extends RefCounted

# The display clock (docs/plans/main_gd_split.md, step 1): game seconds at which bodies, stations
# and ships are placed each frame, from orbital elements and planned paths, so the picture moves
# smoothly even when a simulation tick is slow. Also the spin clock of station rings and the
# visual phases of spins and station orbits, both on the wall clock.

const SIM_TICK_S := 8640.0
const DISPLAY_MAX_LEAD_REAL_S := 3.0
const BodySpinScript := preload("res://scripts/BodySpin.gd")

var display_time_s := -1.0
var snapshot_game_time_s := 0.0
var snapshot_wall_s := 0.0
# The simulation's measured pace (game seconds per real second), smoothed: the display clock
# runs at it, so a slow stretch slows the picture down instead of freezing it.
var sim_pace := 0.0
# Station ring spin runs on real time, but stops while the game is paused (as the top bar shows it).
var spin_clock_s := 0.0
var _spin_wall_s := -1.0
# Visual angles (rad) of each planet's and moon's spin and of each station's orbit round its
# planet (BodySpin.gd). Advanced on the wall clock at the timewarp rate, stopped by pause.
var visual_phase: Dictionary = {}

func wall_time_s() -> float:
    return float(Time.get_ticks_usec()) / 1000000.0

# A snapshot of game time game_time_s arrived at wall_s: the measured pace follows, and the
# clock starts at the first snapshot.
func on_snapshot(game_time_s: float, wall_s: float) -> void:
    if snapshot_wall_s > 0.0 and wall_s > snapshot_wall_s and game_time_s >= snapshot_game_time_s:
        var interval := wall_s - snapshot_wall_s
        var sample := (game_time_s - snapshot_game_time_s) / interval
        sim_pace += (sample - sim_pace) * clampf(interval / 1.5, 0.0, 1.0)
    snapshot_game_time_s = game_time_s
    snapshot_wall_s = wall_s
    if display_time_s < 0.0:
        display_time_s = game_time_s

# Once a frame: `paused` and `requested` (the timewarp) as the bridge last reported them.
func advance(delta: float, paused: bool, requested: float) -> void:
    if display_time_s < 0.0:
        return
    # The starting state is shown before the opening dispatch (a few seconds of planning):
    # the clock starts with the first simulated tick.
    paused = paused or snapshot_game_time_s <= 0.0
    if paused:
        # No time passes while paused: resuming must not look like a long-overdue snapshot.
        snapshot_wall_s = wall_time_s()
        return
    # The clock runs at the simulation's measured pace (at most the requested timewarp): when
    # ticks run slow the picture slows down rather than freezing, and after a timewarp change
    # it follows within a second or two.
    var rate := minf(requested, maxf(sim_pace, requested * 0.1)) if requested > 0.0 else 0.0
    # Where the simulation should be by now; a slow tick lets the display run ahead a
    # little (ships follow their planned paths meanwhile), never backwards.
    var max_lead := maxf(SIM_TICK_S * 1.5, rate * DISPLAY_MAX_LEAD_REAL_S)
    var limit := snapshot_game_time_s + max_lead
    var target := minf(snapshot_game_time_s + rate * (wall_time_s() - snapshot_wall_s), limit)
    # Behind the target: catch up (eased). Ahead of it: slow down (to a fifth when a whole
    # lead ahead), and brake through the last half of the lead window instead of stopping
    # dead at its end; never step back.
    var brake := clampf((limit - display_time_s) / (0.5 * max_lead), 0.0, 1.0)
    var next := display_time_s
    if target > display_time_s:
        next += rate * delta * brake
        next += (target - next) * clampf(delta * 3.0, 0.0, 1.0)
        next = minf(next, limit)
    elif display_time_s < limit:
        var slow := clampf(1.0 - (display_time_s - target) / max_lead, 0.2, 1.0)
        next = minf(next + rate * delta * slow * brake, limit)
    elif wall_time_s() - snapshot_wall_s < 0.5:
        # Past the limit (it shrinks when the simulation slows down after running fast):
        # crawl while snapshots keep coming rather than freeze; stop only if they stop.
        next += rate * delta * 0.2
    display_time_s = maxf(display_time_s, next)

# Advances the spin clock by the wall time since the last frame (unless paused); returns that
# time, capped at half a second, for advance_visual_phases.
func advance_spin(paused: bool) -> float:
    var wall_s := wall_time_s()
    var wall_dt := 0.0
    if _spin_wall_s >= 0.0 and not paused:
        wall_dt = minf(wall_s - _spin_wall_s, 0.5)
        spin_clock_s += wall_s - _spin_wall_s
    _spin_wall_s = wall_s
    return wall_dt

# Advances every planet's and moon's spin and every station's orbit by wall_dt seconds at the
# rate BodySpin gives for the current timewarp (true at slow speeds, capped at fast ones).
func advance_visual_phases(wall_dt: float, body_orbits: Dictionary, station_orbits: Dictionary, timewarp: float) -> void:
    if wall_dt <= 0.0:
        return
    for body_id in body_orbits.keys():
        var orbit: Dictionary = body_orbits[body_id]
        var rate := BodySpinScript.spin_rate(String(body_id), float(orbit["period"]), timewarp)
        visual_phase[body_id] = fmod(float(visual_phase.get(body_id, 0.0)) + rate * wall_dt, TAU)
    for station_id in station_orbits.keys():
        var orbit: Dictionary = station_orbits[station_id]
        var rate := BodySpinScript.station_rate(String(orbit["body"]), float(orbit["r"]), timewarp)
        visual_phase[station_id] = fmod(float(visual_phase.get(station_id, 0.0)) + rate * wall_dt, TAU)

# The visual angles and the spin clock, as a save's side file keeps them (they are not in the
# simulation, so a loaded game would otherwise keep the angles the screen had).
func export_view() -> Dictionary:
    return {"version": 1, "visual_phase": visual_phase.duplicate(), "spin_clock_s": spin_clock_s}

# Restores what export_view gave (a loaded game). Anything missing or malformed is left as it is.
func import_view(view: Dictionary) -> void:
    if int(view.get("version", 0)) != 1:
        return
    var phases: Variant = view.get("visual_phase")
    if typeof(phases) == TYPE_DICTIONARY:
        for entity_id in phases:
            visual_phase[String(entity_id)] = fmod(float(phases[entity_id]), TAU)
    spin_clock_s = float(view.get("spin_clock_s", spin_clock_s))

# A new timeline (a loaded game): the clock waits for its first snapshot again.
func reset() -> void:
    display_time_s = -1.0
    sim_pace = 0.0
    snapshot_wall_s = 0.0
