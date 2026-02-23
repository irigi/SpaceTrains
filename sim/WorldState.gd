class_name WorldState
extends RefCounted
## Contains all simulation data. Pure data — no rendering references.

var sim_time: float = 0.0  # Simulation time in hours
var rng_seed: int = 0
var rng: RandomNumberGenerator = RandomNumberGenerator.new()
var next_entity_id: int = 1

# Data containers
var bodies: Dictionary = {}      # id -> CelestialBodyData
var stations: Dictionary = {}    # id -> StationData
var ships: Dictionary = {}       # id -> ShipData
var factions: Dictionary = {}    # name -> FactionData

func _init(seed_val: int = 0) -> void:
	rng_seed = seed_val if seed_val != 0 else randi()
	rng.seed = rng_seed

func allocate_id() -> int:
	var id = next_entity_id
	next_entity_id += 1
	return id

# --- Serialization ---
func to_dict() -> Dictionary:
	var data := {}
	data["sim_time"] = sim_time
	data["rng_seed"] = rng_seed
	data["next_entity_id"] = next_entity_id

	var bodies_dict := {}
	for id in bodies:
		bodies_dict[str(id)] = bodies[id].to_dict()
	data["bodies"] = bodies_dict

	var stations_dict := {}
	for id in stations:
		stations_dict[str(id)] = stations[id].to_dict()
	data["stations"] = stations_dict

	var ships_dict := {}
	for id in ships:
		ships_dict[str(id)] = ships[id].to_dict()
	data["ships"] = ships_dict

	var factions_dict := {}
	for name in factions:
		factions_dict[name] = factions[name].to_dict()
	data["factions"] = factions_dict

	return data

func from_dict(data: Dictionary) -> void:
	sim_time = data.get("sim_time", 0.0)
	rng_seed = data.get("rng_seed", 0)
	rng.seed = rng_seed
	next_entity_id = data.get("next_entity_id", 1)

	bodies.clear()
	for id_str in data.get("bodies", {}):
		var body = CelestialBodyData.new()
		body.from_dict(data["bodies"][id_str])
		bodies[body.id] = body

	stations.clear()
	for id_str in data.get("stations", {}):
		var station = StationData.new()
		station.from_dict(data["stations"][id_str])
		stations[station.id] = station

	ships.clear()
	for id_str in data.get("ships", {}):
		var ship = ShipData.new()
		ship.from_dict(data["ships"][id_str])
		ships[ship.id] = ship

	factions.clear()
	for name in data.get("factions", {}):
		var faction = FactionData.new()
		faction.from_dict(data["factions"][name])
		factions[faction.name] = faction


# ============================================================
# Data Classes
# ============================================================

class CelestialBodyData:
	var id: int = 0
	var entity_name: String = ""
	var body_type: String = "planet"  # star, planet, moon, dwarf, asteroid
	var orbital_radius: float = 0.0   # AU (scaled in rendering)
	var orbital_period: float = 0.0   # in sim-hours
	var orbital_phase: float = 0.0    # starting angle in radians
	var display_radius: float = 1.0   # visual scale
	var color: Color = Color.WHITE
	var station_ids: Array[int] = []

	func get_position_at_time(t: float) -> Vector3:
		if orbital_radius <= 0.0:
			return Vector3.ZERO  # Sun stays at origin
		var angle = orbital_phase + (TAU * t / orbital_period) if orbital_period > 0 else orbital_phase
		return Vector3(
			cos(angle) * orbital_radius,
			0.0,
			sin(angle) * orbital_radius
		)

	func to_dict() -> Dictionary:
		return {
			"id": id,
			"entity_name": entity_name,
			"body_type": body_type,
			"orbital_radius": orbital_radius,
			"orbital_period": orbital_period,
			"orbital_phase": orbital_phase,
			"display_radius": display_radius,
			"color": color.to_html(),
			"station_ids": station_ids.duplicate(),
		}

	func from_dict(d: Dictionary) -> void:
		id = d.get("id", 0)
		entity_name = d.get("entity_name", "")
		body_type = d.get("body_type", "planet")
		orbital_radius = d.get("orbital_radius", 0.0)
		orbital_period = d.get("orbital_period", 0.0)
		orbital_phase = d.get("orbital_phase", 0.0)
		display_radius = d.get("display_radius", 1.0)
		color = Color.html(d.get("color", "ffffff"))
		var sids = d.get("station_ids", [])
		station_ids = []
		for s in sids:
			station_ids.append(int(s))


class StationData:
	var id: int = 0
	var entity_name: String = ""
	var faction_name: String = ""
	var body_id: int = 0               # Parent celestial body
	var orbit_type: String = "orbital"  # "orbital" or "surface"
	var orbit_offset: Vector3 = Vector3.ZERO  # Offset from body position
	var population: int = 50
	var modules: Array[String] = []
	var inventory: Dictionary = {}     # commodity_name -> amount
	var ship_ids: Array[int] = []      # Ships owned by this station
	var docked_ship_ids: Array[int] = []
	var max_docks: int = 4
	var production_timer: float = 0.0

	func get_world_position(body_pos: Vector3) -> Vector3:
		return body_pos + orbit_offset

	func to_dict() -> Dictionary:
		return {
			"id": id,
			"entity_name": entity_name,
			"faction_name": faction_name,
			"body_id": body_id,
			"orbit_type": orbit_type,
			"orbit_offset": [orbit_offset.x, orbit_offset.y, orbit_offset.z],
			"population": population,
			"modules": modules.duplicate(),
			"inventory": inventory.duplicate(),
			"ship_ids": ship_ids.duplicate(),
			"docked_ship_ids": docked_ship_ids.duplicate(),
			"max_docks": max_docks,
			"production_timer": production_timer,
		}

	func from_dict(d: Dictionary) -> void:
		id = d.get("id", 0)
		entity_name = d.get("entity_name", "")
		faction_name = d.get("faction_name", "")
		body_id = d.get("body_id", 0)
		orbit_type = d.get("orbit_type", "orbital")
		var oo = d.get("orbit_offset", [0, 0, 0])
		orbit_offset = Vector3(oo[0], oo[1], oo[2])
		population = d.get("population", 50)
		var mods = d.get("modules", [])
		modules = []
		for m in mods:
			modules.append(str(m))
		inventory = d.get("inventory", {}).duplicate()
		var sids = d.get("ship_ids", [])
		ship_ids = []
		for s in sids:
			ship_ids.append(int(s))
		var dsids = d.get("docked_ship_ids", [])
		docked_ship_ids = []
		for s in dsids:
			docked_ship_ids.append(int(s))
		max_docks = d.get("max_docks", 4)
		production_timer = d.get("production_timer", 0.0)


class ShipData:
	var id: int = 0
	var entity_name: String = ""
	var faction_name: String = ""
	var home_station_id: int = 0
	var ship_type: String = "LIGHT_FREIGHTER"
	var role: String = "cargo"    # cargo, patrol, pirate, escort, courier

	# Stats
	var hull: float = 100.0
	var hull_max: float = 100.0
	var fuel: float = 100.0
	var fuel_max: float = 100.0
	var cargo_capacity: int = 50
	var base_speed: float = 2.0  # units per sim-hour
	var scan_strength: float = 1.0
	var combat_strength: float = 1.0

	# State
	var position: Vector3 = Vector3.ZERO
	var velocity: Vector3 = Vector3.ZERO
	var state: String = "docked"  # docked, launching, traveling, arriving, docking
	var docked_station_id: int = -1
	var target_station_id: int = -1
	var target_ship_id: int = -1
	var cargo: Dictionary = {}   # commodity -> amount

	# Mission
	var mission_type: String = ""  # "cargo_delivery", "patrol", ""
	var mission_commodity: String = ""
	var mission_amount: int = 0
	var mission_source_id: int = -1
	var mission_dest_id: int = -1

	# Travel
	var travel_origin: Vector3 = Vector3.ZERO
	var travel_destination: Vector3 = Vector3.ZERO
	var travel_progress: float = 0.0  # 0.0 to 1.0
	var travel_duration: float = 0.0  # sim-hours
	var trajectory_class: String = "linear"
	var trajectory_parameters: Dictionary = {}
	var trajectory_points: Array[Vector3] = []
	var trajectory_plan_age: float = 0.0
	var insertion_burn_cost: float = 0.0
	var final_burn_cost: float = 0.0
	var insertion_burn_done: bool = false
	var final_burn_done: bool = false

	func to_dict() -> Dictionary:
		return {
			"id": id,
			"entity_name": entity_name,
			"faction_name": faction_name,
			"home_station_id": home_station_id,
			"ship_type": ship_type,
			"role": role,
			"hull": hull,
			"hull_max": hull_max,
			"fuel": fuel,
			"fuel_max": fuel_max,
			"cargo_capacity": cargo_capacity,
			"base_speed": base_speed,
			"scan_strength": scan_strength,
			"combat_strength": combat_strength,
			"position": [position.x, position.y, position.z],
			"velocity": [velocity.x, velocity.y, velocity.z],
			"state": state,
			"docked_station_id": docked_station_id,
			"target_station_id": target_station_id,
			"target_ship_id": target_ship_id,
			"cargo": cargo.duplicate(),
			"mission_type": mission_type,
			"mission_commodity": mission_commodity,
			"mission_amount": mission_amount,
			"mission_source_id": mission_source_id,
			"mission_dest_id": mission_dest_id,
			"travel_origin": [travel_origin.x, travel_origin.y, travel_origin.z],
			"travel_destination": [travel_destination.x, travel_destination.y, travel_destination.z],
			"travel_progress": travel_progress,
			"travel_duration": travel_duration,
			"trajectory_class": trajectory_class,
			"trajectory_parameters": trajectory_parameters.duplicate(true),
			"trajectory_points": _serialize_points(trajectory_points),
			"trajectory_plan_age": trajectory_plan_age,
			"insertion_burn_cost": insertion_burn_cost,
			"final_burn_cost": final_burn_cost,
			"insertion_burn_done": insertion_burn_done,
			"final_burn_done": final_burn_done,
		}

	func from_dict(d: Dictionary) -> void:
		id = d.get("id", 0)
		entity_name = d.get("entity_name", "")
		faction_name = d.get("faction_name", "")
		home_station_id = d.get("home_station_id", 0)
		ship_type = d.get("ship_type", "LIGHT_FREIGHTER")
		role = d.get("role", "cargo")
		hull = d.get("hull", 100.0)
		hull_max = d.get("hull_max", 100.0)
		fuel = d.get("fuel", 100.0)
		fuel_max = d.get("fuel_max", 100.0)
		cargo_capacity = d.get("cargo_capacity", 50)
		base_speed = d.get("base_speed", 2.0)
		scan_strength = d.get("scan_strength", 1.0)
		combat_strength = d.get("combat_strength", 1.0)
		var p = d.get("position", [0, 0, 0])
		position = Vector3(p[0], p[1], p[2])
		var v = d.get("velocity", [0, 0, 0])
		velocity = Vector3(v[0], v[1], v[2])
		state = d.get("state", "docked")
		docked_station_id = d.get("docked_station_id", -1)
		target_station_id = d.get("target_station_id", -1)
		target_ship_id = d.get("target_ship_id", -1)
		cargo = d.get("cargo", {}).duplicate()
		mission_type = d.get("mission_type", "")
		mission_commodity = d.get("mission_commodity", "")
		mission_amount = d.get("mission_amount", 0)
		mission_source_id = d.get("mission_source_id", -1)
		mission_dest_id = d.get("mission_dest_id", -1)
		var to = d.get("travel_origin", [0, 0, 0])
		travel_origin = Vector3(to[0], to[1], to[2])
		var td = d.get("travel_destination", [0, 0, 0])
		travel_destination = Vector3(td[0], td[1], td[2])
		travel_progress = d.get("travel_progress", 0.0)
		travel_duration = d.get("travel_duration", 0.0)
		trajectory_class = d.get("trajectory_class", "linear")
		trajectory_parameters = d.get("trajectory_parameters", {}).duplicate(true)
		trajectory_points = _deserialize_points(d.get("trajectory_points", []))
		trajectory_plan_age = d.get("trajectory_plan_age", 0.0)
		insertion_burn_cost = d.get("insertion_burn_cost", 0.0)
		final_burn_cost = d.get("final_burn_cost", 0.0)
		insertion_burn_done = d.get("insertion_burn_done", false)
		final_burn_done = d.get("final_burn_done", false)

	func _serialize_points(points: Array[Vector3]) -> Array:
		var serialized: Array = []
		for p in points:
			serialized.append([p.x, p.y, p.z])
		return serialized

	func _deserialize_points(serialized_points: Array) -> Array[Vector3]:
		var points: Array[Vector3] = []
		for p in serialized_points:
			if p is Array and p.size() >= 3:
				points.append(Vector3(float(p[0]), float(p[1]), float(p[2])))
		return points


class FactionData:
	var name: String = ""
	var display_color: Color = Color.WHITE
	var lawfulness: float = 0.5   # 0..1
	var aggression: float = 0.3   # 0..1
	var credits: float = 10000.0
	var relations: Dictionary = {} # faction_name -> float (-1..+1)
	var controlled_station_ids: Array[int] = []
	var doctrine_trade: float = 0.5
	var doctrine_security: float = 0.3
	var doctrine_piracy: float = 0.0

	func to_dict() -> Dictionary:
		return {
			"name": name,
			"display_color": display_color.to_html(),
			"lawfulness": lawfulness,
			"aggression": aggression,
			"credits": credits,
			"relations": relations.duplicate(),
			"controlled_station_ids": controlled_station_ids.duplicate(),
			"doctrine_trade": doctrine_trade,
			"doctrine_security": doctrine_security,
			"doctrine_piracy": doctrine_piracy,
		}

	func from_dict(d: Dictionary) -> void:
		name = d.get("name", "")
		display_color = Color.html(d.get("display_color", "ffffff"))
		lawfulness = d.get("lawfulness", 0.5)
		aggression = d.get("aggression", 0.3)
		credits = d.get("credits", 10000.0)
		relations = d.get("relations", {}).duplicate()
		var csids = d.get("controlled_station_ids", [])
		controlled_station_ids = []
		for s in csids:
			controlled_station_ids.append(int(s))
		doctrine_trade = d.get("doctrine_trade", 0.5)
		doctrine_security = d.get("doctrine_security", 0.3)
		doctrine_piracy = d.get("doctrine_piracy", 0.0)
