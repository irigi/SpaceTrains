extends Node3D

const POSITION_SCALE := PathMotionScript.POSITION_SCALE
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
# The same for stations (their radius includes the habitat). Every station's detailed
# model is built in the background after start-up (StationModels.prefetch), so it is ready
# by the time it is wanted; the switch is early, while the details are still small.
const STATION_DETAIL_PX := 30.0
const STATION_COARSE_PX := 20.0
# The simulation advances in ticks of 0.1 day (Simulation::TICK_S). Between snapshots the
# display clock runs on at the timewarp rate, at most this far ahead of the last snapshot.
const DEFAULT_TIMEWARP := 86400.0
const TIMEWARP_STEPS := [1.0, 600.0, 21600.0, 86400.0, 432000.0]

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
# The display clock (DisplayClock.gd): the game time drawn this frame, the measured pace, the
# station spin clock and the visual phases of spins and station orbits.
var clock := DisplayClockScript.new()
@onready var scene_light: DirectionalLight3D = $DirectionalLight3D

# The simulation bridge's client (BridgeClient.gd): the process, the command file, the files it writes.
var bridge: BridgeClientScript
# Where things are drawn (PathMotion.gd): orbits and planned paths, positions at the display
# clock's time, relative to the focus.
var path_motion := PathMotionScript.new()
# Docked ships' berth slots and poses (Berths.gd).
var berths := BerthsScript.new()
# Orbit rings near the focus, planned and flown paths, burn markers, the destination ghost (FineLines.gd).
var fine_lines := FineLinesScript.new()
var entity_root: Node3D
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
var current_paused := false
# Every station's detailed model has been built (StationModels.prefetch); when that was
# (ms since start-up) and the longest it held up a frame (the shot tour reports them).
var _stations_prefetched := false
var stations_prefetched_ms := -1
var prefetch_worst_ms := 0.0
var current_timewarp := DEFAULT_TIMEWARP
var has_auto_focused := false
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
const BodySpinScript := preload("res://scripts/BodySpin.gd")
const ModelMaterialsScript := preload("res://scripts/ModelMaterials.gd")
const EclipsesScript := preload("res://scripts/Eclipses.gd")
const TopBarPanel := preload("res://scripts/ui/TopBar.gd")
const EntityBrowserPanel := preload("res://scripts/ui/EntityBrowser.gd")
const InspectorPanelScript := preload("res://scripts/ui/InspectorPanel.gd")
const EventTickerPanel := preload("res://scripts/ui/EventTicker.gd")
const MarketPanelScript := preload("res://scripts/ui/MarketPanel.gd")
const HelpPanelScript := preload("res://scripts/ui/HelpPanel.gd")
const StatisticsPanelScript := preload("res://scripts/ui/StatisticsPanel.gd")
const MapIconsScript := preload("res://scripts/MapIcons.gd")
const EconomyHistoryScript := preload("res://scripts/EconomyHistory.gd")
const DisplayClockScript := preload("res://scripts/DisplayClock.gd")
const BridgeClientScript := preload("res://scripts/BridgeClient.gd")
const PathMotionScript := preload("res://scripts/PathMotion.gd")
const BerthsScript := preload("res://scripts/Berths.gd")
const FineLinesScript := preload("res://scripts/FineLines.gd")

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
# Porkchop plots: the bridge's latest file for the pairs asked (_update_porkchop_request).
var porkchop_data: Dictionary = {}
var flow_label: Label
# Price history samples: {day: float, prices: {station_id: {commodity_id: price}}}

func _ready() -> void:
    path_motion.setup(clock, entity_details, entity_kinds)
    bridge = BridgeClientScript.new()
    bridge.name = "BridgeClient"
    bridge.clock = clock
    bridge.stats_wanted = func() -> bool: return (stats_panel != null and stats_panel.visible) or flow_overlay
    bridge.stats_readable = func() -> bool: return stats_panel != null and (stats_panel.visible or flow_overlay)
    if _profile_enabled:
        bridge.profiled = _profiled
    bridge.snapshot_received.connect(_on_snapshot_received)
    bridge.snapshot_rejected.connect(_on_snapshot_rejected)
    bridge.designs_received.connect(_on_designs_received)
    bridge.stats_received.connect(_on_stats_received)
    bridge.porkchops_received.connect(_on_porkchops_received)
    bridge.status_changed.connect(_set_status)
    bridge.epoch_changed.connect(_reset_timeline)
    add_child(bridge)
    # Debug aid: SPACETRAINS_TIMEWARP=432000 starts at that speed (profiling without a keyboard).
    if OS.has_environment("SPACETRAINS_TIMEWARP"):
        current_timewarp = float(OS.get_environment("SPACETRAINS_TIMEWARP"))
    bridge.paused = current_paused
    bridge.timewarp = current_timewarp
    bridge.start()
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
    fine_lines.name = "FineLines"
    add_child(fine_lines)
    fine_lines.setup(self, path_motion, clock)
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
    berths.setup(entity_nodes, entity_details, ship_metres_to_world)
    _start_music()
    if bridge.bridge_started:
        _set_status("Starting bridge…\n%s" % bridge.executable_path)
    else:
        # Emitted by bridge.start() before the status label existed.
        _set_status(bridge.start_error)

# Background music (Music.gd), except in shot tours, headless runs and with --no-music.
func _start_music() -> void:
    if _print_status or DisplayServer.get_name() == "headless" or "--no-music" in OS.get_cmdline_user_args():
        return
    var music: Node = load("res://scripts/Music.gd").new()
    music.name = "Music"
    music.setup(ui_root)
    add_child(music)

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
    stats_panel.visibility_toggled.connect(func(_open: bool) -> void: bridge.write_commands())
    stats_panel.route_changed.connect(_update_porkchop_request)
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
    bridge.set_paused(current_paused)

func _on_timewarp_changed(factor: float) -> void:
    current_timewarp = factor
    bridge.set_timewarp(current_timewarp)

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

# Debug aid: SPACETRAINS_GODOT_PROFILE=1 prints where frame time goes, every 10 s.
var _profile_enabled := OS.has_environment("SPACETRAINS_GODOT_PROFILE")
var _profile_totals: Dictionary = {}
var _profile_worst: Dictionary = {}
var _profile_since_s := 0.0

func _profiled(name: String, started_usec: int) -> void:
    var ms := float(Time.get_ticks_usec() - started_usec) / 1000.0
    _profile_totals[name] = float(_profile_totals.get(name, 0.0)) + ms
    _profile_worst[name] = maxf(float(_profile_worst.get(name, 0.0)), ms)

# The game is quitting (get_tree().quit() or the window closed): models still being built on
# worker threads must finish first, or they call into scripts being freed and Godot crashes.
func _exit_tree() -> void:
    ShipModelsScript.wait_for_builds()
    StationModelsScript.wait_for_builds()

func _process(delta: float) -> void:
    debug_frame += 1
    var t := Time.get_ticks_usec()
    bridge.poll()
    if _profile_enabled: _profiled("read+apply snapshot", t)
    bridge.retry_pending_write()
    bridge.update_status()
    clock.advance(delta, bool(bridge_state.get("paused", current_paused)),
        float(bridge_state.get("timewarp_factor", current_timewarp)))
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
            bridge.set_paused(current_paused)
        elif event.keycode == KEY_1:
            current_timewarp = 1.0
            bridge.set_timewarp(current_timewarp)
        elif event.keycode == KEY_2:
            current_timewarp = 21600.0
            bridge.set_timewarp(current_timewarp)
        elif event.keycode == KEY_3:
            current_timewarp = 86400.0
            bridge.set_timewarp(current_timewarp)
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
            bridge.write_commands()
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

# The quick save's name in user://saves: shot tours use their own (ShotTour.gd), so they never
# overwrite the player's quicksave.
var quicksave_name := "quicksave"

func _quicksave_path() -> String:
    DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path("user://saves"))
    return ProjectSettings.globalize_path("user://saves/%s.json" % quicksave_name)

func _request_save_or_load(save: bool) -> void:
    if save:
        bridge.request_save(_quicksave_path())
        _write_view(_view_path(_quicksave_path()))
    else:
        bridge.request_load(_quicksave_path())
        _pending_view = _read_view(_view_path(_quicksave_path()))

# The visual angles (station orbits round their planets, planet spins: DisplayClock) are not in
# the simulation, so a save keeps them in a side file next to it, <save>.view.json, and a load
# restores them with the loaded game's first snapshot (_reset_timeline). Older saves have none:
# the angles then stay as they are.
var _pending_view: Dictionary = {}

func _view_path(save_path: String) -> String:
    return save_path.get_basename() + ".view.json"

# Written aside and renamed, as the bridge's files are.
func _write_view(path: String) -> void:
    var file := FileAccess.open(path + ".tmp", FileAccess.WRITE)
    if file == null:
        return
    file.store_string(JSON.stringify(clock.export_view(), "", true, true))
    file.close()
    DirAccess.rename_absolute(path + ".tmp", path)

func _read_view(path: String) -> Dictionary:
    if not FileAccess.file_exists(path):
        return {}
    var json := JSON.new()
    if json.parse(FileAccess.get_file_as_string(path)) != OK or typeof(json.data) != TYPE_DICTIONARY:
        return {}
    return json.data

# Ship designs by class id ({name, ..., components: [...]}): the parts of every class for the
# inspector's ship systems section. Written once by the bridge (per-ship fits later).
var bridge_designs: Dictionary = {}
var jet_power_by_class: Dictionary = {}

# The pairs to ask for: the Routes tab's, and the selected station's planet to every other planet
# with a station (the inspector's launch windows).
func _update_porkchop_request() -> void:
    var pairs: Array[String] = []
    if stats_panel != null and stats_panel.visible and stats_panel.get("_tab") == stats_panel.ROUTES_TAB:
        pairs.append("%s>%s" % [stats_panel.route_origin, stats_panel.route_destination])
    if selected_kind == "station" and entity_details.has(selected_id):
        # The routes flown from here (the bridge expands "*", nearest the Sun first).
        pairs.append("%s>*" % path_motion.planet_of(String(entity_details[selected_id].get("parent_body_id", ""))))
    bridge.request_porkchops(";".join(pairs))

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

# The bridge's signals (BridgeClient.gd), emitted from bridge.poll() in the order its files are
# read: designs, stats, porkchops, then the snapshot.
func _on_designs_received(designs: Dictionary) -> void:
    bridge_designs = designs
    jet_power_by_class.clear()

func _on_stats_received(stats: Dictionary) -> void:
    stats_panel.update_stats(stats)
    _update_flow_overlay()

func _on_porkchops_received(data: Dictionary) -> void:
    porkchop_data = data
    stats_panel.update_porkchops(porkchop_data, bridge_state.get("ships", []))

func _on_snapshot_rejected(seq: int, game_time_s: float, shown_game_time_s: float) -> void:
    if debug_map_enabled:
        print("[MapDebugReject] seq=%d current_seq=%d new_game_day=%.3f current_game_day=%.3f" % [seq, bridge.snapshot_seq, game_time_s / 86400.0, shown_game_time_s / 86400.0])

func _on_snapshot_received(state: Dictionary, _seq: int) -> void:
    bridge_state = state
    clock.on_snapshot(float(state.get("game_time_s", 0.0)), bridge.snapshot_arrival_s)
    _apply_snapshot()

func _apply_snapshot() -> void:
    var seen_ids := {}
    var ids_changed := false
    path_motion.cache_orbits(bridge_state)
    for body in bridge_state.get("bodies", []):
        body_display_radii[String(body["id"])] = _body_scale_from_radius(float(body.get("radius_m", 0.0)))
    for ship in bridge_state.get("ships", []):
        path_motion.cache_ship_path(String(ship["id"]), ship)
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
            fine_lines.forget_ship(entity_id)
            map_icons.remove(entity_id)
            ids_changed = true
            if entity_id == focused_id:
                focused_id = ""
                focused_kind = ""

    if ids_changed and selected_id != "" and not entity_details.has(selected_id):
        selected_id = ""
        selected_kind = ""
    for ship_id in path_motion.ship_paths.keys():
        if not seen_ids.has(ship_id):
            path_motion.forget_ship(ship_id)
    berths.rebuild(bridge_state)
    if _profile_enabled: _profiled("  upsert entities", upsert_started)
    var trails_started := Time.get_ticks_usec()
    fine_lines.update_ship_trails(selected_id, selected_kind)
    if _profile_enabled: _profiled("  ship trails", trails_started)
    if not has_auto_focused:
        _hide_debug_guides()
        _auto_focus_initial_entity()
    if status_label != null and status_label.visible and clock.wall_time_s() > bridge.status_until_s:
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
            # The ring lies in Saturn's equator: the same tilt as the planet's spin axis.
            var ring := SpaceEnvironmentScript.make_planet_ring()
            ring.basis = BodySpinScript.axis_basis(float(BodySpinScript.BODIES["saturn"]["obliquity_deg"]))
            container.add_child(ring)

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
    if clock.display_time_s < 0.0:
        return
    path_motion.begin_frame()
    if focused_id != "" and entity_kinds.has(focused_id):
        path_motion.origin_m = path_motion.entity_m(focused_id)
    else:
        path_motion.origin_m = PackedFloat64Array([0.0, 0.0, 0.0])
    last_render_origin = render_origin
    render_origin = path_motion.abs_render(path_motion.origin_m)
    # world_root holds absolute-frame geometry (orbit rings, planned paths, trails).
    world_root.position = -render_origin
    var wall_dt := clock.advance_spin(bool(bridge_state.get("paused", current_paused)))
    clock.advance_visual_phases(wall_dt, path_motion.body_orbits, path_motion.station_orbits, current_timewarp)
    body_positions.clear()
    station_positions.clear()
    for body_id in path_motion.body_orbits.keys():
        body_positions[body_id] = path_motion.abs_render(path_motion.body_m(body_id))
    for station_id in path_motion.station_orbits.keys():
        station_positions[station_id] = path_motion.abs_render(path_motion.station_m(station_id))
    var docked: Array[String] = []
    var spin_time := clock.spin_clock_s
    var sun_world: Variant = (entity_nodes["sun"] as Node3D).global_position if entity_nodes.has("sun") else null
    var stations: Array = []
    for entity_id in entity_nodes.keys():
        var node: Node3D = entity_nodes[entity_id]
        var kind: String = entity_kinds.get(entity_id, "")
        node.position = path_motion.to_render(path_motion.entity_m(entity_id))
        if kind == "station":
            var station: Dictionary = entity_details.get(entity_id, {})
            if not station.is_empty():
                stations.append(station)
            _update_station_lod(node, station)
            StationModelsScript.spin_rings(node, spin_time, sun_world)
        elif kind == "body":
            var spin_angle := float(clock.visual_phase.get(entity_id, 0.0))
            var parent_id := String((path_motion.body_orbits.get(entity_id, {}) as Dictionary).get("parent", ""))
            if parent_id != "" and parent_id != "sun" and BodySpinScript.is_locked(entity_id):
                var here := path_motion.body_m(entity_id)
                var there := path_motion.body_m(parent_id)
                spin_angle = BodySpinScript.locked_angle(entity_id,
                    Vector3(there[0] - here[0], there[1] - here[1], there[2] - here[2]))
            BodySpinScript.orient(node, entity_id, spin_angle)
        elif kind == "ship":
            var detail: Dictionary = entity_details.get(entity_id, {})
            if not String(detail.get("phase", "idle")) in ["in_transit", "derelict", "patrol"]:
                docked.append(entity_id)
            _orient_ship(node, entity_id, path_motion.ship_heading(entity_id))
    if not _stations_prefetched and not stations.is_empty():
        var t0 := Time.get_ticks_usec()
        _stations_prefetched = StationModelsScript.prefetch(stations)
        prefetch_worst_ms = maxf(prefetch_worst_ms, (Time.get_ticks_usec() - t0) / 1000.0)
        if _stations_prefetched:
            stations_prefetched_ms = Time.get_ticks_msec()
    # Docked ships, and ships near a station, after every station has its position for this frame.
    for entity_id in docked:
        berths.place_docked_ship(entity_nodes[entity_id], entity_id)
    _update_eclipses()
    for entity_id in entity_nodes.keys():
        if entity_kinds.get(entity_id, "") == "ship" \
                and String((entity_details.get(entity_id, {}) as Dictionary).get("phase", "idle")) == "in_transit":
            berths.ease_into_berth(entity_nodes[entity_id], entity_id)
    for entity_id in entity_nodes.keys():
        if entity_kinds.get(entity_id, "") == "ship":
            var node: Node3D = entity_nodes[entity_id]
            var detail: Dictionary = entity_details.get(entity_id, {})
            _update_ship_lod(node, detail)
            EntityVisualsScript.update_engine_glow(node, detail, clock.display_time_s, map_icons.world_size_for_pixels(node, 20.0),
                _jet_power_w(String(detail.get("design_id", detail.get("class_id", "")))))
    _update_camera_obstacles()
    fine_lines.update_history_trails(delta)
    if space_env != null:
        space_env.update_orbit_rings(world_root, bridge_state.get("bodies", []), body_positions, MapIconsScript.BODY_ICON_COLOR)
    fine_lines.update_fine_rings(render_origin)
    fine_lines.update_path_frames(selected_id)
    fine_lines.update_fine_paths()
    fine_lines.update_path_clocks()
    if sun_light != null and entity_nodes.has("sun"):
        var sun_node: Node3D = entity_nodes["sun"]
        sun_light.global_position = sun_node.global_position
        if space_env != null:
            space_env.update_sun(sun_node.global_position, sun_node.scale.x,
                map_icons.world_size_for_pixels(sun_node, 110.0))
    if space_env != null:
        space_env.update_camera(camera.global_position)
    fine_lines.update_selected_overlay_positions(selected_id, selected_kind)

# Ships and stations in a planet's or moon's shadow (Eclipses.gd). A docked ship takes
# its station's light.
func _update_eclipses() -> void:
    if not path_motion.body_orbits.has("sun"):
        return
    var sun_m := path_motion.body_m("sun")
    var sun_radius := float(path_motion.body_orbits["sun"]["radius"])
    var bodies: Array = []
    for body_id in path_motion.body_orbits.keys():
        if body_id != "sun":
            bodies.append([path_motion.body_m(body_id), float(path_motion.body_orbits[body_id]["radius"])])
    var shades := EclipsesScript.occluders(sun_m, sun_radius, bodies)
    var at_station := {}
    for entity_id in entity_nodes.keys():
        var kind := String(entity_kinds.get(entity_id, ""))
        if kind != "station" and kind != "ship":
            continue
        var key := String(entity_id)
        if kind == "ship":
            var detail: Dictionary = entity_details.get(entity_id, {})
            if not String(detail.get("phase", "idle")) in ["in_transit", "derelict", "patrol"]:
                key = String(detail.get("current_station_id", entity_id))
        var level: int
        if at_station.has(key):
            level = at_station[key]
        else:
            level = EclipsesScript.level_of(EclipsesScript.lit_fraction(path_motion.entity_m(entity_id), sun_m, sun_radius, shades))
            at_station[key] = level
        EclipsesScript.apply(entity_nodes[entity_id], level)

# Planets, moons and the Sun keep the camera out (OrbitCamera pushes it to their surface).
func _update_camera_obstacles() -> void:
    var obstacles: Array = []
    for body_id in path_motion.body_orbits.keys():
        var node: Node3D = entity_nodes.get(body_id)
        if node != null and node.scale.x > BODY_MIN_MODEL_SCALE:
            obstacles.append([node.global_position, node.scale.x])
    camera_rig.set("obstacles", obstacles)
    # Station habitats near the camera keep it out too: each spinner's frame and the habitat's shapes.
    var hulls: Array = []
    var pivot: Vector3 = camera_rig.get("pivot")
    var reach := float(camera_rig.get("distance"))
    for entity_id in entity_nodes.keys():
        if entity_kinds.get(entity_id, "") != "station":
            continue
        var node: Node3D = entity_nodes[entity_id]
        var station: Dictionary = entity_details.get(entity_id, {})
        if station.is_empty() or not node.has_meta("radius_m"):
            continue
        if node.global_position.distance_to(pivot) > float(node.get_meta("radius_m")) * node.scale.x * 1.5 + reach:
            continue
        var shapes: Array = StationModelsScript.model_info(station).get("collision", [])
        for child in node.get_children():
            if child.has_meta("spin"):
                hulls.append([(child as Node3D).global_transform, shapes])
    camera_rig.set("hulls", hulls)

# --- Display clock and positions ----------------------------------------------

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

# --- Orbit rings near the focus -----------------------------------------------


# --- Planned paths near the focus ---------------------------------------------


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

func _visual_signature(kind: String, data: Dictionary) -> String:
    if kind == "body":
        return "body:%s" % String(data["id"])
    if kind == "station":
        return "station:%s" % String(data.get("faction_id", ""))
    return "ship:%s" % String(data.get("phase", "idle"))

func _refresh_ui(force := false) -> void:
    if top_bar == null:
        return
    var now_s := clock.wall_time_s()
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
    var inspector_started := Time.get_ticks_usec()
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
        "planet": path_motion.planet_of(String(detail.get("parent_body_id", ""))) if selected_kind == "station" else "",
    })
    if _profile_enabled: _profiled("  inspector", inspector_started)

func _entity_name_map() -> Dictionary:
    var names := {}
    for entity_id in entity_details.keys():
        names[entity_id] = String((entity_details[entity_id] as Dictionary).get("name", entity_id))
    return names

func _reset_timeline() -> void:
    clock.reset()
    clock.import_view(_pending_view)
    _pending_view = {}
    path_motion.reset()
    economy_history.clear()
    fine_lines.reset()

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
    # Planets and moons shade ships and stations (Eclipses.gd).
    EclipsesScript.make_lights(sun_light)
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
        bridge.snapshot_seq,
        clock.display_time_s / 86400.0,
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
    bridge.set_timewarp(current_timewarp)

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
        if entity_kind == "station" and node.has_meta("dock_radius_m"):
            # Close to the docking core: the same zoom at every station, whatever its habitat's size.
            min_distance = maxf(min_distance, float(node.get_meta("dock_radius_m")) * node.scale.x * 1.6)
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
