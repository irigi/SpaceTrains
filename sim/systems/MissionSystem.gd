class_name MissionSystem
extends RefCounted
## Creates cargo missions for docked ships. Stations identify needs and dispatch freighters.

const MISSION_CHECK_INTERVAL: float = 30.0  # Check every 30 sim-minutes
const AU_SCALE: float = 50.0

var mission_timer: float = 0.0

func update(world: WorldState, dt: float) -> void:
	mission_timer += dt
	if mission_timer < MISSION_CHECK_INTERVAL:
		return
	mission_timer -= MISSION_CHECK_INTERVAL

	# For each station, check for docked idle ships and assign cargo missions
	for station_id in world.stations:
		var station: WorldState.StationData = world.stations[station_id]
		_try_create_cargo_missions(world, station)

func _try_create_cargo_missions(world: WorldState, station: WorldState.StationData) -> void:
	# Find idle docked ships at this station
	var idle_ships: Array = []
	for ship_id in station.docked_ship_ids:
		if ship_id in world.ships:
			var candidate = world.ships[ship_id]
			if candidate.state == "docked" and candidate.mission_type == "":
				idle_ships.append(candidate)

	if idle_ships.is_empty():
		return

	# Find commodities this station has in surplus
	var surplus_commodities: Array = []
	for commodity in station.inventory:
		if station.inventory[commodity] > 30:  # Threshold for "surplus"
			surplus_commodities.append(commodity)

	if surplus_commodities.is_empty():
		return

	# Find a destination station that needs something
	var dest_station = _find_needy_station(world, station, surplus_commodities)
	if dest_station == null:
		return

	# Assign mission to first idle ship
	var ship = idle_ships[0]
	var commodity = _pick_best_commodity(station, dest_station, surplus_commodities)
	@warning_ignore("integer_division")
	var amount = mini(ship.cargo_capacity, station.inventory.get(commodity, 0) / 2)
	amount = maxi(amount, 1)

	_dispatch_cargo_mission(world, ship, station, dest_station, commodity, amount)

func _find_needy_station(world: WorldState, source: WorldState.StationData, surplus: Array) -> WorldState.StationData:
	var best_station: WorldState.StationData = null
	var best_need: int = 0

	for station_id in world.stations:
		if station_id == source.id:
			continue
		var other = world.stations[station_id]
		for commodity in surplus:
			var other_amount = other.inventory.get(commodity, 0)
			if other_amount < 20:  # Threshold for "needy"
				var need = 20 - other_amount
				if need > best_need:
					best_need = need
					best_station = other

	return best_station

func _pick_best_commodity(_source: WorldState.StationData, dest: WorldState.StationData, surplus: Array) -> String:
	var best_commodity: String = surplus[0]
	var best_deficit: int = 0
	for commodity in surplus:
		var deficit = 30 - dest.inventory.get(commodity, 0)
		if deficit > best_deficit:
			best_deficit = deficit
			best_commodity = commodity
	return best_commodity

func _dispatch_cargo_mission(world: WorldState, ship: WorldState.ShipData, source: WorldState.StationData, dest: WorldState.StationData, commodity: String, amount: int) -> void:
	# Load cargo from station
	var actual_amount = mini(amount, source.inventory.get(commodity, 0))
	if actual_amount <= 0:
		return

	source.inventory[commodity] -= actual_amount
	ship.cargo[commodity] = actual_amount

	# Set mission
	ship.mission_type = "cargo_delivery"
	ship.mission_commodity = commodity
	ship.mission_amount = actual_amount
	ship.mission_source_id = source.id
	ship.mission_dest_id = dest.id
	ship.target_station_id = dest.id

	# Undock
	ship.state = "launching"
	ship.travel_progress = 0.0
	if ship.id in source.docked_ship_ids:
		source.docked_ship_ids.erase(ship.id)

	# Set travel origin
	if source.body_id in world.bodies:
		var body_pos: Vector3 = world.bodies[source.body_id].get_position_at_time(world.sim_time) * AU_SCALE
		ship.travel_origin = source.get_world_position(body_pos)
		ship.position = ship.travel_origin

	EventBus.ship_launched.emit(ship.id, source.id)
	EventBus.emit_log("trade", "%s launched from %s carrying %d %s → %s" % [
		ship.entity_name, source.entity_name, actual_amount, commodity, dest.entity_name
	])
