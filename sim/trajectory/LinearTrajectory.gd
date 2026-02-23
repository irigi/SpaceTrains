class_name LinearTrajectory
extends Trajectory
## Straight-line trajectory with Hermite smoothstep ease-in/ease-out.
##
## This replicates the original ship movement behaviour exactly.
## The destination can be updated each tick to track a moving station,
## which is why this class allows mutable destination (unlike Keplerian
## trajectories, which are replanned as new objects).
##
## Time unit: sim-minutes  |  Space unit: Godot world units (AU_SCALE = 50)


var origin: Vector3 = Vector3.ZERO       # World position at departure
var destination: Vector3 = Vector3.ZERO  # World position at arrival (may update)
var start_time: float = 0.0              # sim_time when travel began (sim-min)
var duration: float = 1.0               # Total travel time (sim-min)


## Convenience constructor. Returns a fully initialised LinearTrajectory.
static func create(from: Vector3, to: Vector3, t_start: float, travel_duration: float) -> LinearTrajectory:
	var traj := LinearTrajectory.new()
	traj.origin = from
	traj.destination = to
	traj.start_time = t_start
	traj.duration = travel_duration
	return traj


# ── Trajectory interface ────────────────────────────────────────────────────

func get_position_at_time(t: float) -> Vector3:
	var smooth_t := _smoothstep(get_progress(t))
	return origin.lerp(destination, smooth_t)


func get_velocity_at_time(t: float) -> Vector3:
	# d/dt [lerp(o, d, smoothstep(p(t)))]
	#   = (d - o) * smoothstep'(p) * dp/dt
	# smoothstep'(p) = 6p(1-p)   (derivative of 3p²-2p³)
	# dp/dt = 1/duration
	if duration <= 0.0:
		return Vector3.ZERO
	var p := get_progress(t)
	var smooth_deriv := 6.0 * p * (1.0 - p)
	return (destination - origin) * (smooth_deriv / duration)


func get_start_time() -> float:
	return start_time


func get_end_time() -> float:
	return start_time + duration


## Update the destination to follow a moving station.
## LinearTrajectory allows this; Keplerian rejects it (no-op in base class).
func update_destination(new_dest: Vector3) -> void:
	destination = new_dest


func get_type() -> String:
	return "linear"


# ── Serialization ───────────────────────────────────────────────────────────

func to_dict() -> Dictionary:
	return {
		"type": "linear",
		"origin":      [origin.x,      origin.y,      origin.z],
		"destination": [destination.x, destination.y, destination.z],
		"start_time":  start_time,
		"duration":    duration,
	}


func from_dict(d: Dictionary) -> void:
	var o    = d.get("origin",      [0, 0, 0])
	var dest = d.get("destination", [0, 0, 0])
	origin      = Vector3(o[0],    o[1],    o[2])
	destination = Vector3(dest[0], dest[1], dest[2])
	start_time  = d.get("start_time", 0.0)
	duration    = d.get("duration",   1.0)


# ── Helpers ─────────────────────────────────────────────────────────────────

func _smoothstep(t: float) -> float:
	## Hermite smoothstep: ease-in/ease-out curve over [0, 1].
	return t * t * (3.0 - 2.0 * t)
