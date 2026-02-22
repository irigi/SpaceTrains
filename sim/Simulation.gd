class_name Simulation
extends Node
## Main simulation controller. Owns WorldState and runs tick-based systems.

const TICK_DELTA: float = 0.2  # 0.2 sim-minutes per tick for smoother movement at low speeds
const AU_SCALE: float = 50.0   # 1 AU = 50 Godot units for rendering

var world: WorldState
var paused: bool = false
var speed_multiplier: float = 1.0
var accumulated_time: float = 0.0
var max_ticks_per_frame: int = 400

# Systems
var orbit_system: OrbitSystem
var ship_movement_system: ShipMovementSystem
var station_economy_system: StationEconomySystem
var mission_system: MissionSystem

func _ready() -> void:
	orbit_system = OrbitSystem.new()
	ship_movement_system = ShipMovementSystem.new()
	station_economy_system = StationEconomySystem.new()
	mission_system = MissionSystem.new()

	if world == null:
		world = WorldState.new()
		_setup_default_world()

func _setup_default_world() -> void:
	_create_factions()
	_create_celestial_bodies()
	_create_stations()
	_spawn_initial_ships()

func _process(delta: float) -> void:
	if paused:
		return

	accumulated_time += delta * speed_multiplier * 60.0  # Convert real seconds to sim-minutes
	var ticks_this_frame := 0

	while accumulated_time >= TICK_DELTA and ticks_this_frame < max_ticks_per_frame:
		_tick(TICK_DELTA)
		accumulated_time -= TICK_DELTA
		ticks_this_frame += 1

func _tick(dt: float) -> void:
	world.sim_time += dt

	# Update systems in order
	orbit_system.update(world, dt)
	station_economy_system.update(world, dt)
	mission_system.update(world, dt)
	ship_movement_system.update(world, dt)

func set_paused(p: bool) -> void:
	paused = p
	if paused:
		EventBus.simulation_paused.emit()
	else:
		EventBus.simulation_resumed.emit()

func set_speed(mult: float) -> void:
	speed_multiplier = mult
	EventBus.simulation_speed_changed.emit(mult)

func get_body_position(body_id: int) -> Vector3:
	if body_id in world.bodies:
		var body = world.bodies[body_id]
		return body.get_position_at_time(world.sim_time) * AU_SCALE
	return Vector3.ZERO

func get_station_position(station_id: int) -> Vector3:
	if station_id in world.stations:
		var station = world.stations[station_id]
		var body_pos = get_body_position(station.body_id)
		return station.get_world_position(body_pos)
	return Vector3.ZERO

# ============================================================
# World Setup
# ============================================================

func _create_factions() -> void:
	var sol_fed := WorldState.FactionData.new()
	sol_fed.name = "Sol Federation"
	sol_fed.display_color = Color(0.2, 0.5, 1.0)
	sol_fed.lawfulness = 0.9
	sol_fed.aggression = 0.2
	sol_fed.credits = 50000.0
	sol_fed.doctrine_trade = 0.6
	sol_fed.doctrine_security = 0.3
	sol_fed.doctrine_piracy = 0.0
	world.factions["Sol Federation"] = sol_fed

	var mars_corp := WorldState.FactionData.new()
	mars_corp.name = "Mars Corp"
	mars_corp.display_color = Color(1.0, 0.3, 0.2)
	mars_corp.lawfulness = 0.7
	mars_corp.aggression = 0.4
	mars_corp.credits = 35000.0
	mars_corp.doctrine_trade = 0.7
	mars_corp.doctrine_security = 0.2
	mars_corp.doctrine_piracy = 0.0
	world.factions["Mars Corp"] = mars_corp

	var neutral := WorldState.FactionData.new()
	neutral.name = "Independent"
	neutral.display_color = Color(0.7, 0.7, 0.7)
	neutral.lawfulness = 0.5
	neutral.aggression = 0.1
	neutral.credits = 15000.0
	neutral.doctrine_trade = 0.8
	neutral.doctrine_security = 0.1
	neutral.doctrine_piracy = 0.0
	world.factions["Independent"] = neutral

	# Set relations
	sol_fed.relations["Mars Corp"] = 0.3
	sol_fed.relations["Independent"] = 0.5
	mars_corp.relations["Sol Federation"] = 0.3
	mars_corp.relations["Independent"] = 0.4
	neutral.relations["Sol Federation"] = 0.5
	neutral.relations["Mars Corp"] = 0.4

func _create_celestial_bodies() -> void:
	# Orbital periods in sim-minutes (scaled for gameplay, not realistic)
	# Real ratios preserved roughly: Mercury fastest, Neptune slowest
	var body_defs := [
		# [name, type, orbital_radius_AU, period_minutes, display_radius, color_hex]
		["Sun", "star", 0.0, 0.0, 5.0, "fff44f"],
		["Mercury", "planet", 0.39, 5280.0, 0.4, "b5b5b5"],
		["Venus", "planet", 0.72, 13500.0, 0.9, "e8cda0"],
		["Earth", "planet", 1.0, 21900.0, 1.0, "4488ff"],
		["Mars", "planet", 1.52, 41160.0, 0.6, "dd6644"],
		["Jupiter", "planet", 5.2, 259560.0, 3.5, "d4a574"],
		["Saturn", "planet", 9.5, 645480.0, 3.0, "e8d5a0"],
		["Uranus", "planet", 19.2, 1839960.0, 1.8, "88ccdd"],
		["Neptune", "planet", 30.0, 3607200.0, 1.7, "4466cc"],
		["Ceres", "dwarf", 2.77, 101520.0, 0.2, "999999"],
	]

	for def in body_defs:
		var body := WorldState.CelestialBodyData.new()
		body.id = world.allocate_id()
		body.entity_name = def[0]
		body.body_type = def[1]
		body.orbital_radius = def[2]
		body.orbital_period = def[3]
		body.orbital_phase = world.rng.randf() * TAU  # Random starting phase
		body.display_radius = def[4]
		body.color = Color.html(def[5])
		world.bodies[body.id] = body

func _create_stations() -> void:
	# Helper to find body by name
	var body_by_name := {}
	for id in world.bodies:
		body_by_name[world.bodies[id].entity_name] = world.bodies[id]

	var station_defs := [
		# [name, faction, body_name, orbit_type, offset_x, offset_y, offset_z, modules, population]
		["Earth Orbital Hub", "Sol Federation", "Earth", "orbital", 0.0, 0.3, 0.8, ["DOCKS", "SHIP_SERVICES", "LIFE_SUPPORT"], 200],
		["Luna Gateway", "Sol Federation", "Earth", "orbital", -0.6, 0.2, -0.5, ["DOCKS", "REFINERY", "LIFE_SUPPORT"], 80],
		["Mars Prime", "Mars Corp", "Mars", "surface", 0.0, 0.0, 0.5, ["DOCKS", "FARM", "LIFE_SUPPORT", "REFINERY"], 300],
		["Phobos Station", "Mars Corp", "Mars", "orbital", 0.4, 0.2, 0.0, ["DOCKS", "SHIP_SERVICES", "SECURITY_OFFICE"], 60],
		["Venus Cloud City", "Independent", "Venus", "orbital", 0.0, 0.5, 0.0, ["DOCKS", "FARM", "LIFE_SUPPORT"], 120],
		["Mercury Mining Post", "Independent", "Mercury", "surface", 0.3, 0.0, 0.0, ["DOCKS", "REFINERY", "LIFE_SUPPORT"], 40],
		["Ceres Depot", "Independent", "Ceres", "orbital", 0.0, 0.15, 0.0, ["DOCKS", "REFINERY", "SHIP_SERVICES"], 50],
		["Jupiter Crossroads", "Sol Federation", "Jupiter", "orbital", 0.0, 0.5, 2.5, ["DOCKS", "SHIP_SERVICES", "LIFE_SUPPORT", "FARM"], 150],
		["Saturn Ring Station", "Mars Corp", "Saturn", "orbital", 2.0, 0.3, 0.0, ["DOCKS", "REFINERY", "LIFE_SUPPORT"], 90],
		["Titan Outpost", "Sol Federation", "Saturn", "orbital", -1.0, 0.4, 1.5, ["DOCKS", "FARM", "LIFE_SUPPORT", "SECURITY_OFFICE"], 70],
	]

	for def in station_defs:
		var station := WorldState.StationData.new()
		station.id = world.allocate_id()
		station.entity_name = def[0]
		station.faction_name = def[1]
		var parent_body = body_by_name.get(def[2])
		if parent_body:
			station.body_id = parent_body.id
			parent_body.station_ids.append(station.id)
		station.orbit_type = def[3]
		station.orbit_offset = Vector3(def[4], def[5], def[6])
		station.modules = []
		for m in def[7]:
			station.modules.append(m)
		station.population = def[8]
		station.max_docks = 2 + station.modules.count("DOCKS") * 3

		# Starting inventory
		station.inventory = {
			"FOOD": world.rng.randi_range(10, 50),
			"WATER": world.rng.randi_range(10, 50),
			"OXYGEN": world.rng.randi_range(10, 40),
			"METALS": world.rng.randi_range(5, 30),
			"FUEL": world.rng.randi_range(20, 60),
			"ELECTRONICS": world.rng.randi_range(5, 20),
			"MEDICAL": world.rng.randi_range(5, 15),
		}

		world.stations[station.id] = station

		# Register with faction
		if station.faction_name in world.factions:
			world.factions[station.faction_name].controlled_station_ids.append(station.id)

func _spawn_initial_ships() -> void:
	var ship_count := 0
	for station_id in world.stations:
		var station = world.stations[station_id]
		# Each station gets 2-4 starting ships
		var num_ships = world.rng.randi_range(2, 4)
		for i in range(num_ships):
			var ship := WorldState.ShipData.new()
			ship.id = world.allocate_id()
			ship.entity_name = "%s Freighter %d" % [station.entity_name.split(" ")[0], i + 1]
			ship.faction_name = station.faction_name
			ship.home_station_id = station_id
			ship.ship_type = "LIGHT_FREIGHTER"
			ship.role = "cargo"
			ship.hull = 100.0
			ship.hull_max = 100.0
			ship.fuel = 100.0
			ship.fuel_max = 100.0
			ship.cargo_capacity = 50
			ship.base_speed = 1.5 + world.rng.randf() * 1.0
			ship.state = "docked"
			ship.docked_station_id = station_id
			ship.position = get_station_position(station_id)

			world.ships[ship.id] = ship
			station.ship_ids.append(ship.id)
			station.docked_ship_ids.append(ship.id)
			ship_count += 1

	EventBus.emit_log("system", "Spawned %d initial ships across all stations." % ship_count)

# ============================================================
# Save / Load
# ============================================================

func save_game(filename: String = "savegame.json") -> bool:
	var save_path = "user://saves/"
	if not DirAccess.dir_exists_absolute(save_path):
		DirAccess.make_dir_recursive_absolute(save_path)

	var file = FileAccess.open(save_path + filename, FileAccess.WRITE)
	if file == null:
		push_error("Failed to open save file: " + save_path + filename)
		return false

	var data = world.to_dict()
	data["paused"] = paused
	data["speed_multiplier"] = speed_multiplier

	file.store_string(JSON.stringify(data, "\t"))
	file.close()
	EventBus.emit_log("system", "Game saved to " + filename)
	return true

func load_game(filename: String = "savegame.json") -> bool:
	var save_path = "user://saves/" + filename
	if not FileAccess.file_exists(save_path):
		push_error("Save file not found: " + save_path)
		return false

	var file = FileAccess.open(save_path, FileAccess.READ)
	if file == null:
		push_error("Failed to open save file: " + save_path)
		return false

	var json_text = file.get_as_text()
	file.close()

	var json = JSON.new()
	var err = json.parse(json_text)
	if err != OK:
		push_error("JSON parse error: " + json.get_error_message())
		return false

	var data = json.data
	world = WorldState.new()
	world.from_dict(data)
	paused = data.get("paused", false)
	speed_multiplier = data.get("speed_multiplier", 1.0)

	EventBus.emit_log("system", "Game loaded from " + filename)
	return true
