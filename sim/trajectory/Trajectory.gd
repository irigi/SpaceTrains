class_name Trajectory
extends RefCounted
## Abstract base class for all ship trajectory types.
##
## A trajectory fully describes a ship's path through space from one point to
## another. Subclasses implement different physics models (linear smoothstep,
## Keplerian ellipse, low-thrust spiral, etc.).
##
## Trajectories use sim_time (in sim-minutes) for all time parameters.
## Positions are in Godot world units (1 AU = AU_SCALE Godot units).


## Returns the world position of the ship at sim_time [param t].
func get_position_at_time(_t: float) -> Vector3:
	push_error("Trajectory.get_position_at_time() not implemented in subclass")
	return Vector3.ZERO


## Returns the velocity vector of the ship at sim_time [param t] (in GU/sim-min).
func get_velocity_at_time(_t: float) -> Vector3:
	push_error("Trajectory.get_velocity_at_time() not implemented in subclass")
	return Vector3.ZERO


## Sim time (sim-minutes) when this trajectory begins.
func get_start_time() -> float:
	push_error("Trajectory.get_start_time() not implemented in subclass")
	return 0.0


## Sim time (sim-minutes) when this trajectory is expected to complete.
func get_end_time() -> float:
	push_error("Trajectory.get_end_time() not implemented in subclass")
	return 0.0


## Total expected duration in sim-minutes. Derived from start/end times.
func get_duration() -> float:
	return get_end_time() - get_start_time()


## Returns true if the trajectory is complete at sim_time [param t].
func is_complete(t: float) -> bool:
	return t >= get_end_time()


## Returns normalized progress from 0.0 (start) to 1.0 (end).
func get_progress(t: float) -> float:
	var dur := get_duration()
	if dur <= 0.0:
		return 1.0
	return clampf((t - get_start_time()) / dur, 0.0, 1.0)


## Delta-v (game units/sim-min) consumed at departure burn.
## Returns 0 for trajectory types that do not model explicit burns.
func get_departure_delta_v() -> float:
	return 0.0


## Delta-v (game units/sim-min) consumed at arrival burn.
## Returns 0 for trajectory types that do not model explicit burns.
func get_arrival_delta_v() -> float:
	return 0.0


## Total delta-v cost of this trajectory.
func get_total_delta_v() -> float:
	return get_departure_delta_v() + get_arrival_delta_v()


## Notify the trajectory of a new destination position.
## For trajectories that allow continuous target tracking (e.g. LinearTrajectory),
## this updates the endpoint. For immutable trajectories (e.g. Keplerian), this
## is a no-op — replanning is handled externally by creating a new trajectory.
func update_destination(_new_dest: Vector3) -> void:
	pass


## Returns a string identifier for this trajectory type (used in serialization).
func get_type() -> String:
	return "base"


## Serialize to a plain Dictionary for save/load.
func to_dict() -> Dictionary:
	return {"type": get_type()}


## Deserialize state from a Dictionary. Called on a freshly constructed instance.
func from_dict(_d: Dictionary) -> void:
	pass
