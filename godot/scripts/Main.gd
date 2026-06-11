extends Node3D

const POSITION_SCALE := 1.0 / 8_000_000_000.0
const BODY_MIN_MODEL_SCALE := 0.00045
const SHIP_MIN_MODEL_SCALE := 0.00008
const STATION_MIN_MODEL_SCALE := 0.00012
const SHIP_MASS_SCALE := 0.00012
const STATION_POPULATION_SCALE := 0.00016
const BRIDGE_STEP_SECONDS := 0.1
const DEFAULT_TIMEWARP := 86400.0
const TIMEWARP_STEPS := [600.0, 3600.0, 21600.0, 86400.0, 432000.0]

const BODY_ICON_SIZE := {
    "star":         14.0,
    "planet":       9.0,
    "dwarf_planet": 7.0,
    "moon":         7.0,
}
const STATION_ICON_SIZE := 8.0
const SHIP_ICON_SIZE := 8.0
const ICON_ALPHA := 0.58
const ICON_PICK_PADDING_PX := 6.0
const DEBUG_SAMPLE_BODY_IDS := ["sun", "mercury", "earth", "mars", "jupiter", "saturn", "neptune"]
const DEBUG_LOG_INTERVAL_S := 1.0
const UI_REFRESH_INTERVAL_S := 0.5

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

@onready var world_root: Node3D = $WorldRoot
@onready var camera_rig: Node3D = $CameraRig
@onready var camera: Camera3D = $CameraRig/Camera3D
@onready var canvas_layer: CanvasLayer = $CanvasLayer
@onready var scene_light: DirectionalLight3D = $DirectionalLight3D

var bridge_pid := -1
var snapshot_path := ""
var command_path := ""
var repo_root := ""
var executable_path := ""
var snapshot_blend := 1.0
var current_snapshot_seq := -1
var previous_snapshot_arrival_s := 0.0
var current_snapshot_arrival_s := 0.0
var previous_snapshot_bridge_time_s := 0.0
var current_snapshot_bridge_time_s := 0.0
var snapshot_interval_s := BRIDGE_STEP_SECONDS
var selected_id := ""
var selected_kind := ""
var focused_id := ""
var focused_kind := ""
var bridge_state: Dictionary = {}
var entity_nodes: Dictionary = {}
var entity_previous_targets: Dictionary = {}
var entity_targets: Dictionary = {}
var entity_details: Dictionary = {}
var entity_kinds: Dictionary = {}
var entity_visual_signatures: Dictionary = {}
var trail_nodes: Dictionary = {}
var trail_path_signatures: Dictionary = {}
var selected_ship_overlay: MeshInstance3D
var destination_body_ghost: MeshInstance3D
var current_paused := false
var current_timewarp := DEFAULT_TIMEWARP
var has_auto_focused := false
var bridge_started := false
var debug_guides: Array[Node3D] = []
var station_positions: Dictionary = {}
var body_positions: Dictionary = {}
var body_display_radii: Dictionary = {}
var render_origin := Vector3.ZERO
var faction_colors := {
    "sol_fed": Color(0.48, 0.72, 1.0),
    "mars_corp": Color(1.0, 0.45, 0.28),
    "independent": Color(0.86, 0.82, 0.72)
}
var sun_light: OmniLight3D
var space_env: Node3D
var map_icon_layer: Control
var _map_icons: Dictionary = {}
var _icon_textures: Dictionary = {}
var debug_map_enabled := false
var debug_log_accum_s := 0.0
var debug_frame := 0
var last_render_origin := Vector3.ZERO
var last_ui_refresh_s := -1000.0

const UiTheme := preload("res://scripts/ui/UiTheme.gd")
const SpaceEnvironmentScript := preload("res://scripts/SpaceEnvironment.gd")
const TopBarPanel := preload("res://scripts/ui/TopBar.gd")
const EntityBrowserPanel := preload("res://scripts/ui/EntityBrowser.gd")
const InspectorPanelScript := preload("res://scripts/ui/InspectorPanel.gd")
const EventTickerPanel := preload("res://scripts/ui/EventTicker.gd")
const MarketPanelScript := preload("res://scripts/ui/MarketPanel.gd")

var ui_root: Control
var top_bar: PanelContainer
var entity_browser: PanelContainer
var inspector_panel: PanelContainer
var event_ticker: PanelContainer
var market_panel: PanelContainer
var status_label: Label
# Price history samples: {day: float, prices: {station_id: {commodity_id: price}}}
var price_history: Array[Dictionary] = []
const PRICE_HISTORY_MAX_SAMPLES := 600
const PRICE_HISTORY_MIN_DAY_STEP := 0.5
const PRICE_TREND_LOOKBACK_DAYS := 10.0
const PRICE_TREND_THRESHOLD := 0.03

func _ready() -> void:
    repo_root = ProjectSettings.globalize_path("res://").get_base_dir().get_base_dir()
    executable_path = repo_root.path_join("build/bin/spacetrains_bridge")
    var session_id := "%d_%d" % [OS.get_process_id(), Time.get_ticks_usec()]
    snapshot_path = ProjectSettings.globalize_path("user://spacetrains_snapshot_%s.json" % session_id)
    command_path = ProjectSettings.globalize_path("user://spacetrains_commands_%s.json" % session_id)
    _write_bridge_commands()
    _start_bridge()
    _create_debug_guides()
    _setup_scene_lighting()
    _setup_map_icon_layer()
    _setup_ui()
    if "--debug-map" in OS.get_cmdline_user_args():
        debug_map_enabled = true
    if bridge_started:
        _set_status("Starting bridge…\n%s" % executable_path)

func _setup_ui() -> void:
    ui_root = Control.new()
    ui_root.name = "UiRoot"
    ui_root.mouse_filter = Control.MOUSE_FILTER_IGNORE
    ui_root.set_anchors_preset(Control.PRESET_FULL_RECT)
    ui_root.theme = UiTheme.build()
    canvas_layer.add_child(ui_root)

    top_bar = TopBarPanel.new()
    top_bar.name = "TopBar"
    top_bar.set_anchors_preset(Control.PRESET_TOP_WIDE)
    top_bar.offset_left = 8.0
    top_bar.offset_top = 8.0
    top_bar.offset_right = -8.0
    top_bar.pause_toggled.connect(_on_pause_toggled)
    top_bar.timewarp_changed.connect(_on_timewarp_changed)
    ui_root.add_child(top_bar)

    entity_browser = EntityBrowserPanel.new()
    entity_browser.name = "EntityBrowser"
    entity_browser.set_anchors_preset(Control.PRESET_LEFT_WIDE)
    entity_browser.offset_left = 8.0
    entity_browser.offset_top = 56.0
    entity_browser.offset_bottom = -8.0
    entity_browser.grow_vertical = Control.GROW_DIRECTION_BOTH
    entity_browser.entity_selected.connect(_on_browser_entity_selected)
    ui_root.add_child(entity_browser)

    inspector_panel = InspectorPanelScript.new()
    inspector_panel.name = "Inspector"
    inspector_panel.set_anchors_preset(Control.PRESET_RIGHT_WIDE)
    inspector_panel.offset_top = 56.0
    inspector_panel.offset_right = -8.0
    inspector_panel.offset_bottom = -212.0
    inspector_panel.grow_horizontal = Control.GROW_DIRECTION_BEGIN
    ui_root.add_child(inspector_panel)

    event_ticker = EventTickerPanel.new()
    event_ticker.name = "EventTicker"
    event_ticker.set_anchors_preset(Control.PRESET_BOTTOM_RIGHT)
    event_ticker.offset_left = -478.0
    event_ticker.offset_top = -198.0
    event_ticker.offset_right = -8.0
    event_ticker.offset_bottom = -8.0
    ui_root.add_child(event_ticker)

    var market_center := CenterContainer.new()
    market_center.name = "MarketCenter"
    market_center.mouse_filter = Control.MOUSE_FILTER_IGNORE
    market_center.set_anchors_preset(Control.PRESET_FULL_RECT)
    ui_root.add_child(market_center)
    market_panel = MarketPanelScript.new()
    market_panel.name = "MarketPanel"
    market_center.add_child(market_panel)

    status_label = Label.new()
    status_label.name = "Status"
    status_label.set_anchors_preset(Control.PRESET_CENTER)
    status_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
    status_label.add_theme_color_override("font_color", UiTheme.WARN)
    ui_root.add_child(status_label)

func _set_status(text: String) -> void:
    if status_label != null:
        status_label.text = text
        status_label.visible = text != ""

func _on_pause_toggled() -> void:
    current_paused = not current_paused
    _write_bridge_commands()

func _on_timewarp_changed(factor: float) -> void:
    current_timewarp = factor
    _write_bridge_commands()

func _on_browser_entity_selected(entity_id: String, kind: String) -> void:
    select_entity(entity_id, kind, true)

func select_entity(entity_id: String, kind: String, focus := false) -> void:
    selected_id = entity_id
    selected_kind = kind
    if entity_browser != null:
        entity_browser.set_selected(entity_id)
    if focus and entity_targets.has(entity_id):
        _focus_entity(entity_id, kind)
    _refresh_ui(true)

func _exit_tree() -> void:
    if bridge_pid > 0:
        OS.kill(bridge_pid)

func _process(delta: float) -> void:
    debug_frame += 1
    _read_snapshot()
    snapshot_blend = _current_snapshot_alpha()
    _update_nodes(delta)
    _update_camera_focus()
    _update_map_icons()
    _update_map_debug(delta)

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
        elif event.keycode == KEY_COMMA:
            _step_timewarp(-1)
        elif event.keycode == KEY_PERIOD:
            _step_timewarp(1)
        elif event.keycode == KEY_F and selected_id != "" and entity_targets.has(selected_id):
            _focus_entity(selected_id, selected_kind)
        elif event.keycode == KEY_M:
            market_panel.toggle()
            if market_panel.visible:
                market_panel.update_market(bridge_state)
        elif event.keycode == KEY_F9:
            debug_map_enabled = not debug_map_enabled
            _debug_map_state("toggle")
        elif event.keycode == KEY_F10:
            _debug_map_state("manual")
    elif event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
        _pick_entity(event.position)
        if event.double_click and selected_id != "" and entity_targets.has(selected_id):
            _focus_entity(selected_id, selected_kind)

func _start_bridge() -> void:
    if not FileAccess.file_exists(executable_path):
        _set_status("Bridge executable not found:\n%s\nBuild the project first." % executable_path)
        return

    var args := [
        "--data-root", repo_root.path_join("data"),
        "--snapshot-file", snapshot_path,
        "--command-file", command_path,
        "--step-seconds", str(BRIDGE_STEP_SECONDS)
    ]
    bridge_pid = OS.create_process(executable_path, args, false)
    if bridge_pid <= 0:
        _set_status("Failed to start bridge process.")
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
    var new_seq := int(json.data.get("snapshot_seq", -1))
    if new_seq <= current_snapshot_seq:
        return
    var new_game_time_s := float(json.data.get("game_time_s", 0.0))
    var current_game_time_s := float(bridge_state.get("game_time_s", -1.0))
    if current_game_time_s >= 0.0 and new_game_time_s + 0.001 < current_game_time_s:
        if debug_map_enabled:
            print("[MapDebugReject] seq=%d current_seq=%d new_game_day=%.3f current_game_day=%.3f" % [new_seq, current_snapshot_seq, new_game_time_s / 86400.0, current_game_time_s / 86400.0])
        return
    var arrival_now_s := _wall_time_s()
    var new_bridge_time_s := float(json.data.get("snapshot_real_time_s", arrival_now_s))
    if current_snapshot_seq >= 0:
        previous_snapshot_arrival_s = current_snapshot_arrival_s
        previous_snapshot_bridge_time_s = current_snapshot_bridge_time_s
    current_snapshot_seq = new_seq
    current_snapshot_arrival_s = arrival_now_s
    current_snapshot_bridge_time_s = new_bridge_time_s
    var arrival_interval_s := current_snapshot_arrival_s - previous_snapshot_arrival_s
    var bridge_interval_s := current_snapshot_bridge_time_s - previous_snapshot_bridge_time_s
    if bridge_interval_s > 0.0:
        snapshot_interval_s = bridge_interval_s
    elif arrival_interval_s > 0.0:
        snapshot_interval_s = arrival_interval_s
    else:
        snapshot_interval_s = BRIDGE_STEP_SECONDS
    bridge_state = json.data
    _apply_snapshot()
    snapshot_blend = 0.0

func _apply_snapshot() -> void:
    var seen_ids := {}
    var ids_changed := false
    station_positions.clear()
    body_positions.clear()
    body_display_radii.clear()

    for body in bridge_state.get("bodies", []):
        var body_id := String(body["id"])
        body_positions[body_id] = _scaled_position(body)
        body_display_radii[body_id] = _body_scale_from_radius(float(body.get("radius_m", 0.0)))

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
            entity_previous_targets.erase(entity_id)
            entity_targets.erase(entity_id)
            entity_details.erase(entity_id)
            entity_kinds.erase(entity_id)
            entity_visual_signatures.erase(entity_id)
            if trail_nodes.has(entity_id):
                trail_nodes[entity_id].queue_free()
                trail_nodes.erase(entity_id)
                trail_path_signatures.erase(entity_id)
            if _map_icons.has(entity_id):
                (_map_icons[entity_id] as Node).queue_free()
                _map_icons.erase(entity_id)
            ids_changed = true
            if entity_id == focused_id:
                focused_id = ""
                focused_kind = ""

    if ids_changed and selected_id != "" and not entity_details.has(selected_id):
        selected_id = ""
        selected_kind = ""
    _update_faction_colors()
    if space_env != null:
        space_env.update_orbit_rings(world_root, bridge_state.get("bodies", []), body_positions, BODY_ICON_COLOR)
    _update_ship_trails()
    if not has_auto_focused:
        _hide_debug_guides()
        _auto_focus_initial_entity()
    if status_label != null and status_label.visible:
        _set_status("")
    _refresh_ui(false)

func _update_faction_colors() -> void:
    for faction in bridge_state.get("factions", []):
        var hex := String(faction.get("color", ""))
        if hex.length() >= 6:
            faction_colors[String(faction.get("id", ""))] = Color.from_string("#" + hex, Color(0.7, 0.7, 0.7))

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
        entity_previous_targets[entity_id] = Vector3.ZERO
        entity_targets[entity_id] = Vector3.ZERO
        entity_visual_signatures[entity_id] = ""
        _attach_entity_label(mesh_instance, kind, data)
        _attach_map_icon(entity_id, kind, data)
        if entity_id == "saturn":
            mesh_instance.add_child(SpaceEnvironmentScript.make_planet_ring())

    var visual_signature := _visual_signature(kind, data)
    if entity_visual_signatures.get(entity_id, "") != visual_signature:
        entity_nodes[entity_id].material_override = _make_material(kind, data)
        entity_nodes[entity_id].scale = _make_scale(kind, data)
        entity_visual_signatures[entity_id] = visual_signature
    var new_target := _display_position(data, kind)
    if entity_nodes.has(entity_id):
        entity_previous_targets[entity_id] = (entity_nodes[entity_id] as Node3D).position
    else:
        entity_previous_targets[entity_id] = entity_targets.get(entity_id, new_target)
    entity_targets[entity_id] = new_target

func _update_nodes(delta: float) -> void:
    var focus_position := Vector3.ZERO
    for entity_id in entity_nodes.keys():
        var node: Node3D = entity_nodes[entity_id]
        var previous_target: Vector3 = entity_previous_targets.get(entity_id, entity_targets[entity_id])
        var target: Vector3 = entity_targets[entity_id]
        var display_position: Vector3 = previous_target.lerp(target, snapshot_blend)
        node.position = display_position
        if entity_id == focused_id:
            focus_position = display_position
    last_render_origin = render_origin
    if focused_id != "" and entity_nodes.has(focused_id):
        render_origin = focus_position
    else:
        render_origin = Vector3.ZERO
    world_root.position = -render_origin
    if sun_light != null and entity_nodes.has("sun"):
        var sun_node: Node3D = entity_nodes["sun"]
        sun_light.global_position = sun_node.global_position
        if space_env != null:
            space_env.update_sun(sun_node.global_position, sun_node.scale.x)
    if space_env != null:
        space_env.update_camera(camera.global_position)
    _update_selected_overlay_positions()

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
    var local_spacing: float = max(_ship_display_scale(data) * 6.0, 0.0006)
    var ring_offset := Vector3(cos(angle), 0.15 + float(index % 3) * 0.08, sin(angle)).normalized() * local_spacing

    if phase == "idle" or phase == "refueling" or phase == "stranded" or phase == "awaiting_departure":
        if station_positions.has(station_id):
            return station_positions[station_id] + ring_offset
        return base + ring_offset

    if phase == "in_transit":
        return base

    return base

func _make_mesh(kind: String) -> Mesh:
    match kind:
        "body":
            var sphere := SphereMesh.new()
            sphere.radius = 1.0
            sphere.height = 2.0
            sphere.radial_segments = 24
            sphere.rings = 12
            return sphere
        "ship":
            var prism := PrismMesh.new()
            prism.size = Vector3(0.7, 0.7, 1.4)
            return prism
        _:
            var box := BoxMesh.new()
            box.size = Vector3.ONE
            return box

func _body_scale_from_radius(radius_m: float) -> float:
    if radius_m <= 0.0:
        return BODY_MIN_MODEL_SCALE
    return max(radius_m * POSITION_SCALE, BODY_MIN_MODEL_SCALE)

func _body_display_scale(body_id: String) -> float:
    var detail: Dictionary = entity_details.get(body_id, {})
    return _body_scale_from_radius(float(detail.get("radius_m", 0.0)))

func _ship_display_scale(data: Dictionary) -> float:
    var mass_kg: float = max(float(data.get("current_mass_kg", data.get("initial_mass_kg", 10000.0))), 1.0)
    return max(pow(mass_kg / 10000.0, 1.0 / 3.0) * SHIP_MASS_SCALE, SHIP_MIN_MODEL_SCALE)

func _station_display_scale(data: Dictionary) -> float:
    var population: float = max(float(data.get("population", 0.0)), 1.0)
    return max(pow(population / 22000.0, 1.0 / 3.0) * STATION_POPULATION_SCALE, STATION_MIN_MODEL_SCALE)

func _model_display_scale(kind: String, data: Dictionary) -> float:
    if kind == "body":
        return _body_display_scale(String(data["id"]))
    if kind == "ship":
        return _ship_display_scale(data)
    if kind == "station":
        return _station_display_scale(data)
    return BODY_MIN_MODEL_SCALE

func _make_scale(kind: String, data: Dictionary) -> Vector3:
    return Vector3.ONE * _model_display_scale(kind, data)

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
        "jupiter":
            return Color(0.85, 0.65, 0.45)
        "saturn":
            return Color(0.90, 0.80, 0.55)
        "uranus":
            return Color(0.50, 0.85, 0.90)
        "neptune":
            return Color(0.30, 0.45, 0.95)
        "luna":
            return Color(0.80, 0.80, 0.80)
        "europa":
            return Color(0.75, 0.70, 0.65)
        "ganymede":
            return Color(0.65, 0.60, 0.55)
        "titan":
            return Color(0.85, 0.65, 0.40)
        "triton":
            return Color(0.60, 0.70, 0.75)
        _:
            return Color(0.75, 0.8, 0.88)

func _make_material(kind: String, data: Dictionary) -> Material:
    var material := StandardMaterial3D.new()
    if kind == "body":
        var body_id := String(data["id"])
        material.albedo_color = _body_color(body_id)
        material.roughness = 0.82
        material.metallic = 0.0
        if body_id == "sun":
            material.albedo_color = Color(1.0, 0.96, 0.31)
            material.emission_enabled = true
            material.emission = Color(1.0, 0.96, 0.31)
            # Bright enough to cross the HDR glow threshold and bloom.
            material.emission_energy_multiplier = 8.0
            material.roughness = 0.6
    elif kind == "station":
        var faction_id := String(data.get("faction_id", ""))
        var color: Color = faction_colors.get(faction_id, Color(0.95, 0.82, 0.36))
        material.albedo_color = color
        material.emission_enabled = true
        material.emission = color * 0.5
        material.emission_energy_multiplier = 0.6
        material.roughness = 0.4
    else:
        var phase := String(data.get("phase", "idle"))
        var ship_color := Color(0.96, 0.97, 1.0)
        material.albedo_color = ship_color
        material.emission_enabled = true
        material.emission = Color(1.0, 1.0, 1.0)
        material.emission_energy_multiplier = 0.85
        material.roughness = 0.35
        if phase == "in_transit":
            material.emission_energy_multiplier = 1.25
        elif phase == "stranded":
            material.albedo_color = Color(1.0, 0.3, 0.3)
            material.emission = Color(0.6, 0.1, 0.1)
            material.emission_energy_multiplier = 0.5
    return material

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

func _visual_signature(kind: String, data: Dictionary) -> String:
    if kind == "body":
        return "body:%s" % String(data["id"])
    if kind == "station":
        return "station:%s" % String(data.get("faction_id", ""))
    return "ship:%s" % String(data.get("phase", "idle"))

func _refresh_ui(force := false) -> void:
    if top_bar == null:
        return
    var now_s := _wall_time_s()
    if not force and now_s - last_ui_refresh_s < UI_REFRESH_INTERVAL_S:
        return
    last_ui_refresh_s = now_s

    _record_price_history()
    top_bar.update_state(bridge_state, bool(bridge_state.get("paused", current_paused)), current_timewarp)
    entity_browser.update_entities(entity_details, entity_kinds, selected_id)
    event_ticker.update_events(bridge_state.get("recent_events", []))
    market_panel.update_market(bridge_state)

    var detail: Dictionary = entity_details.get(selected_id, {})
    inspector_panel.update_selection(detail, selected_kind, {
        "game_time_s": float(bridge_state.get("game_time_s", 0.0)),
        "faction_colors": faction_colors,
        "names": _entity_name_map(),
        "price_trends": _price_trends_for(selected_id) if selected_kind == "station" else {},
    })

func _entity_name_map() -> Dictionary:
    var names := {}
    for entity_id in entity_details.keys():
        names[entity_id] = String((entity_details[entity_id] as Dictionary).get("name", entity_id))
    return names

func _record_price_history() -> void:
    var day := float(bridge_state.get("game_time_days", 0.0))
    if not price_history.is_empty() and day - float(price_history.back()["day"]) < PRICE_HISTORY_MIN_DAY_STEP:
        return
    var sample := {}
    for station in bridge_state.get("stations", []):
        var prices: Dictionary = station.get("prices", {})
        if not prices.is_empty():
            sample[String(station["id"])] = prices.duplicate()
    if sample.is_empty():
        return
    price_history.append({"day": day, "prices": sample})
    if price_history.size() > PRICE_HISTORY_MAX_SAMPLES:
        price_history.pop_front()

func _price_trends_for(station_id: String) -> Dictionary:
    var trends := {}
    if price_history.size() < 2:
        return trends
    var current: Dictionary = price_history.back()
    var current_day := float(current["day"])
    var reference: Dictionary = price_history.front()
    for i in range(price_history.size() - 2, -1, -1):
        if current_day - float(price_history[i]["day"]) >= PRICE_TREND_LOOKBACK_DAYS:
            reference = price_history[i]
            break
    var now_prices: Dictionary = (current["prices"] as Dictionary).get(station_id, {})
    var then_prices: Dictionary = (reference["prices"] as Dictionary).get(station_id, {})
    for commodity_id in now_prices.keys():
        var then := float(then_prices.get(commodity_id, 0.0))
        if then <= 0.0:
            continue
        var ratio := float(now_prices[commodity_id]) / then
        if ratio > 1.0 + PRICE_TREND_THRESHOLD:
            trends[commodity_id] = 1
        elif ratio < 1.0 - PRICE_TREND_THRESHOLD:
            trends[commodity_id] = -1
        else:
            trends[commodity_id] = 0
    return trends

func _pick_entity(mouse_pos: Vector2) -> void:
    var best_id := ""
    var best_distance := 28.0
    var best_kind := ""
    for entity_id in entity_nodes.keys():
        var node: Node3D = entity_nodes[entity_id]
        if camera.is_position_behind(node.global_position):
            continue
        var screen_pos := camera.unproject_position(node.global_position)
        var kind: String = entity_kinds.get(entity_id, "")
        var detail: Dictionary = entity_details.get(entity_id, {})
        var pick_radius: float = max(_icon_pixel_size(kind, detail) + ICON_PICK_PADDING_PX, 20.0)
        var distance := screen_pos.distance_to(mouse_pos)
        if distance < pick_radius and distance < best_distance:
            best_distance = distance
            best_id = entity_id
            best_kind = kind
    if best_id != "":
        select_entity(best_id, best_kind)
    else:
        selected_id = ""
        selected_kind = ""
        entity_browser.set_selected("")
        _refresh_ui(true)

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

func _setup_scene_lighting() -> void:
    if scene_light != null:
        scene_light.visible = false
        scene_light.light_energy = 0.0
    sun_light = OmniLight3D.new()
    sun_light.name = "SunLight"
    sun_light.light_energy = 10.0
    sun_light.omni_range = 420.0
    sun_light.shadow_enabled = false
    sun_light.light_color = Color(1.0, 0.96, 0.82)
    world_root.add_child(sun_light)
    RenderingServer.set_default_clear_color(Color(0.01, 0.012, 0.025, 1.0))

    var env := Environment.new()
    env.background_mode = Environment.BG_COLOR
    env.background_color = Color(0.01, 0.012, 0.025, 1.0)
    env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
    env.ambient_light_color = Color(0.1, 0.1, 0.15, 1.0)
    env.ambient_light_energy = 0.22
    env.tonemap_mode = Environment.TONE_MAPPER_ACES
    env.glow_enabled = true
    env.glow_intensity = 0.6
    env.glow_bloom = 0.15
    env.glow_hdr_threshold = 1.1
    env.glow_blend_mode = Environment.GLOW_BLEND_MODE_ADDITIVE
    var world_env := WorldEnvironment.new()
    world_env.environment = env
    add_child(world_env)

    space_env = SpaceEnvironmentScript.new()
    space_env.name = "SpaceEnvironment"
    add_child(space_env)

func _setup_map_icon_layer() -> void:
    map_icon_layer = Control.new()
    map_icon_layer.name = "MapIconLayer"
    map_icon_layer.mouse_filter = Control.MOUSE_FILTER_IGNORE
    map_icon_layer.set_anchors_preset(Control.PRESET_FULL_RECT)
    canvas_layer.add_child(map_icon_layer)
    canvas_layer.move_child(map_icon_layer, 0)

func _attach_entity_label(node: MeshInstance3D, kind: String, data: Dictionary) -> void:
    var label := Label3D.new()
    label.text = String(data.get("name", data.get("id", "")))
    label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
    label.no_depth_test = true
    label.modulate = Color(0.92, 0.94, 1.0, 0.78)
    label.outline_size = 6
    if kind == "body":
        label.font_size = 22
        label.pixel_size = 0.03
        label.position = Vector3(0, _model_display_scale(kind, data) + 0.08, 0)
    elif kind == "station":
        label.font_size = 16
        label.pixel_size = 0.018
        label.position = Vector3(0, _model_display_scale(kind, data) + 0.045, 0)
    else:
        label.font_size = 14
        label.pixel_size = 0.016
        label.position = Vector3(0, _model_display_scale(kind, data) + 0.04, 0)
        label.visible = false
    node.add_child(label)

func _icon_shape(kind: String, data: Dictionary) -> String:
    if kind == "body":
        return "circle"
    if kind == "station":
        return "diamond"
    return "triangle"

func _icon_pixel_size(kind: String, data: Dictionary) -> float:
    if kind == "body":
        var body_id := String(data.get("id", ""))
        return float(BODY_ICON_SIZE.get(BODY_TYPE.get(body_id, "planet"), 22.0))
    if kind == "station":
        return STATION_ICON_SIZE
    return SHIP_ICON_SIZE

func _icon_color(kind: String, data: Dictionary) -> Color:
    if kind == "body":
        return BODY_ICON_COLOR.get(String(data.get("id", "")), Color(0.7, 0.7, 0.7)) as Color
    if kind == "station":
        return faction_colors.get(String(data.get("faction_id", "")), Color(0.95, 0.82, 0.36)) as Color
    var phase := String(data.get("phase", "idle"))
    if phase == "stranded":
        return Color(1.0, 0.25, 0.22)
    if phase == "in_transit":
        return Color(0.70, 1.0, 0.95)
    return faction_colors.get(String(data.get("faction_id", "")), Color(0.96, 0.97, 1.0)) as Color

func _get_icon_texture(shape: String) -> ImageTexture:
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

func _attach_map_icon(entity_id: String, kind: String, data: Dictionary) -> void:
    if _map_icons.has(entity_id):
        return
    var icon_node := Sprite2D.new()
    icon_node.name = "%s_icon" % entity_id
    icon_node.texture = _get_icon_texture(_icon_shape(kind, data))
    icon_node.centered = true
    var icon_color := _icon_color(kind, data)
    icon_color.a = ICON_ALPHA
    icon_node.modulate = icon_color
    if map_icon_layer != null:
        map_icon_layer.add_child(icon_node)
    else:
        canvas_layer.add_child(icon_node)
    _map_icons[entity_id] = icon_node

func _projected_model_pixels(node: Node3D) -> float:
    var viewport_height: float = max(float(get_viewport().get_visible_rect().size.y), 1.0)
    var dist: float = max(camera.global_position.distance_to(node.global_position), 0.0001)
    return node.scale.x / (2.0 * dist * tan(deg_to_rad(camera.fov) * 0.5)) * viewport_height

func _update_map_icons() -> void:
    var viewport_rect := get_viewport().get_visible_rect()
    for entity_id in _map_icons.keys():
        var icon: Sprite2D = _map_icons[entity_id]
        var model: Node3D = entity_nodes.get(entity_id)
        if not model or not entity_details.has(entity_id) or camera.is_position_behind(model.global_position):
            icon.visible = false
            continue

        var kind: String = entity_kinds.get(entity_id, "")
        var data: Dictionary = entity_details[entity_id]
        var pixel_size := _icon_pixel_size(kind, data)
        var selected_scale := 1.25 if entity_id == selected_id else 1.0
        var target_size := pixel_size * selected_scale
        var screen_pos := camera.unproject_position(model.global_position)

        icon.visible = viewport_rect.grow(pixel_size).has_point(screen_pos)
        if not icon.visible:
            continue
        icon.position = screen_pos
        var texture_size := Vector2(icon.texture.get_width(), icon.texture.get_height())
        icon.scale = Vector2.ONE * (target_size / max(texture_size.x, texture_size.y))
        var color := (_icon_color(kind, data).lightened(0.35) if entity_id == selected_id else _icon_color(kind, data))
        color.a = ICON_ALPHA
        icon.modulate = color

func _update_map_debug(delta: float) -> void:
    if not debug_map_enabled:
        return
    debug_log_accum_s += delta
    if debug_log_accum_s < DEBUG_LOG_INTERVAL_S:
        return
    debug_log_accum_s = 0.0
    _debug_map_state("periodic")

func _debug_vec(v: Vector3) -> String:
    return "(%.6f, %.6f, %.6f)" % [v.x, v.y, v.z]

func _debug_map_state(reason: String) -> void:
    var origin_delta := render_origin.distance_to(last_render_origin)
    var camera_distance := float(camera_rig.get("distance")) if camera_rig != null else 0.0
    print("[MapDebug] reason=%s frame=%d seq=%d blend=%.3f focused=%s/%s selected=%s/%s camera_distance=%.6f render_origin=%s origin_delta=%.6f world_root=%s pivot=%s" % [
        reason,
        debug_frame,
        current_snapshot_seq,
        snapshot_blend,
        focused_kind,
        focused_id,
        selected_kind,
        selected_id,
        camera_distance,
        _debug_vec(render_origin),
        origin_delta,
        _debug_vec(world_root.position),
        _debug_vec(camera_rig.get("pivot") as Vector3)
    ])
    for body_id in DEBUG_SAMPLE_BODY_IDS:
        if not entity_nodes.has(body_id):
            continue
        var node: Node3D = entity_nodes[body_id]
        var detail: Dictionary = entity_details.get(body_id, {})
        var icon: Sprite2D = _map_icons.get(body_id)
        var screen := camera.unproject_position(node.global_position)
        var icon_pixels := 0.0
        var icon_visible := false
        var icon_position := Vector2.ZERO
        if icon != null:
            icon_visible = icon.visible
            icon_pixels = max(icon.texture.get_width(), icon.texture.get_height()) * icon.scale.x
            icon_position = icon.position
        print("[MapDebugBody] id=%s local=%s global=%s target=%s screen=(%.1f, %.1f) behind=%s model_scale=%.6f model_px=%.2f icon_px=%.2f icon_target=%.1f icon_pos=(%.1f, %.1f) icon_visible=%s radius_km=%.0f" % [
            body_id,
            _debug_vec(node.position),
            _debug_vec(node.global_position),
            _debug_vec(entity_targets.get(body_id, Vector3.ZERO)),
            screen.x,
            screen.y,
            str(camera.is_position_behind(node.global_position)),
            node.scale.x,
            _projected_model_pixels(node),
            icon_pixels,
            _icon_pixel_size("body", detail),
            icon_position.x,
            icon_position.y,
            str(icon_visible),
            float(detail.get("radius_m", 0.0)) / 1000.0
        ])

func _wall_time_s() -> float:
    return float(Time.get_ticks_usec()) / 1000000.0

func _current_snapshot_alpha() -> float:
    if current_snapshot_arrival_s <= 0.0:
        return 1.0
    return clamp((_wall_time_s() - current_snapshot_arrival_s) / max(snapshot_interval_s, 0.001), 0.0, 1.0)


func _step_timewarp(direction: int) -> void:
    var best_index := 0
    for i in range(TIMEWARP_STEPS.size()):
        if is_equal_approx(current_timewarp, TIMEWARP_STEPS[i]):
            best_index = i
            break
        if current_timewarp >= TIMEWARP_STEPS[i]:
            best_index = i
    best_index = clamp(best_index + direction, 0, TIMEWARP_STEPS.size() - 1)
    current_timewarp = TIMEWARP_STEPS[best_index]
    _write_bridge_commands()

func _auto_focus_initial_entity() -> void:
    if entity_targets.has("sun"):
        selected_id = "sun"
        selected_kind = "body"
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
    focused_id = selected_id
    focused_kind = selected_kind
    if entity_browser != null:
        entity_browser.set_selected(selected_id)
    has_auto_focused = true

func _ensure_trail_node(ship_id: String) -> Node3D:
    if trail_nodes.has(ship_id):
        return trail_nodes[ship_id]
    var node := Node3D.new()
    node.name = "%s_trail" % ship_id
    var path_mesh := MeshInstance3D.new()
    path_mesh.name = "path"
    var material := StandardMaterial3D.new()
    material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
    material.albedo_color = Color(0.3, 1.0, 0.8)
    material.emission_enabled = true
    material.emission = Color(0.3, 1.0, 0.8)
    material.emission_energy_multiplier = 1.2
    path_mesh.material_override = material
    node.add_child(path_mesh)
    world_root.add_child(node)
    trail_nodes[ship_id] = node
    return node

func _update_ship_trails() -> void:
    for ship_id in trail_nodes.keys():
        trail_nodes[ship_id].visible = false
    if destination_body_ghost != null:
        destination_body_ghost.visible = false

    if selected_kind != "ship" or selected_id == "":
        return
    if not entity_details.has(selected_id):
        return

    var ship: Dictionary = entity_details[selected_id]
    var phase := String(ship.get("phase", "idle"))
    if phase != "in_transit" and phase != "awaiting_departure":
        return

    var trajectory_path: Array = ship.get("trajectory_path", [])
    if trajectory_path.size() < 2:
        return

    var trail_node := _ensure_trail_node(selected_id)
    var signature := _trajectory_path_signature(trajectory_path)
    if trail_path_signatures.get(selected_id, "") != signature:
        _rebuild_trail_mesh(trail_node, trajectory_path)
        trail_path_signatures[selected_id] = signature
    trail_node.visible = true
    _update_destination_body_ghost(ship)

func _trajectory_path_signature(trajectory_path: Array) -> String:
    var parts: Array[String] = [str(trajectory_path.size())]
    for point in trajectory_path:
        parts.append("%.3f,%.3f,%.3f" % [float(point.get("x", 0.0)), float(point.get("y", 0.0)), float(point.get("z", 0.0))])
    return "|".join(parts)

func _rebuild_trail_mesh(trail_node: Node3D, trajectory_path: Array) -> void:
    var mesh := ImmediateMesh.new()
    mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
    for point in trajectory_path:
        mesh.surface_add_vertex(_scaled_position(point))
    mesh.surface_end()
    var path_mesh: MeshInstance3D = trail_node.get_node("path")
    path_mesh.mesh = mesh

func _ensure_selected_ship_overlay() -> MeshInstance3D:
    if selected_ship_overlay != null:
        return selected_ship_overlay
    selected_ship_overlay = MeshInstance3D.new()
    selected_ship_overlay.name = "SelectedShipOverlay"
    var mesh := SphereMesh.new()
    mesh.radius = 1.0
    mesh.height = 2.0
    mesh.radial_segments = 16
    mesh.rings = 8
    selected_ship_overlay.mesh = mesh
    var material := StandardMaterial3D.new()
    material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
    material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
    material.albedo_color = Color(0.25, 1.0, 0.95, 0.62)
    material.emission_enabled = true
    material.emission = Color(0.35, 1.0, 0.95)
    material.emission_energy_multiplier = 1.8
    selected_ship_overlay.material_override = material
    selected_ship_overlay.scale = Vector3.ONE * SHIP_MIN_MODEL_SCALE * 3.0
    selected_ship_overlay.visible = false
    world_root.add_child(selected_ship_overlay)
    return selected_ship_overlay

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

func _update_selected_overlay_positions() -> void:
    var overlay := _ensure_selected_ship_overlay()
    if selected_kind == "ship" and selected_id != "" and entity_nodes.has(selected_id):
        var selected_node: Node3D = entity_nodes[selected_id]
        overlay.position = selected_node.position
        overlay.scale = selected_node.scale * 3.0
        overlay.visible = true
    else:
        overlay.visible = false
    if destination_body_ghost != null and destination_body_ghost.visible and selected_kind == "ship" and entity_details.has(selected_id):
        var ship: Dictionary = entity_details[selected_id]
        var destination_body: Dictionary = ship.get("destination_body_at_arrival", {})
        if not destination_body.is_empty():
            destination_body_ghost.position = _scaled_position(destination_body)

func _update_destination_body_ghost(ship: Dictionary) -> void:
    var destination_body: Dictionary = ship.get("destination_body_at_arrival", {})
    if destination_body.is_empty():
        if destination_body_ghost != null:
            destination_body_ghost.visible = false
        return
    var ghost := _ensure_destination_body_ghost()
    var body_id := String(destination_body.get("id", ""))
    ghost.position = _scaled_position(destination_body)
    ghost.scale = Vector3.ONE * _body_display_scale(body_id)
    ghost.material_override = _make_destination_ghost_material(body_id)
    ghost.visible = true

func _focus_entity(entity_id: String, entity_kind: String) -> void:
    if not entity_targets.has(entity_id):
        return
    focused_id = entity_id
    focused_kind = entity_kind
    if entity_nodes.has(entity_id):
        camera_rig.focus_point(entity_nodes[entity_id].global_position)
    else:
        camera_rig.focus_point(entity_targets[entity_id] - render_origin)

func _update_camera_focus() -> void:
    if focused_id == "":
        return
    if not entity_nodes.has(focused_id):
        focused_id = ""
        focused_kind = ""
        return
    camera_rig.focus_point(entity_nodes[focused_id].global_position)

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

    var body_radius := float(body_display_radii.get(body_id, BODY_MIN_MODEL_SCALE))
    var station_scale := _station_display_scale(data)
    var clearance: float = max(station_scale * 5.0, body_radius * 0.22)
    var angle_jitter := float(abs(station_id.hash()) % 5) * station_scale * 1.5
    return body_center + radial * (body_radius + clearance + angle_jitter)
