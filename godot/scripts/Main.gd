extends Node3D

const POSITION_SCALE := 1.0 / 8_000_000_000.0
const SHIP_SCALE := 0.06
const STATION_SCALE := 0.11
const SNAPSHOT_POLL_INTERVAL := 0.12
const DEFAULT_TIMEWARP := 86400.0

@onready var world_root: Node3D = $WorldRoot
@onready var camera_rig: Node3D = $CameraRig
@onready var camera: Camera3D = $CameraRig/Camera3D
@onready var info_label: Label = $CanvasLayer/Info
@onready var entity_list: ItemList = $CanvasLayer/EntityList
@onready var selection_label: Label = $CanvasLayer/Selection
@onready var event_log: Label = $CanvasLayer/EventLog

var bridge_pid := -1
var snapshot_path := ""
var command_path := ""
var repo_root := ""
var executable_path := ""
var poll_accumulator := 0.0
var selected_id := ""
var selected_kind := ""
var bridge_state: Dictionary = {}
var entity_nodes: Dictionary = {}
var entity_targets: Dictionary = {}
var entity_details: Dictionary = {}
var entity_kinds: Dictionary = {}
var entity_visual_signatures: Dictionary = {}
var trail_nodes: Dictionary = {}
var current_paused := false
var current_timewarp := DEFAULT_TIMEWARP
var has_auto_focused := false
var bridge_started := false
var debug_guides: Array[Node3D] = []
var station_positions: Dictionary = {}
var body_positions: Dictionary = {}
var body_display_radii: Dictionary = {}

func _ready() -> void:
    repo_root = ProjectSettings.globalize_path("res://").get_base_dir().get_base_dir()
    executable_path = repo_root.path_join("build/bin/spacetrains_bridge")
    snapshot_path = ProjectSettings.globalize_path("user://spacetrains_snapshot.json")
    command_path = ProjectSettings.globalize_path("user://spacetrains_commands.json")
    _write_bridge_commands()
    _start_bridge()
    _create_debug_guides()
    if bridge_started:
        info_label.text = "SpaceTrains\nStarting bridge...\n%s" % executable_path
    selection_label.text = "No selection\nControls: RMB rotate, MMB pan, wheel zoom, left click select, F focus, Space pause, 1/2/3 timewarp"
    event_log.text = "Events\n"
    entity_list.item_selected.connect(_on_entity_selected)

func _exit_tree() -> void:
    if bridge_pid > 0:
        OS.kill(bridge_pid)

func _process(delta: float) -> void:
    poll_accumulator += delta
    if poll_accumulator >= SNAPSHOT_POLL_INTERVAL:
        poll_accumulator = 0.0
        _read_snapshot()
    _update_nodes(delta)

func _unhandled_input(event: InputEvent) -> void:
    if event is InputEventKey and event.pressed and not event.echo:
        if event.keycode == KEY_SPACE:
            current_paused = not current_paused
            _write_bridge_commands()
        elif event.keycode == KEY_1:
            current_timewarp = 3600.0
            _write_bridge_commands()
        elif event.keycode == KEY_2:
            current_timewarp = 21600.0
            _write_bridge_commands()
        elif event.keycode == KEY_3:
            current_timewarp = 86400.0
            _write_bridge_commands()
        elif event.keycode == KEY_F and selected_id != "" and entity_targets.has(selected_id):
            camera_rig.focus_point(entity_targets[selected_id])
    elif event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
        _pick_entity(event.position)

func _start_bridge() -> void:
    if not FileAccess.file_exists(executable_path):
        info_label.text = "Bridge executable not found:\n%s\nBuild the project first." % executable_path
        return

    var args := [
        "--data-root", repo_root.path_join("data"),
        "--snapshot-file", snapshot_path,
        "--command-file", command_path,
        "--step-seconds", "0.1"
    ]
    bridge_pid = OS.create_process(executable_path, args, false)
    if bridge_pid <= 0:
        info_label.text = "Failed to start bridge process."
        bridge_started = false
    else:
        bridge_started = true

func _write_bridge_commands() -> void:
    var file := FileAccess.open(command_path, FileAccess.WRITE)
    if file == null:
        return
    var payload := {
        "paused": current_paused,
        "timewarp_factor": current_timewarp
    }
    file.store_string(JSON.stringify(payload))

func _read_snapshot() -> void:
    if not FileAccess.file_exists(snapshot_path):
        return
    var file := FileAccess.open(snapshot_path, FileAccess.READ)
    if file == null:
        return
    var json := JSON.new()
    var parse_status := json.parse(file.get_as_text())
    if parse_status != OK or typeof(json.data) != TYPE_DICTIONARY:
        return
    bridge_state = json.data
    _apply_snapshot()

func _apply_snapshot() -> void:
    var seen_ids := {}
    var ids_changed := false
    station_positions.clear()
    body_positions.clear()
    body_display_radii.clear()

    for body in bridge_state.get("bodies", []):
        var body_id := String(body["id"])
        body_positions[body_id] = _scaled_position(body)
        body_display_radii[body_id] = _body_display_scale(body_id)

    for station in bridge_state.get("stations", []):
        var station_id := String(station["id"])
        station_positions[station_id] = _station_display_position(station)

    for body in bridge_state.get("bodies", []):
        _upsert_entity(body, "body")
        seen_ids[body["id"]] = true
    for station in bridge_state.get("stations", []):
        _upsert_entity(station, "station")
        seen_ids[station["id"]] = true
    for ship in bridge_state.get("ships", []):
        _upsert_entity(ship, "ship")
        seen_ids[ship["id"]] = true

    for entity_id in entity_nodes.keys():
        if not seen_ids.has(entity_id):
            entity_nodes[entity_id].queue_free()
            entity_nodes.erase(entity_id)
            entity_targets.erase(entity_id)
            entity_details.erase(entity_id)
            entity_kinds.erase(entity_id)
            entity_visual_signatures.erase(entity_id)
            ids_changed = true

    if ids_changed or entity_list.item_count != seen_ids.size():
        _rebuild_entity_list()
    _update_ship_trails()
    if not has_auto_focused:
        _hide_debug_guides()
        _auto_focus_initial_entity()
    _refresh_labels()

func _upsert_entity(data: Dictionary, kind: String) -> void:
    var entity_id: String = data["id"]
    entity_details[entity_id] = data
    entity_kinds[entity_id] = kind

    if not entity_nodes.has(entity_id):
        var mesh_instance := MeshInstance3D.new()
        mesh_instance.name = entity_id
        mesh_instance.mesh = _make_mesh(kind)
        world_root.add_child(mesh_instance)
        entity_nodes[entity_id] = mesh_instance
        entity_targets[entity_id] = Vector3.ZERO
        entity_visual_signatures[entity_id] = ""

    var visual_signature := _visual_signature(kind, data)
    if entity_visual_signatures.get(entity_id, "") != visual_signature:
        entity_nodes[entity_id].material_override = _make_material(kind, data)
        entity_nodes[entity_id].scale = _make_scale(kind, data)
        entity_visual_signatures[entity_id] = visual_signature
    entity_targets[entity_id] = _display_position(data, kind)

func _update_nodes(delta: float) -> void:
    for entity_id in entity_nodes.keys():
        var node: Node3D = entity_nodes[entity_id]
        var target: Vector3 = entity_targets[entity_id]
        node.position = node.position.lerp(target, clamp(delta * 8.0, 0.0, 1.0))

func _scaled_position(data: Dictionary) -> Vector3:
    return Vector3(
        float(data["x"]) * POSITION_SCALE,
        float(data.get("y", 0.0)) * POSITION_SCALE,
        float(data["z"]) * POSITION_SCALE
    )

func _display_position(data: Dictionary, kind: String) -> Vector3:
    var base := _scaled_position(data)
    if kind == "station":
        return station_positions.get(String(data["id"]), base)
    if kind != "ship":
        return base

    var ship_id := String(data["id"])
    var station_id := String(data.get("current_station_id", ""))
    var phase := String(data.get("phase", "idle"))
    var index: int = abs(ship_id.hash()) % 7
    var angle := float(index) * 0.8975979
    var ring_offset := Vector3(cos(angle), 0.02 + float(index % 3) * 0.01, sin(angle)) * 0.12

    if phase == "idle" or phase == "refueling" or phase == "stranded":
        if station_positions.has(station_id):
            return station_positions[station_id] + ring_offset
        return base + ring_offset

    if phase == "in_transit":
        var remaining := float(data.get("remaining_travel_time_s", 0.0))
        var total: float = max(float(data.get("total_travel_time_s", 0.0)), 1.0)
        var progress: float = clamp(1.0 - remaining / total, 0.0, 1.0)
        var arc_height: float = 0.18 + sin(progress * PI) * 0.75
        return base + Vector3(0.0, arc_height, 0.0)

    return base

func _make_mesh(kind: String) -> Mesh:
    match kind:
        "body":
            var sphere := SphereMesh.new()
            sphere.radius = 1.0
            sphere.height = 2.0
            return sphere
        "ship":
            var ship_mesh := SphereMesh.new()
            ship_mesh.radius = 1.0
            ship_mesh.height = 2.0
            return ship_mesh
        _:
            var box := BoxMesh.new()
            box.size = Vector3.ONE
            return box

func _body_display_scale(body_id: String) -> float:
    match body_id:
        "sun":
            return 2.1
        "mercury":
            return 0.24
        "venus":
            return 0.48
        "earth":
            return 0.50
        "mars":
            return 0.38
        "ceres":
            return 0.15
        _:
            return 0.28

func _make_scale(kind: String, data: Dictionary) -> Vector3:
    if kind == "body":
        return Vector3.ONE * _body_display_scale(String(data["id"]))
    if kind == "ship":
        return Vector3.ONE * SHIP_SCALE
    return Vector3.ONE * STATION_SCALE

func _body_color(body_id: String) -> Color:
    match body_id:
        "sun":
            return Color(1.0, 0.76, 0.28)
        "mercury":
            return Color(0.7, 0.66, 0.62)
        "venus":
            return Color(0.88, 0.72, 0.38)
        "earth":
            return Color(0.36, 0.58, 1.0)
        "mars":
            return Color(0.89, 0.42, 0.25)
        "ceres":
            return Color(0.72, 0.72, 0.78)
        _:
            return Color(0.75, 0.8, 0.88)

func _make_material(kind: String, data: Dictionary) -> Material:
    var material := StandardMaterial3D.new()
    material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
    if kind == "body":
        var body_id := String(data["id"])
        material.albedo_color = _body_color(body_id)
        if body_id == "sun":
            material.emission_enabled = true
            material.emission = Color(1.0, 0.72, 0.2)
            material.emission_energy_multiplier = 2.0
    elif kind == "station":
        material.albedo_color = Color(0.98, 0.8, 0.32)
    else:
        var phase := String(data.get("phase", "idle"))
        if phase == "in_transit":
            material.albedo_color = Color(0.35, 1.0, 0.8)
        elif phase == "stranded":
            material.albedo_color = Color(1.0, 0.3, 0.3)
        else:
            material.albedo_color = Color(0.92, 0.95, 1.0)
    return material

func _visual_signature(kind: String, data: Dictionary) -> String:
    if kind == "body":
        return "body:%s" % String(data["id"])
    if kind == "station":
        return "station"
    return "ship:%s" % String(data.get("phase", "idle"))

func _refresh_labels() -> void:
    var sim_day := float(bridge_state.get("game_time_days", 0.0))
    var paused := bool(bridge_state.get("paused", false))
    var warp := float(bridge_state.get("timewarp_factor", current_timewarp))
    var run_state := "paused" if paused else "running"
    info_label.text = "SpaceTrains\nDay %.2f\nState: %s\nTimewarp: %.0fx real second\nBodies: %d  Stations: %d  Ships: %d\nSeeded content currently contains orbital stations only." % [
        sim_day,
        run_state,
        warp,
        len(bridge_state.get("bodies", [])),
        len(bridge_state.get("stations", [])),
        len(bridge_state.get("ships", []))
    ]

    var event_lines := ["Recent events"]
    for event in bridge_state.get("recent_events", []):
        event_lines.append("[%.1f] %s" % [float(event["time_s"]) / 86400.0, String(event["text"])])
    event_log.text = "\n".join(event_lines)

    if selected_id == "" or not entity_details.has(selected_id):
        selection_label.text = "No selection\nControls: RMB rotate, MMB pan, wheel zoom, left click select, F focus, Space pause, 1/2/3 timewarp\nUse the entity list on the left if picking is awkward."
        return

    var detail: Dictionary = entity_details[selected_id]
    if selected_kind == "station":
        selection_label.text = "%s\nType: station\nFaction: %s\nPopulation: %s\nFood: %.1f  Fuel: %.1f  Metals: %.1f" % [
            detail["name"], detail["faction_id"], str(detail["population"]),
            float(detail.get("food", 0.0)), float(detail.get("fuel", 0.0)), float(detail.get("metals", 0.0))
        ]
    elif selected_kind == "ship":
        var total_days := float(detail.get("total_travel_time_s", 0.0)) / 86400.0
        var remaining_days := float(detail.get("remaining_travel_time_s", 0.0)) / 86400.0
        var progress_pct := 0.0
        if total_days > 0.0:
            progress_pct = clamp((1.0 - remaining_days / total_days) * 100.0, 0.0, 100.0)
        selection_label.text = "%s\nType: ship\nPhase: %s\nPropellant: %.0f kg\nOrigin: %s\nDestination: %s\nETA days: %.2f" % [
            detail["name"], detail["phase"], float(detail.get("propellant_kg", 0.0)),
            String(detail.get("origin_station_id", "")), String(detail.get("destination_station_id", "")),
            remaining_days
        ]
        selection_label.text += "\nMission progress: %.1f%%" % progress_pct
    else:
        selection_label.text = "%s\nType: body\nDisplay scale: %.2f\nRadius: %.0f km" % [
            detail["name"], _body_display_scale(String(detail["id"])), float(detail.get("radius_m", 0.0)) / 1000.0
        ]

func _pick_entity(mouse_pos: Vector2) -> void:
    var best_id := ""
    var best_distance := 28.0
    var best_kind := ""
    for entity_id in entity_nodes.keys():
        var node: Node3D = entity_nodes[entity_id]
        var screen_pos := camera.unproject_position(node.global_position)
        if not camera.is_position_behind(node.global_position):
            var distance := screen_pos.distance_to(mouse_pos)
            if distance < best_distance:
                best_distance = distance
                best_id = entity_id
                best_kind = entity_kinds.get(entity_id, "")
    selected_id = best_id
    selected_kind = best_kind
    if selected_id != "":
        _select_entity_in_list(selected_id)
    _refresh_labels()

func _create_debug_guides() -> void:
    var axes := [
        {"name": "AxisX", "color": Color(1.0, 0.25, 0.25), "position": Vector3(6.0, 0.0, 0.0), "scale": Vector3(12.0, 0.06, 0.06)},
        {"name": "AxisY", "color": Color(0.25, 1.0, 0.4), "position": Vector3(0.0, 6.0, 0.0), "scale": Vector3(0.06, 12.0, 0.06)},
        {"name": "AxisZ", "color": Color(0.3, 0.7, 1.0), "position": Vector3(0.0, 0.0, 6.0), "scale": Vector3(0.06, 0.06, 12.0)},
        {"name": "Origin", "color": Color(1.0, 1.0, 1.0), "position": Vector3.ZERO, "scale": Vector3.ONE * 0.35}
    ]
    for axis in axes:
        var marker := MeshInstance3D.new()
        marker.name = axis["name"]
        var mesh := BoxMesh.new()
        mesh.size = Vector3.ONE
        marker.mesh = mesh
        var material := StandardMaterial3D.new()
        material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
        material.albedo_color = axis["color"]
        marker.material_override = material
        marker.position = axis["position"]
        marker.scale = axis["scale"]
        world_root.add_child(marker)
        debug_guides.append(marker)

func _hide_debug_guides() -> void:
    for marker in debug_guides:
        marker.visible = false

func _rebuild_entity_list() -> void:
    var previous_selected := selected_id
    entity_list.clear()
    var ids: Array = entity_details.keys()
    ids.sort()
    for entity_id in ids:
        var detail: Dictionary = entity_details[entity_id]
        var kind: String = entity_kinds.get(entity_id, "entity")
        entity_list.add_item("[%s] %s" % [kind, detail.get("name", entity_id)])
        entity_list.set_item_metadata(entity_list.item_count - 1, entity_id)
    if previous_selected != "":
        _select_entity_in_list(previous_selected)

func _select_entity_in_list(entity_id: String) -> void:
    for i in range(entity_list.item_count):
        if String(entity_list.get_item_metadata(i)) == entity_id:
            entity_list.select(i)
            return

func _auto_focus_initial_entity() -> void:
    if station_positions.has("earth_l1"):
        selected_id = "earth_l1"
        selected_kind = "station"
    elif bridge_state.get("stations", []).size() > 0:
        var station: Dictionary = bridge_state.get("stations", [])[0]
        selected_id = station["id"]
        selected_kind = "station"
    elif bridge_state.get("bodies", []).size() > 0:
        var body: Dictionary = bridge_state.get("bodies", [])[0]
        selected_id = body["id"]
        selected_kind = "body"
    else:
        return

    camera_rig.focus_point(entity_targets.get(selected_id, Vector3.ZERO))
    _select_entity_in_list(selected_id)
    has_auto_focused = true

func _ensure_trail_node(ship_id: String) -> Node3D:
    if trail_nodes.has(ship_id):
        return trail_nodes[ship_id]
    var node := Node3D.new()
    node.name = "%s_trail" % ship_id
    var segment_a := MeshInstance3D.new()
    segment_a.name = "segment_a"
    var mesh_a := BoxMesh.new()
    mesh_a.size = Vector3(0.02, 0.02, 1.0)
    segment_a.mesh = mesh_a
    var segment_b := MeshInstance3D.new()
    segment_b.name = "segment_b"
    var mesh_b := BoxMesh.new()
    mesh_b.size = Vector3(0.02, 0.02, 1.0)
    segment_b.mesh = mesh_b
    node.add_child(segment_a)
    node.add_child(segment_b)
    world_root.add_child(node)
    trail_nodes[ship_id] = node
    return node

func _update_ship_trails() -> void:
    for ship_id in trail_nodes.keys():
        trail_nodes[ship_id].visible = false

    if selected_kind != "ship" or selected_id == "":
        return
    if not entity_details.has(selected_id):
        return

    var ship: Dictionary = entity_details[selected_id]
    var phase := String(ship.get("phase", "idle"))
    if phase != "in_transit":
        return

    var origin_id := String(ship.get("origin_station_id", ""))
    var destination_id := String(ship.get("destination_station_id", ""))
    if not station_positions.has(origin_id) or not station_positions.has(destination_id):
        return

    var origin: Vector3 = station_positions[origin_id]
    var destination: Vector3 = station_positions[destination_id]
    var midpoint: Vector3 = (origin + destination) * 0.5 + Vector3.UP * max(0.18, origin.distance_to(destination) * 0.10)

    var trail_node := _ensure_trail_node(selected_id)
    var material := StandardMaterial3D.new()
    material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
    material.albedo_color = Color(0.3, 1.0, 0.8)
    var segment_a: MeshInstance3D = trail_node.get_node("segment_a")
    var segment_b: MeshInstance3D = trail_node.get_node("segment_b")
    segment_a.material_override = material
    segment_b.material_override = material
    _update_trail_segment(segment_a, origin, midpoint)
    _update_trail_segment(segment_b, midpoint, destination)
    trail_node.visible = true

func _on_entity_selected(index: int) -> void:
    var entity_id := String(entity_list.get_item_metadata(index))
    selected_id = entity_id
    selected_kind = entity_kinds.get(entity_id, "")
    if entity_targets.has(entity_id):
        camera_rig.focus_point(entity_targets[entity_id])
    _refresh_labels()

func _update_trail_segment(segment: MeshInstance3D, from_point: Vector3, to_point: Vector3) -> void:
    var midpoint: Vector3 = (from_point + to_point) * 0.5
    var direction: Vector3 = to_point - from_point
    var length: float = max(direction.length(), 0.001)
    segment.position = midpoint
    segment.look_at(to_point, Vector3.UP)
    var mesh := segment.mesh as BoxMesh
    if mesh != null:
        mesh.size = Vector3(0.02, 0.02, length)

func _station_display_position(data: Dictionary) -> Vector3:
    var station_id := String(data["id"])
    var body_id := String(data.get("parent_body_id", ""))
    if not body_positions.has(body_id):
        return _scaled_position(data)

    var body_center: Vector3 = body_positions[body_id]
    var physical_position: Vector3 = _scaled_position(data)
    var radial: Vector3 = physical_position - body_center
    if radial.length() <= 0.0001:
        radial = Vector3.UP
    else:
        radial = radial.normalized()

    var body_radius := float(body_display_radii.get(body_id, 0.3))
    var clearance: float = max(STATION_SCALE * 3.0, body_radius * 0.22)
    var angle_jitter := float(abs(station_id.hash()) % 5) * 0.04
    return body_center + radial * (body_radius + clearance + angle_jitter)
