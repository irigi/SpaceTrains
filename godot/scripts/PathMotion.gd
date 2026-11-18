extends RefCounted

# Where things are drawn (docs/plans/main_gd_split.md, step 3): the orbits and planned paths from
# the snapshots, and positions at the display clock's time, in metres (double precision) relative
# to the focus, then cast to render units. Main calls begin_frame() at the start of _update_nodes.

const POSITION_SCALE := 1.0 / 8_000_000_000.0
const DisplayClockScript := preload("res://scripts/DisplayClock.gd")

var clock: DisplayClockScript  # display_time_s, visual_phase
# Main's entity dictionaries, shared by reference (never reassigned).
var entity_details: Dictionary
var entity_kinds: Dictionary

var body_orbits: Dictionary = {}     # body_id -> {parent, a, period, phase}
var station_orbits: Dictionary = {}  # station_id -> {body, r, theta}
var ship_paths: Dictionary = {}      # ship_id -> {sig, origin, destination, departure, arrival, t, off (x,y,z,... from the rail anchor)}
var frame_positions_m: Dictionary = {}  # per-frame cache: entity_id -> PackedFloat64Array [x, y, z] in metres
var origin_m := PackedFloat64Array([0.0, 0.0, 0.0])  # the focus; entities are drawn relative to it

func setup(display_clock: DisplayClockScript, details: Dictionary, kinds: Dictionary) -> void:
    clock = display_clock
    entity_details = details
    entity_kinds = kinds

# A new frame: positions are computed afresh at the display clock's time.
func begin_frame() -> void:
    frame_positions_m.clear()

# A ship gone from the snapshot.
func forget_ship(ship_id: String) -> void:
    ship_paths.erase(ship_id)

# A new timeline (a loaded game).
func reset() -> void:
    ship_paths.clear()

func cache_orbits(bridge_state: Dictionary) -> void:
    for body in bridge_state.get("bodies", []):
        body_orbits[String(body["id"])] = {
            "parent": String(body.get("parent_id", "")),
            "a": float(body.get("semi_major_axis_m", 0.0)),
            "period": float(body.get("orbital_period_s", 0.0)),
            "phase": float(body.get("phase_at_epoch_rad", 0.0)),
            "radius": float(body.get("radius_m", 0.0)),
        }
    for station in bridge_state.get("stations", []):
        var body_id := String(station.get("parent_body_id", ""))
        var body_radius := float((body_orbits.get(body_id, {}) as Dictionary).get("radius", 0.0))
        station_orbits[String(station["id"])] = {
            "body": body_id,
            "r": body_radius + float(station.get("altitude_m", 0.0)),
            "theta": float(station.get("theta_rad", 0.0)),
        }

# Planned paths as flat arrays, rebuilt only when a ship's plan changes.
func cache_ship_path(ship_id: String, ship: Dictionary) -> void:
    var path: Array = ship.get("trajectory_path", [])
    if path.size() < 2:
        ship_paths.erase(ship_id)
        return
    var signature := String(ship.get("path_id", ""))
    if signature == "":
        signature = trajectory_path_signature(path)
    if ship_paths.has(ship_id) and String(ship_paths[ship_id]["sig"]) == signature:
        return
    # Wrecks and cutters fly free heliocentric paths: no planet rails.
    var free := String(ship.get("phase", "")) in ["derelict", "patrol"]
    var cached := {
        "sig": signature,
        "origin": "" if free else String((station_orbits.get(String(ship.get("origin_station_id", "")), {}) as Dictionary).get("body", "")),
        "destination": "" if free else String((station_orbits.get(String(ship.get("destination_station_id", "")), {}) as Dictionary).get("body", "")),
        "departure": float(ship.get("departure_time_s", 0.0)),
        "arrival": float(ship.get("arrival_time_s", 0.0)),
        # The stations the flight leaves and enters (their drawn positions, _station_blend_m).
        "origin_station": "" if free else String(ship.get("origin_station_id", "")),
        "destination_station": "" if free else String(ship.get("destination_station_id", "")),
    }
    var origin_planet := planet_of(String(cached["origin"]))
    cached["helio"] = origin_planet != "" and origin_planet != planet_of(String(cached["destination"]))
    # A flight within one planet's system (Earth-Luna ferries, moon hops) is drawn around that
    # planet and moves with it: in the Sun's frame the planet's own motion would smear it out.
    cached["frame"] = origin_planet if origin_planet != "" and not bool(cached["helio"]) else ""
    var times := PackedFloat64Array()
    var offsets := PackedFloat64Array()
    var raw := PackedFloat64Array()
    times.resize(path.size())
    offsets.resize(path.size() * 3)
    raw.resize(path.size() * 3)
    for i in range(path.size()):
        var point: Dictionary = path[i]
        var t := float(point.get("t_s", 0.0))
        times[i] = t
        var anchor := _rail_anchor_m(cached, t)
        var xyz := [float(point.get("x", 0.0)), float(point.get("y", 0.0)), float(point.get("z", 0.0))]
        for axis in range(3):
            offsets[3 * i + axis] = xyz[axis] - anchor[axis]
            raw[3 * i + axis] = xyz[axis]
    cached["t"] = times
    cached["off"] = offsets
    cached["raw"] = raw
    cached["sense"] = _path_sense(times, raw, float(cached["departure"]), float(cached["arrival"]))
    cached["doff"] = _offset_rates(times, offsets, float(cached["departure"]), float(cached["arrival"]))
    if bool(cached["helio"]):
        _cache_helio_polar(cached)
    ship_paths[ship_id] = cached

# Radius and unwound angle of each heliocentric sample, with their rates at the samples
# (from the neighbours within the flight, so the wait samples before departure don't
# bend the start), for the cubic drawing in _helio_point_m.
func _cache_helio_polar(path: Dictionary) -> void:
    var times: PackedFloat64Array = path["t"]
    var raw: PackedFloat64Array = path["raw"]
    var sense := float(path["sense"])
    var count := times.size()
    var radius := PackedFloat64Array()
    var angle := PackedFloat64Array()
    radius.resize(count)
    angle.resize(count)
    for i in range(count):
        radius[i] = sqrt(raw[3 * i] * raw[3 * i] + raw[3 * i + 2] * raw[3 * i + 2])
        var a := atan2(raw[3 * i + 2], raw[3 * i])
        if i == 0:
            angle[i] = a
            continue
        var turn := wrapf(a - atan2(raw[3 * i - 1], raw[3 * i - 3]), -PI, PI)
        if turn * sense < 0.0 and absf(turn) > 0.5 * PI:
            turn += TAU * sense
        angle[i] = angle[i - 1] + turn
    var first := clampi(times.bsearch(float(path["departure"]), true), 0, count - 1)
    var last := clampi(times.bsearch(float(path["arrival"]), false) - 1, first, count - 1)
    path["r"] = radius
    path["a"] = angle
    path["dr"] = _sample_rates(times, radius, first, last)
    path["da"] = _sample_rates(times, angle, first, last)

# Rate of y at each sample: the time-weighted mean of the slopes on either side, one-sided
# at the ends of [first, last].
func _sample_rates(times: PackedFloat64Array, y: PackedFloat64Array, first: int, last: int) -> PackedFloat64Array:
    var rates := PackedFloat64Array()
    rates.resize(times.size())
    for i in range(times.size()):
        var lo := clampi(i - 1, first, last)
        var hi := clampi(i + 1, first, last)
        if i <= lo or i >= hi:
            lo = mini(lo, i)
            hi = maxi(hi, i)
            rates[i] = (y[hi] - y[lo]) / (times[hi] - times[lo]) if hi > lo and times[hi] > times[lo] else 0.0
            continue
        var h0 := times[i] - times[lo]
        var h1 := times[hi] - times[i]
        if h0 <= 0.0 or h1 <= 0.0:
            rates[i] = (y[hi] - y[lo]) / maxf(times[hi] - times[lo], 1.0)
            continue
        rates[i] = (h1 * (y[i] - y[lo]) / h0 + h0 * (y[hi] - y[i]) / h1) / (h0 + h1)
    return rates

# Rates of the rail offsets at each sample (x, y, z interleaved), for the cubic drawing in
# _rail_point_m. Neighbours are taken within one stretch only (the wait before departure,
# the flight, the time after arrival), so the burns at the ends keep their corners.
func _offset_rates(times: PackedFloat64Array, offsets: PackedFloat64Array, departure: float, arrival: float) -> PackedFloat64Array:
    var count := times.size()
    var first := clampi(times.bsearch(departure, true), 0, count - 1)
    var last := clampi(times.bsearch(arrival, false) - 1, first, count - 1)
    var stretches := [[0, first], [first, last], [last, count - 1]]
    var rates := PackedFloat64Array()
    rates.resize(offsets.size())
    var y := PackedFloat64Array()
    y.resize(count)
    for axis in range(3):
        for i in range(count):
            y[i] = offsets[3 * i + axis]
        for stretch in stretches:
            var lo: int = stretch[0]
            var hi: int = stretch[1]
            if hi <= lo:
                continue
            var axis_rates := _sample_rates(times, y, lo, hi)
            for i in range(lo, hi + 1):
                rates[3 * i + axis] = axis_rates[i]
    return rates

# The planet a body belongs to (itself for a planet, its parent for a moon); "" for the Sun.
func planet_of(body_id: String) -> String:
    var body := body_id
    for _depth in range(4):
        var parent := String((body_orbits.get(body, {}) as Dictionary).get("parent", ""))
        if parent == "":
            return ""
        if String((body_orbits.get(parent, {}) as Dictionary).get("parent", "")) == "":
            return body
        body = parent
    return body

# +1 when a heliocentric path runs anticlockwise (increasing angle), -1 otherwise.
func _path_sense(times: PackedFloat64Array, raw: PackedFloat64Array, departure: float, arrival: float) -> float:
    var total := 0.0
    for i in range(times.size() - 1):
        if times[i + 1] <= departure or times[i] >= arrival:
            continue
        var a0 := atan2(raw[3 * i + 2], raw[3 * i])
        var a1 := atan2(raw[3 * i + 5], raw[3 * i + 3])
        total += wrapf(a1 - a0, -PI, PI)
    return 1.0 if total >= 0.0 else -1.0

# Ships ride their planets' rails. A planned path is sampled every few days (the wait for
# a launch window is a handful of points along the planet's orbit), so straight lines
# between samples cut across the planet's curved track. Each sample is kept as an offset
# from an anchor that moves with the origin planet and, for a hop within one planet's
# system, hands over smoothly to the destination during the flight; the offsets are
# interpolated and the anchor's exact position is added back. At the sample times this is
# the planned path itself. An interplanetary flight is drawn from its heliocentric samples
# instead (see _helio_point_m): blending a planet into the anchor over a years-long flight,
# with samples weeks apart on flat spirals, drew the planet's yearly circle into the path
# as a sawtooth wobble.
func _rail_anchor_m(path: Dictionary, t: float) -> PackedFloat64Array:
    var origin := String(path.get("origin", ""))
    var destination := String(path.get("destination", ""))
    if origin == "" or destination == "":
        return PackedFloat64Array([0.0, 0.0, 0.0])
    var frame := String(path.get("frame", ""))
    if frame != "":
        return body_at_m(frame, t)
    var departure := float(path["departure"])
    var arrival := float(path["arrival"])
    if bool(path.get("helio", false)):
        return body_at_m(origin if t < 0.5 * (departure + arrival) else destination, t)
    var s := clampf((t - departure) / maxf(arrival - departure, 1.0), 0.0, 1.0)
    var w := s * s * (3.0 - 2.0 * s)
    if w <= 0.0:
        return body_at_m(origin, t)
    if w >= 1.0:
        return body_at_m(destination, t)
    var a := body_at_m(origin, t)
    var b := body_at_m(destination, t)
    return PackedFloat64Array([a[0] + (b[0] - a[0]) * w, a[1] + (b[1] - a[1]) * w, a[2] + (b[2] - a[2]) * w])

# The planet a path is drawn around (its "frame"), now and at time t; the Sun's origin for
# paths without one.
func frame_now_m(frame: String) -> PackedFloat64Array:
    return body_m(frame) if frame != "" else PackedFloat64Array([0.0, 0.0, 0.0])

func _frame_at_m(frame: String, t: float) -> PackedFloat64Array:
    return body_at_m(frame, t) if frame != "" else PackedFloat64Array([0.0, 0.0, 0.0])

# A path's point at time t as drawn: relative to its frame planet at t (absolute without one).
func drawn_point_m(path: Dictionary, t: float) -> PackedFloat64Array:
    var p := rail_point_m(path, t)
    var frame := String(path.get("frame", ""))
    if frame == "":
        return p
    var base := body_at_m(frame, t)
    return PackedFloat64Array([p[0] - base[0], p[1] - base[1], p[2] - base[2]])

func plus_m(a: PackedFloat64Array, b: PackedFloat64Array) -> PackedFloat64Array:
    return PackedFloat64Array([a[0] + b[0], a[1] + b[1], a[2] + b[2]])

func minus_m(a: PackedFloat64Array, b: PackedFloat64Array) -> PackedFloat64Array:
    return PackedFloat64Array([a[0] - b[0], a[1] - b[1], a[2] - b[2]])

# Position on a cached path at time t, on the rail.
func rail_point_m(path: Dictionary, t: float) -> PackedFloat64Array:
    var times: PackedFloat64Array = path["t"]
    var offsets: PackedFloat64Array = path["off"]
    var i := clampi(times.bsearch(t, true) - 1, 0, times.size() - 2)
    var span := times[i + 1] - times[i]
    var f := clampf((t - times[i]) / span, 0.0, 1.0) if span > 0.0 else 1.0
    var out := PackedFloat64Array([0.0, 0.0, 0.0])
    if bool(path.get("helio", false)) and t > float(path["departure"]) and t < float(path["arrival"]):
        out = helio_point_m(path, i, f)
    else:
        var anchor := _rail_anchor_m(path, t)
        var rates: PackedFloat64Array = path["doff"]
        # Cubic (Hermite) between the samples, so the ship's motion has no corner at each one.
        var f2 := f * f
        var f3 := f2 * f
        var h00 := 2.0 * f3 - 3.0 * f2 + 1.0
        var h10 := (f3 - 2.0 * f2 + f) * span
        var h01 := 3.0 * f2 - 2.0 * f3
        var h11 := (f3 - f2) * span
        for axis in range(3):
            var j := 3 * i + axis
            out[axis] = anchor[axis] + h00 * offsets[j] + h10 * rates[j] + h01 * offsets[j + 3] + h11 * rates[j + 3]
    var blend := _station_blend_m(path, t)
    for axis in range(3):
        out[axis] += blend[axis]
    return out

# The flight's samples are the simulation's station positions; the stations are drawn at
# their visual orbit (BodySpin.gd). Near the origin the flight starts at the drawn station and
# near the destination it ends at the drawn one: the offset fades linearly along the flight.
func _station_blend_m(path: Dictionary, t: float) -> PackedFloat64Array:
    var out := PackedFloat64Array([0.0, 0.0, 0.0])
    var duration := float(path["arrival"]) - float(path["departure"])
    if duration <= 0.0:
        return out
    var s := clampf((t - float(path["departure"])) / duration, 0.0, 1.0)
    var origin := String(path.get("origin_station", ""))
    var destination := String(path.get("destination_station", ""))
    if origin != "" and station_orbits.has(origin) and s < 1.0:
        var d := _station_visual_offset_m(origin)
        for axis in range(3):
            out[axis] += (1.0 - s) * d[axis]
    if destination != "" and station_orbits.has(destination) and s > 0.0:
        var d := _station_visual_offset_m(destination)
        for axis in range(3):
            out[axis] += s * d[axis]
    return out

# A station's drawn position minus its simulated one (the planet's position cancels out).
func _station_visual_offset_m(station_id: String) -> PackedFloat64Array:
    var orbit: Dictionary = station_orbits[station_id]
    var r := float(orbit["r"])
    var theta := float(orbit["theta"])
    var drawn := theta + float(clock.visual_phase.get(station_id, 0.0))
    return PackedFloat64Array([r * (cos(drawn) - cos(theta)), 0.0, r * (sin(drawn) - sin(theta))])

# Between two heliocentric samples: radius and angle (the short way, unless that runs
# against the flight's sense of motion) are interpolated, so spirals and conic arcs stay
# smooth however far apart the samples are. The interpolation is cubic (Hermite, with the
# rates at the samples): a linear one bowed the long, nearly radial stretches of
# outer-system flights by up to 0.03 AU, with a corner at every sample.
func helio_point_m(path: Dictionary, i: int, f: float) -> PackedFloat64Array:
    var raw: PackedFloat64Array = path["raw"]
    var times: PackedFloat64Array = path["t"]
    var span := times[i + 1] - times[i]
    var r := _hermite(path["r"], path["dr"], i, f, span)
    var a := _hermite(path["a"], path["da"], i, f, span)
    var y := raw[3 * i + 1] + (raw[3 * i + 4] - raw[3 * i + 1]) * f
    return PackedFloat64Array([cos(a) * r, y, sin(a) * r])

func _hermite(y: PackedFloat64Array, rate: PackedFloat64Array, i: int, f: float, span: float) -> float:
    var f2 := f * f
    var f3 := f2 * f
    return (2.0 * f3 - 3.0 * f2 + 1.0) * y[i] + (f3 - 2.0 * f2 + f) * span * rate[i] \
        + (3.0 * f2 - 2.0 * f3) * y[i + 1] + (f3 - f2) * span * rate[i + 1]

# A body's position at any time t (the per-frame _body_m caches the display time only).
func body_at_m(body_id: String, t: float) -> PackedFloat64Array:
    var orbit: Dictionary = body_orbits.get(body_id, {})
    var parent_id := String(orbit.get("parent", ""))
    if parent_id == "":
        return PackedFloat64Array([0.0, 0.0, 0.0])
    var base := body_at_m(parent_id, t)
    var period := float(orbit["period"])
    if period <= 0.0:
        return base
    var angle := float(orbit["phase"]) + (t / period) * TAU
    var a := float(orbit["a"])
    return PackedFloat64Array([base[0] + cos(angle) * a, base[1], base[2] + sin(angle) * a])

# Same formula as CelestialMechanics::get_body_position (circular, coplanar orbits).
func body_m(body_id: String) -> PackedFloat64Array:
    if frame_positions_m.has(body_id):
        return frame_positions_m[body_id]
    var result := PackedFloat64Array([0.0, 0.0, 0.0])
    var orbit: Dictionary = body_orbits.get(body_id, {})
    var parent_id := String(orbit.get("parent", ""))
    if parent_id != "":
        var base := body_m(parent_id)
        var period := float(orbit["period"])
        result = base
        if period > 0.0:
            var angle := float(orbit["phase"]) + (clock.display_time_s / period) * TAU
            var a := float(orbit["a"])
            result = PackedFloat64Array([base[0] + cos(angle) * a, base[1], base[2] + sin(angle) * a])
    frame_positions_m[body_id] = result
    return result

func station_m(station_id: String) -> PackedFloat64Array:
    var key := "station:" + station_id
    if frame_positions_m.has(key):
        return frame_positions_m[key]
    var orbit: Dictionary = station_orbits.get(station_id, {})
    var result := PackedFloat64Array([0.0, 0.0, 0.0])
    if not orbit.is_empty():
        var base := body_m(String(orbit["body"]))
        var r := float(orbit["r"])
        # The sim's angle plus the visual orbit (BodySpin.gd): drawn only, the planner keeps theta.
        var theta := float(orbit["theta"]) + float(clock.visual_phase.get(station_id, 0.0))
        result = PackedFloat64Array([base[0] + cos(theta) * r, base[1], base[2] + sin(theta) * r])
    frame_positions_m[key] = result
    return result

# A ship in transit (or a wreck, or a cutter under way) flies its path; otherwise it is at its station.
func ship_m(ship_id: String) -> PackedFloat64Array:
    var detail: Dictionary = entity_details.get(ship_id, {})
    if String(detail.get("phase", "idle")) in ["in_transit", "derelict", "patrol"] and ship_paths.has(ship_id):
        var sample := _path_sample(ship_id, clock.display_time_s)
        return PackedFloat64Array([sample[0], sample[1], sample[2]])
    var station_id := String(detail.get("current_station_id", ""))
    if station_orbits.has(station_id):
        return station_m(station_id)
    return PackedFloat64Array([float(detail.get("x", 0.0)), float(detail.get("y", 0.0)), float(detail.get("z", 0.0))])

# Position [x, y, z] and direction of motion [dx, dy, dz] on a ship's path at time t.
# At the display time it is kept for the frame: position, heading and focus all ask for it.
func _path_sample(ship_id: String, t: float) -> PackedFloat64Array:
    var key := "@" + ship_id
    if t == clock.display_time_s and frame_positions_m.has(key):
        return frame_positions_m[key]
    var path: Dictionary = ship_paths[ship_id]
    var here := rail_point_m(path, t)
    var ahead := rail_point_m(path, t + 600.0)
    var sample := PackedFloat64Array([here[0], here[1], here[2], ahead[0] - here[0], ahead[1] - here[1], ahead[2] - here[2]])
    if t == clock.display_time_s:
        frame_positions_m[key] = sample
    return sample

func ship_heading(ship_id: String) -> Vector3:
    if not ship_paths.has(ship_id):
        return Vector3.ZERO
    var sample := _path_sample(ship_id, clock.display_time_s)
    return Vector3(sample[3], sample[4], sample[5])

func entity_m(entity_id: String) -> PackedFloat64Array:
    match String(entity_kinds.get(entity_id, "")):
        "body":
            return body_m(entity_id)
        "station":
            return station_m(entity_id)
        "ship":
            return ship_m(entity_id)
    return PackedFloat64Array([0.0, 0.0, 0.0])

# Relative to the focus, subtracted in double precision before the cast to float.
func to_render(p: PackedFloat64Array) -> Vector3:
    return Vector3((p[0] - origin_m[0]) * POSITION_SCALE, (p[1] - origin_m[1]) * POSITION_SCALE, (p[2] - origin_m[2]) * POSITION_SCALE)

func abs_render(p: PackedFloat64Array) -> Vector3:
    return Vector3(p[0] * POSITION_SCALE, p[1] * POSITION_SCALE, p[2] * POSITION_SCALE)

# Runs every frame for every active ship, so it samples a few points rather than
# formatting the whole path (long launch waits can add hundreds of samples).
# A new plan always changes the size or the endpoints.
func trajectory_path_signature(trajectory_path: Array) -> String:
    var parts: Array[String] = [str(trajectory_path.size())]
    for index in [0, trajectory_path.size() / 2, trajectory_path.size() - 1]:
        var point: Dictionary = trajectory_path[index]
        parts.append("%.3f,%.3f,%.3f,%.0f" % [float(point.get("x", 0.0)), float(point.get("y", 0.0)), float(point.get("z", 0.0)), float(point.get("t_s", 0.0))])
    return "|".join(parts)
