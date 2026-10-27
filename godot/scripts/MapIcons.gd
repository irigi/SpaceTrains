extends Control

# The map's screen-space layer: an icon and a label per entity, placed over its 3D position
# each frame, handing off to the model once that is large on screen; a ring marks the
# selected icon. Main owns the entities; this reads them through the shared dictionaries.

const BODY_ICON_SIZE := {
    "star":         14.0,
    "planet":       9.0,
    "dwarf_planet": 7.0,
    "moon":         7.0,
}
const STATION_ICON_SIZE := 8.0
const SHIP_ICON_SIZE := 8.0
const ICON_ALPHA := 0.58
const BODY_ICON_ALPHA := 0.92
# Screen-space labels next to the map icons.
const LABEL_FONT_SIZE := {"body": 14, "station": 12, "ship": 11}
const LABEL_OFFSET_PX := Vector2(10.0, -9.0)
const BODY_TYPE := {
    "sun": "star",
    "mercury": "planet", "venus": "planet", "earth": "planet",
    "mars": "planet", "jupiter": "planet", "saturn": "planet",
    "uranus": "planet", "neptune": "planet",
    "ceres": "dwarf_planet",
    "luna": "moon", "europa": "moon", "ganymede": "moon",
    "titan": "moon", "triton": "moon",
}
const BODY_ICON_COLOR := {
    "sun":      Color(1.00, 0.88, 0.30),
    "mercury":  Color(0.70, 0.65, 0.60),
    "venus":    Color(0.95, 0.85, 0.50),
    "earth":    Color(0.20, 0.50, 1.00),
    "luna":     Color(0.80, 0.80, 0.80),
    "mars":     Color(0.90, 0.35, 0.20),
    "ceres":    Color(0.60, 0.55, 0.50),
    "jupiter":  Color(0.85, 0.65, 0.45),
    "europa":   Color(0.75, 0.70, 0.65),
    "ganymede": Color(0.65, 0.60, 0.55),
    "saturn":   Color(0.90, 0.80, 0.55),
    "titan":    Color(0.85, 0.65, 0.40),
    "uranus":   Color(0.50, 0.85, 0.90),
    "neptune":  Color(0.30, 0.45, 0.95),
    "triton":   Color(0.60, 0.70, 0.75),
}

var camera: Camera3D
var entity_nodes: Dictionary
var entity_details: Dictionary
var entity_kinds: Dictionary
var faction_colors: Dictionary
var icons: Dictionary = {}  # entity_id -> Sprite2D
var labels: Dictionary = {}  # entity_id -> Label
var _icon_textures: Dictionary = {}
var _selection_ring: Sprite2D
# Flow overlay (G): [{from: station id, to: station id, amount: float, color: Color}], drawn under
# the icons as lines between the stations, width by amount; empty = off.
var flow_lines: Array = []
var _flow_points: Array = []   # [[from px, to px, width, color]] for _draw

func _init() -> void:
    name = "MapIconLayer"
    mouse_filter = Control.MOUSE_FILTER_IGNORE
    set_anchors_preset(Control.PRESET_FULL_RECT)

func setup(camera_: Camera3D, nodes: Dictionary, details: Dictionary, kinds: Dictionary, colors: Dictionary) -> void:
    camera = camera_
    entity_nodes = nodes
    entity_details = details
    entity_kinds = kinds
    faction_colors = colors

func remove(entity_id: String) -> void:
    if icons.has(entity_id):
        (icons[entity_id] as Node).queue_free()
        icons.erase(entity_id)
    if labels.has(entity_id):
        (labels[entity_id] as Node).queue_free()
        labels.erase(entity_id)

func icon(entity_id: String) -> Sprite2D:
    return icons.get(entity_id)

# The size a model is drawn at in world units (its radius when it has one).
static func model_world_size(node: Node3D) -> float:
    if node.has_meta("radius_m"):
        return float(node.get_meta("radius_m")) * node.scale.x
    return node.scale.x

static func icon_shape(kind: String, _data: Dictionary) -> String:
    if kind == "body":
        return "circle"
    if kind == "station":
        return "diamond"
    return "triangle"

static func icon_pixel_size(kind: String, data: Dictionary) -> float:
    if kind == "body":
        var body_id := String(data.get("id", ""))
        return float(BODY_ICON_SIZE.get(BODY_TYPE.get(body_id, "planet"), 22.0))
    if kind == "station":
        return STATION_ICON_SIZE
    return SHIP_ICON_SIZE

func icon_color(kind: String, data: Dictionary) -> Color:
    if kind == "body":
        return BODY_ICON_COLOR.get(String(data.get("id", "")), Color(0.7, 0.7, 0.7)) as Color
    if kind == "station":
        return faction_colors.get(String(data.get("faction_id", "")), Color(0.95, 0.82, 0.36)) as Color
    var phase := String(data.get("phase", "idle"))
    if phase == "stranded":
        return Color(1.0, 0.25, 0.22)
    if phase == "derelict":
        return Color(1.0, 0.62, 0.25)
    if phase == "patrol":
        return Color(0.55, 0.75, 1.0)
    if phase == "in_transit":
        return Color(0.70, 1.0, 0.95)
    return faction_colors.get(String(data.get("faction_id", "")), Color(0.96, 0.97, 1.0)) as Color

func icon_texture(shape: String) -> ImageTexture:
    if _icon_textures.has(shape):
        return _icon_textures[shape] as ImageTexture
    var sz := 96
    var img := Image.create(sz, sz, false, Image.FORMAT_RGBA8)
    var center := Vector2(sz * 0.5, sz * 0.5)
    var outer := sz * 0.36
    for y in range(sz):
        for x in range(sz):
            var p: Vector2 = Vector2(x + 0.5, y + 0.5)
            var inside := false
            if shape == "circle":
                inside = p.distance_to(center) <= outer
            elif shape == "ring":
                var d := p.distance_to(center)
                inside = d <= sz * 0.46 and d >= sz * 0.40
            elif shape == "diamond":
                inside = abs(p.x - center.x) + abs(p.y - center.y) <= outer
            else:
                var top: Vector2 = Vector2(center.x, center.y - outer)
                var left: Vector2 = Vector2(center.x - outer * 0.82, center.y + outer * 0.72)
                var right: Vector2 = Vector2(center.x + outer * 0.82, center.y + outer * 0.72)
                var area: float = abs((left.x - top.x) * (right.y - top.y) - (right.x - top.x) * (left.y - top.y))
                var a: float = abs((top.x - p.x) * (left.y - p.y) - (left.x - p.x) * (top.y - p.y)) / area
                var b: float = abs((left.x - p.x) * (right.y - p.y) - (right.x - p.x) * (left.y - p.y)) / area
                var c: float = abs((right.x - p.x) * (top.y - p.y) - (top.x - p.x) * (right.y - p.y)) / area
                inside = a + b + c <= 1.01
            img.set_pixel(x, y, Color(1.0, 1.0, 1.0, 1.0 if inside else 0.0))
    var texture: ImageTexture = ImageTexture.create_from_image(img)
    _icon_textures[shape] = texture
    return texture


func _ensure_selection_ring() -> Sprite2D:
    if _selection_ring == null:
        _selection_ring = Sprite2D.new()
        _selection_ring.name = "SelectionRing"
        _selection_ring.texture = icon_texture("ring")
        _selection_ring.centered = true
        _selection_ring.modulate = Color(0.85, 0.95, 1.0, 0.9)
        _selection_ring.visible = false
        add_child(_selection_ring)
    return _selection_ring

func attach(entity_id: String, kind: String, data: Dictionary) -> void:
    if icons.has(entity_id):
        return
    var icon_node := Sprite2D.new()
    icon_node.name = "%s_icon" % entity_id
    icon_node.texture = icon_texture(icon_shape(kind, data))
    icon_node.centered = true
    var icon_color := icon_color(kind, data)
    icon_color.a = ICON_ALPHA
    icon_node.modulate = icon_color
    add_child(icon_node)
    icons[entity_id] = icon_node
    var label := Label.new()
    label.name = "%s_label" % entity_id
    label.text = String(data.get("name", entity_id))
    label.mouse_filter = Control.MOUSE_FILTER_IGNORE
    label.add_theme_font_size_override("font_size", int(LABEL_FONT_SIZE.get(kind, 11)))
    var label_color := icon_color(kind, data).lightened(0.45)
    label_color.a = 0.9 if kind == "body" else 0.75
    label.add_theme_color_override("font_color", label_color)
    label.add_theme_color_override("font_outline_color", Color(0.0, 0.0, 0.0, 0.85))
    label.add_theme_constant_override("outline_size", 4)
    label.visible = false
    add_child(label)
    labels[entity_id] = label

func projected_model_pixels(node: Node3D) -> float:
    var viewport_height: float = max(float(get_viewport().get_visible_rect().size.y), 1.0)
    var dist: float = max(camera.global_position.distance_to(node.global_position), 1.0e-14)
    return model_world_size(node) / (2.0 * dist * tan(deg_to_rad(camera.fov) * 0.5)) * viewport_height

# Inverse of the above: the world size that projects to target_pixels at the
# node's current camera distance.
func world_size_for_pixels(node: Node3D, target_pixels: float) -> float:
    var viewport_height: float = max(float(get_viewport().get_visible_rect().size.y), 1.0)
    var dist: float = max(camera.global_position.distance_to(node.global_position), 1.0e-14)
    return target_pixels / viewport_height * 2.0 * dist * tan(deg_to_rad(camera.fov) * 0.5)

func update(selected_id: String) -> void:
    _place_flow_lines()
    var viewport_rect := get_viewport().get_visible_rect()
    # Labels are placed in priority order (bodies, stations, ships; selected first) and
    # skipped where they would overlap one already placed.
    var label_candidates: Array = []
    var ring := _ensure_selection_ring()
    ring.visible = false
    for entity_id in icons.keys():
        var icon: Sprite2D = icons[entity_id]
        var label: Label = labels.get(entity_id)
        if label != null:
            label.visible = false
        var model: Node3D = entity_nodes.get(entity_id)
        if not model or not entity_details.has(entity_id) or camera.is_position_behind(model.global_position):
            icon.visible = false
            continue

        var kind: String = entity_kinds.get(entity_id, "")
        var data: Dictionary = entity_details[entity_id]
        var pixel_size := icon_pixel_size(kind, data)
        var selected_scale := 1.25 if entity_id == selected_id else 1.0
        var target_size := pixel_size * selected_scale
        var screen_pos := camera.unproject_position(model.global_position)
        if not viewport_rect.grow(pixel_size).has_point(screen_pos):
            icon.visible = false
            continue
        var model_pixels := projected_model_pixels(model)
        # Hand off from map icon to the actual mesh once it is large on screen.
        # The Sun's halo marks it at every zoom.
        icon.visible = model_pixels <= target_size * 1.5 and entity_id != "sun"
        if icon.visible and entity_id == selected_id:
            # A ring around the selected icon; up close the model itself is the marker.
            ring.visible = true
            ring.position = screen_pos
            ring.scale = Vector2.ONE * ((target_size + 10.0) / (0.92 * float(ring.texture.get_width())))
        if icon.visible:
            icon.position = screen_pos
            var texture_size := Vector2(icon.texture.get_width(), icon.texture.get_height())
            icon.scale = Vector2.ONE * (target_size / max(texture_size.x, texture_size.y))
            var color := (icon_color(kind, data).lightened(0.35) if entity_id == selected_id else icon_color(kind, data))
            color.a = BODY_ICON_ALPHA if kind == "body" else ICON_ALPHA
            icon.modulate = color
        if label == null:
            continue
        var priority := {"body": 0, "station": 1, "ship": 2}.get(kind, 3) as int
        if entity_id == selected_id:
            priority = -1
        elif kind == "ship" and not String(data.get("phase", "")) in ["in_transit", "derelict", "patrol"]:
            continue  # docked ships are listed by their station
        var offset := LABEL_OFFSET_PX
        if not icon.visible:
            # Beside the body's disc rather than over it.
            offset.x += minf(model_pixels, viewport_rect.size.x * 0.25)
        label_candidates.append({"id": entity_id, "priority": priority, "position": screen_pos + offset})
    label_candidates.sort_custom(func(a, b): return a["priority"] < b["priority"])
    var placed: Array[Rect2] = []
    for candidate in label_candidates:
        var label: Label = labels[candidate["id"]]
        var rect := Rect2(candidate["position"], label.get_minimum_size())
        var free := true
        for other in placed:
            if other.grow(2.0).intersects(rect):
                free = false
                break
        if not free:
            continue
        label.position = candidate["position"]
        label.visible = true
        placed.append(rect)

# The flow overlay's lines at the stations' screen positions (called each frame from update()).
func _place_flow_lines() -> void:
    var before := _flow_points.size()
    _flow_points.clear()
    if flow_lines.is_empty():
        if before > 0:
            queue_redraw()
        return
    var top := 0.0
    for line in flow_lines:
        top = maxf(top, float(line.get("amount", 0.0)))
    for line in flow_lines:
        var a: Node3D = entity_nodes.get(String(line.get("from", "")))
        var b: Node3D = entity_nodes.get(String(line.get("to", "")))
        if a == null or b == null or camera.is_position_behind(a.global_position) or camera.is_position_behind(b.global_position):
            continue
        var pa := camera.unproject_position(a.global_position)
        var pb := camera.unproject_position(b.global_position)
        if pa.distance_to(pb) < 6.0:
            continue
        var width := 1.0 + 9.0 * sqrt(float(line.get("amount", 0.0)) / maxf(top, 1e-9))
        _flow_points.append([pa, pb, width, line.get("color", Color(0.35, 0.85, 0.95))])
    queue_redraw()

func _draw() -> void:
    for p in _flow_points:
        var a: Vector2 = p[0]
        var b: Vector2 = p[1]
        var width: float = p[2]
        var color: Color = p[3]
        # Each direction on its own side of the line between the stations, with an arrow at its middle.
        var along := (b - a).normalized()
        var side := Vector2(-along.y, along.x) * (width * 0.5 + 1.5)
        var c := Color(color, 0.55)
        draw_line(a + side, b + side, c, width, true)
        var mid := (a + b) * 0.5 + side
        var head := 5.0 + width
        draw_colored_polygon(PackedVector2Array([mid + along * head, mid - along * head * 0.3 + side.normalized() * head * 0.8,
            mid - along * head * 0.3 - side.normalized() * head * 0.2]), Color(color, 0.8))
