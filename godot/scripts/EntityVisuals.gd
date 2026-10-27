# Procedural entity meshes: bodies, stations and ships. All entities are Node3D
# containers; Main.gd scales the container. Bodies are unit spheres; stations
# (StationModels.gd) and ships (ShipModels.gd, built from their design, nose at -Z so
# look_at() orients them along their velocity) are built in metres, and their container
# scale converts metres to world units.
extends RefCounted

const ShipModels := preload("res://scripts/ShipModels.gd")
const StationModels := preload("res://scripts/StationModels.gd")
const Materials := preload("res://scripts/ModelMaterials.gd")

static var _glow_texture: ImageTexture

static func make_entity(kind: String, data: Dictionary) -> Node3D:
    var container := Node3D.new()
    match kind:
        "body":
            # Dense enough for a smooth limb when a planet fills the screen.
            container.add_child(_named(_sphere(1.0, 128, 64), "hull"))
        "station":
            StationModels.ensure(container, data)
        "ship":
            # The model itself comes from ShipModels.ensure() once the design is known.
            var glow := _make_engine_glow()
            container.add_child(glow)
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
                var texture: Texture2D = load(texture_path)
                material.albedo_texture = texture
                materials["hull"] = material
            else:
                materials["hull"] = _standard(body_color(body_id), 0.82)
    elif kind == "station":
        # The faction band on the habitat ring(s); the rest is ModelMaterials' shared paint.
        Materials.set_band(Materials.instance_materials(container)["band"], faction_color)
        return
    else:
        # Ships: the faction band, red when stranded; the rest is ShipModels' shared paint.
        var phase := String(data.get("phase", "idle"))
        var band := faction_color
        if phase == "stranded":
            band = Color(0.85, 0.25, 0.2)
        elif phase == "derelict":
            band = Color(0.25, 0.22, 0.2)   # a dead hull: no lights, scorched band
        Materials.set_band(ShipModels.instance_materials(container)["band"], band)
        return
    for child in container.get_children():
        var key := "wing" if String(child.name).begins_with("wing") else String(child.name)
        if child is MeshInstance3D and materials.has(key):
            child.material_override = materials[key]

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
    # Without this a billboard ignores its node's scale and is always one world unit
    # (0.05 AU) across: a ship's glow filled the screen near Jupiter.
    material.billboard_keep_scale = true
    material.albedo_texture = _glow_falloff_texture()
    material.albedo_color = Color(1.0, 0.8, 0.45)
    instance.material_override = material
    instance.visible = false
    return instance

# Fallback burn length for a snapshot without engine_burn_s.
const BURN_WINDOW_S := 6.0 * 3600.0

# Engine glow and the ship model's live state, per frame.
# - Nuclear-thermal ships burn for the bridge's engine_burn_s after departure and before
#   arrival (half the mission's propellant each, at the engine's mass flow). Plasma drives
#   run for the whole transit.
# - Far out (COARSE model) a billboard glow marks a running engine; its size follows jet
#   power, log-scaled: 10 MW plasma ~18 m, 700 MW liquid core ~34 m. max_glow_scale caps
#   it in world units so a nearby burn never projects larger on screen than the cap the
#   caller computed from camera distance (a flare bigger than Earth ruins close-up views).
# - Close up (DETAIL) the nozzle glows and a plume of that length replaces the billboard.
# - Plasma ships' radiators show in false colour (ThermalPalette) while thrusting.
const GLOW_METRES_PER_UNIT := 20.0

static func update_engine_glow(container: Node3D, data: Dictionary, game_time_s: float, max_glow_scale: float = 1.0e9, jet_power_w: float = 0.0) -> void:
    var glow := container.get_node_or_null("engine_glow") as MeshInstance3D
    var phase := String(data.get("phase", "idle"))
    var plasma := String(data.get("propulsion_type", "nuclear_thermal")) == "variable_isp"
    var engine_on := false
    if phase == "patrol":
        engine_on = plasma   # cutters are plasma ships, always under way when out
    elif phase == "in_transit":
        if plasma:
            engine_on = true
        else:
            var departure_s := float(data.get("departure_time_s", 0.0))
            var arrival_s := float(data.get("arrival_time_s", 0.0))
            var burn_s := float(data.get("engine_burn_s", BURN_WINDOW_S))
            engine_on = (game_time_s >= departure_s and game_time_s - departure_s < burn_s) \
                or (game_time_s <= arrival_s and arrival_s - game_time_s < burn_s)
    var size := 0.9 if plasma else 1.6
    if jet_power_w > 0.0:
        size = clampf(0.45 * log(jet_power_w / 1.0e5) / log(10.0), 0.5, 2.4)
    var size_m := size * GLOW_METRES_PER_UNIT
    var detail := int(container.get_meta("lod", ShipModels.COARSE)) == ShipModels.DETAIL
    if container.has_meta("model_key"):
        ShipModels.update_live(container, data, engine_on, plasma, size_m * (1.5 if plasma else 1.0))
    if glow == null:
        return
    glow.visible = engine_on and not detail
    if glow.visible:
        # Container scale converts local (metre) glow size to world units.
        var world_per_local: float = maxf(container.scale.x, 1.0e-12)
        glow.scale = Vector3.ONE * minf(size_m, max_glow_scale / world_per_local)
        (glow.material_override as StandardMaterial3D).albedo_color = Color(0.45, 0.7, 1.0, 0.8) if plasma else Color(1.0, 0.8, 0.45, 0.95)
