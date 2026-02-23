class_name ShipMovementSystem
extends RefCounted
## Handles ship travel between stations via pluggable Trajectory objects.
##
## The state machine (docked → launching → traveling → arriving → docking)
## is unchanged from Phase 0. The key difference is that during "traveling"
## the ship's position and velocity are computed by its Trajectory object
## rather than a direct lerp, enabling different physics models to be
## swapped in without touching this file.
##
## Phase 1: LinearTrajectory (smoothstep straight-line, identical to original).
## Phase 2+: KeplerianTrajectory, LowThrustTrajectory, etc.

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
	# Update position to match station (station moves with planet).
	if ship.docked_station_id >= 0 and ship.docked_station_id in world.stations:
		var station := world.stations[ship.docked_station_id]
		if station.body_id in world.bodies:
			var body_pos := world.bodies[station.body_id].get_position_at_time(world.sim_time) * AU_SCALE
			ship.position = station.get_world_position(body_pos)


func _update_launching(world: WorldState, ship: WorldState.ShipData, dt: float) -> void:
	# Brief undocking phase, then create trajectory and transition to traveling.
	ship.travel_progress += dt / 5.0  # 5 sim-minutes to undock
	if ship.travel_progress >= 1.0:
		ship.state = "traveling"
		ship.travel_progress = 0.0

		# Compute destination at this moment (stations orbit with their planet).
		if ship.target_station_id >= 0 and ship.target_station_id in world.stations:
			var dest_station := world.stations[ship.target_station_id]
			if dest_station.body_id in world.bodies:
				var body_pos := world.bodies[dest_station.body_id].get_position_at_time(world.sim_time) * AU_SCALE
				ship.travel_destination = dest_station.get_world_position(body_pos)

		ship.travel_origin = ship.position
		var distance := ship.travel_origin.distance_to(ship.travel_destination)
		ship.travel_duration = maxf(distance / ship.base_speed, 1.0)

		# Create the trajectory object. Phase 1 always uses LinearTrajectory.
		ship.trajectory = LinearTrajectory.create(
			ship.travel_origin,
			ship.travel_destination,
			world.sim_time,
			ship.travel_duration
		)


func _update_traveling(world: WorldState, ship: WorldState.ShipData, dt: float) -> void:
	# Guard: if no trajectory (e.g. loaded from a legacy save mid-travel),
	# reconstruct a LinearTrajectory from the stored travel fields.
	if ship.trajectory == null:
		var elapsed := ship.travel_progress * ship.travel_duration
		ship.trajectory = LinearTrajectory.create(
			ship.travel_origin,
			ship.travel_destination,
			world.sim_time - elapsed,
			ship.travel_duration
		)

	# Recalculate destination every tick — target station moves with its planet.
	if ship.target_station_id >= 0 and ship.target_station_id in world.stations:
		var dest_station := world.stations[ship.target_station_id]
		if dest_station.body_id in world.bodies:
			var body_pos := world.bodies[dest_station.body_id].get_position_at_time(world.sim_time) * AU_SCALE
			var new_dest := dest_station.get_world_position(body_pos)
			ship.travel_destination = new_dest
			ship.trajectory.update_destination(new_dest)

	# Position and velocity come from the trajectory object.
	ship.position = ship.trajectory.get_position_at_time(world.sim_time)
	ship.velocity = ship.trajectory.get_velocity_at_time(world.sim_time)

	# Keep travel_progress in sync for UI queries (ETA, progress bar).
	ship.travel_progress = ship.trajectory.get_progress(world.sim_time)

	# Fuel consumption — unchanged from Phase 0, will be replaced in Phase 3.
	ship.fuel -= dt * 0.01 * ship.base_speed
	ship.fuel = maxf(ship.fuel, 0.0)

	if ship.trajectory.is_complete(world.sim_time):
		ship.state = "arriving"
		ship.travel_progress = 0.0


func _update_arriving(_world: WorldState, ship: WorldState.ShipData, _dt: float) -> void:
	# Snap to destination and begin the docking sequence.
	ship.position = ship.travel_destination
	ship.velocity = Vector3.ZERO
	ship.state = "docking"
	ship.travel_progress = 0.0


func _update_docking(world: WorldState, ship: WorldState.ShipData, dt: float) -> void:
	ship.travel_progress += dt / 3.0  # 3 sim-minutes to dock
	if ship.travel_progress >= 1.0:
		_dock_ship(world, ship)


func _dock_ship(world: WorldState, ship: WorldState.ShipData) -> void:
	ship.state = "docked"
	ship.docked_station_id = ship.target_station_id
	ship.target_station_id = -1
	ship.travel_progress = 0.0
	ship.trajectory = null  # Clear trajectory — ship is no longer in transit.

	if ship.docked_station_id in world.stations:
		var station := world.stations[ship.docked_station_id]
		if ship.id not in station.docked_ship_ids:
			station.docked_ship_ids.append(ship.id)

		# Deliver cargo if on a cargo mission to this station.
		if ship.mission_type == "cargo_delivery" and ship.docked_station_id == ship.mission_dest_id:
			_deliver_cargo(world, ship, station)

		EventBus.ship_docked.emit(ship.id, ship.docked_station_id)
		EventBus.emit_log("trade", "%s docked at %s" % [ship.entity_name, station.entity_name])

	# Clear mission fields.
	ship.mission_type = ""
	ship.mission_commodity = ""
	ship.mission_amount = 0
	ship.mission_source_id = -1
	ship.mission_dest_id = -1

	# Refuel at station.
	ship.fuel = ship.fuel_max


func _deliver_cargo(_world: WorldState, ship: WorldState.ShipData, station: WorldState.StationData) -> void:
	for commodity in ship.cargo:
		var amount: int = ship.cargo[commodity]
		if commodity in station.inventory:
			station.inventory[commodity] += amount
		else:
			station.inventory[commodity] = amount
		EventBus.cargo_delivered.emit(ship.id, station.id, commodity, amount)
		EventBus.emit_log("trade", "%s delivered %d %s to %s" % [ship.entity_name, amount, commodity, station.entity_name])
	ship.cargo.clear()
