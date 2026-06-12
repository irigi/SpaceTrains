# Procedural space ambience: starfield skybox, bloom + sun halo, orbit rings,
# Saturn's ring. Everything is generated at runtime — no texture assets.
extends Node3D

const STAR_COUNT := 3800
const STARFIELD_RADIUS := 3000.0
const MILKY_WAY_BAND_DEG := 14.0
const ORBIT_RING_SEGMENTS := 160
const ORBIT_RING_ALPHA := 0.13

var starfield: Node3D
var sun_halo: MeshInstance3D
var _orbit_rings: Dictionary = {}  # body_id -> {node, radius, parent_id}
var _halo_texture: ImageTexture

func _ready() -> void:
    starfield = _build_starfield()
    add_child(starfield)

func update_camera(camera_position: Vector3) -> void:
    # The starfield follows the camera (rotation stays fixed), acting as a skybox.
    if starfield != null:
        starfield.global_position = camera_position

# min_world_scale keeps the halo from vanishing at system-wide zooms — the sun
# should read as a glowing beacon at any distance.
func update_sun(sun_position: Vector3, sun_display_radius: float, min_world_scale := 0.0) -> void:
    if sun_halo == null:
        sun_halo = _build_sun_halo()
        add_child(sun_halo)
    sun_halo.global_position = sun_position
    sun_halo.scale = Vector3.ONE * maxf(sun_display_radius * 10.0, min_world_scale)

# --- Starfield ---------------------------------------------------------------

func _build_starfield() -> Node3D:
    var root := Node3D.new()
    root.name = "Starfield"
    var rng := RandomNumberGenerator.new()
    rng.seed = 0x5A57  # deterministic sky

    # Three brightness tiers as separate point meshes (StandardMaterial3D point
    # size is per-material, which avoids custom shader compatibility issues).
    # Kept deliberately faint: the stars are backdrop, the planets are the show.
    var tiers := [
        {"size": 1.2, "count": int(STAR_COUNT * 0.62), "brightness": 0.32},
        {"size": 2.0, "count": int(STAR_COUNT * 0.30), "brightness": 0.5},
        {"size": 3.0, "count": int(STAR_COUNT * 0.08), "brightness": 0.72},
    ]
    var band_basis := Basis(Vector3(1.0, 0.3, 0.2).normalized(), deg_to_rad(63.0))
    for tier in tiers:
        var vertices := PackedVector3Array()
        var colors := PackedColorArray()
        for i in range(tier["count"]):
            var direction := _random_star_direction(rng, band_basis)
            vertices.append(direction * STARFIELD_RADIUS)
            colors.append(_star_color(rng, float(tier["brightness"])))
        var arrays := []
        arrays.resize(Mesh.ARRAY_MAX)
        arrays[Mesh.ARRAY_VERTEX] = vertices
        arrays[Mesh.ARRAY_COLOR] = colors
        var mesh := ArrayMesh.new()
        mesh.add_surface_from_arrays(Mesh.PRIMITIVE_POINTS, arrays)
        var instance := MeshInstance3D.new()
        instance.mesh = mesh
        var material := StandardMaterial3D.new()
        material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
        material.vertex_color_use_as_albedo = true
        material.use_point_size = true
        material.point_size = float(tier["size"])
        material.disable_receive_shadows = true
        instance.material_override = material
        instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
        # Skybox: render behind everything, never culled by the moving origin.
        instance.extra_cull_margin = 16384.0
        root.add_child(instance)
    return root

func _random_star_direction(rng: RandomNumberGenerator, band_basis: Basis) -> Vector3:
    # ~38% of stars cluster into a tilted milky-way band.
    if rng.randf() < 0.38:
        var along := rng.randf_range(0.0, TAU)
        var band_sigma := deg_to_rad(MILKY_WAY_BAND_DEG) * 0.5
        var off := rng.randfn(0.0, band_sigma)
        var direction := Vector3(cos(along) * cos(off), sin(off), sin(along) * cos(off))
        return (band_basis * direction).normalized()
    var u := rng.randf_range(-1.0, 1.0)
    var theta := rng.randf_range(0.0, TAU)
    var s := sqrt(1.0 - u * u)
    return Vector3(s * cos(theta), u, s * sin(theta))

func _star_color(rng: RandomNumberGenerator, brightness: float) -> Color:
    # Loose blackbody spread: blue-white through yellow to faint orange.
    var temperature := rng.randf()
    var color: Color
    if temperature < 0.15:
        color = Color(0.72, 0.78, 1.0)
    elif temperature < 0.7:
        color = Color(0.95, 0.96, 1.0)
    elif temperature < 0.92:
        color = Color(1.0, 0.93, 0.78)
    else:
        color = Color(1.0, 0.8, 0.6)
    var value := brightness * rng.randf_range(0.55, 1.0)
    return Color(color.r * value, color.g * value, color.b * value)

# --- Sun halo ----------------------------------------------------------------

func _radial_falloff_texture() -> ImageTexture:
    if _halo_texture != null:
        return _halo_texture
    var size := 256
    var image := Image.create(size, size, false, Image.FORMAT_RGBA8)
    var center := Vector2(size, size) * 0.5
    for y in range(size):
        for x in range(size):
            var distance := Vector2(x + 0.5, y + 0.5).distance_to(center) / (size * 0.5)
            # Bright compact core with a long soft tail.
            var alpha: float = clamp(pow(maxf(1.0 - distance, 0.0), 2.6), 0.0, 1.0)
            var core: float = clamp(pow(maxf(1.0 - distance * 2.4, 0.0), 1.4), 0.0, 1.0)
            var warm := Color(1.0, 0.92, 0.66) * alpha + Color(1.0, 1.0, 0.95) * core
            image.set_pixel(x, y, Color(minf(warm.r, 1.0), minf(warm.g, 1.0), minf(warm.b, 1.0), minf(alpha + core, 1.0)))
    _halo_texture = ImageTexture.create_from_image(image)
    return _halo_texture

func _build_sun_halo() -> MeshInstance3D:
    var instance := MeshInstance3D.new()
    instance.name = "SunHalo"
    var quad := QuadMesh.new()
    quad.size = Vector2.ONE
    instance.mesh = quad
    var material := StandardMaterial3D.new()
    material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
    material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
    material.blend_mode = BaseMaterial3D.BLEND_MODE_ADD
    material.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
    material.albedo_texture = _radial_falloff_texture()
    # HDR push so the halo core crosses the glow threshold and blooms.
    material.albedo_color = Color(1.6, 1.45, 1.15)
    material.no_depth_test = true
    material.disable_receive_shadows = true
    instance.material_override = material
    instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
    return instance

# --- Orbit rings -------------------------------------------------------------

func _make_ring_mesh(color: Color) -> MeshInstance3D:
    var mesh := ImmediateMesh.new()
    mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
    for i in range(ORBIT_RING_SEGMENTS + 1):
        var angle := TAU * float(i) / float(ORBIT_RING_SEGMENTS)
        mesh.surface_add_vertex(Vector3(cos(angle), 0.0, sin(angle)))
    mesh.surface_end()
    var instance := MeshInstance3D.new()
    instance.mesh = mesh
    var material := StandardMaterial3D.new()
    material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
    material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
    material.albedo_color = Color(color.r, color.g, color.b, ORBIT_RING_ALPHA)
    instance.material_override = material
    return instance

# world_parent: node the rings live under (world root, so the origin shift applies).
# bodies: snapshot body dicts; positions: body_id -> scaled Vector3; colors: body_id -> Color.
func update_orbit_rings(world_parent: Node3D, bodies: Array, positions: Dictionary, colors: Dictionary) -> void:
    for body in bodies:
        var body_id := String(body.get("id", ""))
        var parent_id := String(body.get("parent_id", ""))
        if parent_id == "" or not positions.has(body_id) or not positions.has(parent_id):
            continue
        var body_position: Vector3 = positions[body_id]
        var parent_position: Vector3 = positions[parent_id]
        var radius := body_position.distance_to(parent_position)
        if radius <= 0.0001:
            continue
        if not _orbit_rings.has(body_id):
            var ring := _make_ring_mesh(colors.get(body_id, Color(0.6, 0.65, 0.7)))
            ring.name = "%s_orbit" % body_id
            world_parent.add_child(ring)
            _orbit_rings[body_id] = ring
        var ring_node: MeshInstance3D = _orbit_rings[body_id]
        ring_node.position = parent_position
        ring_node.scale = Vector3.ONE * radius

# --- Saturn ring -------------------------------------------------------------

# Flat annulus around the planet, in units of the (already scaled) planet radius.
static func make_planet_ring() -> MeshInstance3D:
    var inner := 1.35
    var outer := 2.3
    var segments := 96
    var vertices := PackedVector3Array()
    var colors := PackedColorArray()
    var indices := PackedInt32Array()
    for i in range(segments + 1):
        var angle := TAU * float(i) / float(segments)
        var direction := Vector3(cos(angle), 0.0, sin(angle))
        vertices.append(direction * inner)
        vertices.append(direction * outer)
        # Subtle radial banding baked into vertex alpha.
        var band := 0.55 + 0.25 * sin(angle * 0.0 + float(i % 5))
        colors.append(Color(0.85, 0.78, 0.6, 0.55))
        colors.append(Color(0.7, 0.62, 0.48, 0.12 * band))
        if i < segments:
            var base := i * 2
            indices.append_array([base, base + 1, base + 2, base + 1, base + 3, base + 2])
    var arrays := []
    arrays.resize(Mesh.ARRAY_MAX)
    arrays[Mesh.ARRAY_VERTEX] = vertices
    arrays[Mesh.ARRAY_COLOR] = colors
    arrays[Mesh.ARRAY_INDEX] = indices
    var mesh := ArrayMesh.new()
    mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
    var instance := MeshInstance3D.new()
    instance.name = "PlanetRing"
    instance.mesh = mesh
    var material := StandardMaterial3D.new()
    material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
    material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
    material.vertex_color_use_as_albedo = true
    material.cull_mode = BaseMaterial3D.CULL_DISABLED
    instance.material_override = material
    instance.rotation_degrees = Vector3(27.0, 0.0, 0.0)
    return instance
