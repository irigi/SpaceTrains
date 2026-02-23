class_name KeplerianTrajectory
extends Trajectory
## Two-body heliocentric trajectory in the simulation XZ plane.
##
## The trajectory is defined from an initial position/velocity state vector,
## then propagated via Kepler's equation for an elliptical orbit.

var mu: float = 0.0  # Solar gravitational parameter in GU^3 / sim-min^2
var start_time: float = 0.0
var end_time: float = 0.0

# Stored for serialization/debugging
var initial_position: Vector3 = Vector3.ZERO
var initial_velocity: Vector3 = Vector3.ZERO

# Orbital elements (coplanar XZ)
var semi_major_axis: float = 0.0
var eccentricity: float = 0.0
var argument_of_periapsis: float = 0.0
var mean_anomaly_at_epoch: float = 0.0
var mean_motion: float = 0.0

var departure_delta_v: float = 0.0
var arrival_delta_v: float = 0.0


static func create(
	from: Vector3,
	velocity_at_departure: Vector3,
	t_start: float,
	t_end: float,
	mu_sun: float,
	dep_dv: float = 0.0,
	arr_dv: float = 0.0
) -> KeplerianTrajectory:
	var traj := KeplerianTrajectory.new()
	traj.mu = mu_sun
	traj.start_time = t_start
	traj.end_time = t_end
	traj.initial_position = from
	traj.initial_velocity = velocity_at_departure
	traj.departure_delta_v = dep_dv
	traj.arrival_delta_v = arr_dv
	traj._build_from_state_vectors(from, velocity_at_departure)
	return traj


func _build_from_state_vectors(r: Vector3, v: Vector3) -> void:
	var rmag := r.length()
	var h := r.cross(v)
	var e_vec := (v.cross(h) / mu) - (r / maxf(rmag, 1e-6))
	var e := e_vec.length()
	var specific_energy := 0.5 * v.length_squared() - (mu / maxf(rmag, 1e-6))
	if absf(specific_energy) < 1e-8:
		specific_energy = -1e-8
	semi_major_axis = -mu / (2.0 * specific_energy)
	eccentricity = clampf(e, 0.0, 0.999999)

	if eccentricity > 1e-6:
		argument_of_periapsis = atan2(e_vec.z, e_vec.x)
	else:
		argument_of_periapsis = atan2(r.z, r.x)

	var true_anomaly := _compute_true_anomaly(r, v, e_vec, eccentricity)
	var e_anomaly := _eccentric_from_true_anomaly(true_anomaly, eccentricity)
	mean_anomaly_at_epoch = e_anomaly - eccentricity * sin(e_anomaly)

	mean_motion = sqrt(mu / maxf(pow(semi_major_axis, 3.0), 1e-8))


func _compute_true_anomaly(r: Vector3, v: Vector3, e_vec: Vector3, e: float) -> float:
	if e <= 1e-6:
		return wrapf(atan2(r.z, r.x) - argument_of_periapsis, 0.0, TAU)
	var cos_nu := clampf(e_vec.dot(r) / maxf(e * r.length(), 1e-8), -1.0, 1.0)
	var nu := acos(cos_nu)
	if r.dot(v) < 0.0:
		nu = TAU - nu
	return nu


func _eccentric_from_true_anomaly(nu: float, e: float) -> float:
	if e <= 1e-6:
		return nu
	return 2.0 * atan2(
		sqrt(1.0 - e) * sin(nu * 0.5),
		sqrt(1.0 + e) * cos(nu * 0.5)
	)


func _solve_kepler_equation(m: float, e: float) -> float:
	var e_anomaly := m
	if e > 0.8:
		e_anomaly = PI
	for _i in range(8):
		var f := e_anomaly - e * sin(e_anomaly) - m
		var fp := 1.0 - e * cos(e_anomaly)
		if absf(fp) < 1e-8:
			break
		e_anomaly -= f / fp
	return e_anomaly


func get_position_at_time(t: float) -> Vector3:
	var clamped_t := clampf(t, start_time, end_time)
	var m := mean_anomaly_at_epoch + mean_motion * (clamped_t - start_time)
	m = wrapf(m, -PI, PI)
	var e_anomaly := _solve_kepler_equation(m, eccentricity)
	var cos_e := cos(e_anomaly)
	var sin_e := sin(e_anomaly)

	var x_orb := semi_major_axis * (cos_e - eccentricity)
	var z_orb := semi_major_axis * sqrt(maxf(1.0 - eccentricity * eccentricity, 0.0)) * sin_e

	var cos_w := cos(argument_of_periapsis)
	var sin_w := sin(argument_of_periapsis)
	return Vector3(
		x_orb * cos_w - z_orb * sin_w,
		0.0,
		x_orb * sin_w + z_orb * cos_w
	)


func get_velocity_at_time(t: float) -> Vector3:
	var clamped_t := clampf(t, start_time, end_time)
	var m := mean_anomaly_at_epoch + mean_motion * (clamped_t - start_time)
	m = wrapf(m, -PI, PI)
	var e_anomaly := _solve_kepler_equation(m, eccentricity)
	var cos_e := cos(e_anomaly)
	var sin_e := sin(e_anomaly)
	var denom := maxf(1.0 - eccentricity * cos_e, 1e-8)

	var vx_orb := -semi_major_axis * mean_motion * sin_e / denom
	var vz_orb := semi_major_axis * mean_motion * sqrt(maxf(1.0 - eccentricity * eccentricity, 0.0)) * cos_e / denom

	var cos_w := cos(argument_of_periapsis)
	var sin_w := sin(argument_of_periapsis)
	return Vector3(
		vx_orb * cos_w - vz_orb * sin_w,
		0.0,
		vx_orb * sin_w + vz_orb * cos_w
	)


func get_start_time() -> float:
	return start_time


func get_end_time() -> float:
	return end_time


func get_departure_delta_v() -> float:
	return departure_delta_v


func get_arrival_delta_v() -> float:
	return arrival_delta_v


func get_type() -> String:
	return "keplerian"


func to_dict() -> Dictionary:
	return {
		"type": "keplerian",
		"mu": mu,
		"start_time": start_time,
		"end_time": end_time,
		"initial_position": [initial_position.x, initial_position.y, initial_position.z],
		"initial_velocity": [initial_velocity.x, initial_velocity.y, initial_velocity.z],
		"semi_major_axis": semi_major_axis,
		"eccentricity": eccentricity,
		"argument_of_periapsis": argument_of_periapsis,
		"mean_anomaly_at_epoch": mean_anomaly_at_epoch,
		"mean_motion": mean_motion,
		"departure_delta_v": departure_delta_v,
		"arrival_delta_v": arrival_delta_v,
	}


func from_dict(d: Dictionary) -> void:
	mu = d.get("mu", 0.0)
	start_time = d.get("start_time", 0.0)
	end_time = d.get("end_time", 0.0)
	var ip = d.get("initial_position", [0, 0, 0])
	initial_position = Vector3(ip[0], ip[1], ip[2])
	var iv = d.get("initial_velocity", [0, 0, 0])
	initial_velocity = Vector3(iv[0], iv[1], iv[2])
	semi_major_axis = d.get("semi_major_axis", 0.0)
	eccentricity = d.get("eccentricity", 0.0)
	argument_of_periapsis = d.get("argument_of_periapsis", 0.0)
	mean_anomaly_at_epoch = d.get("mean_anomaly_at_epoch", 0.0)
	mean_motion = d.get("mean_motion", 0.0)
	departure_delta_v = d.get("departure_delta_v", 0.0)
	arrival_delta_v = d.get("arrival_delta_v", 0.0)
