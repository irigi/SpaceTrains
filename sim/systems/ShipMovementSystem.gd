class_name ShipMovementSystem
extends RefCounted
## Handles ship travel and trajectory planning.
## Supports pluggable trajectory classes and fast replanning for moving targets.

const DOCKING_RADIUS: float = 0.5
const AU_SCALE: float = 50.0
const LAUNCH_PHASE_DURATION: float = 5.0
const DOCKING_PHASE_DURATION: float = 3.0
const TRAJECTORY_REPLAN_INTERVAL: float = 4.0
const LINEAR_FUEL_PER_DISTANCE: float = 0.02

func update(world: WorldState, dt: float) -> void:
	for ship_id in world.ships:
		var ship: WorldState.ShipData = world.ships[ship_id]
		match ship.state:
			"docked":
				_update_docked(world, ship)
			"launching":
				_update_launching(world, ship, dt)
			"traveling":
				_update_traveling(world, ship, dt)
			"arriving":
				_update_arriving(ship)
			"docking":
				_update_docking(world, ship, dt)

func _update_docked(world: WorldState, ship: WorldState.ShipData) -> void:
	if ship.docked_station_id >= 0 and ship.docked_station_id in world.stations:
		var station = world.stations[ship.docked_station_id]
		if station.body_id in world.bodies:
			var body_pos = world.bodies[station.body_id].get_position_at_time(world.sim_time) * AU_SCALE
			ship.position = station.get_world_position(body_pos)

func _update_launching(world: WorldState, ship: WorldState.ShipData, dt: float) -> void:
	ship.travel_progress += dt / LAUNCH_PHASE_DURATION
	if ship.travel_progress < 1.0:
		return

	ship.state = "traveling"
	ship.travel_progress = 0.0
	ship.insertion_burn_done = false
	ship.final_burn_done = false
	ship.trajectory_plan_age = 0.0

	if not _plan_trajectory(world, ship):
		ship.state = "arriving"

func _update_traveling(world: WorldState, ship: WorldState.ShipData, dt: float) -> void:
	if ship.travel_duration <= 0.0:
		ship.state = "arriving"
		return

	ship.trajectory_plan_age += dt
	if ship.trajectory_plan_age >= TRAJECTORY_REPLAN_INTERVAL:
		ship.trajectory_plan_age = 0.0
		_plan_trajectory(world, ship)

	ship.travel_progress = minf(ship.travel_progress + dt / ship.travel_duration, 1.0)
	ship.position = _sample_trajectory_position(ship)

	if ship.trajectory_class == "kepler_transfer":
		var cruise_distance = maxf(ship.travel_origin.distance_to(ship.travel_destination), 0.001)
		ship.fuel = maxf(0.0, ship.fuel - dt * cruise_distance * 0.0006)
	else:
		ship.fuel = maxf(0.0, ship.fuel - dt * LINEAR_FUEL_PER_DISTANCE * ship.base_speed)

	if ship.travel_progress >= 1.0:
		ship.state = "arriving"
		ship.travel_progress = 0.0

func _update_arriving(ship: WorldState.ShipData) -> void:
	ship.position = ship.travel_destination
	ship.state = "docking"
	ship.travel_progress = 0.0

func _update_docking(world: WorldState, ship: WorldState.ShipData, dt: float) -> void:
	ship.travel_progress += dt / DOCKING_PHASE_DURATION
	if ship.travel_progress >= 1.0:
		_dock_ship(world, ship)

func _plan_trajectory(world: WorldState, ship: WorldState.ShipData) -> bool:
	var origin = ship.position
	var predicted_target = _estimate_target_position(world, ship)
	if predicted_target == Vector3.INF:
		return false

	ship.travel_origin = origin
	ship.travel_destination = predicted_target

	match ship.trajectory_class:
		"kepler_transfer":
			return _plan_kepler_transfer(ship, origin, predicted_target)
		_:
			return _plan_linear_transfer(ship, origin, predicted_target)

func _estimate_target_position(world: WorldState, ship: WorldState.ShipData) -> Vector3:
	if ship.target_ship_id >= 0 and ship.target_ship_id in world.ships:
		var target_ship: WorldState.ShipData = world.ships[ship.target_ship_id]
		var relative = target_ship.position - ship.position
		var relative_speed = maxf(ship.base_speed + target_ship.velocity.length(), 0.2)
		var eta = clampf(relative.length() / relative_speed, 1.0, 180.0)
		return target_ship.position + target_ship.velocity * eta

	if ship.target_station_id >= 0 and ship.target_station_id in world.stations:
		var dest_station = world.stations[ship.target_station_id]
		if dest_station.body_id in world.bodies:
			var body = world.bodies[dest_station.body_id]
			var initial = dest_station.get_world_position(body.get_position_at_time(world.sim_time) * AU_SCALE)
			var rough_eta = maxf(ship.position.distance_to(initial) / maxf(ship.base_speed, 0.3), 2.0)
			var predicted_body = body.get_position_at_time(world.sim_time + rough_eta) * AU_SCALE
			return dest_station.get_world_position(predicted_body)

	return Vector3.INF

func _plan_linear_transfer(ship: WorldState.ShipData, origin: Vector3, destination: Vector3) -> bool:
	var distance = origin.distance_to(destination)
	ship.travel_duration = maxf(distance / maxf(ship.base_speed, 0.1), 1.0)
	ship.trajectory_points = [origin, destination]
	ship.insertion_burn_cost = 0.0
	ship.final_burn_cost = 0.0
	ship.insertion_burn_done = true
	ship.final_burn_done = true
	return true

func _plan_kepler_transfer(ship: WorldState.ShipData, origin: Vector3, destination: Vector3) -> bool:
	var chord = destination - origin
	var distance = maxf(chord.length(), 0.001)
	var direction = chord.normalized()
	var normal = Vector3.UP
	var side = direction.cross(normal)
	if side.length_squared() < 0.0001:
		side = direction.cross(Vector3.FORWARD)
	side = side.normalized()

	var arc_height = distance * 0.28
	var midpoint = (origin + destination) * 0.5
	var apoapsis = midpoint + side * arc_height

	ship.trajectory_points = [origin, apoapsis, destination]

	var insertion_dv = 0.35 + 0.15 * (distance / maxf(AU_SCALE, 1.0))
	var final_dv = 0.30 + 0.10 * (distance / maxf(AU_SCALE, 1.0))
	ship.insertion_burn_cost = insertion_dv * 10.0
	ship.final_burn_cost = final_dv * 10.0

	if not ship.insertion_burn_done:
		if ship.fuel < ship.insertion_burn_cost:
			return false
		ship.fuel -= ship.insertion_burn_cost
		ship.insertion_burn_done = true

	if not ship.final_burn_done and ship.fuel < ship.final_burn_cost:
		return false

	var estimated_orbit_path = distance + arc_height * 0.8
	ship.travel_duration = maxf(estimated_orbit_path / maxf(ship.base_speed * 0.9, 0.1), 2.0)
	return true

func _sample_trajectory_position(ship: WorldState.ShipData) -> Vector3:
	if ship.trajectory_points.size() < 2:
		return ship.travel_origin.lerp(ship.travel_destination, clampf(ship.travel_progress, 0.0, 1.0))

	if ship.trajectory_points.size() == 2:
		return ship.trajectory_points[0].lerp(ship.trajectory_points[1], _smoothstep(ship.travel_progress))

	# Quadratic Bezier sampling for 3 control points (cheap and smooth)
	var t = _smoothstep(ship.travel_progress)
	var a = ship.trajectory_points[0].lerp(ship.trajectory_points[1], t)
	var b = ship.trajectory_points[1].lerp(ship.trajectory_points[2], t)
	return a.lerp(b, t)

func _dock_ship(world: WorldState, ship: WorldState.ShipData) -> void:
	if ship.trajectory_class == "kepler_transfer" and not ship.final_burn_done:
		if ship.fuel >= ship.final_burn_cost:
			ship.fuel -= ship.final_burn_cost
			ship.final_burn_done = true
		else:
			ship.state = "arriving"
			ship.travel_progress = 0.9
			return

	ship.state = "docked"
	ship.docked_station_id = ship.target_station_id
	ship.target_station_id = -1
	ship.target_ship_id = -1
	ship.travel_progress = 0.0
	ship.trajectory_points.clear()
	ship.trajectory_plan_age = 0.0

	if ship.docked_station_id in world.stations:
		var station = world.stations[ship.docked_station_id]
		if ship.id not in station.docked_ship_ids:
			station.docked_ship_ids.append(ship.id)

		if ship.mission_type == "cargo_delivery" and ship.docked_station_id == ship.mission_dest_id:
			_deliver_cargo(ship, station)

		EventBus.ship_docked.emit(ship.id, ship.docked_station_id)
		EventBus.emit_log("trade", "%s docked at %s" % [ship.entity_name, station.entity_name])

	ship.mission_type = ""
	ship.mission_commodity = ""
	ship.mission_amount = 0
	ship.mission_source_id = -1
	ship.mission_dest_id = -1
	ship.fuel = ship.fuel_max

func _deliver_cargo(ship: WorldState.ShipData, station: WorldState.StationData) -> void:
	for commodity in ship.cargo:
		var amount = ship.cargo[commodity]
		if commodity in station.inventory:
			station.inventory[commodity] += amount
		else:
			station.inventory[commodity] = amount
		EventBus.cargo_delivered.emit(ship.id, station.id, commodity, amount)
		EventBus.emit_log("trade", "%s delivered %d %s to %s" % [ship.entity_name, amount, commodity, station.entity_name])
	ship.cargo.clear()

func _smoothstep(t: float) -> float:
	var clamped = clampf(t, 0.0, 1.0)
	return clamped * clamped * (3.0 - 2.0 * clamped)
