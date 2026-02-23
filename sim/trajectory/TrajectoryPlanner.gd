class_name TrajectoryPlanner
extends RefCounted
## Plans ship trajectories. Phase 2 uses Keplerian transfers with
## a Lambert universal-variable solve and falls back to linear travel.

const AU_SCALE: float = 50.0
const EARTH_ORBIT_RADIUS_GU: float = 1.0 * AU_SCALE
const EARTH_ORBIT_PERIOD_MIN: float = 21900.0
const MU_SUN: float = (TAU * TAU) * pow(EARTH_ORBIT_RADIUS_GU, 3.0) / pow(EARTH_ORBIT_PERIOD_MIN, 2.0)


func plan_station_transfer(
	origin: Vector3,
	destination: Vector3,
	start_time: float,
	base_speed: float,
	origin_body_pos: Vector3,
	dest_body_pos: Vector3
) -> Trajectory:
	var transfer_duration := _estimate_transfer_duration(origin.length(), destination.length(), base_speed)
	var solve := _solve_lambert_universal(origin, destination, transfer_duration, MU_SUN)
	if not solve.get("ok", false):
		return LinearTrajectory.create(origin, destination, start_time, maxf(origin.distance_to(destination) / maxf(base_speed, 0.1), 1.0))

	var v1: Vector3 = solve["v1"]
	var v2: Vector3 = solve["v2"]
	if not _is_elliptic_state(origin, v1):
		return LinearTrajectory.create(origin, destination, start_time, maxf(origin.distance_to(destination) / maxf(base_speed, 0.1), 1.0))
	var dep_dv := (v1 - _circular_velocity_at_position(origin_body_pos)).length()
	var arr_dv := (v2 - _circular_velocity_at_position(dest_body_pos)).length()

	return KeplerianTrajectory.create(
		origin,
		v1,
		start_time,
		start_time + transfer_duration,
		MU_SUN,
		dep_dv,
		arr_dv
	)


func _is_elliptic_state(pos: Vector3, vel: Vector3) -> bool:
	var r := pos.length()
	if r <= 1e-6:
		return false
	var specific_energy := 0.5 * vel.length_squared() - MU_SUN / r
	return specific_energy < 0.0


func _estimate_transfer_duration(r1: float, r2: float, base_speed: float) -> float:
	var a_transfer := maxf((r1 + r2) * 0.5, 1e-3)
	var hohmann_time := PI * sqrt(pow(a_transfer, 3.0) / MU_SUN)
	var speed_factor := clampf(1.2 - base_speed * 0.2, 0.55, 1.15)
	return maxf(1.0, hohmann_time * speed_factor)


func _circular_velocity_at_position(pos: Vector3) -> Vector3:
	var r := pos.length()
	if r <= 1e-6:
		return Vector3.ZERO
	var speed := sqrt(MU_SUN / r)
	var radial := pos / r
	# Prograde tangent in XZ plane.
	var tangent := Vector3(-radial.z, 0.0, radial.x)
	return tangent * speed


func _solve_lambert_universal(r1: Vector3, r2: Vector3, dt: float, mu: float) -> Dictionary:
	var r1_mag := r1.length()
	var r2_mag := r2.length()
	if r1_mag < 1e-6 or r2_mag < 1e-6 or dt <= 0.0:
		return {"ok": false}

	var cos_dnu := clampf(r1.dot(r2) / (r1_mag * r2_mag), -1.0, 1.0)
	var cross_y := r1.cross(r2).y
	var sin_dnu := sqrt(maxf(1.0 - cos_dnu * cos_dnu, 0.0))
	if cross_y < 0.0:
		sin_dnu = -sin_dnu

	var denom := maxf(1.0 - cos_dnu, 1e-8)
	var a_term := sin_dnu * sqrt(r1_mag * r2_mag / denom)
	if absf(a_term) < 1e-8:
		return {"ok": false}

	var z_low := -4.0 * PI * PI
	var z_high := 4.0 * PI * PI
	var z := 0.0
	var y := 0.0
	var x := 0.0

	for _i in range(60):
		var c2 := _stumpff_c2(z)
		var c3 := _stumpff_c3(z)
		if c2 <= 1e-12:
			z = (z + z_high) * 0.5
			continue

		y = r1_mag + r2_mag + a_term * ((z * c3 - 1.0) / sqrt(c2))
		if y <= 1e-8:
			z = (z + z_high) * 0.5
			continue

		x = sqrt(y / c2)
		var dt_est := (x * x * x * c3 + a_term * sqrt(y)) / sqrt(mu)

		if absf(dt_est - dt) < 1e-4:
			break

		if dt_est <= dt:
			z_low = z
		else:
			z_high = z
		z = (z_low + z_high) * 0.5

	var g := a_term * sqrt(y / mu)
	if absf(g) < 1e-8:
		return {"ok": false}
	var f := 1.0 - y / r1_mag
	var gdot := 1.0 - y / r2_mag
	var v1 := (r2 - r1 * f) / g
	var v2 := (r2 * gdot - r1) / g

	return {
		"ok": true,
		"v1": v1,
		"v2": v2,
	}


func _stumpff_c2(z: float) -> float:
	if z > 1e-8:
		var sz := sqrt(z)
		return (1.0 - cos(sz)) / z
	if z < -1e-8:
		var sz := sqrt(-z)
		return (1.0 - cosh(sz)) / z
	return 0.5


func _stumpff_c3(z: float) -> float:
	if z > 1e-8:
		var sz := sqrt(z)
		return (sz - sin(sz)) / (sz * sz * sz)
	if z < -1e-8:
		var sz := sqrt(-z)
		return (sinh(sz) - sz) / (sz * sz * sz)
	return 1.0 / 6.0
