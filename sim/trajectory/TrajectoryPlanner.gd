class_name TrajectoryPlanner
extends RefCounted
## Plans ship trajectories.
##
## Linear trajectories are intentionally disabled for now. All mission travel
## should use Keplerian transfers (Hohmann-like first, Lambert fallback).

const AU_SCALE: float = 50.0
const EARTH_ORBIT_RADIUS_GU: float = 1.0 * AU_SCALE
const EARTH_ORBIT_PERIOD_MIN: float = 525600.0
const MU_SUN: float = (TAU * TAU) * pow(EARTH_ORBIT_RADIUS_GU, 3.0) / pow(EARTH_ORBIT_PERIOD_MIN, 2.0)

const MIN_TRANSFER_DURATION_MIN: float = 5.0
const MIN_ELLIPTIC_ENERGY_EPS: float = 1e-8


func plan_station_transfer(
	origin: Vector3,
	destination: Vector3,
	start_time: float,
	base_speed: float,
	origin_body_pos: Vector3,
	dest_body_pos: Vector3
) -> Trajectory:
	var hohmann := _build_hohmann_like_trajectory(origin, start_time, base_speed, origin_body_pos, dest_body_pos)
	if hohmann != null:
		return hohmann

	# Fallback: Lambert with Hohmann-like duration target. Still Keplerian.
	var lambert_duration := _estimate_transfer_duration(origin_body_pos.length(), dest_body_pos.length(), base_speed)
	var solve := _solve_lambert_universal(origin, destination, lambert_duration, MU_SUN)
	if solve.get("ok", false):
		var v1: Vector3 = solve["v1"]
		var v2: Vector3 = solve["v2"]
		if _is_elliptic_state(origin, v1):
			var dep_dv := (v1 - _circular_velocity_at_position(origin_body_pos)).length()
			var arr_dv := (v2 - _circular_velocity_at_position(dest_body_pos)).length()
			return KeplerianTrajectory.create(
				origin,
				v1,
				start_time,
				start_time + lambert_duration,
				MU_SUN,
				dep_dv,
				arr_dv
			)

	# Last-resort Keplerian: circular coasting orbit at current radius.
	# This preserves the "no linear" rule even in pathological geometries.
	return _build_circular_coast_trajectory(origin, start_time, base_speed)


func _build_hohmann_like_trajectory(
	origin: Vector3,
	start_time: float,
	base_speed: float,
	origin_body_pos: Vector3,
	dest_body_pos: Vector3
) -> KeplerianTrajectory:
	var r1 := maxf(origin_body_pos.length(), 1e-3)
	var r2 := maxf(dest_body_pos.length(), 1e-3)
	var a_transfer := maxf((r1 + r2) * 0.5, 1e-3)

	var hohmann_duration := PI * sqrt(pow(a_transfer, 3.0) / MU_SUN)
	var speed_factor := clampf(1.05 - base_speed * 0.03, 0.85, 1.10)
	var transfer_duration := maxf(MIN_TRANSFER_DURATION_MIN, hohmann_duration * speed_factor)

	var radial := Vector3(origin.x, 0.0, origin.z)
	var radial_len := radial.length()
	if radial_len <= 1e-6:
		return null
	radial /= radial_len
	var tangent := Vector3(-radial.z, 0.0, radial.x)

	var transfer_speed := sqrt(maxf(MU_SUN * (2.0 / r1 - 1.0 / a_transfer), 0.0))
	if transfer_speed <= 1e-6:
		return null

	var departure_velocity := tangent * transfer_speed
	if not _is_elliptic_state(origin, departure_velocity):
		return null

	var dep_dv := absf(transfer_speed - sqrt(MU_SUN / r1))
	var arr_speed := sqrt(maxf(MU_SUN * (2.0 / r2 - 1.0 / a_transfer), 0.0))
	var arr_dv := absf(arr_speed - sqrt(MU_SUN / r2))

	return KeplerianTrajectory.create(
		origin,
		departure_velocity,
		start_time,
		start_time + transfer_duration,
		MU_SUN,
		dep_dv,
		arr_dv
	)


func _build_circular_coast_trajectory(origin: Vector3, start_time: float, _base_speed: float) -> KeplerianTrajectory:
	var radius := maxf(origin.length(), 1e-3)
	var tangent := Vector3(-origin.z, 0.0, origin.x)
	if tangent.length() <= 1e-6:
		tangent = Vector3(0.0, 0.0, 1.0)
	tangent = tangent.normalized()
	var circ_speed := sqrt(MU_SUN / radius)
	var period := TAU * sqrt(pow(radius, 3.0) / MU_SUN)
	return KeplerianTrajectory.create(
		origin,
		tangent * circ_speed,
		start_time,
		start_time + maxf(period, MIN_TRANSFER_DURATION_MIN),
		MU_SUN,
		0.0,
		0.0
	)


func _is_elliptic_state(pos: Vector3, vel: Vector3) -> bool:
	var r := pos.length()
	if r <= 1e-6:
		return false
	var specific_energy := 0.5 * vel.length_squared() - MU_SUN / r
	return specific_energy < -MIN_ELLIPTIC_ENERGY_EPS


func _estimate_transfer_duration(r1: float, r2: float, base_speed: float) -> float:
	var a_transfer := maxf((r1 + r2) * 0.5, 1e-3)
	var hohmann_time := PI * sqrt(pow(a_transfer, 3.0) / MU_SUN)
	var speed_factor := clampf(1.05 - base_speed * 0.03, 0.85, 1.10)
	return maxf(MIN_TRANSFER_DURATION_MIN, hohmann_time * speed_factor)


func _circular_velocity_at_position(pos: Vector3) -> Vector3:
	var r := pos.length()
	if r <= 1e-6:
		return Vector3.ZERO
	var speed := sqrt(MU_SUN / r)
	var radial := pos / r
	var tangent := Vector3(-radial.z, 0.0, radial.x)
	return tangent * speed


func _solve_lambert_universal(r1: Vector3, r2: Vector3, dt: float, mu: float) -> Dictionary:
	var short_way := _solve_lambert_branch(r1, r2, dt, mu, true)
	if short_way.get("ok", false):
		return short_way
	return _solve_lambert_branch(r1, r2, dt, mu, false)


func _solve_lambert_branch(r1: Vector3, r2: Vector3, dt: float, mu: float, prograde_short_way: bool) -> Dictionary:
	var r1_mag := r1.length()
	var r2_mag := r2.length()
	if r1_mag < 1e-6 or r2_mag < 1e-6 or dt <= 0.0:
		return {"ok": false}

	var cos_dnu := clampf(r1.dot(r2) / (r1_mag * r2_mag), -1.0, 1.0)
	var sin_dnu_mag := sqrt(maxf(1.0 - cos_dnu * cos_dnu, 0.0))
	if sin_dnu_mag <= 1e-8:
		return {"ok": false}
	var sin_dnu := sin_dnu_mag if prograde_short_way else -sin_dnu_mag

	var denom := maxf(1.0 - cos_dnu, 1e-8)
	var a_term := sin_dnu * sqrt(r1_mag * r2_mag / denom)
	if absf(a_term) < 1e-8:
		return {"ok": false}

	var z_low := -4.0 * PI * PI
	var z_high := 4.0 * PI * PI
	var z := 0.0
	var y := 0.0
	var x := 0.0
	var converged := false

	for _i in range(70):
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
			converged = true
			break

		if dt_est <= dt:
			z_low = z
		else:
			z_high = z
		z = (z_low + z_high) * 0.5

	if not converged:
		return {"ok": false}

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
