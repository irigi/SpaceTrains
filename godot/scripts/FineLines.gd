extends Node

# The fine lines (docs/plans/main_gd_split.md, step 5): orbit rings redrawn near the focus, the
# planned paths (coarse trails and fine paths near the camera), the flown-path history trails,
# burn markers and the selected ship's destination ghost, with their materials. The meshes go
# under Main's world_root and entity_root as before; this node (a child of Main) only gives them
# the viewport.

const MapIconsScript := preload("res://scripts/MapIcons.gd")
const PathMotionScript := preload("res://scripts/PathMotion.gd")
const DisplayClockScript := preload("res://scripts/DisplayClock.gd")
const POSITION_SCALE := PathMotionScript.POSITION_SCALE

const BURN_MARKER_SCALE := 0.012  # world units (~96,000 km): the burn markers' size from afar
var trail_nodes: Dictionary = {}
var trail_path_signatures: Dictionary = {}
var trail_points: Dictionary = {}  # ship_id -> {points (metres + time, as drawn), color, alpha}
var ship_trail_history: Dictionary = {}   # ship_id -> Array of Vector3 (world_root local, or its frame planet's)
var history_frames: Dictionary = {}  # ship_id -> the planet its history trail is drawn around ("" for the Sun)
var history_trail_nodes: Dictionary = {}  # ship_id -> MeshInstance3D
var trail_sample_accum_s := 0.0
const TRAIL_MAX_POINTS := 40
const TRAIL_SAMPLE_INTERVAL_S := 0.15
const TRAJECTORY_COLORS := {
    "keplerian_hohmann": Color(1.0, 0.62, 0.2),
    "keplerian_lambert": Color(0.3, 1.0, 0.8),
    "keplerian_local": Color(0.4, 1.0, 0.45),
    "variable_isp": Color(0.75, 0.45, 1.0),
}
# Chases stand out: salvage racers and police cutters in bright yellow, drawn strong even when
# not selected; a wreck's drift is a dark grey line (MapIcons uses the same colours).
const CHASE_COLOR := Color(1.0, 0.95, 0.15)
const DERELICT_COLOR := Color(0.42, 0.42, 0.45)
var destination_body_ghost: MeshInstance3D
# A fixed polygon looks like one up close, and the planet sits on the true circle. Rings
# that pass near the focus are redrawn each frame, densest where the camera is, in
# focus-relative double precision; the coarse ring is hidden meanwhile.
var fine_rings: Dictionary = {}  # body_id -> MeshInstance3D
const FINE_RING_POINTS := 720
# Planned paths are drawn in absolute coordinates (32-bit floats: steps of ~16 km at 1 AU,
# ~500 km at Neptune, about the size of a ship) as straight pieces a tenth of a day or a
# day long. Up close that zig-zags against the ship, which is placed in double precision.
# So when the camera is close, a shader hides the coarse lines within a radius of the focus,
# and every stretch of a path inside that radius is redrawn finely, relative to an anchor
# near the focus (offsets small enough for floats), densest near the focus. The fine lines
# are rebuilt only when the camera, the focus or the ship has moved a good part of the way.
var fine_paths: Dictionary = {}  # ship_id -> {node, sig, focus (metres), camera, time, radius}
var trail_material: ShaderMaterial
const FINE_PATH_MAX_CAMERA_M := 3.0e9   # ~0.02 AU of zoom distance
const FINE_PATH_REACH := 8.0            # the fine stretch reaches this many camera distances
const FINE_PATH_SPACING := 1.0 / 60.0   # point spacing as a share of the distance to the focus
const FINE_PATH_MAX_POINTS := 1500
const FINE_PATH_MAX := 8
const TRAIL_CHUNK := 32
const TRAIL_SHADER := """
shader_type spatial;
render_mode unshaded, blend_mix, cull_disabled, depth_draw_never;
uniform vec3 focus = vec3(0.0);
uniform float hide_radius = 0.0;
// Each vertex carries its time in UV.x (seconds after the line's epoch). The part before
// now_s is drawn in flown_color, so the flown/ahead split follows the ship every frame
// and falls exactly where the ship is, even inside a long segment.
instance uniform float now_s = -1.0e30;
instance uniform vec4 flown_color = vec4(0.0);
varying vec3 world_pos;
varying float t_s;
void vertex() {
    world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    t_s = UV.x;
}
void fragment() {
    if (hide_radius > 0.0 && distance(world_pos, focus) < hide_radius) {
        discard;
    }
    vec4 color = t_s < now_s ? flown_color : COLOR;
    ALBEDO = color.rgb;
    ALPHA = color.a;
}
"""
var fine_path_material: ShaderMaterial

# Set in setup(): Main's nodes and helpers (entity dictionaries and body_positions by reference,
# never reassigned).
var world_root: Node3D
var entity_root: Node3D
var camera: Camera3D
var camera_rig: Node3D
var space_env: Node3D
var map_icons: MapIconsScript
var path_motion: PathMotionScript
var clock: DisplayClockScript
var entity_nodes: Dictionary
var entity_details: Dictionary
var entity_kinds: Dictionary
var body_positions: Dictionary
var _body_color_fn: Callable
var _body_display_scale_fn: Callable
var _scaled_position_fn: Callable
# Given by the entry points that read them (Main's values at the call).
var selected_id := ""
var selected_kind := ""
var render_origin := Vector3.ZERO

func setup(host: Node3D, motion: PathMotionScript, display_clock: DisplayClockScript) -> void:
    world_root = host.world_root
    entity_root = host.entity_root
    camera = host.camera
    camera_rig = host.camera_rig
    space_env = host.space_env
    map_icons = host.map_icons
    path_motion = motion
    clock = display_clock
    entity_nodes = host.entity_nodes
    entity_details = host.entity_details
    entity_kinds = host.entity_kinds
    body_positions = host.body_positions
    _body_color_fn = Callable(host, "_body_color")
    _body_display_scale_fn = Callable(host, "_body_display_scale")
    _scaled_position_fn = Callable(host, "_scaled_position")

func _body_color(body_id: String) -> Color:
    return _body_color_fn.call(body_id)

func _body_display_scale(body_id: String) -> float:
    return _body_display_scale_fn.call(body_id)

func _scaled_position(data: Dictionary) -> Vector3:
    return _scaled_position_fn.call(data)

# An entity gone from the snapshot: its lines go with it.
func forget_ship(entity_id: String) -> void:
    if trail_nodes.has(entity_id):
        trail_nodes[entity_id].queue_free()
        trail_nodes.erase(entity_id)
        trail_path_signatures.erase(entity_id)
        trail_points.erase(entity_id)
    if fine_paths.has(entity_id):
        if fine_paths[entity_id]["node"] != null:
            (fine_paths[entity_id]["node"] as Node).queue_free()
        fine_paths.erase(entity_id)
    if history_trail_nodes.has(entity_id):
        (history_trail_nodes[entity_id] as Node).queue_free()
        history_trail_nodes.erase(entity_id)
        ship_trail_history.erase(entity_id)
# A new timeline (a loaded game): the history trails drawn from the old one go with it.
func reset() -> void:
    ship_trail_history.clear()
    # The trails drawn from the old timeline go with it (after a load, a ship docked in the
    # save kept its old trail on screen).
    for ship_id in history_trail_nodes.keys():
        (history_trail_nodes[ship_id] as Node).queue_free()
    history_trail_nodes.clear()
    trail_path_signatures.clear()

func update_fine_rings(origin: Vector3) -> void:
    render_origin = origin
    if space_env == null:
        return
    var camera_distance_m := float(camera_rig.get("distance")) / POSITION_SCALE
    var active := {}
    for body_id in path_motion.body_orbits.keys():
        var orbit: Dictionary = path_motion.body_orbits[body_id]
        var parent_id := String(orbit["parent"])
        var a := float(orbit["a"])
        if parent_id == "" or a <= 0.0:
            continue
        var center := path_motion.body_m(parent_id)
        var dx := path_motion.origin_m[0] - center[0]
        var dz := path_motion.origin_m[2] - center[2]
        var d := sqrt(dx * dx + dz * dz)
        # Only rings the camera is close to, compared with their size.
        if absf(d - a) > 0.02 * a or camera_distance_m > 0.2 * a:
            continue
        active[body_id] = true
        var ring := _ensure_fine_ring(String(body_id))
        var theta_c := atan2(dz, dx)
        # Points every ~1/300 of the camera distance at the focus, coarser away from it:
        # theta = theta_c + pi sinh(k u) / sinh(k), u uniform in [-1, 1].
        var du := 2.0 / float(FINE_RING_POINTS)
        var wanted := clampf(camera_distance_m / 300.0 / a, 1.0e-12, PI * du)
        var k := _sinh_stretch(wanted / (PI * du))
        var sinh_k := sinh(k)
        var mesh := ImmediateMesh.new()
        mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
        for i in range(FINE_RING_POINTS + 1):
            var u := -1.0 + float(i) * du
            var theta := theta_c + PI * (sinh(k * u) / sinh_k if k > 1.0e-4 else u)
            mesh.surface_add_vertex(path_motion.to_render(PackedFloat64Array([center[0] + cos(theta) * a, center[1], center[2] + sin(theta) * a])))
        mesh.surface_end()
        ring.mesh = mesh
        ring.visible = true
    for body_id in fine_rings.keys():
        if not active.has(body_id):
            (fine_rings[body_id] as MeshInstance3D).visible = false
    var viewport_height: float = maxf(float(get_viewport().get_visible_rect().size.y), 1.0)
    var focal: float = viewport_height / (2.0 * tan(deg_to_rad(camera.fov) * 0.5))
    for body_id in path_motion.body_orbits.keys():
        space_env.set_ring_visible(String(body_id), not active.has(body_id))
        var orbit: Dictionary = path_motion.body_orbits[body_id]
        var parent_id := String(orbit["parent"])
        if parent_id == "" or parent_id == "sun" or not body_positions.has(parent_id):
            continue
        # Moons: fade the ring between 6 and 30 pixels of radius on screen.
        var parent_screen: Vector3 = body_positions[parent_id] - render_origin
        var distance := maxf(camera.global_position.distance_to(parent_screen), 1.0e-9)
        var radius_px := float(orbit["a"]) * POSITION_SCALE / distance * focal
        space_env.set_ring_fade(String(body_id), clampf((radius_px - 6.0) / 24.0, 0.0, 1.0))

func _trail_material() -> ShaderMaterial:
    if trail_material == null:
        var shader := Shader.new()
        shader.code = TRAIL_SHADER
        trail_material = ShaderMaterial.new()
        trail_material.shader = shader
    return trail_material

# Fine lines use the same shader but are never hidden near the focus.
func _fine_path_material() -> ShaderMaterial:
    if fine_path_material == null:
        fine_path_material = ShaderMaterial.new()
        fine_path_material.shader = _trail_material().shader
    return fine_path_material

# Lines drawn around a planet (planned paths and history trails of flights within one planet's
# system) follow that planet every frame.
func update_path_frames(sel_id: String) -> void:
    selected_id = sel_id
    for ship_id in trail_points.keys():
        if trail_nodes.has(ship_id):
            (trail_nodes[ship_id] as Node3D).position = path_motion.abs_render(path_motion.frame_now_m(String(trail_points[ship_id].get("frame", ""))))
    for ship_id in history_frames.keys():
        if history_trail_nodes.has(ship_id):
            (history_trail_nodes[ship_id] as Node3D).position = path_motion.abs_render(path_motion.frame_now_m(String(history_frames[ship_id])))
    # Burn markers of the selected path: as before from afar, never more than a few pixels
    # close up (their fixed size is ~100,000 km, larger than an Earth-Luna transfer).
    if trail_nodes.has(selected_id):
        for marker_name in ["burn0", "burn1"]:
            var marker: Node3D = (trail_nodes[selected_id] as Node3D).get_node_or_null(marker_name)
            if marker != null and marker.visible and marker.is_inside_tree():
                marker.scale = Vector3.ONE * minf(BURN_MARKER_SCALE, map_icons.world_size_for_pixels(marker, 4.0))

# Moves the flown/ahead split of every drawn path to the display time (cheap: one
# per-instance value per line, no rebuild).
func update_path_clocks() -> void:
    for ship_id in trail_points.keys():
        var style: Dictionary = trail_points[ship_id]
        var now := float(clock.display_time_s - float(style["epoch"]))
        if trail_nodes.has(ship_id):
            var path_mesh: MeshInstance3D = (trail_nodes[ship_id] as Node3D).get_node("path")
            path_mesh.set_instance_shader_parameter("now_s", now)
            path_mesh.set_instance_shader_parameter("flown_color", style["flown"])
        if fine_paths.has(ship_id) and fine_paths[ship_id]["node"] != null:
            var node: MeshInstance3D = fine_paths[ship_id]["node"]
            node.set_instance_shader_parameter("now_s", now)
            node.set_instance_shader_parameter("flown_color", style["flown"])

func update_fine_paths() -> void:
    var camera_distance_m := float(camera_rig.get("distance")) / POSITION_SCALE
    var active := {}
    var hide_radius_m := 0.0
    var pivot: Vector3 = camera_rig.get("pivot")
    if camera_distance_m <= FINE_PATH_MAX_CAMERA_M:
        var focus_m := PackedFloat64Array([path_motion.origin_m[0] + pivot.x / POSITION_SCALE,
            path_motion.origin_m[1] + pivot.y / POSITION_SCALE, path_motion.origin_m[2] + pivot.z / POSITION_SCALE])
        # Far enough out that the coarse line's float error is under a pixel or so.
        var float_error_m := 1.2e-7 * sqrt(focus_m[0] * focus_m[0] + focus_m[2] * focus_m[2]) + 2.0e4
        hide_radius_m = maxf(FINE_PATH_REACH * camera_distance_m, 300.0 * float_error_m)
        var draw_radius_m := 1.35 * hide_radius_m
        var candidates: Array = []
        for ship_id in trail_points.keys():
            if not path_motion.ship_paths.has(ship_id) or not trail_nodes.has(ship_id) or not (trail_nodes[ship_id] as Node3D).visible:
                continue
            var local_m := path_motion.minus_m(focus_m, path_motion.frame_now_m(String(trail_points[ship_id].get("frame", ""))))
            var gap := INF
            for box in trail_points[ship_id]["boxes"]:
                gap = minf(gap, _box_gap(box, local_m))
            if gap <= draw_radius_m:
                candidates.append([gap, ship_id])
        candidates.sort_custom(func(x, y): return x[0] < y[0])
        for k in range(mini(candidates.size(), FINE_PATH_MAX)):
            var ship_id: String = candidates[k][1]
            # Paths around a planet are handled in its frame: the focus relative to the planet.
            var frame_m := path_motion.frame_now_m(String(trail_points[ship_id].get("frame", "")))
            var local_m := path_motion.minus_m(focus_m, frame_m)
            if not _fine_path_current(ship_id, local_m, camera_distance_m, hide_radius_m):
                _build_fine_path(ship_id, local_m, camera_distance_m, draw_radius_m)
            var entry: Dictionary = fine_paths[ship_id]
            var node: MeshInstance3D = entry["node"]
            if node == null or not bool(entry["drawn"]):
                continue  # the path passes near the focus's box only, not the focus
            node.position = path_motion.to_render(path_motion.plus_m(entry["focus"], frame_m))
            node.visible = true
            active[ship_id] = true
    # A fine path drawn replaces the ship's history trail; one not drawn is hidden (and the
    # history trail stays as it is).
    for ship_id in fine_paths.keys():
        var node: MeshInstance3D = fine_paths[ship_id]["node"]
        if active.has(ship_id):
            if history_trail_nodes.has(ship_id):
                (history_trail_nodes[ship_id] as MeshInstance3D).visible = false
        elif node != null:
            node.visible = false
    var material := _trail_material()
    material.set_shader_parameter("focus", pivot)
    material.set_shader_parameter("hide_radius", hide_radius_m * POSITION_SCALE)

# Whether the fine line built last still covers the view: same plan, similar zoom, and
# neither the focus nor the ship has moved more than a fraction of the radius.
func _fine_path_current(ship_id: String, focus_m: PackedFloat64Array, camera_distance_m: float, hide_radius_m: float) -> bool:
    if not fine_paths.has(ship_id):
        return false
    var entry: Dictionary = fine_paths[ship_id]
    if String(entry["sig"]) != String(trail_points[ship_id]["sig"]):
        return false
    var ratio := camera_distance_m / float(entry["camera"])
    if ratio < 0.8 or ratio > 1.25:
        return false
    var built: PackedFloat64Array = entry["focus"]
    var moved := sqrt(pow(focus_m[0] - built[0], 2.0) + pow(focus_m[1] - built[1], 2.0) + pow(focus_m[2] - built[2], 2.0))
    return moved + hide_radius_m <= float(entry["radius"]) * 0.95

# How far p lies outside a box [min x, y, z, max x, y, z] (0 inside), per axis at most.
func _box_gap(box: PackedFloat64Array, p: PackedFloat64Array) -> float:
    var gap := 0.0
    for axis in range(3):
        gap = maxf(gap, maxf(box[axis] - p[axis], p[axis] - box[axis + 3]))
    return gap

# Redraws every stretch of the path within draw_radius_m of the focus (entry "drawn" false
# if there is none, so the next frames skip the path until the view changes).
func _build_fine_path(ship_id: String, focus_m: PackedFloat64Array, camera_distance_m: float, draw_radius_m: float) -> void:
    var style: Dictionary = trail_points[ship_id]
    var points: Array = style["points"]
    var path: Dictionary = path_motion.ship_paths[ship_id]
    var line_color: Color = style["color"]
    var alpha: float = style["alpha"]
    var epoch: float = style["epoch"]
    var ahead := Color(line_color.r, line_color.g, line_color.b, alpha)
    var runs: Array = []  # each a list of [x, y, z, t] relative to focus_m
    var run: Array = []
    var budget := FINE_PATH_MAX_POINTS
    var boxes: Array = style["boxes"]
    for i in range(points.size() - 1):
        var a: PackedFloat64Array = points[i]
        var b: PackedFloat64Array = points[i + 1]
        var chunk_far := _box_gap(boxes[i / TRAIL_CHUNK], focus_m) > draw_radius_m
        if chunk_far or budget <= 0 or _segment_distance(a, b, focus_m) > draw_radius_m:
            if run.size() >= 2:
                runs.append(run)
            run = []
            continue
        var span := b[3] - a[3]
        if span <= 0.0:
            continue
        var length := sqrt(pow(b[0] - a[0], 2.0) + pow(b[1] - a[1], 2.0) + pow(b[2] - a[2], 2.0))
        var speed := maxf(length / span, 1.0e-3)
        # March along the segment with steps a fixed share of the distance to the focus.
        var t := a[3]
        var joined := not run.is_empty()  # the previous segment's end is this one's start
        while budget > 0:
            var p := path_motion.drawn_point_m(path, t)
            if not joined:
                run.append(PackedFloat64Array([p[0] - focus_m[0], p[1] - focus_m[1], p[2] - focus_m[2], t]))
                budget -= 1
            joined = false
            if t >= b[3]:
                break
            var d := sqrt(pow(p[0] - focus_m[0], 2.0) + pow(p[1] - focus_m[1], 2.0) + pow(p[2] - focus_m[2], 2.0))
            t = minf(b[3], t + maxf(maxf(d, camera_distance_m) * FINE_PATH_SPACING / speed, span * 1.0e-4))
    if run.size() >= 2:
        runs.append(run)
    var node: MeshInstance3D = fine_paths[ship_id]["node"] if fine_paths.has(ship_id) else null
    fine_paths[ship_id] = {"node": node, "drawn": not runs.is_empty(), "sig": style["sig"], "focus": focus_m,
        "camera": camera_distance_m, "radius": draw_radius_m}
    if runs.is_empty():
        return
    var mesh := ImmediateMesh.new()
    for piece in runs:
        mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
        for q in piece:
            mesh.surface_set_color(ahead)
            mesh.surface_set_uv(Vector2(float(q[3] - epoch), 0.0))
            mesh.surface_add_vertex(Vector3(q[0] * POSITION_SCALE, q[1] * POSITION_SCALE, q[2] * POSITION_SCALE))
        mesh.surface_end()
    if node == null:
        node = MeshInstance3D.new()
        node.name = "%s_fine_path" % ship_id
        node.material_override = _fine_path_material()
        entity_root.add_child(node)
    node.mesh = mesh
    fine_paths[ship_id]["node"] = node

# Distance from p to the segment a-b (points [x, y, z, ...]).
func _segment_distance(a: PackedFloat64Array, b: PackedFloat64Array, p: PackedFloat64Array) -> float:
    var abx := b[0] - a[0]
    var aby := b[1] - a[1]
    var abz := b[2] - a[2]
    var len2 := abx * abx + aby * aby + abz * abz
    var s := 0.0
    if len2 > 0.0:
        s = clampf(((p[0] - a[0]) * abx + (p[1] - a[1]) * aby + (p[2] - a[2]) * abz) / len2, 0.0, 1.0)
    var dx := a[0] + abx * s - p[0]
    var dy := a[1] + aby * s - p[1]
    var dz := a[2] + abz * s - p[2]
    return sqrt(dx * dx + dy * dy + dz * dz)

# k with k / sinh(k) = ratio (0 < ratio <= 1), by bisection.
func _sinh_stretch(ratio: float) -> float:
    if ratio >= 1.0:
        return 0.0
    var lo := 0.0
    var hi := 60.0
    for _i in range(50):
        var mid := 0.5 * (lo + hi)
        if mid / sinh(mid) > ratio:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)

func _ensure_fine_ring(body_id: String) -> MeshInstance3D:
    if fine_rings.has(body_id):
        return fine_rings[body_id]
    var instance := MeshInstance3D.new()
    instance.name = "%s_fine_orbit" % body_id
    var material := StandardMaterial3D.new()
    material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
    material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
    var color: Color = MapIconsScript.BODY_ICON_COLOR.get(body_id, Color(0.6, 0.65, 0.7))
    material.albedo_color = Color(color.r, color.g, color.b, space_env.ORBIT_RING_ALPHA)
    instance.material_override = material
    instance.extra_cull_margin = 16384.0
    entity_root.add_child(instance)
    fine_rings[body_id] = instance
    return instance

func update_history_trails(delta: float) -> void:
    trail_sample_accum_s += delta
    if trail_sample_accum_s < TRAIL_SAMPLE_INTERVAL_S:
        return
    trail_sample_accum_s = 0.0
    for entity_id in entity_nodes.keys():
        if entity_kinds.get(entity_id, "") != "ship":
            continue
        var detail: Dictionary = entity_details.get(entity_id, {})
        var in_transit: bool = String(detail.get("phase", "idle")) == "in_transit"
        if not in_transit:
            if ship_trail_history.has(entity_id):
                ship_trail_history.erase(entity_id)
                if history_trail_nodes.has(entity_id):
                    (history_trail_nodes[entity_id] as MeshInstance3D).visible = false
            continue
        # Around the planet for a flight within its system (as the planned path is drawn).
        var frame := String((path_motion.ship_paths.get(entity_id, {}) as Dictionary).get("frame", ""))
        if String(history_frames.get(entity_id, "")) != frame:
            ship_trail_history.erase(entity_id)
        history_frames[entity_id] = frame
        var history: Array = ship_trail_history.get(entity_id, [])
        history.append(path_motion.abs_render(path_motion.minus_m(path_motion.ship_m(entity_id), path_motion.frame_now_m(frame))))
        if history.size() > TRAIL_MAX_POINTS:
            history.pop_front()
        ship_trail_history[entity_id] = history
        if history.size() >= 2:
            _rebuild_history_trail(entity_id, history)

func _ensure_history_trail_node(ship_id: String) -> MeshInstance3D:
    if history_trail_nodes.has(ship_id):
        return history_trail_nodes[ship_id]
    var instance := MeshInstance3D.new()
    instance.name = "%s_history" % ship_id
    var material := StandardMaterial3D.new()
    material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
    material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
    material.vertex_color_use_as_albedo = true
    instance.material_override = material
    world_root.add_child(instance)
    history_trail_nodes[ship_id] = instance
    return instance

func _rebuild_history_trail(ship_id: String, history: Array) -> void:
    var instance := _ensure_history_trail_node(ship_id)
    var mesh := ImmediateMesh.new()
    mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
    var count := history.size()
    for i in range(count):
        var alpha := 0.7 * float(i) / float(count - 1)
        mesh.surface_set_color(Color(0.85, 0.92, 1.0, alpha))
        mesh.surface_add_vertex(history[i])
    mesh.surface_end()
    instance.mesh = mesh
    instance.visible = true

func _make_destination_ghost_material(body_id: String) -> Material:
    var material := StandardMaterial3D.new()
    material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
    material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
    var color := _body_color(body_id)
    color.a = 0.32
    material.albedo_color = color
    material.emission_enabled = true
    material.emission = _body_color(body_id)
    material.emission_energy_multiplier = 0.45
    return material

func _ensure_trail_node(ship_id: String) -> Node3D:
    if trail_nodes.has(ship_id):
        return trail_nodes[ship_id]
    var node := Node3D.new()
    node.name = "%s_trail" % ship_id
    var path_mesh := MeshInstance3D.new()
    path_mesh.name = "path"
    # Hides itself near the focus when a fine line is drawn there (_update_fine_paths).
    path_mesh.material_override = _trail_material()
    node.add_child(path_mesh)
    world_root.add_child(node)
    trail_nodes[ship_id] = node
    return node

# Draw planned trajectories for every moving ship: the selected one bright with
# burn markers, the rest as faint context lines. The flown-past portion of the
# path dims to gray; the remaining arc keeps the per-planner color.
func update_ship_trails(sel_id: String, sel_kind: String) -> void:
    selected_id = sel_id
    selected_kind = sel_kind
    var active := {}
    for ship_id in entity_details.keys():
        if entity_kinds.get(ship_id, "") != "ship":
            continue
        var ship: Dictionary = entity_details[ship_id]
        var phase := String(ship.get("phase", "idle"))
        if not phase in ["in_transit", "awaiting_departure", "derelict", "patrol"]:
            continue
        var trajectory_path: Array = ship.get("trajectory_path", [])
        if trajectory_path.size() < 2 or not path_motion.ship_paths.has(ship_id):
            continue
        active[ship_id] = true
        var trail_node := _ensure_trail_node(ship_id)
        var is_selected: bool = ship_id == selected_id
        # The flown part dims in the shader (_update_path_clocks), so the line is rebuilt only
        # for a new plan or a new selection. The bridge's path id names the plan.
        var plan_id := String(ship.get("path_id", ""))
        if plan_id == "":
            plan_id = path_motion.trajectory_path_signature(trajectory_path)
        var signature := "%s|%s|%s" % [plan_id, is_selected, String(ship.get("salvage_derelict_id", ""))]
        if trail_path_signatures.get(ship_id, "") != signature:
            _rebuild_trail_mesh(trail_node, ship_id, ship, is_selected)
            trail_path_signatures[ship_id] = signature
        trail_node.visible = true

    for ship_id in trail_nodes.keys():
        if not active.has(ship_id):
            trail_nodes[ship_id].visible = false

    if selected_kind == "ship" and active.has(selected_id):
        _update_destination_body_ghost(entity_details[selected_id])
    elif destination_body_ghost != null:
        destination_body_ghost.visible = false

func _trajectory_color(trajectory_type: String) -> Color:
    return TRAJECTORY_COLORS.get(trajectory_type, Color(0.3, 1.0, 0.8)) as Color

func _rebuild_trail_mesh(trail_node: Node3D, ship_id: String, ship: Dictionary, is_selected: bool) -> void:
    var line_color := _trajectory_color(String(ship.get("trajectory_type", "")))
    var base_alpha := 0.95 if is_selected else 0.2
    var elapsed_color := Color(0.5, 0.52, 0.55, base_alpha * 0.45)
    var phase := String(ship.get("phase", ""))
    if phase == "derelict":
        line_color = DERELICT_COLOR
        base_alpha = 0.95 if is_selected else 0.7
        elapsed_color = Color(0, 0, 0, 0)   # the drift so far is not drawn
    elif phase == "patrol" or String(ship.get("salvage_derelict_id", "")) != "":
        line_color = CHASE_COLOR
        base_alpha = 1.0 if is_selected else 0.8
        elapsed_color = Color(0.5, 0.52, 0.55, 0.3)
    var path: Dictionary = path_motion.ship_paths[ship_id]
    var points := _rail_line_m(path)
    # Bounding boxes of runs of TRAIL_CHUNK segments, so the fine drawing finds the stretches
    # near the focus without measuring every segment.
    var boxes: Array = []
    for start in range(0, points.size() - 1, TRAIL_CHUNK):
        var box := PackedFloat64Array([INF, INF, INF, -INF, -INF, -INF])
        for i in range(start, mini(start + TRAIL_CHUNK + 1, points.size())):
            var point: PackedFloat64Array = points[i]
            for axis in range(3):
                box[axis] = minf(box[axis], point[axis])
                box[axis + 3] = maxf(box[axis + 3], point[axis])
        boxes.append(box)
    var epoch: float = points.front()[3]
    var frame := String(path.get("frame", ""))
    trail_points[ship_id] = {"points": points, "boxes": boxes, "color": line_color, "alpha": base_alpha, "flown": elapsed_color, "epoch": epoch,
        "frame": frame, "sig": "%s|%s|%.3f" % [String(path["sig"]), is_selected, base_alpha]}
    trail_node.position = path_motion.abs_render(path_motion.frame_now_m(frame))
    var mesh := ImmediateMesh.new()
    mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
    var ahead := Color(line_color.r, line_color.g, line_color.b, base_alpha)
    for point in points:
        mesh.surface_set_color(ahead)
        mesh.surface_set_uv(Vector2(float(point[3] - epoch), 0.0))
        mesh.surface_add_vertex(path_motion.abs_render(point))
    mesh.surface_end()
    var path_mesh: MeshInstance3D = trail_node.get_node("path")
    path_mesh.mesh = mesh
    _update_burn_markers(trail_node, points.front(), points.back(), ship, is_selected, line_color)

# The drawn line follows the rail from departure to arrival (the wait for the launch
# window lies on the planet's own orbit ring and is not drawn). Segments are subdivided,
# finely near the planets where the camera may be close. Points are relative to the path's
# frame planet when it has one (the trail node then follows the planet).
func _rail_line_m(path: Dictionary) -> Array:
    var times: PackedFloat64Array = path["t"]
    var departure := maxf(float(path["departure"]), times[0])
    var arrival := minf(float(path["arrival"]), times[times.size() - 1])
    if arrival <= departure:
        departure = times[0]
        arrival = times[times.size() - 1]
    var flight := maxf(arrival - departure, 1.0)
    var stops := PackedFloat64Array([departure])
    for t in times:
        if t > departure and t < arrival:
            stops.append(t)
    stops.append(arrival)
    var points: Array = []
    for k in range(stops.size() - 1):
        var t0 := stops[k]
        var t1 := stops[k + 1]
        var near_planet := minf(t1 - departure, arrival - t0) < maxf(3.0 * 86400.0, 0.1 * flight)
        var step := 0.1 * 86400.0 if near_planet else 86400.0
        var pieces := clampi(int(ceil((t1 - t0) / step)), 1, 64)
        for j in range(pieces):
            var t := t0 + (t1 - t0) * float(j) / float(pieces)
            var p := path_motion.drawn_point_m(path, t)
            points.append(PackedFloat64Array([p[0], p[1], p[2], t]))
    var last := path_motion.drawn_point_m(path, arrival)
    points.append(PackedFloat64Array([last[0], last[1], last[2], arrival]))
    return points

# Impulsive-burn planners get small emissive markers at the departure and
# arrival burns; continuous-thrust (variable ISP) paths have no discrete burns.
func _update_burn_markers(trail_node: Node3D, first: PackedFloat64Array, last: PackedFloat64Array, ship: Dictionary, is_selected: bool, line_color: Color) -> void:
    var wants_markers: bool = is_selected and not String(ship.get("trajectory_type", "")) in ["variable_isp", "derelict", "patrol"]
    for marker_index in range(2):
        var marker_name := "burn%d" % marker_index
        var marker: MeshInstance3D = trail_node.get_node_or_null(marker_name)
        if not wants_markers:
            if marker != null:
                marker.visible = false
            continue
        if marker == null:
            marker = MeshInstance3D.new()
            marker.name = marker_name
            var sphere := SphereMesh.new()
            sphere.radius = 1.0
            sphere.height = 2.0
            sphere.radial_segments = 10
            sphere.rings = 5
            marker.mesh = sphere
            var material := StandardMaterial3D.new()
            material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
            material.albedo_color = line_color
            material.emission_enabled = true
            material.emission = line_color
            material.emission_energy_multiplier = 1.6
            marker.material_override = material
            marker.scale = Vector3.ONE * BURN_MARKER_SCALE
            trail_node.add_child(marker)
        (marker.material_override as StandardMaterial3D).albedo_color = line_color
        (marker.material_override as StandardMaterial3D).emission = line_color
        marker.position = path_motion.abs_render(first if marker_index == 0 else last)
        marker.visible = true

func _ensure_destination_body_ghost() -> MeshInstance3D:
    if destination_body_ghost != null:
        return destination_body_ghost
    destination_body_ghost = MeshInstance3D.new()
    destination_body_ghost.name = "DestinationBodyGhost"
    var mesh := SphereMesh.new()
    mesh.radius = 1.0
    mesh.height = 2.0
    mesh.radial_segments = 32
    mesh.rings = 16
    destination_body_ghost.mesh = mesh
    destination_body_ghost.visible = false
    world_root.add_child(destination_body_ghost)
    return destination_body_ghost

func update_selected_overlay_positions(sel_id: String, sel_kind: String) -> void:
    selected_id = sel_id
    selected_kind = sel_kind
    if destination_body_ghost != null and destination_body_ghost.visible and selected_kind == "ship" and entity_details.has(selected_id):
        var ship: Dictionary = entity_details[selected_id]
        var destination_body: Dictionary = ship.get("destination_body_at_arrival", {})
        if not destination_body.is_empty():
            destination_body_ghost.position = _destination_ghost_position(destination_body)

# Where the selected ship's destination body will be at arrival; for a flight around a planet,
# where it will be relative to the planet, drawn around the planet now (at the path's end).
func _destination_ghost_position(destination_body: Dictionary) -> Vector3:
    var path: Dictionary = path_motion.ship_paths.get(selected_id, {})
    var frame := String(path.get("frame", ""))
    if frame == "":
        return _scaled_position(destination_body)
    var at_arrival := PackedFloat64Array([float(destination_body["x"]), float(destination_body.get("y", 0.0)), float(destination_body["z"])])
    var relative := path_motion.minus_m(at_arrival, path_motion.body_at_m(frame, float(path["arrival"])))
    return path_motion.abs_render(path_motion.plus_m(relative, path_motion.body_m(frame)))

func _update_destination_body_ghost(ship: Dictionary) -> void:
    var destination_body: Dictionary = ship.get("destination_body_at_arrival", {})
    if destination_body.is_empty():
        if destination_body_ghost != null:
            destination_body_ghost.visible = false
        return
    var ghost := _ensure_destination_body_ghost()
    var body_id := String(destination_body.get("id", ""))
    ghost.position = _destination_ghost_position(destination_body)
    ghost.scale = Vector3.ONE * _body_display_scale(body_id)
    ghost.material_override = _make_destination_ghost_material(body_id)
    ghost.visible = true
