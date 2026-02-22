class_name SolarSystemRenderer
extends Node3D
## Renders all solar system entities (bodies, stations, ships).
## Reads from WorldState each frame - no simulation logic here.

var simulation: Simulation
var floating_origin: FloatingOrigin

# Render node pools
var body_nodes: Dictionary = {}     # body_id -> Node3D
var station_nodes: Dictionary = {}  # station_id -> Node3D
var ship_nodes: Dictionary = {}     # ship_id -> Node3D
var orbit_lines: Dictionary = {}    # body_id -> MeshInstance3D

# Materials
var ship_material: StandardMaterial3D
var orbit_material: StandardMaterial3D

func _ready() -> void:
	_create_materials()

func _create_materials() -> void:
	ship_material = StandardMaterial3D.new()
	ship_material.albedo_color = Color(0.8, 0.8, 0.9)
	ship_material.emission_enabled = true
	ship_material.emission = Color(0.5, 0.5, 0.7)
	ship_material.emission_energy_multiplier = 0.3

	orbit_material = StandardMaterial3D.new()
	orbit_material.albedo_color = Color(1.0, 1.0, 1.0, 0.15)
	orbit_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	orbit_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	orbit_material.no_depth_test = true

func initialize(sim: Simulation) -> void:
	_clear_render_nodes()
	simulation = sim
	_create_body_nodes()
	_create_station_nodes()
	_create_orbit_lines()

func _clear_render_nodes() -> void:
	for node in body_nodes.values():
		node.queue_free()
	for node in station_nodes.values():
		node.queue_free()
	for node in ship_nodes.values():
		node.queue_free()
	for node in orbit_lines.values():
		node.queue_free()

	body_nodes.clear()
	station_nodes.clear()
	ship_nodes.clear()
	orbit_lines.clear()

func _process(_delta: float) -> void:
	if simulation == null or simulation.world == null:
		return
	_update_bodies()
	_update_stations()
	_update_ships()

func _create_body_nodes() -> void:
	for body_id in simulation.world.bodies:
		var body_data: WorldState.CelestialBodyData = simulation.world.bodies[body_id]
		var node := Node3D.new()
		node.name = "Body_%s" % body_data.entity_name

		# Create mesh
		var mesh_instance := MeshInstance3D.new()
		var sphere := SphereMesh.new()
		sphere.radius = body_data.display_radius
		sphere.height = body_data.display_radius * 2.0
		sphere.radial_segments = 32
		sphere.rings = 16
		mesh_instance.mesh = sphere

		# Material
		var mat := StandardMaterial3D.new()
		mat.albedo_color = body_data.color
		if body_data.body_type == "star":
			mat.emission_enabled = true
			mat.emission = body_data.color
			mat.emission_energy_multiplier = 2.0
		mesh_instance.material_override = mat

		node.add_child(mesh_instance)

		# Add label
		var label := Label3D.new()
		label.text = body_data.entity_name
		label.font_size = 24
		label.pixel_size = 0.05
		label.position = Vector3(0, body_data.display_radius + 1.0, 0)
		label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
		label.modulate = Color(1, 1, 1, 0.9)
		label.no_depth_test = true
		label.outline_size = 8
		node.add_child(label)

		add_child(node)
		body_nodes[body_id] = node

		# Add collision for selection via Area3D
		var area := Area3D.new()
		var collision := CollisionShape3D.new()
		var shape := SphereShape3D.new()
		shape.radius = body_data.display_radius * 1.5
		collision.shape = shape
		area.add_child(collision)
		area.set_meta("entity_type", "body")
		area.set_meta("entity_id", body_id)
		node.add_child(area)

func _create_station_nodes() -> void:
	for station_id in simulation.world.stations:
		var station_data: WorldState.StationData = simulation.world.stations[station_id]
		var node := Node3D.new()
		node.name = "Station_%s" % station_data.entity_name

		# Station mesh (small box marker)
		var mesh_instance := MeshInstance3D.new()
		var box := BoxMesh.new()
		box.size = Vector3(0.3, 0.3, 0.3)
		mesh_instance.mesh = box

		# Color by faction
		var mat := StandardMaterial3D.new()
		if station_data.faction_name in simulation.world.factions:
			mat.albedo_color = simulation.world.factions[station_data.faction_name].display_color
		else:
			mat.albedo_color = Color.WHITE
		mat.emission_enabled = true
		mat.emission = mat.albedo_color * 0.5
		mat.emission_energy_multiplier = 0.5
		mesh_instance.material_override = mat

		node.add_child(mesh_instance)

		# Label
		var label := Label3D.new()
		label.text = station_data.entity_name
		label.font_size = 18
		label.pixel_size = 0.03
		label.position = Vector3(0, 0.5, 0)
		label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
		label.modulate = Color(1, 1, 1, 0.8)
		label.no_depth_test = true
		label.outline_size = 6
		node.add_child(label)

		# Collision for selection
		var area := Area3D.new()
		var collision := CollisionShape3D.new()
		var shape := SphereShape3D.new()
		shape.radius = 0.8
		collision.shape = shape
		area.add_child(collision)
		area.set_meta("entity_type", "station")
		area.set_meta("entity_id", station_id)
		node.add_child(area)

		add_child(node)
		station_nodes[station_id] = node

func _create_orbit_lines() -> void:
	for body_id in simulation.world.bodies:
		var body_data: WorldState.CelestialBodyData = simulation.world.bodies[body_id]
		if body_data.orbital_radius <= 0.0:
			continue

		var mesh_instance := MeshInstance3D.new()
		var im := ImmediateMesh.new()
		mesh_instance.mesh = im
		mesh_instance.material_override = orbit_material

		# Draw circle
		var segments := 128
		im.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
		for i in range(segments + 1):
			var angle = TAU * float(i) / float(segments)
			var r = body_data.orbital_radius * Simulation.AU_SCALE
			im.surface_add_vertex(Vector3(cos(angle) * r, 0, sin(angle) * r))
		im.surface_end()

		add_child(mesh_instance)
		orbit_lines[body_id] = mesh_instance

func _update_bodies() -> void:
	for body_id in body_nodes:
		if body_id in simulation.world.bodies:
			var pos = simulation.get_body_position(body_id)
			body_nodes[body_id].position = pos

func _update_stations() -> void:
	for station_id in station_nodes:
		if station_id in simulation.world.stations:
			var pos = simulation.get_station_position(station_id)
			station_nodes[station_id].position = pos

func _update_ships() -> void:
	# Create nodes for new ships, update positions
	for ship_id in simulation.world.ships:
		var ship_data: WorldState.ShipData = simulation.world.ships[ship_id]

		if ship_id not in ship_nodes:
			_create_ship_node(ship_id, ship_data)

		var node = ship_nodes[ship_id]
		node.position = ship_data.position

		# Hide docked ships
		node.visible = ship_data.state != "docked"

	# Remove nodes for deleted ships
	var to_remove: Array = []
	for ship_id in ship_nodes:
		if ship_id not in simulation.world.ships:
			to_remove.append(ship_id)
	for ship_id in to_remove:
		ship_nodes[ship_id].queue_free()
		ship_nodes.erase(ship_id)

func _create_ship_node(ship_id: int, ship_data: WorldState.ShipData) -> void:
	var node := Node3D.new()
	node.name = "Ship_%d" % ship_id

	# Ship mesh (small cone/prism)
	var mesh_instance := MeshInstance3D.new()
	var prism := PrismMesh.new()
	prism.size = Vector3(0.15, 0.15, 0.3)
	mesh_instance.mesh = prism
	mesh_instance.material_override = ship_material.duplicate()

	# Color by faction
	if ship_data.faction_name in simulation.world.factions:
		var faction_color = simulation.world.factions[ship_data.faction_name].display_color
		(mesh_instance.material_override as StandardMaterial3D).albedo_color = faction_color
		(mesh_instance.material_override as StandardMaterial3D).emission = faction_color * 0.3

	node.add_child(mesh_instance)

	# Label
	var label := Label3D.new()
	label.text = ship_data.entity_name
	label.font_size = 14
	label.pixel_size = 0.02
	label.position = Vector3(0, 0.3, 0)
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.modulate = Color(1, 1, 1, 0.7)
	label.no_depth_test = true
	label.outline_size = 4
	node.add_child(label)

	# Collision for selection
	var area := Area3D.new()
	var collision := CollisionShape3D.new()
	var shape := SphereShape3D.new()
	shape.radius = 0.4
	collision.shape = shape
	area.add_child(collision)
	area.set_meta("entity_type", "ship")
	area.set_meta("entity_id", ship_id)
	node.add_child(area)

	add_child(node)
	ship_nodes[ship_id] = node
