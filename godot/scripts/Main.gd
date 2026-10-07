extends Node3D

const POSITION_SCALE := 1.0 / 8_000_000_000.0
# Bodies are drawn at their true radius (a map icon stands in when they are a few pixels
# across, as in Kerbal Space Program's map view). Ships and stations are tiny next to a
# planet; their models are a few tens of kilometres, shown only up close, icons otherwise.
const BODY_MIN_MODEL_SCALE := 1.0e-9
const SHIP_MIN_MODEL_SCALE := 0.000004
const STATION_MIN_MODEL_SCALE := 0.000006
const SHIP_MASS_SCALE := 0.000006
const STATION_POPULATION_SCALE := 0.000008
const BRIDGE_STEP_SECONDS := 0.1
# The simulation advances in ticks of 0.1 day (Simulation::TICK_S). Between snapshots the
# display clock runs on at the timewarp rate, at most this far ahead of the last snapshot.
const SIM_TICK_S := 8640.0
const DISPLAY_MAX_LEAD_REAL_S := 3.0
# Some simulation steps plan many trajectories at once (the opening dispatch, fleet reviews):
# say so when no snapshot has come for this long, rather than look frozen.
const BRIDGE_BUSY_NOTICE_S := 2.0
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
const BODY_ICON_ALPHA := 0.92
# Screen-space labels next to the map icons.
const LABEL_FONT_SIZE := {"body": 14, "station": 12, "ship": 11}
const LABEL_OFFSET_PX := Vector2(10.0, -9.0)
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
# Display clock (game seconds): bodies, stations and ships are placed at this time each
# frame, from orbital elements and planned paths, so the picture moves smoothly even when
# a simulation tick is slow.
var display_time_s := -1.0
var snapshot_game_time_s := 0.0
var snapshot_wall_s := 0.0
# The simulation's measured pace (game seconds per real second), smoothed: the display clock
# runs at it, so a slow stretch slows the picture down instead of freezing it.
var sim_pace := 0.0
var body_orbits: Dictionary = {}     # body_id -> {parent, a, period, phase}
var station_orbits: Dictionary = {}  # station_id -> {body, r, theta}
var ship_paths: Dictionary = {}      # ship_id -> {sig, t: PackedFloat64Array, p: PackedFloat64Array (x,y,z,...)}
var frame_positions_m: Dictionary = {}  # per-frame cache: entity_id -> PackedFloat64Array [x, y, z] in metres
var origin_m := PackedFloat64Array([0.0, 0.0, 0.0])  # the focus; entities are drawn relative to it
var entity_root: Node3D
var current_snapshot_seq := -1
var current_snapshot_arrival_s := 0.0
var selected_id := ""
var selected_kind := ""
var focused_id := ""
var focused_kind := ""
var bridge_state: Dictionary = {}
var entity_nodes: Dictionary = {}
var entity_targets: Dictionary = {}
var entity_details: Dictionary = {}
var entity_kinds: Dictionary = {}
var entity_visual_signatures: Dictionary = {}
var trail_nodes: Dictionary = {}
var trail_path_signatures: Dictionary = {}
var ship_trail_history: Dictionary = {}   # ship_id -> Array of Vector3 (world_root local)
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
var selected_ship_overlay: MeshInstance3D
var destination_body_ghost: MeshInstance3D
var current_paused := false
# Save/load requests to the bridge (F5 quick save, F9 quick load).
var command_request := 0
var command_save_path := ""
var command_load_path := ""
var bridge_epoch := 0
var bridge_status_text := ""
var bridge_status_until_s := 0.0
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
var _map_labels: Dictionary = {}
var _icon_textures: Dictionary = {}
var debug_map_enabled := false
var debug_log_accum_s := 0.0
var debug_frame := 0
var last_render_origin := Vector3.ZERO
var last_ui_refresh_s := -1000.0

const UiTheme := preload("res://scripts/ui/UiTheme.gd")
const SpaceEnvironmentScript := preload("res://scripts/SpaceEnvironment.gd")
const EntityVisualsScript := preload("res://scripts/EntityVisuals.gd")
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
    # Entities are placed relative to the focus (in double precision, then cast), so a
    # planet seen up close does not jitter; world_root holds the absolute-frame lines.
    entity_root = Node3D.new()
    entity_root.name = "EntityRoot"
    add_child(entity_root)
    _create_debug_guides()
    _setup_scene_lighting()
    _setup_map_icon_layer()
    _setup_ui()
    if "--debug-map" in OS.get_cmdline_user_args():
        debug_map_enabled = true
    for arg in OS.get_cmdline_user_args():
        if arg.begins_with("--shot-tour="):
            add_child(load("res://scripts/ShotTour.gd").new())
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
    for path in [snapshot_path, snapshot_path + ".tmp", snapshot_path + ".seq", snapshot_path + ".seq.tmp",
            snapshot_path + ".paths", snapshot_path + ".paths.tmp", snapshot_path + ".paths.seq", snapshot_path + ".paths.seq.tmp", command_path]:
        if FileAccess.file_exists(path):
            DirAccess.remove_absolute(path)

# Debug aid: SPACETRAINS_GODOT_PROFILE=1 prints where frame time goes, every 10 s.
var _profile_enabled := OS.has_environment("SPACETRAINS_GODOT_PROFILE")
var _profile_totals: Dictionary = {}
var _profile_worst: Dictionary = {}
var _profile_since_s := 0.0

func _profiled(name: String, started_usec: int) -> void:
    var ms := float(Time.get_ticks_usec() - started_usec) / 1000.0
    _profile_totals[name] = float(_profile_totals.get(name, 0.0)) + ms
    _profile_worst[name] = maxf(float(_profile_worst.get(name, 0.0)), ms)

func _process(delta: float) -> void:
    debug_frame += 1
    var t := Time.get_ticks_usec()
    _read_snapshot()
    if _profile_enabled: _profiled("read+apply snapshot", t)
    _update_bridge_status()
    _advance_display_clock(delta)
    t = Time.get_ticks_usec()
    _update_nodes(delta)
    if _profile_enabled: _profiled("update nodes", t)
    _update_camera_focus()
    t = Time.get_ticks_usec()
    _update_map_icons()
    if _profile_enabled: _profiled("map icons", t)
    _update_map_debug(delta)
    if _profile_enabled:
        _profile_since_s += delta
        if _profile_since_s >= 10.0:
            var parts: Array[String] = []
            for key in _profile_totals.keys():
                parts.append("%s %.1f ms/s (worst %.1f)" % [key, float(_profile_totals[key]) / _profile_since_s, float(_profile_worst[key])])
            print("[GodotProfile] " + ", ".join(parts))
            _profile_totals.clear()
            _profile_worst.clear()
            _profile_since_s = 0.0

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
        elif event.keycode == KEY_F5:
            _request_save_or_load(true)
        elif event.keycode == KEY_F9:
            _request_save_or_load(false)
        elif event.keycode == KEY_F11:
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
        "--step-seconds", str(BRIDGE_STEP_SECONDS),
        "--opening-cache", ProjectSettings.globalize_path("user://opening_cache")
    ]
    bridge_pid = OS.create_process(executable_path, args, false)
    if bridge_pid <= 0:
        _set_status("Failed to start bridge process.")
        bridge_started = false
    else:
        bridge_started = true

func _update_bridge_status() -> void:
    if not bridge_started:
        return
    if not OS.is_process_running(bridge_pid):
        bridge_started = false
        _set_status("The simulation bridge stopped.\nSee the terminal for its output.")
        return
    if current_snapshot_seq < 0:
        return
    if bridge_status_text != "" and _wall_time_s() > bridge_status_until_s:
        bridge_status_text = ""
        _set_status("")
    # The bridge writes a snapshot per tick (0.1 day): while paused, or at a slow timewarp,
    # none is due for a while.
    if current_paused:
        return
    var tick_interval_s: float = SIM_TICK_S / maxf(current_timewarp, 1.0)
    var waited_s := _wall_time_s() - current_snapshot_arrival_s
    if waited_s > maxf(BRIDGE_BUSY_NOTICE_S, 2.0 * tick_interval_s):
        _set_status("Simulation busy: planning ship missions… %d s" % int(waited_s))

func _quicksave_path() -> String:
    DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path("user://saves"))
    return ProjectSettings.globalize_path("user://saves/quicksave.json")

func _request_save_or_load(save: bool) -> void:
    command_request += 1
    command_save_path = _quicksave_path() if save else ""
    command_load_path = "" if save else _quicksave_path()
    _write_bridge_commands()
    _show_notice("Saving…" if save else "Loading…")

func _show_notice(text: String) -> void:
    bridge_status_text = text
    bridge_status_until_s = _wall_time_s() + 3.0
    _set_status(text)

func _write_bridge_commands() -> void:
    var file := FileAccess.open(command_path, FileAccess.WRITE)
    if file == null:
        return
    var payload := {
        "paused": current_paused,
        "timewarp_factor": current_timewarp,
        "request": command_request,
        "save_path": command_save_path,
        "load_path": command_load_path,
    }
    file.store_string(JSON.stringify(payload))

# Planned paths by ship id ({path_id, trajectory_path, destination_body_at_arrival}), from
# the bridge's paths file, which changes only when a ship gets a new plan.
var bridge_paths: Dictionary = {}
var paths_seq := -1

func _read_paths() -> void:
    var seq_text := FileAccess.get_file_as_string(snapshot_path + ".paths.seq")
    if seq_text == "" or int(seq_text) == paths_seq:
        return
    var json := JSON.new()
    if json.parse(FileAccess.get_file_as_string(snapshot_path + ".paths")) != OK or typeof(json.data) != TYPE_DICTIONARY:
        return
    paths_seq = int(seq_text)
    bridge_paths = json.data.get("paths", {})

func _read_snapshot() -> void:
    _read_paths()
    # The bridge bumps a small sequence file with every snapshot: parse only new ones.
    var seq_text := FileAccess.get_file_as_string(snapshot_path + ".seq")
    if seq_text != "" and int(seq_text) == current_snapshot_seq:
        return
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
    var bridge_info: Dictionary = json.data.get("bridge", {})
    var status := String(bridge_info.get("status", ""))
    if status != "" and status != String(bridge_state.get("bridge", {}).get("status", "")):
        _show_notice(status)
    var epoch := int(bridge_info.get("epoch", 0))
    if epoch != bridge_epoch:
        # A loaded game: a new timeline, possibly earlier. Forget the old one.
        bridge_epoch = epoch
        _reset_timeline()
        current_game_time_s = -1.0
    if current_game_time_s >= 0.0 and new_game_time_s + 0.001 < current_game_time_s:
        if debug_map_enabled:
            print("[MapDebugReject] seq=%d current_seq=%d new_game_day=%.3f current_game_day=%.3f" % [new_seq, current_snapshot_seq, new_game_time_s / 86400.0, current_game_time_s / 86400.0])
        return
    var arrival_now_s := _wall_time_s()
    current_snapshot_seq = new_seq
    current_snapshot_arrival_s = arrival_now_s
    bridge_state = json.data
    # Ships in flight carry only a path id; attach the planned path from the paths file.
    for ship in bridge_state.get("ships", []):
        var path_id := String(ship.get("path_id", ""))
        if path_id == "":
            continue
        var cached: Dictionary = bridge_paths.get(String(ship.get("id", "")), {})
        if String(cached.get("path_id", "")) == path_id:
            ship["trajectory_path"] = cached.get("trajectory_path", [])
            if cached.has("destination_body_at_arrival"):
                ship["destination_body_at_arrival"] = cached["destination_body_at_arrival"]
    if snapshot_wall_s > 0.0 and arrival_now_s > snapshot_wall_s and new_game_time_s >= snapshot_game_time_s:
        var interval := arrival_now_s - snapshot_wall_s
        var sample := (new_game_time_s - snapshot_game_time_s) / interval
        sim_pace += (sample - sim_pace) * clampf(interval / 1.5, 0.0, 1.0)
    snapshot_game_time_s = new_game_time_s
    snapshot_wall_s = arrival_now_s
    if display_time_s < 0.0:
        display_time_s = new_game_time_s
    _apply_snapshot()

func _apply_snapshot() -> void:
    var seen_ids := {}
    var ids_changed := false
    _cache_orbits()
    for body in bridge_state.get("bodies", []):
        body_display_radii[String(body["id"])] = _body_scale_from_radius(float(body.get("radius_m", 0.0)))
    for ship in bridge_state.get("ships", []):
        _cache_ship_path(String(ship["id"]), ship)

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
            if trail_nodes.has(entity_id):
                trail_nodes[entity_id].queue_free()
                trail_nodes.erase(entity_id)
                trail_path_signatures.erase(entity_id)
            if history_trail_nodes.has(entity_id):
                (history_trail_nodes[entity_id] as Node).queue_free()
                history_trail_nodes.erase(entity_id)
                ship_trail_history.erase(entity_id)
            if _map_icons.has(entity_id):
                (_map_icons[entity_id] as Node).queue_free()
                _map_icons.erase(entity_id)
            if _map_labels.has(entity_id):
                (_map_labels[entity_id] as Node).queue_free()
                _map_labels.erase(entity_id)
            ids_changed = true
            if entity_id == focused_id:
                focused_id = ""
                focused_kind = ""

    if ids_changed and selected_id != "" and not entity_details.has(selected_id):
        selected_id = ""
        selected_kind = ""
    for ship_id in ship_paths.keys():
        if not seen_ids.has(ship_id):
            ship_paths.erase(ship_id)
    _update_faction_colors()
    _update_ship_trails()
    if not has_auto_focused:
        _hide_debug_guides()
        _auto_focus_initial_entity()
    if status_label != null and status_label.visible and _wall_time_s() > bridge_status_until_s:
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
        var container: Node3D = EntityVisualsScript.make_entity(kind, data)
        container.name = entity_id
        entity_root.add_child(container)
        entity_nodes[entity_id] = container
        entity_targets[entity_id] = Vector3.ZERO
        entity_visual_signatures[entity_id] = ""
        _attach_map_icon(entity_id, kind, data)
        if entity_id == "saturn":
            container.add_child(SpaceEnvironmentScript.make_planet_ring())

    var visual_signature := _visual_signature(kind, data)
    if entity_visual_signatures.get(entity_id, "") != visual_signature:
        EntityVisualsScript.apply_visuals(entity_nodes[entity_id], kind, data, faction_colors)
        entity_nodes[entity_id].scale = _make_scale(kind, data)
        entity_visual_signatures[entity_id] = visual_signature
    # Positions are computed every frame (_update_nodes); this marks the entity as placeable.
    entity_targets[entity_id] = _scaled_position(data)

func _update_nodes(delta: float) -> void:
    if display_time_s < 0.0:
        return
    frame_positions_m.clear()
    if focused_id != "" and entity_kinds.has(focused_id):
        origin_m = _entity_m(focused_id)
    else:
        origin_m = PackedFloat64Array([0.0, 0.0, 0.0])
    last_render_origin = render_origin
    render_origin = _abs_render(origin_m)
    # world_root holds absolute-frame geometry (orbit rings, planned paths, trails).
    world_root.position = -render_origin
    body_positions.clear()
    station_positions.clear()
    for body_id in body_orbits.keys():
        body_positions[body_id] = _abs_render(_body_m(body_id))
    for station_id in station_orbits.keys():
        station_positions[station_id] = _abs_render(_station_m(station_id))
    for entity_id in entity_nodes.keys():
        var node: Node3D = entity_nodes[entity_id]
        var kind: String = entity_kinds.get(entity_id, "")
        node.position = _to_render(_entity_m(entity_id))
        if kind == "ship":
            var detail: Dictionary = entity_details.get(entity_id, {})
            if String(detail.get("phase", "idle")) != "in_transit":
                node.position += _docked_ship_offset(entity_id)
            _orient_ship(node, entity_id, _ship_heading(entity_id))
            EntityVisualsScript.update_engine_glow(node, detail, display_time_s, _world_size_for_pixels(node, 20.0))
    _update_history_trails(delta)
    if space_env != null:
        space_env.update_orbit_rings(world_root, bridge_state.get("bodies", []), body_positions, BODY_ICON_COLOR)
    _update_fine_rings()
    if sun_light != null and entity_nodes.has("sun"):
        var sun_node: Node3D = entity_nodes["sun"]
        sun_light.global_position = sun_node.global_position
        if space_env != null:
            space_env.update_sun(sun_node.global_position, sun_node.scale.x,
                _world_size_for_pixels(sun_node, 110.0))
    if space_env != null:
        space_env.update_camera(camera.global_position)
    _update_selected_overlay_positions()

# --- Display clock and positions ----------------------------------------------

func _advance_display_clock(delta: float) -> void:
    if display_time_s < 0.0:
        return
    # The starting state is shown before the opening dispatch (a few seconds of planning):
    # the clock starts with the first simulated tick.
    var paused := bool(bridge_state.get("paused", current_paused)) or snapshot_game_time_s <= 0.0
    var requested := 0.0 if paused else float(bridge_state.get("timewarp_factor", current_timewarp))
    # The clock runs at the simulation's measured pace (at most the requested timewarp): when
    # ticks run slow the picture slows down rather than freezing, and after a timewarp change
    # it follows within a second or two.
    var rate := minf(requested, maxf(sim_pace, requested * 0.1)) if requested > 0.0 else 0.0
    # Where the simulation should be by now; a slow tick lets the display run ahead a
    # little (ships follow their planned paths meanwhile), never backwards.
    var max_lead := maxf(SIM_TICK_S * 1.5, rate * DISPLAY_MAX_LEAD_REAL_S)
    var limit := snapshot_game_time_s + max_lead
    var target := minf(snapshot_game_time_s + rate * (_wall_time_s() - snapshot_wall_s), limit)
    # Behind the target: catch up (eased). Ahead of it: slow down (to a fifth when a whole
    # lead ahead), and brake through the last half of the lead window instead of stopping
    # dead at its end; never step back.
    var brake := clampf((limit - display_time_s) / (0.5 * max_lead), 0.0, 1.0)
    var next := display_time_s
    if target > display_time_s:
        next += rate * delta * brake
        next += (target - next) * clampf(delta * 3.0, 0.0, 1.0)
    else:
        var slow := clampf(1.0 - (display_time_s - target) / max_lead, 0.2, 1.0)
        next += rate * delta * slow * brake
    display_time_s = maxf(display_time_s, minf(next, limit))

func _cache_orbits() -> void:
    for body in bridge_state.get("bodies", []):
        body_orbits[String(body["id"])] = {
            "parent": String(body.get("parent_id", "")),
            "a": float(body.get("semi_major_axis_m", 0.0)),
            "period": float(body.get("orbital_period_s", 0.0)),
            "phase": float(body.get("phase_at_epoch_rad", 0.0)),
            "radius": float(body.get("radius_m", 0.0)),
        }
    for station in bridge_state.get("stations", []):
        var body_id := String(station.get("parent_body_id", ""))
        var body_radius := float((body_orbits.get(body_id, {}) as Dictionary).get("radius", 0.0))
        station_orbits[String(station["id"])] = {
            "body": body_id,
            "r": body_radius + float(station.get("altitude_m", 0.0)),
            "theta": float(station.get("theta_rad", 0.0)),
        }

# Planned paths as flat arrays, rebuilt only when a ship's plan changes.
func _cache_ship_path(ship_id: String, ship: Dictionary) -> void:
    var path: Array = ship.get("trajectory_path", [])
    if path.size() < 2:
        ship_paths.erase(ship_id)
        return
    var signature := _trajectory_path_signature(path)
    if ship_paths.has(ship_id) and String(ship_paths[ship_id]["sig"]) == signature:
        return
    var times := PackedFloat64Array()
    var points := PackedFloat64Array()
    times.resize(path.size())
    points.resize(path.size() * 3)
    for i in range(path.size()):
        var point: Dictionary = path[i]
        times[i] = float(point.get("t_s", 0.0))
        points[3 * i] = float(point.get("x", 0.0))
        points[3 * i + 1] = float(point.get("y", 0.0))
        points[3 * i + 2] = float(point.get("z", 0.0))
    ship_paths[ship_id] = {"sig": signature, "t": times, "p": points}

# Same formula as CelestialMechanics::get_body_position (circular, coplanar orbits).
func _body_m(body_id: String) -> PackedFloat64Array:
    if frame_positions_m.has(body_id):
        return frame_positions_m[body_id]
    var result := PackedFloat64Array([0.0, 0.0, 0.0])
    var orbit: Dictionary = body_orbits.get(body_id, {})
    var parent_id := String(orbit.get("parent", ""))
    if parent_id != "":
        var base := _body_m(parent_id)
        var period := float(orbit["period"])
        result = base
        if period > 0.0:
            var angle := float(orbit["phase"]) + (display_time_s / period) * TAU
            var a := float(orbit["a"])
            result = PackedFloat64Array([base[0] + cos(angle) * a, base[1], base[2] + sin(angle) * a])
    frame_positions_m[body_id] = result
    return result

func _station_m(station_id: String) -> PackedFloat64Array:
    var key := "station:" + station_id
    if frame_positions_m.has(key):
        return frame_positions_m[key]
    var orbit: Dictionary = station_orbits.get(station_id, {})
    var result := PackedFloat64Array([0.0, 0.0, 0.0])
    if not orbit.is_empty():
        var base := _body_m(String(orbit["body"]))
        var r := float(orbit["r"])
        var theta := float(orbit["theta"])
        result = PackedFloat64Array([base[0] + cos(theta) * r, base[1], base[2] + sin(theta) * r])
    frame_positions_m[key] = result
    return result

# A ship in transit flies its planned path; otherwise it is at its station.
func _ship_m(ship_id: String) -> PackedFloat64Array:
    var detail: Dictionary = entity_details.get(ship_id, {})
    if String(detail.get("phase", "idle")) == "in_transit" and ship_paths.has(ship_id):
        var sample := _path_sample(ship_id, display_time_s)
        return PackedFloat64Array([sample[0], sample[1], sample[2]])
    var station_id := String(detail.get("current_station_id", ""))
    if station_orbits.has(station_id):
        return _station_m(station_id)
    return PackedFloat64Array([float(detail.get("x", 0.0)), float(detail.get("y", 0.0)), float(detail.get("z", 0.0))])

# Position [x, y, z] and segment direction [dx, dy, dz] on a ship's path at time t.
func _path_sample(ship_id: String, t: float) -> PackedFloat64Array:
    var path: Dictionary = ship_paths[ship_id]
    var times: PackedFloat64Array = path["t"]
    var points: PackedFloat64Array = path["p"]
    var count := times.size()
    var i := clampi(times.bsearch(t, true) - 1, 0, count - 2)
    var span := times[i + 1] - times[i]
    var f := clampf((t - times[i]) / span, 0.0, 1.0) if span > 0.0 else 1.0
    var out := PackedFloat64Array()
    out.resize(6)
    for axis in range(3):
        var a := points[3 * i + axis]
        var b := points[3 * (i + 1) + axis]
        out[axis] = a + (b - a) * f
        out[3 + axis] = b - a
    return out

func _ship_heading(ship_id: String) -> Vector3:
    if not ship_paths.has(ship_id):
        return Vector3.ZERO
    var sample := _path_sample(ship_id, display_time_s)
    return Vector3(sample[3], sample[4], sample[5])

func _entity_m(entity_id: String) -> PackedFloat64Array:
    match String(entity_kinds.get(entity_id, "")):
        "body":
            return _body_m(entity_id)
        "station":
            return _station_m(entity_id)
        "ship":
            return _ship_m(entity_id)
    return PackedFloat64Array([0.0, 0.0, 0.0])

# Relative to the focus, subtracted in double precision before the cast to float.
func _to_render(p: PackedFloat64Array) -> Vector3:
    return Vector3((p[0] - origin_m[0]) * POSITION_SCALE, (p[1] - origin_m[1]) * POSITION_SCALE, (p[2] - origin_m[2]) * POSITION_SCALE)

func _abs_render(p: PackedFloat64Array) -> Vector3:
    return Vector3(p[0] * POSITION_SCALE, p[1] * POSITION_SCALE, p[2] * POSITION_SCALE)

# Docked ships sit in a small ring around their station, so several stay pickable.
func _docked_ship_offset(ship_id: String) -> Vector3:
    var detail: Dictionary = entity_details.get(ship_id, {})
    var index: int = abs(ship_id.hash()) % 7
    var angle := float(index) * 0.8975979
    var spacing := _ship_display_scale(detail) * 6.0
    return Vector3(cos(angle), 0.15 + float(index % 3) * 0.08, sin(angle)).normalized() * spacing

# --- Orbit rings near the focus -----------------------------------------------

# A fixed polygon looks like one up close, and the planet sits on the true circle. Rings
# that pass near the focus are redrawn each frame, densest where the camera is, in
# focus-relative double precision; the coarse ring is hidden meanwhile.
var fine_rings: Dictionary = {}  # body_id -> MeshInstance3D
const FINE_RING_POINTS := 720

func _update_fine_rings() -> void:
    if space_env == null:
        return
    var camera_distance_m := float(camera_rig.get("distance")) / POSITION_SCALE
    var active := {}
    for body_id in body_orbits.keys():
        var orbit: Dictionary = body_orbits[body_id]
        var parent_id := String(orbit["parent"])
        var a := float(orbit["a"])
        if parent_id == "" or a <= 0.0:
            continue
        var center := _body_m(parent_id)
        var dx := origin_m[0] - center[0]
        var dz := origin_m[2] - center[2]
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
            mesh.surface_add_vertex(_to_render(PackedFloat64Array([center[0] + cos(theta) * a, center[1], center[2] + sin(theta) * a])))
        mesh.surface_end()
        ring.mesh = mesh
        ring.visible = true
    for body_id in fine_rings.keys():
        if not active.has(body_id):
            (fine_rings[body_id] as MeshInstance3D).visible = false
    for body_id in body_orbits.keys():
        space_env.set_ring_visible(String(body_id), not active.has(body_id))

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
    var color: Color = BODY_ICON_COLOR.get(body_id, Color(0.6, 0.65, 0.7))
    material.albedo_color = Color(color.r, color.g, color.b, space_env.ORBIT_RING_ALPHA)
    instance.material_override = material
    instance.extra_cull_margin = 16384.0
    entity_root.add_child(instance)
    fine_rings[body_id] = instance
    return instance

func _orient_ship(node: Node3D, ship_id: String, motion: Vector3) -> void:
    var detail: Dictionary = entity_details.get(ship_id, {})
    if String(detail.get("phase", "idle")) != "in_transit":
        return
    if not node.is_inside_tree():
        return
    if motion.length_squared() < 1.0e-16:
        return
    var direction := motion.normalized()
    if absf(direction.dot(Vector3.UP)) > 0.999:
        return
    node.look_at(node.global_position + direction, Vector3.UP)

func _update_history_trails(delta: float) -> void:
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
        var history: Array = ship_trail_history.get(entity_id, [])
        history.append(_abs_render(_ship_m(entity_id)))
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

func _scaled_position(data: Dictionary) -> Vector3:
    return Vector3(
        float(data["x"]) * POSITION_SCALE,
        float(data.get("y", 0.0)) * POSITION_SCALE,
        float(data["z"]) * POSITION_SCALE
    )

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
    return EntityVisualsScript.body_color(body_id)

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
    var t := Time.get_ticks_usec()
    _refresh_ui_now()
    if _profile_enabled: _profiled("ui refresh", t)

func _refresh_ui_now() -> void:
    _record_price_history()
    _record_economy_history()
    if economy_history.size() >= 2:
        bridge_state["unmet_30d"] = _recent_unmet_share(30.0)
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
        "ships": bridge_state.get("ships", []),
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

func _reset_timeline() -> void:
    display_time_s = -1.0
    sim_pace = 0.0
    snapshot_wall_s = 0.0
    ship_paths.clear()
    economy_history.clear()
    price_history.clear()
    ship_trail_history.clear()
    trail_path_signatures.clear()

# Cumulative demand and unmet demand (base value), sampled once per game day, for the
# share that went short over a recent window.
var economy_history: Array = []

func _record_economy_history() -> void:
    var economy: Dictionary = bridge_state.get("economy", {})
    if economy.is_empty():
        return
    var day := float(bridge_state.get("game_time_days", 0.0))
    if not economy_history.is_empty() and day - float(economy_history.back()["day"]) < 1.0:
        return
    economy_history.append({"day": day, "demand": float(economy.get("demand_value", 0.0)), "unmet": float(economy.get("unmet_value", 0.0))})
    if economy_history.size() > 400:
        economy_history.pop_front()

func _recent_unmet_share(days: float) -> float:
    var latest: Dictionary = economy_history.back()
    var reference: Dictionary = economy_history.front()
    for i in range(economy_history.size() - 1, -1, -1):
        reference = economy_history[i]
        if float(latest["day"]) - float(reference["day"]) >= days:
            break
    var demand := float(latest["demand"]) - float(reference["demand"])
    return (float(latest["unmet"]) - float(reference["unmet"])) / demand if demand > 0.0 else 0.0

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
    sun_light.light_energy = 7.0
    sun_light.omni_range = 700.0  # past Neptune (~562 units) so outer planets get sunlight
    sun_light.omni_attenuation = 0.15  # far gentler than physical falloff: outer planets stay readable
    sun_light.shadow_enabled = false
    sun_light.light_color = Color(1.0, 0.96, 0.82)
    world_root.add_child(sun_light)
    RenderingServer.set_default_clear_color(Color(0.01, 0.012, 0.025, 1.0))

    var env := Environment.new()
    env.background_mode = Environment.BG_COLOR
    env.background_color = Color(0.01, 0.012, 0.025, 1.0)
    env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
    env.ambient_light_color = Color(0.16, 0.17, 0.22, 1.0)
    # High enough that textured night sides stay readable, low enough that the
    # sunward terminator still shows.
    env.ambient_light_energy = 0.55
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
    var label := Label.new()
    label.name = "%s_label" % entity_id
    label.text = String(data.get("name", entity_id))
    label.mouse_filter = Control.MOUSE_FILTER_IGNORE
    label.add_theme_font_size_override("font_size", int(LABEL_FONT_SIZE.get(kind, 11)))
    var label_color := _icon_color(kind, data).lightened(0.45)
    label_color.a = 0.9 if kind == "body" else 0.75
    label.add_theme_color_override("font_color", label_color)
    label.add_theme_color_override("font_outline_color", Color(0.0, 0.0, 0.0, 0.85))
    label.add_theme_constant_override("outline_size", 4)
    label.visible = false
    (map_icon_layer if map_icon_layer != null else canvas_layer).add_child(label)
    _map_labels[entity_id] = label

func _projected_model_pixels(node: Node3D) -> float:
    var viewport_height: float = max(float(get_viewport().get_visible_rect().size.y), 1.0)
    var dist: float = max(camera.global_position.distance_to(node.global_position), 1.0e-9)
    return node.scale.x / (2.0 * dist * tan(deg_to_rad(camera.fov) * 0.5)) * viewport_height

# Inverse of the above: the world size that projects to target_pixels at the
# node's current camera distance.
func _world_size_for_pixels(node: Node3D, target_pixels: float) -> float:
    var viewport_height: float = max(float(get_viewport().get_visible_rect().size.y), 1.0)
    var dist: float = max(camera.global_position.distance_to(node.global_position), 1.0e-9)
    return target_pixels / viewport_height * 2.0 * dist * tan(deg_to_rad(camera.fov) * 0.5)

func _update_map_icons() -> void:
    var viewport_rect := get_viewport().get_visible_rect()
    # Labels are placed in priority order (bodies, stations, ships; selected first) and
    # skipped where they would overlap one already placed.
    var label_candidates: Array = []
    for entity_id in _map_icons.keys():
        var icon: Sprite2D = _map_icons[entity_id]
        var label: Label = _map_labels.get(entity_id)
        if label != null:
            label.visible = false
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
        if not viewport_rect.grow(pixel_size).has_point(screen_pos):
            icon.visible = false
            continue
        var model_pixels := _projected_model_pixels(model)
        # Hand off from map icon to the actual mesh once it is large on screen.
        icon.visible = model_pixels <= target_size * 1.5
        if icon.visible:
            icon.position = screen_pos
            var texture_size := Vector2(icon.texture.get_width(), icon.texture.get_height())
            icon.scale = Vector2.ONE * (target_size / max(texture_size.x, texture_size.y))
            var color := (_icon_color(kind, data).lightened(0.35) if entity_id == selected_id else _icon_color(kind, data))
            color.a = BODY_ICON_ALPHA if kind == "body" else ICON_ALPHA
            icon.modulate = color
        if label == null:
            continue
        var priority := {"body": 0, "station": 1, "ship": 2}.get(kind, 3) as int
        if entity_id == selected_id:
            priority = -1
        elif kind == "ship" and String(data.get("phase", "")) != "in_transit":
            continue  # docked ships are listed by their station
        var offset := LABEL_OFFSET_PX
        if not icon.visible:
            # Beside the body's disc rather than over it.
            offset.x += minf(model_pixels, viewport_rect.size.x * 0.25)
        label_candidates.append({"id": entity_id, "priority": priority, "position": screen_pos + offset})
    label_candidates.sort_custom(func(a, b): return a["priority"] < b["priority"])
    var placed: Array[Rect2] = []
    for candidate in label_candidates:
        var label: Label = _map_labels[candidate["id"]]
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
    print("[MapDebug] reason=%s frame=%d seq=%d display_day=%.3f focused=%s/%s selected=%s/%s camera_distance=%.6f render_origin=%s origin_delta=%.6f world_root=%s pivot=%s" % [
        reason,
        debug_frame,
        current_snapshot_seq,
        display_time_s / 86400.0,
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
    material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
    material.vertex_color_use_as_albedo = true
    path_mesh.material_override = material
    node.add_child(path_mesh)
    world_root.add_child(node)
    trail_nodes[ship_id] = node
    return node

# Draw planned trajectories for every moving ship: the selected one bright with
# burn markers, the rest as faint context lines. The flown-past portion of the
# path dims to gray; the remaining arc keeps the per-planner color.
func _update_ship_trails() -> void:
    var game_time_s := float(bridge_state.get("game_time_s", 0.0))
    var active := {}
    for ship_id in entity_details.keys():
        if entity_kinds.get(ship_id, "") != "ship":
            continue
        var ship: Dictionary = entity_details[ship_id]
        var phase := String(ship.get("phase", "idle"))
        if phase != "in_transit" and phase != "awaiting_departure":
            continue
        var trajectory_path: Array = ship.get("trajectory_path", [])
        if trajectory_path.size() < 2:
            continue
        active[ship_id] = true
        var trail_node := _ensure_trail_node(ship_id)
        var is_selected: bool = ship_id == selected_id
        var departure_s := float(ship.get("departure_time_s", 0.0))
        var arrival_s := float(ship.get("arrival_time_s", 0.0))
        var progress_bucket := int(clamp((game_time_s - departure_s) / max(arrival_s - departure_s, 1.0), 0.0, 1.0) * 50.0)
        var signature := "%s|%d|%s" % [_trajectory_path_signature(trajectory_path), progress_bucket, is_selected]
        if trail_path_signatures.get(ship_id, "") != signature:
            _rebuild_trail_mesh(trail_node, trajectory_path, ship, is_selected, game_time_s)
            trail_path_signatures[ship_id] = signature
        trail_node.visible = true

    for ship_id in trail_nodes.keys():
        if not active.has(ship_id):
            trail_nodes[ship_id].visible = false

    if selected_kind == "ship" and active.has(selected_id):
        _update_destination_body_ghost(entity_details[selected_id])
    elif destination_body_ghost != null:
        destination_body_ghost.visible = false

# Runs every frame for every active ship, so it samples a few points rather than
# formatting the whole path (long launch waits can add hundreds of samples).
# A new plan always changes the size or the endpoints.
func _trajectory_path_signature(trajectory_path: Array) -> String:
    var parts: Array[String] = [str(trajectory_path.size())]
    for index in [0, trajectory_path.size() / 2, trajectory_path.size() - 1]:
        var point: Dictionary = trajectory_path[index]
        parts.append("%.3f,%.3f,%.3f,%.0f" % [float(point.get("x", 0.0)), float(point.get("y", 0.0)), float(point.get("z", 0.0)), float(point.get("t_s", 0.0))])
    return "|".join(parts)

func _trajectory_color(trajectory_type: String) -> Color:
    return TRAJECTORY_COLORS.get(trajectory_type, Color(0.3, 1.0, 0.8)) as Color

func _rebuild_trail_mesh(trail_node: Node3D, trajectory_path: Array, ship: Dictionary, is_selected: bool, game_time_s: float) -> void:
    var line_color := _trajectory_color(String(ship.get("trajectory_type", "")))
    var base_alpha := 0.95 if is_selected else 0.2
    var elapsed_color := Color(0.5, 0.52, 0.55, base_alpha * 0.45)
    var mesh := ImmediateMesh.new()
    mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
    for point in trajectory_path:
        var point_time_s := float(point.get("t_s", 0.0))
        var color := elapsed_color if point_time_s < game_time_s else Color(line_color.r, line_color.g, line_color.b, base_alpha)
        mesh.surface_set_color(color)
        mesh.surface_add_vertex(_scaled_position(point))
    mesh.surface_end()
    var path_mesh: MeshInstance3D = trail_node.get_node("path")
    path_mesh.mesh = mesh
    _update_burn_markers(trail_node, trajectory_path, ship, is_selected, line_color)

# Impulsive-burn planners get small emissive markers at the departure and
# arrival burns; continuous-thrust (variable ISP) paths have no discrete burns.
func _update_burn_markers(trail_node: Node3D, trajectory_path: Array, ship: Dictionary, is_selected: bool, line_color: Color) -> void:
    var wants_markers: bool = is_selected and String(ship.get("trajectory_type", "")) != "variable_isp"
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
            marker.scale = Vector3.ONE * 0.012
            trail_node.add_child(marker)
        (marker.material_override as StandardMaterial3D).albedo_color = line_color
        (marker.material_override as StandardMaterial3D).emission = line_color
        marker.position = _scaled_position(trajectory_path.front() if marker_index == 0 else trajectory_path.back())
        marker.visible = true

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
    entity_root.add_child(selected_ship_overlay)
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
        # Locator beacon for sub-pixel ships; the mesh itself takes over up close.
        overlay.visible = _projected_model_pixels(selected_node) < 24.0
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
    # Zoom stops outside the focused body (bodies are drawn at true size now).
    var min_distance := 0.00002
    if entity_kind == "body":
        min_distance = maxf(min_distance, _body_display_scale(entity_id) * 1.6)
    camera_rig.set("min_distance", min_distance)
    camera_rig.set("distance", maxf(float(camera_rig.get("distance")), min_distance))
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
