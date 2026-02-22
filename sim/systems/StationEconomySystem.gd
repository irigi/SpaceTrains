class_name StationEconomySystem
extends RefCounted
## Handles station production and consumption of commodities.

const PRODUCTION_INTERVAL: float = 60.0  # Produce every 60 sim-minutes

func update(world: WorldState, dt: float) -> void:
	for station_id in world.stations:
		var station: WorldState.StationData = world.stations[station_id]
		station.production_timer += dt
		if station.production_timer >= PRODUCTION_INTERVAL:
			station.production_timer -= PRODUCTION_INTERVAL
			_run_production(world, station)
			_run_consumption(world, station)

func _run_production(world: WorldState, station: WorldState.StationData) -> void:
	for module in station.modules:
		match module:
			"FARM":
				_produce(station, "FOOD", 5 + station.population / 50)
			"REFINERY":
				if station.inventory.get("METALS", 0) >= 3:
					station.inventory["METALS"] -= 3
					_produce(station, "FUEL", 5)
			"LIFE_SUPPORT":
				_produce(station, "OXYGEN", 2)
				_produce(station, "WATER", 2)

func _run_consumption(world: WorldState, station: WorldState.StationData) -> void:
	# Population consumes resources
	var food_need = max(1, station.population / 100)
	var water_need = max(1, station.population / 100)
	var oxygen_need = max(1, station.population / 150)

	_consume(station, "FOOD", food_need)
	_consume(station, "WATER", water_need)
	_consume(station, "OXYGEN", oxygen_need)

func _produce(station: WorldState.StationData, commodity: String, amount: int) -> void:
	if commodity in station.inventory:
		station.inventory[commodity] += amount
	else:
		station.inventory[commodity] = amount
	EventBus.station_production.emit(station.id, commodity, amount)

func _consume(station: WorldState.StationData, commodity: String, amount: int) -> void:
	if commodity in station.inventory:
		station.inventory[commodity] -= amount
		if station.inventory[commodity] < 0:
			station.inventory[commodity] = 0
			EventBus.station_shortage.emit(station.id, commodity)
			EventBus.emit_log("economy", "%s is short on %s!" % [station.entity_name, commodity])
	else:
		station.inventory[commodity] = 0
		EventBus.station_shortage.emit(station.id, commodity)
