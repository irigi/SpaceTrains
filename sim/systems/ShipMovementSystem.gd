class_name ShipMovementSystem
extends RefCounted
## Handles ship travel between stations using straight-line movement.
## Ships accelerate, cruise, and decelerate (simplified 3-phase).

const DOCKING_RADIUS: float = 0.5  # Distance at which docking triggers
const AU_SCALE: float = 50.0

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
				_update_arriving(world, ship, dt)
			"docking":
				_update_docking(world, ship, dt)

func _update_docked(world: WorldState, ship: WorldState.ShipData) -> void:
	# Update position to match station (station moves with planet)
	if ship.docked_station_id >= 0 and ship.docked_station_id in world.stations:
		var station = world.stations[ship.docked_station_id]
		if station.body_id in world.bodies:
			var body_pos = world.bodies[station.body_id].get_position_at_time(world.sim_time) * AU_SCALE
			ship.position = station.get_world_position(body_pos)

func _update_launching(world: WorldState, ship: WorldState.ShipData, dt: float) -> void:
	# Brief launch phase then transition to traveling
	ship.travel_progress += dt / 5.0  # 5 minutes to undock
	if ship.travel_progress >= 1.0:
		ship.state = "traveling"
		ship.travel_progress = 0.0

		# Compute travel parameters
		if ship.target_station_id >= 0 and ship.target_station_id in world.stations:
			var dest_station = world.stations[ship.target_station_id]
			if dest_station.body_id in world.bodies:
				var body_pos = world.bodies[dest_station.body_id].get_position_at_time(world.sim_time) * AU_SCALE
				ship.travel_destination = dest_station.get_world_position(body_pos)

		ship.travel_origin = ship.position
		var distance = ship.travel_origin.distance_to(ship.travel_destination)
		ship.travel_duration = maxf(distance / ship.base_speed, 1.0)

func _update_traveling(world: WorldState, ship: WorldState.ShipData, dt: float) -> void:
	if ship.travel_duration <= 0.0:
		ship.state = "arriving"
		return

	ship.travel_progress += dt / ship.travel_duration

	# Recalculate destination (target station moves with its planet)
	if ship.target_station_id >= 0 and ship.target_station_id in world.stations:
		var dest_station = world.stations[ship.target_station_id]
		if dest_station.body_id in world.bodies:
			var body_pos = world.bodies[dest_station.body_id].get_position_at_time(world.sim_time) * AU_SCALE
			ship.travel_destination = dest_station.get_world_position(body_pos)

	# Smooth interpolation with ease-in/ease-out
	var t = clampf(ship.travel_progress, 0.0, 1.0)
	var smooth_t = _smoothstep(t)
	ship.position = ship.travel_origin.lerp(ship.travel_destination, smooth_t)

	# Fuel consumption
	ship.fuel -= dt * 0.01 * ship.base_speed

	if ship.travel_progress >= 1.0:
		ship.state = "arriving"
		ship.travel_progress = 0.0

func _update_arriving(_world: WorldState, ship: WorldState.ShipData, _dt: float) -> void:
	# Snap to destination, transition to docking
	ship.position = ship.travel_destination
	ship.state = "docking"
	ship.travel_progress = 0.0

func _update_docking(world: WorldState, ship: WorldState.ShipData, dt: float) -> void:
	ship.travel_progress += dt / 3.0  # 3 minutes to dock
	if ship.travel_progress >= 1.0:
		_dock_ship(world, ship)

func _dock_ship(world: WorldState, ship: WorldState.ShipData) -> void:
	ship.state = "docked"
	ship.docked_station_id = ship.target_station_id
	ship.target_station_id = -1
	ship.travel_progress = 0.0

	if ship.docked_station_id in world.stations:
		var station = world.stations[ship.docked_station_id]
		if ship.id not in station.docked_ship_ids:
			station.docked_ship_ids.append(ship.id)

		# Deliver cargo if on cargo mission
		if ship.mission_type == "cargo_delivery" and ship.docked_station_id == ship.mission_dest_id:
			_deliver_cargo(world, ship, station)

		EventBus.ship_docked.emit(ship.id, ship.docked_station_id)
		EventBus.emit_log("trade", "%s docked at %s" % [ship.entity_name, station.entity_name])

	# Clear mission
	ship.mission_type = ""
	ship.mission_commodity = ""
	ship.mission_amount = 0
	ship.mission_source_id = -1
	ship.mission_dest_id = -1

	# Refuel
	ship.fuel = ship.fuel_max

func _deliver_cargo(_world: WorldState, ship: WorldState.ShipData, station: WorldState.StationData) -> void:
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
	# Hermite smoothstep for ease-in/ease-out
	return t * t * (3.0 - 2.0 * t)
