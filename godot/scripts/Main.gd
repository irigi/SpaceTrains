extends Node3D

const POSITION_SCALE := 1.0 / 8_000_000_000.0
# Bodies are drawn at their true radius (a map icon stands in when they are a few pixels
# across, as in Kerbal Space Program's map view). Ships and stations are tiny next to a
# planet; their models are a few hundred kilometres, shown only up close, icons otherwise.
const BODY_MIN_MODEL_SCALE := 1.0e-9
# Ship and station models are built in metres (ShipModels.gd, StationModels.gd) and drawn
# MODEL_SIZE_EXAGGERATION times real size; 1 is true scale (a 49 m light freighter is
# 6e-9 world units). Ships and stations share the factor, so their size ratios stay real.
# The user arg --model-scale=N overrides it.
const MODEL_SIZE_EXAGGERATION := 1.0
var ship_metres_to_world := POSITION_SCALE * MODEL_SIZE_EXAGGERATION
var _print_status := false  # the shot tour logs status messages
# A ship's detailed model replaces the coarse one above this on-screen radius (pixels),
# and goes back below the lower one: a docked ship stays detailed until its station
# (~20x its size) is well away.
const SHIP_DETAIL_PX := 6.0
const SHIP_COARSE_PX := 4.0
# The same for stations (their radius includes the ring).
const STATION_DETAIL_PX := 90.0
const STATION_COARSE_PX := 60.0
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

const ICON_PICK_PADDING_PX := 6.0
const DEBUG_SAMPLE_BODY_IDS := ["sun", "mercury", "earth", "mars", "jupiter", "saturn", "neptune"]
const DEBUG_LOG_INTERVAL_S := 1.0
const UI_REFRESH_INTERVAL_S := 0.5

@onready var world_root: Node3D = $WorldRoot
@onready var camera_rig: Node3D = $CameraRig
@onready var camera: Camera3D = $CameraRig/Camera3D
@onready var canvas_layer: CanvasLayer = $CanvasLayer
var map_icons: MapIconsScript  # the icons and labels over the map
var economy_history := EconomyHistoryScript.new()  # prices and unmet demand over time
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
var ship_paths: Dictionary = {}      # ship_id -> {sig, origin, destination, departure, arrival, t, off (x,y,z,... from the rail anchor)}
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
var trail_points: Dictionary = {}  # ship_id -> {points (metres + time, as drawn), color, alpha}
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
var destination_body_ghost: MeshInstance3D
var current_paused := false
# Station ring spin runs on real time, but stops while the game is paused (as the top bar shows it).
var spin_clock_s := 0.0
var _spin_wall_s := -1.0
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
var debug_map_enabled := false
var debug_log_accum_s := 0.0
var debug_frame := 0
var last_render_origin := Vector3.ZERO
var last_ui_refresh_s := -1000.0

const UiTheme := preload("res://scripts/ui/UiTheme.gd")
const SpaceEnvironmentScript := preload("res://scripts/SpaceEnvironment.gd")
const EntityVisualsScript := preload("res://scripts/EntityVisuals.gd")
const ShipModelsScript := preload("res://scripts/ShipModels.gd")
const StationModelsScript := preload("res://scripts/StationModels.gd")
const ModelMaterialsScript := preload("res://scripts/ModelMaterials.gd")
const TopBarPanel := preload("res://scripts/ui/TopBar.gd")
const EntityBrowserPanel := preload("res://scripts/ui/EntityBrowser.gd")
const InspectorPanelScript := preload("res://scripts/ui/InspectorPanel.gd")
const EventTickerPanel := preload("res://scripts/ui/EventTicker.gd")
const MarketPanelScript := preload("res://scripts/ui/MarketPanel.gd")
const HelpPanelScript := preload("res://scripts/ui/HelpPanel.gd")
const StatisticsPanelScript := preload("res://scripts/ui/StatisticsPanel.gd")
const MapIconsScript := preload("res://scripts/MapIcons.gd")
const EconomyHistoryScript := preload("res://scripts/EconomyHistory.gd")

var ui_root: Control
var top_bar: PanelContainer
var entity_browser: PanelContainer
var inspector_panel: PanelContainer
var event_ticker: PanelContainer
var market_panel: PanelContainer
var help_panel: PanelContainer
var stats_panel: PanelContainer
var status_label: Label
var thermal_label: Label
# Flow overlay (G): goods between stations as lines on the map, from the statistics file (the
# Flows tab's period, good and measure). The bridge writes that file while the overlay is on.
var flow_overlay := false
# Porkchop plots: the planet pairs asked of the bridge ("a>b;c>d"), and its latest file.
var porkchop_request := ""
var porkchop_seq := -1
var porkchop_data: Dictionary = {}
var flow_label: Label
# Price history samples: {day: float, prices: {station_id: {commodity_id: price}}}

func _ready() -> void:
    repo_root = ProjectSettings.globalize_path("res://").get_base_dir().get_base_dir()
    executable_path = repo_root.path_join("build/bin/spacetrains_bridge")
    var session_id := "%d_%d" % [OS.get_process_id(), Time.get_ticks_usec()]
    snapshot_path = ProjectSettings.globalize_path("user://spacetrains_snapshot_%s.json" % session_id)
    command_path = ProjectSettings.globalize_path("user://spacetrains_commands_%s.json" % session_id)
    # Debug aid: SPACETRAINS_TIMEWARP=432000 starts at that speed (profiling without a keyboard).
    if OS.has_environment("SPACETRAINS_TIMEWARP"):
        current_timewarp = float(OS.get_environment("SPACETRAINS_TIMEWARP"))
    _write_bridge_commands()
    _start_bridge()
    # Entities are placed relative to the focus (in double precision, then cast), so a
    # planet seen up close does not jitter; world_root holds the absolute-frame lines.
    entity_root = Node3D.new()
    entity_root.name = "EntityRoot"
    add_child(entity_root)
    _create_debug_guides()
    _setup_scene_lighting()
    map_icons = MapIconsScript.new()
    canvas_layer.add_child(map_icons)
    canvas_layer.move_child(map_icons, 0)
    map_icons.setup(camera, entity_nodes, entity_details, entity_kinds, faction_colors)
    _setup_ui()
    if "--debug-map" in OS.get_cmdline_user_args():
        debug_map_enabled = true
    for arg in OS.get_cmdline_user_args():
        if arg.begins_with("--shot-tour="):
            _print_status = true
        if arg.begins_with("--model-scale="):
            ship_metres_to_world = POSITION_SCALE * maxf(float(arg.trim_prefix("--model-scale=")), 1.0e-3)
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
    top_bar.save_requested.connect(func() -> void: _request_save_or_load(true))
    top_bar.load_requested.connect(func() -> void: _request_save_or_load(false))
    top_bar.help_requested.connect(func() -> void: help_panel.toggle())
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
    inspector_panel.entity_selected.connect(func(entity_id: String, kind: String) -> void: select_entity(entity_id, kind, true))
    inspector_panel.route_requested.connect(func(origin: String, destination: String) -> void:
        stats_panel.show_route(origin, destination)
        _update_porkchop_request())
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

    var help_center := CenterContainer.new()
    help_center.name = "HelpCenter"
    help_center.mouse_filter = Control.MOUSE_FILTER_IGNORE
    help_center.set_anchors_preset(Control.PRESET_FULL_RECT)
    ui_root.add_child(help_center)
    help_panel = HelpPanelScript.new()
    help_panel.name = "HelpPanel"
    help_center.add_child(help_panel)

    var stats_center := CenterContainer.new()
    stats_center.name = "StatsCenter"
    stats_center.mouse_filter = Control.MOUSE_FILTER_IGNORE
    stats_center.set_anchors_preset(Control.PRESET_FULL_RECT)
    ui_root.add_child(stats_center)
    stats_panel = StatisticsPanelScript.new()
    stats_panel.name = "StatisticsPanel"
    stats_panel.ship_selected.connect(func(ship_id: String) -> void: select_entity(ship_id, "ship", true))
    # The bridge writes its statistics file only while the screen is open.
    stats_panel.visibility_toggled.connect(func(_open: bool) -> void: _write_bridge_commands())
    stats_center.add_child(stats_panel)

    status_label = Label.new()
    status_label.name = "Status"
    status_label.set_anchors_preset(Control.PRESET_CENTER)
    status_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
    status_label.add_theme_color_override("font_color", UiTheme.WARN)
    ui_root.add_child(status_label)

    thermal_label = Label.new()
    thermal_label.name = "ThermalView"
    thermal_label.text = "Thermal view (T)"
    thermal_label.set_anchors_and_offsets_preset(Control.PRESET_CENTER_BOTTOM)
    thermal_label.offset_top = -64.0
    thermal_label.offset_bottom = -40.0
    thermal_label.grow_horizontal = Control.GROW_DIRECTION_BOTH
    thermal_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
    thermal_label.add_theme_color_override("font_color", UiTheme.WARN)
    thermal_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
    thermal_label.visible = false
    ui_root.add_child(thermal_label)
    flow_label = thermal_label.duplicate()
    flow_label.name = "FlowOverlay"
    flow_label.offset_top = -88.0
    flow_label.offset_bottom = -64.0
    flow_label.add_theme_color_override("font_color", UiTheme.ACCENT)
    ui_root.add_child(flow_label)

func _set_status(text: String) -> void:
    if _print_status and status_label != null and text != status_label.text and text != "":
        print("[status] ", text)
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
            snapshot_path + ".paths", snapshot_path + ".paths.tmp", snapshot_path + ".paths.seq", snapshot_path + ".paths.seq.tmp",
            snapshot_path + ".designs", snapshot_path + ".designs.tmp", snapshot_path + ".designs.seq", snapshot_path + ".designs.seq.tmp",
            snapshot_path + ".stats", snapshot_path + ".stats.tmp", snapshot_path + ".stats.seq", snapshot_path + ".stats.seq.tmp",
            snapshot_path + ".porkchop", snapshot_path + ".porkchop.tmp", snapshot_path + ".porkchop.seq", snapshot_path + ".porkchop.seq.tmp",
            command_path, command_path + ".tmp"]:
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
    map_icons.update(selected_id)
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
        elif event.keycode == KEY_G:
            flow_overlay = not flow_overlay
            flow_label.visible = flow_overlay
            _write_bridge_commands()
            _update_flow_overlay()
        elif event.keycode == KEY_T:
            ModelMaterialsScript.set_thermal_view(not ModelMaterialsScript.thermal_view)
            thermal_label.visible = ModelMaterialsScript.thermal_view
        elif event.keycode == KEY_H:
            help_panel.toggle()
        elif event.keycode == KEY_S:
            stats_panel.toggle()
        elif event.keycode == KEY_ESCAPE:
            # Close the topmost open panel.
            if help_panel.visible:
                help_panel.hide()
            elif market_panel.visible:
                market_panel.hide()
            elif stats_panel.visible:
                stats_panel.toggle()
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
        get_viewport().gui_release_focus()  # a click on the map takes the keys back from a text field
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
        "--opening-cache", ProjectSettings.globalize_path("user://opening_cache"),
        # The bridge exits when this process is gone (no orphan if the UI crashes).
        "--parent-pid", str(OS.get_process_id())
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

# Written aside and renamed, so the bridge never reads a half-written command (a save or
# load request read half-written was lost).
func _write_bridge_commands() -> void:
    var temp_path := command_path + ".tmp"
    var file := FileAccess.open(temp_path, FileAccess.WRITE)
    if file == null:
        return
    var payload := {
        "paused": current_paused,
        "timewarp_factor": current_timewarp,
        "request": command_request,
        "save_path": command_save_path,
        "load_path": command_load_path,
        "stats_open": (stats_panel != null and stats_panel.visible) or flow_overlay,
        "porkchop": porkchop_request,
    }
    file.store_string(JSON.stringify(payload))
    file.close()
    DirAccess.rename_absolute(temp_path, command_path)

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

# Ship designs by class id ({name, ..., components: [...]}): the parts of every class for the
# inspector's ship systems section. Written once by the bridge (per-ship fits later).
var bridge_designs: Dictionary = {}
var jet_power_by_class: Dictionary = {}
var designs_seq := -1

func _read_designs() -> void:
    var seq_text := FileAccess.get_file_as_string(snapshot_path + ".designs.seq")
    if seq_text == "" or int(seq_text) == designs_seq:
        return
    var json := JSON.new()
    if json.parse(FileAccess.get_file_as_string(snapshot_path + ".designs")) != OK or typeof(json.data) != TYPE_DICTIONARY:
        return
    designs_seq = int(seq_text)
    bridge_designs = json.data.get("designs", {})
    jet_power_by_class.clear()

# The pairs to ask for: the Routes tab's, and the selected station's planet to every other planet
# with a station (the inspector's launch windows).
func _update_porkchop_request() -> void:
    var pairs: Array[String] = []
    if stats_panel != null and stats_panel.visible and stats_panel.get("_tab") == stats_panel.ROUTES_TAB:
        pairs.append("%s>%s" % [stats_panel.route_origin, stats_panel.route_destination])
    if selected_kind == "station" and entity_details.has(selected_id):
        var here := _planet_of(String(entity_details[selected_id].get("parent_body_id", "")))
        var planets: Array[String] = []
        for station in bridge_state.get("stations", []):
            var planet := _planet_of(String(station.get("parent_body_id", "")))
            if planet != here and not planets.has(planet):
                planets.append(planet)
        # Nearest the Sun first: the inner pairs take seconds, the outer planets about a minute.
        const ORDER := ["mercury", "venus", "earth", "mars", "ceres", "jupiter", "saturn", "uranus", "neptune"]
        planets.sort_custom(func(a, b) -> bool: return ORDER.find(a) < ORDER.find(b))
        for planet in planets:
            if not pairs.has("%s>%s" % [here, planet]):
                pairs.append("%s>%s" % [here, planet])
    var text := ";".join(pairs)
    if text != porkchop_request:
        porkchop_request = text
        _write_bridge_commands()

func _read_porkchops() -> void:
    if porkchop_request == "":
        return
    var seq_text := FileAccess.get_file_as_string(snapshot_path + ".porkchop.seq")
    if seq_text == "" or int(seq_text) == porkchop_seq:
        return
    var json := JSON.new()
    if json.parse(FileAccess.get_file_as_string(snapshot_path + ".porkchop")) != OK or typeof(json.data) != TYPE_DICTIONARY:
        return
    porkchop_seq = int(seq_text)
    porkchop_data = json.data
    stats_panel.update_porkchops(porkchop_data, bridge_state.get("ships", []))

# The statistics side file (money journal), written by the bridge while the Statistics screen is open.
var stats_seq := -1

func _read_stats() -> void:
    if stats_panel == null or not (stats_panel.visible or flow_overlay):
        return
    var seq_text := FileAccess.get_file_as_string(snapshot_path + ".stats.seq")
    if seq_text == "" or int(seq_text) == stats_seq:
        return
    var json := JSON.new()
    if json.parse(FileAccess.get_file_as_string(snapshot_path + ".stats")) != OK or typeof(json.data) != TYPE_DICTIONARY:
        return
    stats_seq = int(seq_text)
    stats_panel.update_stats(json.data)
    _update_flow_overlay()

# The overlay's lines: goods delivered from station to station (surface trade left out), for the
# Flows tab's period, good and measure.
func _update_flow_overlay() -> void:
    if map_icons == null:
        return
    if not flow_overlay:
        map_icons.flow_lines = []
        return
    var period := String(stats_panel.get("_period"))
    var good := String(stats_panel.get("_flow_good"))
    var by_value := bool(stats_panel.get("_flow_value"))
    var sums := {}
    for e in stats_panel.stats.get("flows", {}).get(period, {}).get("entries", []):
        if String(e[0]) != "delivered" or String(e[1]).begins_with("surface:") or String(e[2]).begins_with("surface:"):
            continue
        if good != "" and String(e[3]) != good:
            continue
        var key := String(e[1]) + "|" + String(e[2])
        sums[key] = float(sums.get(key, 0.0)) + (float(e[5]) if by_value else float(e[4]))
    var lines: Array = []
    var color := UiTheme.commodity_color(good) if good != "" else UiTheme.ACCENT
    for key in sums.keys():
        var parts: PackedStringArray = String(key).split("|")
        lines.append({"from": parts[0], "to": parts[1], "amount": float(sums[key]), "color": color})
    map_icons.flow_lines = lines
    var names := {"lifetime": "since the start", "last_year": "last year", "last_30": "last 30 days"}
    flow_label.text = "Flows (G): %s, %s, %s · set in Statistics → Flows" % [good.replace("_", " ") if good != "" else "all goods",
        "credits" if by_value else "units", String(names.get(period, period))]

# Ships in flight carry only a path id; attach the planned path from the paths file.
# Returns how many ships name a path the paths file does not have.
func _attach_paths() -> int:
    var missing := 0
    for ship in bridge_state.get("ships", []):
        var path_id := String(ship.get("path_id", ""))
        if path_id == "":
            continue
        var cached: Dictionary = bridge_paths.get(String(ship.get("id", "")), {})
        if String(cached.get("path_id", "")) == path_id:
            ship["trajectory_path"] = cached.get("trajectory_path", [])
            if cached.has("destination_body_at_arrival"):
                ship["destination_body_at_arrival"] = cached["destination_body_at_arrival"]
        else:
            missing += 1
    return missing

func _read_snapshot() -> void:
    _read_paths()
    _read_designs()
    _read_stats()
    _read_porkchops()
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
    var parse_started := Time.get_ticks_usec()
    var parse_status := json.parse(file.get_as_text())
    if _profile_enabled: _profiled("  snapshot parse", parse_started)
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
    # The bridge writes the paths a snapshot names before the snapshot: one read in between
    # (the paths file read first, the new snapshot after) has an older paths file, read again.
    if _attach_paths() > 0:
        _read_paths()
        _attach_paths()
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
    # Before the entities: new stations and labels take their faction's colour from the data.
    _update_faction_colors()

    var upsert_started := Time.get_ticks_usec()
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
                trail_points.erase(entity_id)
            if fine_paths.has(entity_id):
                if fine_paths[entity_id]["node"] != null:
                    (fine_paths[entity_id]["node"] as Node).queue_free()
                fine_paths.erase(entity_id)
            if history_trail_nodes.has(entity_id):
                (history_trail_nodes[entity_id] as Node).queue_free()
                history_trail_nodes.erase(entity_id)
                ship_trail_history.erase(entity_id)
            map_icons.remove(entity_id)
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
    _rebuild_docked_slots()
    if _profile_enabled: _profiled("  upsert entities", upsert_started)
    var trails_started := Time.get_ticks_usec()
    _update_ship_trails()
    if _profile_enabled: _profiled("  ship trails", trails_started)
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
        map_icons.attach(entity_id, kind, data)
        if entity_id == "saturn":
            container.add_child(SpaceEnvironmentScript.make_planet_ring())

    if kind == "ship":
        _ensure_ship_model(entity_nodes[entity_id], data)
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
    var docked: Array[String] = []
    var wall_s := _wall_time_s()
    if _spin_wall_s >= 0.0 and not bool(bridge_state.get("paused", current_paused)):
        spin_clock_s += wall_s - _spin_wall_s
    _spin_wall_s = wall_s
    var spin_time := spin_clock_s
    for entity_id in entity_nodes.keys():
        var node: Node3D = entity_nodes[entity_id]
        var kind: String = entity_kinds.get(entity_id, "")
        node.position = _to_render(_entity_m(entity_id))
        if kind == "station":
            _update_station_lod(node, entity_details.get(entity_id, {}))
            StationModelsScript.spin_rings(node, spin_time)
        elif kind == "ship":
            var detail: Dictionary = entity_details.get(entity_id, {})
            if String(detail.get("phase", "idle")) != "in_transit":
                docked.append(entity_id)
            _orient_ship(node, entity_id, _ship_heading(entity_id))
    # Docked ships, and ships near a station, after every station has its position for this frame.
    for entity_id in docked:
        _place_docked_ship(entity_nodes[entity_id], entity_id)
    for entity_id in entity_nodes.keys():
        if entity_kinds.get(entity_id, "") == "ship" \
                and String((entity_details.get(entity_id, {}) as Dictionary).get("phase", "idle")) == "in_transit":
            _ease_into_berth(entity_nodes[entity_id], entity_id)
    for entity_id in entity_nodes.keys():
        if entity_kinds.get(entity_id, "") == "ship":
            var node: Node3D = entity_nodes[entity_id]
            var detail: Dictionary = entity_details.get(entity_id, {})
            _update_ship_lod(node, detail)
            EntityVisualsScript.update_engine_glow(node, detail, display_time_s, map_icons.world_size_for_pixels(node, 20.0),
                _jet_power_w(String(detail.get("design_id", detail.get("class_id", "")))))
    _update_camera_obstacles()
    _update_history_trails(delta)
    if space_env != null:
        space_env.update_orbit_rings(world_root, bridge_state.get("bodies", []), body_positions, MapIconsScript.BODY_ICON_COLOR)
    _update_fine_rings()
    _update_fine_paths()
    if sun_light != null and entity_nodes.has("sun"):
        var sun_node: Node3D = entity_nodes["sun"]
        sun_light.global_position = sun_node.global_position
        if space_env != null:
            space_env.update_sun(sun_node.global_position, sun_node.scale.x,
                map_icons.world_size_for_pixels(sun_node, 110.0))
    if space_env != null:
        space_env.update_camera(camera.global_position)
    _update_selected_overlay_positions()

# Planets, moons and the Sun keep the camera out (OrbitCamera pushes it to their surface).
func _update_camera_obstacles() -> void:
    var obstacles: Array = []
    for body_id in body_orbits.keys():
        var node: Node3D = entity_nodes.get(body_id)
        if node != null and node.scale.x > BODY_MIN_MODEL_SCALE:
            obstacles.append([node.global_position, node.scale.x])
    camera_rig.set("obstacles", obstacles)

# --- Display clock and positions ----------------------------------------------

func _advance_display_clock(delta: float) -> void:
    if display_time_s < 0.0:
        return
    # The starting state is shown before the opening dispatch (a few seconds of planning):
    # the clock starts with the first simulated tick.
    var paused := bool(bridge_state.get("paused", current_paused)) or snapshot_game_time_s <= 0.0
    if paused:
        # No time passes while paused: resuming must not look like a long-overdue snapshot.
        snapshot_wall_s = _wall_time_s()
        return
    var requested := float(bridge_state.get("timewarp_factor", current_timewarp))
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
        next = minf(next, limit)
    elif display_time_s < limit:
        var slow := clampf(1.0 - (display_time_s - target) / max_lead, 0.2, 1.0)
        next = minf(next + rate * delta * slow * brake, limit)
    elif _wall_time_s() - snapshot_wall_s < 0.5:
        # Past the limit (it shrinks when the simulation slows down after running fast):
        # crawl while snapshots keep coming rather than freeze; stop only if they stop.
        next += rate * delta * 0.2
    display_time_s = maxf(display_time_s, next)

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
    var signature := String(ship.get("path_id", ""))
    if signature == "":
        signature = _trajectory_path_signature(path)
    if ship_paths.has(ship_id) and String(ship_paths[ship_id]["sig"]) == signature:
        return
    var cached := {
        "sig": signature,
        "origin": String((station_orbits.get(String(ship.get("origin_station_id", "")), {}) as Dictionary).get("body", "")),
        "destination": String((station_orbits.get(String(ship.get("destination_station_id", "")), {}) as Dictionary).get("body", "")),
        "departure": float(ship.get("departure_time_s", 0.0)),
        "arrival": float(ship.get("arrival_time_s", 0.0)),
    }
    var origin_planet := _planet_of(String(cached["origin"]))
    cached["helio"] = origin_planet != "" and origin_planet != _planet_of(String(cached["destination"]))
    var times := PackedFloat64Array()
    var offsets := PackedFloat64Array()
    var raw := PackedFloat64Array()
    times.resize(path.size())
    offsets.resize(path.size() * 3)
    raw.resize(path.size() * 3)
    for i in range(path.size()):
        var point: Dictionary = path[i]
        var t := float(point.get("t_s", 0.0))
        times[i] = t
        var anchor := _rail_anchor_m(cached, t)
        var xyz := [float(point.get("x", 0.0)), float(point.get("y", 0.0)), float(point.get("z", 0.0))]
        for axis in range(3):
            offsets[3 * i + axis] = xyz[axis] - anchor[axis]
            raw[3 * i + axis] = xyz[axis]
    cached["t"] = times
    cached["off"] = offsets
    cached["raw"] = raw
    cached["sense"] = _path_sense(times, raw, float(cached["departure"]), float(cached["arrival"]))
    cached["doff"] = _offset_rates(times, offsets, float(cached["departure"]), float(cached["arrival"]))
    if bool(cached["helio"]):
        _cache_helio_polar(cached)
    ship_paths[ship_id] = cached

# Radius and unwound angle of each heliocentric sample, with their rates at the samples
# (from the neighbours within the flight, so the wait samples before departure don't
# bend the start), for the cubic drawing in _helio_point_m.
func _cache_helio_polar(path: Dictionary) -> void:
    var times: PackedFloat64Array = path["t"]
    var raw: PackedFloat64Array = path["raw"]
    var sense := float(path["sense"])
    var count := times.size()
    var radius := PackedFloat64Array()
    var angle := PackedFloat64Array()
    radius.resize(count)
    angle.resize(count)
    for i in range(count):
        radius[i] = sqrt(raw[3 * i] * raw[3 * i] + raw[3 * i + 2] * raw[3 * i + 2])
        var a := atan2(raw[3 * i + 2], raw[3 * i])
        if i == 0:
            angle[i] = a
            continue
        var turn := wrapf(a - atan2(raw[3 * i - 1], raw[3 * i - 3]), -PI, PI)
        if turn * sense < 0.0 and absf(turn) > 0.5 * PI:
            turn += TAU * sense
        angle[i] = angle[i - 1] + turn
    var first := clampi(times.bsearch(float(path["departure"]), true), 0, count - 1)
    var last := clampi(times.bsearch(float(path["arrival"]), false) - 1, first, count - 1)
    path["r"] = radius
    path["a"] = angle
    path["dr"] = _sample_rates(times, radius, first, last)
    path["da"] = _sample_rates(times, angle, first, last)

# Rate of y at each sample: the time-weighted mean of the slopes on either side, one-sided
# at the ends of [first, last].
func _sample_rates(times: PackedFloat64Array, y: PackedFloat64Array, first: int, last: int) -> PackedFloat64Array:
    var rates := PackedFloat64Array()
    rates.resize(times.size())
    for i in range(times.size()):
        var lo := clampi(i - 1, first, last)
        var hi := clampi(i + 1, first, last)
        if i <= lo or i >= hi:
            lo = mini(lo, i)
            hi = maxi(hi, i)
            rates[i] = (y[hi] - y[lo]) / (times[hi] - times[lo]) if hi > lo and times[hi] > times[lo] else 0.0
            continue
        var h0 := times[i] - times[lo]
        var h1 := times[hi] - times[i]
        if h0 <= 0.0 or h1 <= 0.0:
            rates[i] = (y[hi] - y[lo]) / maxf(times[hi] - times[lo], 1.0)
            continue
        rates[i] = (h1 * (y[i] - y[lo]) / h0 + h0 * (y[hi] - y[i]) / h1) / (h0 + h1)
    return rates

# Rates of the rail offsets at each sample (x, y, z interleaved), for the cubic drawing in
# _rail_point_m. Neighbours are taken within one stretch only (the wait before departure,
# the flight, the time after arrival), so the burns at the ends keep their corners.
func _offset_rates(times: PackedFloat64Array, offsets: PackedFloat64Array, departure: float, arrival: float) -> PackedFloat64Array:
    var count := times.size()
    var first := clampi(times.bsearch(departure, true), 0, count - 1)
    var last := clampi(times.bsearch(arrival, false) - 1, first, count - 1)
    var stretches := [[0, first], [first, last], [last, count - 1]]
    var rates := PackedFloat64Array()
    rates.resize(offsets.size())
    var y := PackedFloat64Array()
    y.resize(count)
    for axis in range(3):
        for i in range(count):
            y[i] = offsets[3 * i + axis]
        for stretch in stretches:
            var lo: int = stretch[0]
            var hi: int = stretch[1]
            if hi <= lo:
                continue
            var axis_rates := _sample_rates(times, y, lo, hi)
            for i in range(lo, hi + 1):
                rates[3 * i + axis] = axis_rates[i]
    return rates

# A class's engine jet power from the designs file (0 until it has been read).
func _jet_power_w(class_id: String) -> float:
    if jet_power_by_class.has(class_id):
        return jet_power_by_class[class_id]
    var design: Dictionary = bridge_designs.get(class_id, {})
    if design.is_empty():
        return 0.0
    var power := 0.0
    for component in design.get("components", []):
        if String(component.get("kind", "")) == "engine":
            power = float(component.get("power_w", 0.0))
    jet_power_by_class[class_id] = power
    return power

# The planet a body belongs to (itself for a planet, its parent for a moon); "" for the Sun.
func _planet_of(body_id: String) -> String:
    var body := body_id
    for _depth in range(4):
        var parent := String((body_orbits.get(body, {}) as Dictionary).get("parent", ""))
        if parent == "":
            return ""
        if String((body_orbits.get(parent, {}) as Dictionary).get("parent", "")) == "":
            return body
        body = parent
    return body

# +1 when a heliocentric path runs anticlockwise (increasing angle), -1 otherwise.
func _path_sense(times: PackedFloat64Array, raw: PackedFloat64Array, departure: float, arrival: float) -> float:
    var total := 0.0
    for i in range(times.size() - 1):
        if times[i + 1] <= departure or times[i] >= arrival:
            continue
        var a0 := atan2(raw[3 * i + 2], raw[3 * i])
        var a1 := atan2(raw[3 * i + 5], raw[3 * i + 3])
        total += wrapf(a1 - a0, -PI, PI)
    return 1.0 if total >= 0.0 else -1.0

# Ships ride their planets' rails. A planned path is sampled every few days (the wait for
# a launch window is a handful of points along the planet's orbit), so straight lines
# between samples cut across the planet's curved track. Each sample is kept as an offset
# from an anchor that moves with the origin planet and, for a hop within one planet's
# system, hands over smoothly to the destination during the flight; the offsets are
# interpolated and the anchor's exact position is added back. At the sample times this is
# the planned path itself. An interplanetary flight is drawn from its heliocentric samples
# instead (see _helio_point_m): blending a planet into the anchor over a years-long flight,
# with samples weeks apart on flat spirals, drew the planet's yearly circle into the path
# as a sawtooth wobble.
func _rail_anchor_m(path: Dictionary, t: float) -> PackedFloat64Array:
    var origin := String(path.get("origin", ""))
    var destination := String(path.get("destination", ""))
    if origin == "" or destination == "":
        return PackedFloat64Array([0.0, 0.0, 0.0])
    var departure := float(path["departure"])
    var arrival := float(path["arrival"])
    if bool(path.get("helio", false)):
        return _body_at_m(origin if t < 0.5 * (departure + arrival) else destination, t)
    var s := clampf((t - departure) / maxf(arrival - departure, 1.0), 0.0, 1.0)
    var w := s * s * (3.0 - 2.0 * s)
    if w <= 0.0:
        return _body_at_m(origin, t)
    if w >= 1.0:
        return _body_at_m(destination, t)
    var a := _body_at_m(origin, t)
    var b := _body_at_m(destination, t)
    return PackedFloat64Array([a[0] + (b[0] - a[0]) * w, a[1] + (b[1] - a[1]) * w, a[2] + (b[2] - a[2]) * w])

# Position on a cached path at time t, on the rail.
func _rail_point_m(path: Dictionary, t: float) -> PackedFloat64Array:
    var times: PackedFloat64Array = path["t"]
    var offsets: PackedFloat64Array = path["off"]
    var i := clampi(times.bsearch(t, true) - 1, 0, times.size() - 2)
    var span := times[i + 1] - times[i]
    var f := clampf((t - times[i]) / span, 0.0, 1.0) if span > 0.0 else 1.0
    if bool(path.get("helio", false)) and t > float(path["departure"]) and t < float(path["arrival"]):
        return _helio_point_m(path, i, f)
    var anchor := _rail_anchor_m(path, t)
    var rates: PackedFloat64Array = path["doff"]
    var out := PackedFloat64Array([0.0, 0.0, 0.0])
    # Cubic (Hermite) between the samples, so the ship's motion has no corner at each one.
    var f2 := f * f
    var f3 := f2 * f
    var h00 := 2.0 * f3 - 3.0 * f2 + 1.0
    var h10 := (f3 - 2.0 * f2 + f) * span
    var h01 := 3.0 * f2 - 2.0 * f3
    var h11 := (f3 - f2) * span
    for axis in range(3):
        var j := 3 * i + axis
        out[axis] = anchor[axis] + h00 * offsets[j] + h10 * rates[j] + h01 * offsets[j + 3] + h11 * rates[j + 3]
    return out

# Between two heliocentric samples: radius and angle (the short way, unless that runs
# against the flight's sense of motion) are interpolated, so spirals and conic arcs stay
# smooth however far apart the samples are. The interpolation is cubic (Hermite, with the
# rates at the samples): a linear one bowed the long, nearly radial stretches of
# outer-system flights by up to 0.03 AU, with a corner at every sample.
func _helio_point_m(path: Dictionary, i: int, f: float) -> PackedFloat64Array:
    var raw: PackedFloat64Array = path["raw"]
    var times: PackedFloat64Array = path["t"]
    var span := times[i + 1] - times[i]
    var r := _hermite(path["r"], path["dr"], i, f, span)
    var a := _hermite(path["a"], path["da"], i, f, span)
    var y := raw[3 * i + 1] + (raw[3 * i + 4] - raw[3 * i + 1]) * f
    return PackedFloat64Array([cos(a) * r, y, sin(a) * r])

func _hermite(y: PackedFloat64Array, rate: PackedFloat64Array, i: int, f: float, span: float) -> float:
    var f2 := f * f
    var f3 := f2 * f
    return (2.0 * f3 - 3.0 * f2 + 1.0) * y[i] + (f3 - 2.0 * f2 + f) * span * rate[i] \
        + (3.0 * f2 - 2.0 * f3) * y[i + 1] + (f3 - f2) * span * rate[i + 1]

# A body's position at any time t (the per-frame _body_m caches the display time only).
func _body_at_m(body_id: String, t: float) -> PackedFloat64Array:
    var orbit: Dictionary = body_orbits.get(body_id, {})
    var parent_id := String(orbit.get("parent", ""))
    if parent_id == "":
        return PackedFloat64Array([0.0, 0.0, 0.0])
    var base := _body_at_m(parent_id, t)
    var period := float(orbit["period"])
    if period <= 0.0:
        return base
    var angle := float(orbit["phase"]) + (t / period) * TAU
    var a := float(orbit["a"])
    return PackedFloat64Array([base[0] + cos(angle) * a, base[1], base[2] + sin(angle) * a])

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

# Position [x, y, z] and direction of motion [dx, dy, dz] on a ship's path at time t.
# At the display time it is kept for the frame: position, heading and focus all ask for it.
func _path_sample(ship_id: String, t: float) -> PackedFloat64Array:
    var key := "@" + ship_id
    if t == display_time_s and frame_positions_m.has(key):
        return frame_positions_m[key]
    var path: Dictionary = ship_paths[ship_id]
    var here := _rail_point_m(path, t)
    var ahead := _rail_point_m(path, t + 600.0)
    var sample := PackedFloat64Array([here[0], here[1], here[2], ahead[0] - here[0], ahead[1] - here[1], ahead[2] - here[2]])
    if t == display_time_s:
        frame_positions_m[key] = sample
    return sample

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

# A docked ship sits at one of its station's berths (StationModels: the ends of the
# berthing arms), bow to the hub; ships beyond the berth count queue further out. A ship
# keeps its slot while docked; an arriving ship takes the lowest free one (_docked_slots).
func _place_docked_ship(node: Node3D, ship_id: String) -> void:
    var detail: Dictionary = entity_details.get(ship_id, {})
    var station: Node3D = entity_nodes.get(String(detail.get("current_station_id", "")))
    var pose := _berth_pose(station, int(_docked_slots.get(ship_id, 0)), float(node.get_meta("length_m", 50.0)))
    if pose.is_empty():
        return
    node.position = pose["position"]
    node.basis = (pose["basis"] as Basis).scaled(Vector3.ONE * ship_metres_to_world)

# Where a ship of this length sits at a berth slot: {position, basis (unscaled)}, or {}.
func _berth_pose(station: Node3D, slot: int, length_m: float) -> Dictionary:
    if station == null or not station.has_meta("berths"):
        return {}
    var berths: Array = station.get_meta("berths")
    if berths.is_empty():
        return {}
    var berth: Dictionary = berths[slot % berths.size()]
    var layer := slot / berths.size()
    var dir := (station.basis * (berth["dir"] as Vector3)).normalized()
    var axis := (station.basis * Vector3(0.0, 0.0, 1.0)).normalized()
    # Queued ships stand off a ship length apart (the longest is ~140 m).
    var out_m := length_m * 0.5 + 3.0 + float(layer) * 160.0
    return {"position": station.position + station.basis * (berth["pos"] as Vector3) + dir * out_m * ship_metres_to_world,
        "basis": Basis.looking_at(-dir, axis)}

# Near a station a ship in flight eases between its path (which ends at the station's
# centre) and its berth: the one it left, or the lowest free one where it is going. The
# blend follows distance (full within 1.3 station radii, none beyond 4), so the ship
# docks without a jump and passes over the habitat ring rather than through it.
func _ease_into_berth(node: Node3D, ship_id: String) -> void:
    var detail: Dictionary = entity_details.get(ship_id, {})
    var best: Node3D = null
    var best_id := ""
    var best_d := INF
    for key in ["origin_station_id", "destination_station_id"]:
        var station_id := String(detail.get(key, ""))
        var station: Node3D = entity_nodes.get(station_id)
        if station == null:
            continue
        var d := node.position.distance_to(station.position)
        if d < best_d:
            best = station
            best_id = station_id
            best_d = d
    if best == null:
        return
    var radius := float(best.get_meta("radius_m", 1000.0)) * best.scale.x
    var w := 1.0 - smoothstep(1.3 * radius, 4.0 * radius, best_d)
    if w <= 0.0:
        return
    var slot := 0
    var last: Array = _last_berth.get(ship_id, [])
    if not last.is_empty() and String(last[0]) == best_id and best_id == String(detail.get("origin_station_id", "")):
        slot = int(last[1])
    else:
        slot = _free_slot(best_id)
    var pose := _berth_pose(best, slot, float(node.get_meta("length_m", 50.0)))
    if pose.is_empty():
        return
    node.position += ((pose["position"] as Vector3) - best.position) * w
    var turn := node.basis.get_rotation_quaternion().slerp((pose["basis"] as Basis).get_rotation_quaternion(), w)
    node.basis = Basis(turn).scaled(Vector3.ONE * ship_metres_to_world)

var _docked_slots: Dictionary = {}    # ship id -> berth slot, while docked
var _docked_station: Dictionary = {}  # ship id -> station id, while docked
var _last_berth: Dictionary = {}      # ship id -> [station id, slot] of its last stay

func _rebuild_docked_slots() -> void:
    var by_station := {}
    for ship in bridge_state.get("ships", []):
        if String(ship.get("phase", "idle")) == "in_transit":
            continue
        var station_id := String(ship.get("current_station_id", ""))
        if not by_station.has(station_id):
            by_station[station_id] = []
        by_station[station_id].append(String(ship["id"]))
    var kept := {}
    for station_id in by_station.keys():
        var ids: Array = by_station[station_id]
        ids.sort()
        for ship_id in ids:
            if String(_docked_station.get(ship_id, "")) == station_id:
                kept[ship_id] = int(_docked_slots[ship_id])
    var slots := {}
    var stations := {}
    for station_id in by_station.keys():
        var used := {}
        for ship_id in by_station[station_id]:
            if kept.has(ship_id):
                used[kept[ship_id]] = true
        var next := 0
        for ship_id in by_station[station_id]:
            var slot: int
            if kept.has(ship_id):
                slot = kept[ship_id]
            else:
                while used.has(next):
                    next += 1
                slot = next
                used[slot] = true
            slots[ship_id] = slot
            stations[ship_id] = station_id
            _last_berth[ship_id] = [station_id, slot]
    _docked_slots = slots
    _docked_station = stations

func _free_slot(station_id: String) -> int:
    var used := {}
    for ship_id in _docked_station.keys():
        if String(_docked_station[ship_id]) == station_id:
            used[int(_docked_slots[ship_id])] = true
    var slot := 0
    while used.has(slot):
        slot += 1
    return slot

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
    var viewport_height: float = maxf(float(get_viewport().get_visible_rect().size.y), 1.0)
    var focal: float = viewport_height / (2.0 * tan(deg_to_rad(camera.fov) * 0.5))
    for body_id in body_orbits.keys():
        space_env.set_ring_visible(String(body_id), not active.has(body_id))
        var orbit: Dictionary = body_orbits[body_id]
        var parent_id := String(orbit["parent"])
        if parent_id == "" or parent_id == "sun" or not body_positions.has(parent_id):
            continue
        # Moons: fade the ring between 6 and 30 pixels of radius on screen.
        var parent_screen: Vector3 = body_positions[parent_id] - render_origin
        var distance := maxf(camera.global_position.distance_to(parent_screen), 1.0e-9)
        var radius_px := float(orbit["a"]) * POSITION_SCALE / distance * focal
        space_env.set_ring_fade(String(body_id), clampf((radius_px - 6.0) / 24.0, 0.0, 1.0))

# --- Planned paths near the focus ---------------------------------------------

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
varying vec3 world_pos;
void vertex() {
    world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}
void fragment() {
    if (hide_radius > 0.0 && distance(world_pos, focus) < hide_radius) {
        discard;
    }
    ALBEDO = COLOR.rgb;
    ALPHA = COLOR.a;
}
"""

func _trail_material() -> ShaderMaterial:
    if trail_material == null:
        var shader := Shader.new()
        shader.code = TRAIL_SHADER
        trail_material = ShaderMaterial.new()
        trail_material.shader = shader
    return trail_material

func _update_fine_paths() -> void:
    var camera_distance_m := float(camera_rig.get("distance")) / POSITION_SCALE
    var active := {}
    var hide_radius_m := 0.0
    var pivot: Vector3 = camera_rig.get("pivot")
    if camera_distance_m <= FINE_PATH_MAX_CAMERA_M:
        var focus_m := PackedFloat64Array([origin_m[0] + pivot.x / POSITION_SCALE,
            origin_m[1] + pivot.y / POSITION_SCALE, origin_m[2] + pivot.z / POSITION_SCALE])
        # Far enough out that the coarse line's float error is under a pixel or so.
        var float_error_m := 1.2e-7 * sqrt(focus_m[0] * focus_m[0] + focus_m[2] * focus_m[2]) + 2.0e4
        hide_radius_m = maxf(FINE_PATH_REACH * camera_distance_m, 300.0 * float_error_m)
        var draw_radius_m := 1.35 * hide_radius_m
        var candidates: Array = []
        for ship_id in trail_points.keys():
            if not ship_paths.has(ship_id) or not trail_nodes.has(ship_id) or not (trail_nodes[ship_id] as Node3D).visible:
                continue
            var gap := INF
            for box in trail_points[ship_id]["boxes"]:
                gap = minf(gap, _box_gap(box, focus_m))
            if gap <= draw_radius_m:
                candidates.append([gap, ship_id])
        candidates.sort_custom(func(x, y): return x[0] < y[0])
        for k in range(mini(candidates.size(), FINE_PATH_MAX)):
            var ship_id: String = candidates[k][1]
            if not _fine_path_current(ship_id, focus_m, camera_distance_m, hide_radius_m):
                _build_fine_path(ship_id, focus_m, camera_distance_m, draw_radius_m)
            var entry: Dictionary = fine_paths[ship_id]
            var node: MeshInstance3D = entry["node"]
            if node == null or not bool(entry["drawn"]):
                continue  # the path passes near the focus's box only, not the focus
            node.position = _to_render(entry["focus"])
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
    if moved + hide_radius_m > float(entry["radius"]) * 0.95:
        return false
    # The flown part is grey: rebuild as the ship moves along.
    return absf(display_time_s - float(entry["time"])) * float(entry["speed"]) < 0.05 * hide_radius_m

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
    var path: Dictionary = ship_paths[ship_id]
    var line_color: Color = style["color"]
    var alpha: float = style["alpha"]
    var flown := Color(0.5, 0.52, 0.55, alpha * 0.45)
    var ahead := Color(line_color.r, line_color.g, line_color.b, alpha)
    var runs: Array = []  # each a list of [x, y, z, t] relative to focus_m
    var run: Array = []
    var budget := FINE_PATH_MAX_POINTS
    var top_speed := 0.0
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
        top_speed = maxf(top_speed, speed)
        # March along the segment with steps a fixed share of the distance to the focus.
        var t := a[3]
        var joined := not run.is_empty()  # the previous segment's end is this one's start
        while budget > 0:
            var p := _rail_point_m(path, t)
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
        "camera": camera_distance_m, "time": display_time_s, "radius": draw_radius_m, "speed": top_speed}
    if runs.is_empty():
        return
    var mesh := ImmediateMesh.new()
    for piece in runs:
        mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
        for q in piece:
            mesh.surface_set_color(flown if q[3] < display_time_s else ahead)
            mesh.surface_add_vertex(Vector3(q[0] * POSITION_SCALE, q[1] * POSITION_SCALE, q[2] * POSITION_SCALE))
        mesh.surface_end()
    if node == null:
        node = MeshInstance3D.new()
        node.name = "%s_fine_path" % ship_id
        var material := StandardMaterial3D.new()
        material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
        material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
        material.vertex_color_use_as_albedo = true
        node.material_override = material
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

func _ship_display_scale(_data: Dictionary) -> float:
    return ship_metres_to_world

# The ship's model for its design (a stand-in until the designs file has it), rebuilt when
# the design changes (a refit, worn radiators).
func _ensure_ship_model(node: Node3D, data: Dictionary) -> void:
    var key := String(data.get("design_id", data.get("class_id", "")))
    var design: Dictionary = bridge_designs.get(key, {})
    if design.is_empty():
        var propulsion := String(data.get("propulsion_type", "nuclear_thermal"))
        key = "fallback:" + propulsion
        design = ShipModelsScript.fallback_design(propulsion)
    if String(node.get_meta("model_key", "")) != key:
        ShipModelsScript.ensure(node, key, design, int(node.get_meta("lod", ShipModelsScript.COARSE)))

# Detailed model close up, coarse otherwise (with hysteresis).
func _update_ship_lod(node: Node3D, data: Dictionary) -> void:
    if not node.has_meta("model_key"):
        return
    var lod := int(node.get_meta("lod", ShipModelsScript.COARSE))
    var pixels := map_icons.projected_model_pixels(node)
    var want := lod
    if lod == ShipModelsScript.COARSE and pixels > SHIP_DETAIL_PX:
        want = ShipModelsScript.DETAIL
    elif lod == ShipModelsScript.DETAIL and pixels < SHIP_COARSE_PX:
        want = ShipModelsScript.COARSE
    if want == lod:
        return
    var key := String(node.get_meta("model_key"))
    var design: Dictionary = bridge_designs.get(key, {})
    if design.is_empty():
        design = ShipModelsScript.fallback_design(String(data.get("propulsion_type", "nuclear_thermal")))
    # The detailed mesh is built in the background; the coarse one stays until it's ready.
    if ShipModelsScript.request(key, design, want):
        ShipModelsScript.ensure(node, key, design, want)

# A station's detailed model close up (built in the background), coarse otherwise.
func _update_station_lod(node: Node3D, data: Dictionary) -> void:
    if data.is_empty() or not node.has_meta("model_key"):
        return
    var lod := int(node.get_meta("lod", StationModelsScript.COARSE))
    var pixels := map_icons.projected_model_pixels(node)
    if lod == StationModelsScript.COARSE and pixels > STATION_DETAIL_PX:
        if StationModelsScript.request_detail(data):
            StationModelsScript.ensure(node, data, StationModelsScript.DETAIL)
    elif lod == StationModelsScript.DETAIL and pixels < STATION_COARSE_PX:
        StationModelsScript.ensure(node, data, StationModelsScript.COARSE)

# A model's size in world units: its bounding radius for ships (built in metres), the
# container scale for unit-size models (bodies, stations).
# Stations are drawn at the ships' scale. A guard for large --model-scale values: a station
# never reaches more than 60% of the way down to its planet (never binds at true scale).
func _station_display_scale(data: Dictionary) -> float:
    var radius_m := float(StationModelsScript.model_info(data)["radius_m"])
    var limit := 0.6 * float(data.get("altitude_m", 1.0e9)) * POSITION_SCALE / maxf(radius_m, 1.0)
    return minf(ship_metres_to_world, limit)

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
    _update_porkchop_request()
    economy_history.record(bridge_state)
    if economy_history.has_unmet_window():
        bridge_state["unmet_30d"] = economy_history.recent_unmet_share(30.0)
    top_bar.update_state(bridge_state, bool(bridge_state.get("paused", current_paused)), current_timewarp)
    entity_browser.update_entities(entity_details, entity_kinds, selected_id)
    event_ticker.update_events(bridge_state.get("recent_events", []))
    market_panel.update_market(bridge_state)

    var detail: Dictionary = entity_details.get(selected_id, {})
    inspector_panel.update_selection(detail, selected_kind, {
        "game_time_s": float(bridge_state.get("game_time_s", 0.0)),
        "faction_colors": faction_colors,
        "names": _entity_name_map(),
        "price_trends": economy_history.price_trends_for(selected_id) if selected_kind == "station" else {},
        "ships": bridge_state.get("ships", []),
        "bodies": bridge_state.get("bodies", []),
        "stations": bridge_state.get("stations", []),
        "designs": bridge_designs,
        "porkchops": porkchop_data,
        "planet": _planet_of(String(detail.get("parent_body_id", ""))) if selected_kind == "station" else "",
    })

func _entity_name_map() -> Dictionary:
    var names := {}
    for entity_id in entity_details.keys():
        names[entity_id] = String((entity_details[entity_id] as Dictionary).get("name", entity_id))
    return names

func _reset_timeline() -> void:
    display_time_s = -1.0
    sim_pace = 0.0
    snapshot_wall_s = 0.0
    ship_paths.clear()
    economy_history.clear()
    ship_trail_history.clear()
    # The trails drawn from the old timeline go with it (after a load, a ship docked in the
    # save kept its old trail on screen).
    for ship_id in history_trail_nodes.keys():
        (history_trail_nodes[ship_id] as Node).queue_free()
    history_trail_nodes.clear()
    trail_path_signatures.clear()

func _pick_entity(mouse_pos: Vector2) -> void:
    var best_id := ""
    var best_distance := INF
    var best_kind := ""
    for entity_id in entity_nodes.keys():
        var node: Node3D = entity_nodes[entity_id]
        if camera.is_position_behind(node.global_position):
            continue
        var screen_pos := camera.unproject_position(node.global_position)
        var kind: String = entity_kinds.get(entity_id, "")
        var detail: Dictionary = entity_details.get(entity_id, {})
        var pick_radius: float = max(MapIconsScript.icon_pixel_size(kind, detail) + ICON_PICK_PADDING_PX, 20.0)
        var distance := screen_pos.distance_to(mouse_pos)
        # Near the icon: the nearest wins. Up close a ship or station is also hit anywhere on
        # its model (its projected radius); then the smallest model wins, so a docked ship
        # beats the station around it.
        var score := INF
        if distance < minf(pick_radius, 28.0):
            score = distance
        elif kind == "ship" or kind == "station":
            var model_px := map_icons.projected_model_pixels(node)
            if distance < model_px:
                score = 1000.0 + model_px
        if score < best_distance:
            best_distance = score
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
    # Nearly even sunlight from Mercury to Neptune (~562 units): a map, not a photograph.
    # Strong falloff overexposed the inner planets and left the outer ones dim.
    sun_light.light_energy = 1.9
    sun_light.omni_range = 1500.0
    sun_light.omni_attenuation = 0.05
    sun_light.shadow_enabled = false
    sun_light.light_color = Color(1.0, 0.96, 0.82)
    world_root.add_child(sun_light)
    # A weak light from the camera for ships and stations only (render layer 2, see
    # ModelMaterials.MODEL_LAYERS): their night sides stay readable without glowing, and
    # planets keep their dark sides.
    var fill := DirectionalLight3D.new()
    fill.name = "ModelFillLight"
    fill.light_cull_mask = ModelMaterialsScript.FILL_LAYER
    fill.light_energy = ModelMaterialsScript.FILL_ENERGY
    fill.light_color = Color(0.85, 0.9, 1.0)
    fill.shadow_enabled = false
    camera.add_child(fill)
    RenderingServer.set_default_clear_color(Color(0.01, 0.012, 0.025, 1.0))

    var env := Environment.new()
    env.background_mode = Environment.BG_COLOR
    env.background_color = Color(0.01, 0.012, 0.025, 1.0)
    env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
    env.ambient_light_color = Color(0.16, 0.17, 0.22, 1.0)
    # Night sides dark but not black.
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
        var icon: Sprite2D = map_icons.icon(body_id)
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
            map_icons.projected_model_pixels(node),
            icon_pixels,
            MapIconsScript.icon_pixel_size("body", detail),
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
    # Hides itself near the focus when a fine line is drawn there (_update_fine_paths).
    path_mesh.material_override = _trail_material()
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
        if trajectory_path.size() < 2 or not ship_paths.has(ship_id):
            continue
        active[ship_id] = true
        var trail_node := _ensure_trail_node(ship_id)
        var is_selected: bool = ship_id == selected_id
        var departure_s := float(ship.get("departure_time_s", 0.0))
        var arrival_s := float(ship.get("arrival_time_s", 0.0))
        # The flown part dims as the ship moves: 50 steps for the selected ship, 8 for the rest
        # (each step rebuilds the line). The bridge's path id names the plan.
        var steps := 50.0 if is_selected else 8.0
        var progress_bucket := int(clamp((game_time_s - departure_s) / max(arrival_s - departure_s, 1.0), 0.0, 1.0) * steps)
        var plan_id := String(ship.get("path_id", ""))
        if plan_id == "":
            plan_id = _trajectory_path_signature(trajectory_path)
        var signature := "%s|%d|%s" % [plan_id, progress_bucket, is_selected]
        if trail_path_signatures.get(ship_id, "") != signature:
            _rebuild_trail_mesh(trail_node, ship_id, ship, is_selected, game_time_s)
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

func _rebuild_trail_mesh(trail_node: Node3D, ship_id: String, ship: Dictionary, is_selected: bool, game_time_s: float) -> void:
    var line_color := _trajectory_color(String(ship.get("trajectory_type", "")))
    var base_alpha := 0.95 if is_selected else 0.2
    var elapsed_color := Color(0.5, 0.52, 0.55, base_alpha * 0.45)
    var path: Dictionary = ship_paths[ship_id]
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
    trail_points[ship_id] = {"points": points, "boxes": boxes, "color": line_color, "alpha": base_alpha,
        "sig": "%s|%s|%.3f" % [String(path["sig"]), is_selected, base_alpha]}
    var mesh := ImmediateMesh.new()
    mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
    for point in points:
        var color := elapsed_color if point[3] < game_time_s else Color(line_color.r, line_color.g, line_color.b, base_alpha)
        mesh.surface_set_color(color)
        mesh.surface_add_vertex(_abs_render(point))
    mesh.surface_end()
    var path_mesh: MeshInstance3D = trail_node.get_node("path")
    path_mesh.mesh = mesh
    _update_burn_markers(trail_node, points.front(), points.back(), ship, is_selected, line_color)

# The drawn line follows the rail from departure to arrival (the wait for the launch
# window lies on the planet's own orbit ring and is not drawn). Segments are subdivided,
# finely near the planets where the camera may be close.
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
            var p := _rail_point_m(path, t)
            points.append(PackedFloat64Array([p[0], p[1], p[2], t]))
    var last := _rail_point_m(path, arrival)
    points.append(PackedFloat64Array([last[0], last[1], last[2], arrival]))
    return points

# Impulsive-burn planners get small emissive markers at the departure and
# arrival burns; continuous-thrust (variable ISP) paths have no discrete burns.
func _update_burn_markers(trail_node: Node3D, first: PackedFloat64Array, last: PackedFloat64Array, ship: Dictionary, is_selected: bool, line_color: Color) -> void:
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
        marker.position = _abs_render(first if marker_index == 0 else last)
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

func _update_selected_overlay_positions() -> void:
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
    # Zoom stops outside the focused body (bodies are drawn at true size now); a ship or
    # station can be zoomed until it fills most of the view.
    var min_distance := 1.0e-12
    if entity_kind == "body":
        min_distance = maxf(min_distance, _body_display_scale(entity_id) * 1.6)
    elif entity_nodes.has(entity_id):
        var node: Node3D = entity_nodes[entity_id]
        if entity_kind == "station" and node.has_meta("core_radius_m"):
            # Inside the habitat ring, close to the docking core.
            min_distance = maxf(min_distance, float(node.get_meta("core_radius_m")) * node.scale.x * 1.6)
        else:
            min_distance = maxf(min_distance, MapIconsScript.model_world_size(node) * (1.6 if entity_kind == "ship" else 2.5))
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
