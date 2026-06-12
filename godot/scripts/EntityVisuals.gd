# Procedural entity meshes: composite primitives per ship class / station / body.
# All entities are Node3D containers of unit-scale MeshInstance3D children;
# Main.gd scales the container. Ships are built with the nose pointing -Z so
# look_at() orients them along their velocity.
extends RefCounted

static var _glow_texture: ImageTexture

static func make_entity(kind: String, data: Dictionary) -> Node3D:
    var container := Node3D.new()
    match kind:
        "body":
            container.add_child(_named(_sphere(1.0, 24, 12), "hull"))
        "station":
            _build_station(container)
        "ship":
            _build_ship(container, String(data.get("class_id", "")), String(data.get("propulsion_type", "chemical")))
    return container

static func _named(instance: MeshInstance3D, instance_name: String) -> MeshInstance3D:
    instance.name = instance_name
    return instance

static func _sphere(radius: float, radial := 12, rings := 6) -> MeshInstance3D:
    var mesh := SphereMesh.new()
    mesh.radius = radius
    mesh.height = radius * 2.0
    mesh.radial_segments = radial
    mesh.rings = rings
    var instance := MeshInstance3D.new()
    instance.mesh = mesh
    return instance

static func _box(size: Vector3) -> MeshInstance3D:
    var mesh := BoxMesh.new()
    mesh.size = size
    var instance := MeshInstance3D.new()
    instance.mesh = mesh
    return instance

static func _cylinder(top_radius: float, bottom_radius: float, height: float, segments := 10) -> MeshInstance3D:
    var mesh := CylinderMesh.new()
    mesh.top_radius = top_radius
    mesh.bottom_radius = bottom_radius
    mesh.height = height
    mesh.radial_segments = segments
    var instance := MeshInstance3D.new()
    instance.mesh = mesh
    return instance

static func _capsule(radius: float, height: float) -> MeshInstance3D:
    var mesh := CapsuleMesh.new()
    mesh.radius = radius
    mesh.height = height
    mesh.radial_segments = 12
    var instance := MeshInstance3D.new()
    instance.mesh = mesh
    return instance

# --- Stations: ring habitat with spokes and a docking hub ---------------------

static func _build_station(container: Node3D) -> void:
    var torus := TorusMesh.new()
    torus.inner_radius = 0.72
    torus.outer_radius = 1.0
    torus.rings = 24
    torus.ring_segments = 8
    var ring := MeshInstance3D.new()
    ring.mesh = torus
    container.add_child(_named(ring, "accent"))

    for i in range(4):
        var spoke := _cylinder(0.05, 0.05, 1.7, 6)
        spoke.rotation.z = PI * 0.5
        spoke.rotation.y = PI * 0.25 * float(i) * 2.0
        container.add_child(_named(spoke, "hull"))

    var hub := _sphere(0.3, 12, 8)
    container.add_child(_named(hub, "hull"))
    var mast := _cylinder(0.06, 0.06, 1.2, 6)
    container.add_child(_named(mast, "hull"))

# --- Ships: distinct silhouettes per propulsion / class ----------------------

static func _build_ship(container: Node3D, class_id: String, propulsion: String) -> void:
    if propulsion == "electric_ion":
        _build_ion_ship(container)
    elif class_id == "ntr_freighter":
        _build_ntr_ship(container)
    else:
        _build_chemical_ship(container, class_id)
    var glow := _make_engine_glow()
    glow.position = Vector3(0.0, 0.0, 1.15)
    container.add_child(glow)

static func _build_chemical_ship(container: Node3D, class_id: String) -> void:
    var fuselage := _capsule(0.3, 1.5)
    fuselage.rotation.x = PI * 0.5
    container.add_child(_named(fuselage, "hull"))

    # Drop tanks: tankers get three, couriers one slim tank, freighters two.
    var tank_count := 2
    var tank_radius := 0.16
    if class_id == "tanker":
        tank_count = 3
        tank_radius = 0.2
    elif class_id == "fast_courier":
        tank_count = 1
        tank_radius = 0.12
    for i in range(tank_count):
        var tank := _capsule(tank_radius, 0.9)
        tank.rotation.x = PI * 0.5
        var angle := TAU * float(i) / float(max(tank_count, 1))
        tank.position = Vector3(cos(angle), sin(angle), 0.1) * 0.42
        container.add_child(_named(tank, "tank"))

    var engine := _cylinder(0.12, 0.32, 0.4, 10)
    engine.rotation.x = PI * 0.5
    engine.position.z = 0.95
    container.add_child(_named(engine, "engine"))

static func _build_ntr_ship(container: Node3D) -> void:
    # Long truss keeps the crew capsule far from the reactor, behind a shadow shield.
    var truss := _box(Vector3(0.14, 0.14, 2.3))
    container.add_child(_named(truss, "hull"))
    var crew := _sphere(0.26, 12, 8)
    crew.position.z = -1.15
    container.add_child(_named(crew, "hull"))
    var shield := _cylinder(0.4, 0.4, 0.08, 12)
    shield.rotation.x = PI * 0.5
    shield.position.z = 0.55
    container.add_child(_named(shield, "accent"))
    var reactor := _box(Vector3(0.3, 0.3, 0.5))
    reactor.position.z = 0.95
    container.add_child(_named(reactor, "tank"))
    var nozzle := _cylinder(0.1, 0.3, 0.35, 10)
    nozzle.rotation.x = PI * 0.5
    nozzle.position.z = 1.3
    container.add_child(_named(nozzle, "engine"))

static func _build_ion_ship(container: Node3D) -> void:
    var bus := _box(Vector3(0.45, 0.22, 0.7))
    container.add_child(_named(bus, "hull"))
    for side in [-1.0, 1.0]:
        var wing := _box(Vector3(1.3, 0.02, 0.55))
        wing.position.x = side * 0.9
        container.add_child(_named(wing, "wing"))
    var thruster := _cylinder(0.1, 0.14, 0.18, 8)
    thruster.rotation.x = PI * 0.5
    thruster.position.z = 0.45
    container.add_child(_named(thruster, "engine"))

# --- Materials & phase tinting ------------------------------------------------

static func body_color(body_id: String) -> Color:
    match body_id:
        "sun": return Color(1.0, 0.76, 0.28)
        "mercury": return Color(0.7, 0.66, 0.62)
        "venus": return Color(0.88, 0.72, 0.38)
        "earth": return Color(0.36, 0.58, 1.0)
        "mars": return Color(0.89, 0.42, 0.25)
        "ceres": return Color(0.72, 0.72, 0.78)
        "jupiter": return Color(0.85, 0.65, 0.45)
        "saturn": return Color(0.90, 0.80, 0.55)
        "uranus": return Color(0.50, 0.85, 0.90)
        "neptune": return Color(0.30, 0.45, 0.95)
        "luna": return Color(0.80, 0.80, 0.80)
        "europa": return Color(0.75, 0.70, 0.65)
        "ganymede": return Color(0.65, 0.60, 0.55)
        "titan": return Color(0.85, 0.65, 0.40)
        "triton": return Color(0.60, 0.70, 0.75)
        _: return Color(0.75, 0.8, 0.88)

static func _standard(albedo: Color, roughness := 0.5, emission := Color.BLACK, emission_energy := 0.0, metallic := 0.0) -> StandardMaterial3D:
    var material := StandardMaterial3D.new()
    material.albedo_color = albedo
    material.roughness = roughness
    material.metallic = metallic
    if emission_energy > 0.0:
        material.emission_enabled = true
        material.emission = emission
        material.emission_energy_multiplier = emission_energy
    return material

static func apply_visuals(container: Node3D, kind: String, data: Dictionary, faction_colors: Dictionary) -> void:
    var faction_color: Color = faction_colors.get(String(data.get("faction_id", "")), Color(0.95, 0.82, 0.36))
    var materials := {}
    if kind == "body":
        var body_id := String(data.get("id", ""))
        if body_id == "sun":
            # Bright enough to cross the HDR glow threshold and bloom.
            materials["hull"] = _standard(Color(1.0, 0.96, 0.31), 0.6, Color(1.0, 0.96, 0.31), 12.0)
        else:
            var texture_path := "res://assets/planets/%s.jpg" % body_id
            if ResourceLoader.exists(texture_path):
                var material := _standard(Color.WHITE, 0.9)
                material.albedo_texture = load(texture_path)
                materials["hull"] = material
            else:
                materials["hull"] = _standard(body_color(body_id), 0.82)
    elif kind == "station":
        materials["hull"] = _standard(Color(0.62, 0.66, 0.72), 0.45, Color.BLACK, 0.0, 0.4)
        materials["accent"] = _standard(faction_color, 0.4, faction_color * 0.5, 0.6)
    else:
        var phase := String(data.get("phase", "idle"))
        var hull := Color(0.82, 0.85, 0.9)
        var hull_emission := 0.35
        var engine_emission := 0.2
        if phase == "in_transit":
            hull_emission = 0.55
            engine_emission = 1.6
        elif phase == "stranded":
            hull = Color(0.9, 0.35, 0.3)
            hull_emission = 0.3
        materials["hull"] = _standard(hull, 0.4, hull, hull_emission, 0.3)
        materials["tank"] = _standard(hull * 0.7, 0.55, Color.BLACK, 0.0, 0.5)
        materials["accent"] = _standard(faction_color, 0.45, faction_color * 0.5, 0.5)
        materials["wing"] = _standard(Color(0.2, 0.3, 0.6), 0.3, Color(0.25, 0.45, 1.0), 0.8, 0.6)
        materials["engine"] = _standard(Color(0.25, 0.26, 0.3), 0.3, Color(1.0, 0.75, 0.4), engine_emission, 0.7)
    for child in container.get_children():
        if child is MeshInstance3D and materials.has(child.name):
            child.material_override = materials[child.name]

# --- Engine glow ---------------------------------------------------------------

static func _glow_falloff_texture() -> ImageTexture:
    if _glow_texture != null:
        return _glow_texture
    var size := 64
    var image := Image.create(size, size, false, Image.FORMAT_RGBA8)
    var center := Vector2(size, size) * 0.5
    for y in range(size):
        for x in range(size):
            var distance := Vector2(x + 0.5, y + 0.5).distance_to(center) / (size * 0.5)
            var alpha: float = clamp(pow(maxf(1.0 - distance, 0.0), 1.8), 0.0, 1.0)
            image.set_pixel(x, y, Color(1.0, 1.0, 1.0, alpha))
    _glow_texture = ImageTexture.create_from_image(image)
    return _glow_texture

static func _make_engine_glow() -> MeshInstance3D:
    var instance := MeshInstance3D.new()
    instance.name = "engine_glow"
    var quad := QuadMesh.new()
    quad.size = Vector2.ONE
    instance.mesh = quad
    var material := StandardMaterial3D.new()
    material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
    material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
    material.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
    material.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
    material.albedo_texture = _glow_falloff_texture()
    material.albedo_color = Color(1.0, 0.8, 0.45)
    instance.material_override = material
    instance.visible = false
    return instance

const BURN_WINDOW_S := 6.0 * 3600.0

# Chemical/NTR ships flare near their impulsive burns; ion drives glow softly
# for the whole transit. max_glow_scale caps the billboard in world units so a
# nearby burn never projects larger on screen than the cap the caller computed
# from camera distance (a flare bigger than Earth ruins close-up views).
static func update_engine_glow(container: Node3D, data: Dictionary, game_time_s: float, max_glow_scale: float = 1.0e9) -> void:
    var glow := container.get_node_or_null("engine_glow")
    if glow == null:
        return
    var phase := String(data.get("phase", "idle"))
    if phase != "in_transit":
        glow.visible = false
        return
    # Container scale converts local glow scale to world units.
    var world_per_local: float = maxf(container.scale.x, 1.0e-12)
    var propulsion := String(data.get("propulsion_type", "chemical"))
    if propulsion == "electric_ion":
        glow.visible = true
        glow.scale = Vector3.ONE * minf(0.9, max_glow_scale / world_per_local)
        (glow.material_override as StandardMaterial3D).albedo_color = Color(0.45, 0.7, 1.0, 0.8)
        return
    var departure_s := float(data.get("departure_time_s", 0.0))
    var arrival_s := float(data.get("arrival_time_s", 0.0))
    var near_burn: bool = absf(game_time_s - departure_s) < BURN_WINDOW_S or absf(game_time_s - arrival_s) < BURN_WINDOW_S
    glow.visible = near_burn
    if near_burn:
        glow.scale = Vector3.ONE * minf(2.4, max_glow_scale / world_per_local)
        (glow.material_override as StandardMaterial3D).albedo_color = Color(1.0, 0.8, 0.45, 0.95)
