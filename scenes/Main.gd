extends Node
## Main scene controller. Sets up the simulation, renderer, camera, and UI.
## Handles entity selection via raycasting.

var simulation: Simulation
var renderer: SolarSystemRenderer
var camera: OrbitCamera
var floating_origin: FloatingOrigin

# UI references (set after scene tree is ready)
var selection_panel: SelectionPanel
var event_log: EventLog
var time_controls: TimeControls
var help_overlay: HelpOverlay

func _ready() -> void:
	# Create simulation
	simulation = Simulation.new()
	simulation.name = "Simulation"
	add_child(simulation)

	# Create 3D world container
	var world_3d := Node3D.new()
	world_3d.name = "World3D"
	add_child(world_3d)

	# Create floating origin
	floating_origin = FloatingOrigin.new()
	floating_origin.name = "FloatingOrigin"
	world_3d.add_child(floating_origin)

	# Create camera
	camera = OrbitCamera.new()
	camera.name = "OrbitCamera"
	camera.far = 10000.0
	camera.near = 0.05
	camera.fov = 60.0
	world_3d.add_child(camera)
	camera.make_current()

	floating_origin.set_camera(camera)

	# Create environment
	_setup_environment(world_3d)

	# Create renderer
	renderer = SolarSystemRenderer.new()
	renderer.name = "SolarSystemRenderer"
	floating_origin.add_child(renderer)

	# Wait one frame for simulation to initialize
	await get_tree().process_frame
	renderer.initialize(simulation)

	# Create UI
	_create_ui()

	# Initial camera position - focus on Earth area
	camera.distance = 80.0
	camera.pivot_point = Vector3.ZERO
	camera.pitch = -0.4
	camera.yaw = 0.3

	EventBus.emit_log("system", "SpaceTrains simulation started.")
	EventBus.emit_log("system", "Press H for controls help.")

func _setup_environment(parent: Node3D) -> void:
	# Directional light (sun-like)
	var light := DirectionalLight3D.new()
	light.name = "SunLight"
	light.light_color = Color(1.0, 0.98, 0.9)
	light.light_energy = 1.5
	light.rotation_degrees = Vector3(-30, -45, 0)
	light.shadow_enabled = false
	parent.add_child(light)

	# Ambient light
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color(0.02, 0.02, 0.05)
	env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	env.ambient_light_color = Color(0.1, 0.1, 0.15)
	env.ambient_light_energy = 0.3
	env.tonemap_mode = Environment.TONE_MAPPER_ACES
	env.glow_enabled = true
	env.glow_intensity = 0.3
	env.glow_bloom = 0.1

	var world_env := WorldEnvironment.new()
	world_env.environment = env
	parent.add_child(world_env)

func _create_ui() -> void:
	var ui_layer := CanvasLayer.new()
	ui_layer.name = "UILayer"
	add_child(ui_layer)

	# Main UI container
	var margin := MarginContainer.new()
	margin.set_anchors_preset(Control.PRESET_FULL_RECT)
	margin.mouse_filter = Control.MOUSE_FILTER_IGNORE
	margin.add_theme_constant_override("margin_left", 10)
	margin.add_theme_constant_override("margin_right", 10)
	margin.add_theme_constant_override("margin_top", 10)
	margin.add_theme_constant_override("margin_bottom", 10)
	ui_layer.add_child(margin)

	var hbox := HBoxContainer.new()
	hbox.set_anchors_preset(Control.PRESET_FULL_RECT)
	hbox.mouse_filter = Control.MOUSE_FILTER_IGNORE
	margin.add_child(hbox)

	# --- Left Panel (Selection) ---
	selection_panel = _create_selection_panel()
	selection_panel.simulation = simulation
	hbox.add_child(selection_panel)

	# Spacer
	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	spacer.mouse_filter = Control.MOUSE_FILTER_IGNORE
	hbox.add_child(spacer)

	# --- Right side (vertical) ---
	var right_vbox := VBoxContainer.new()
	right_vbox.custom_minimum_size = Vector2(350, 0)
	right_vbox.mouse_filter = Control.MOUSE_FILTER_IGNORE
	hbox.add_child(right_vbox)

	# Time controls at top-right
	time_controls = _create_time_controls()
	time_controls.simulation = simulation
	right_vbox.add_child(time_controls)

	# Spacer
	var right_spacer := Control.new()
	right_spacer.size_flags_vertical = Control.SIZE_EXPAND_FILL
	right_spacer.mouse_filter = Control.MOUSE_FILTER_IGNORE
	right_vbox.add_child(right_spacer)

	# Event log at bottom-right
	event_log = _create_event_log()
	right_vbox.add_child(event_log)

	# Help overlay (centered, hidden by default)
	help_overlay = _create_help_overlay()
	ui_layer.add_child(help_overlay)

func _create_selection_panel() -> SelectionPanel:
	var panel := SelectionPanel.new()
	panel.custom_minimum_size = Vector2(280, 0)
	panel.size_flags_vertical = Control.SIZE_EXPAND_FILL

	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.08, 0.08, 0.12, 0.85)
	style.border_color = Color(0.3, 0.3, 0.5, 0.6)
	style.set_border_width_all(1)
	style.set_corner_radius_all(4)
	style.set_content_margin_all(10)
	panel.add_theme_stylebox_override("panel", style)

	var vbox := VBoxContainer.new()
	panel.add_child(vbox)

	var title_label := Label.new()
	title_label.unique_name_in_owner = true
	title_label.name = "TitleLabel"
	title_label.text = "Nothing Selected"
	title_label.add_theme_font_size_override("font_size", 20)
	vbox.add_child(title_label)
	panel.title_label = title_label

	var type_label := Label.new()
	type_label.unique_name_in_owner = true
	type_label.name = "TypeLabel"
	type_label.text = ""
	type_label.add_theme_font_size_override("font_size", 14)
	type_label.add_theme_color_override("font_color", Color(0.6, 0.6, 0.8))
	vbox.add_child(type_label)
	panel.type_label = type_label

	var sep := HSeparator.new()
	vbox.add_child(sep)

	var details := RichTextLabel.new()
	details.unique_name_in_owner = true
	details.name = "DetailsText"
	details.bbcode_enabled = true
	details.fit_content = true
	details.scroll_active = true
	details.size_flags_vertical = Control.SIZE_EXPAND_FILL
	vbox.add_child(details)
	panel.details_text = details

	return panel

func _create_event_log() -> EventLog:
	var panel := EventLog.new()
	panel.custom_minimum_size = Vector2(350, 250)

	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.06, 0.06, 0.1, 0.85)
	style.border_color = Color(0.3, 0.3, 0.5, 0.6)
	style.set_border_width_all(1)
	style.set_corner_radius_all(4)
	style.set_content_margin_all(8)
	panel.add_theme_stylebox_override("panel", style)

	var vbox := VBoxContainer.new()
	panel.add_child(vbox)

	var header := Label.new()
	header.text = "Event Log"
	header.add_theme_font_size_override("font_size", 16)
	vbox.add_child(header)

	# Filters
	var filter_box := HBoxContainer.new()
	vbox.add_child(filter_box)

	var filter_trade := CheckBox.new()
	filter_trade.unique_name_in_owner = true
	filter_trade.name = "FilterTrade"
	filter_trade.text = "Trade"
	filter_trade.button_pressed = true
	filter_trade.add_theme_font_size_override("font_size", 11)
	filter_box.add_child(filter_trade)
	panel.filter_trade = filter_trade

	var filter_economy := CheckBox.new()
	filter_economy.unique_name_in_owner = true
	filter_economy.name = "FilterEconomy"
	filter_economy.text = "Economy"
	filter_economy.button_pressed = true
	filter_economy.add_theme_font_size_override("font_size", 11)
	filter_box.add_child(filter_economy)
	panel.filter_economy = filter_economy

	var filter_system := CheckBox.new()
	filter_system.unique_name_in_owner = true
	filter_system.name = "FilterSystem"
	filter_system.text = "System"
	filter_system.button_pressed = true
	filter_system.add_theme_font_size_override("font_size", 11)
	filter_box.add_child(filter_system)
	panel.filter_system = filter_system

	var log_text := RichTextLabel.new()
	log_text.unique_name_in_owner = true
	log_text.name = "LogText"
	log_text.bbcode_enabled = true
	log_text.scroll_following = true
	log_text.size_flags_vertical = Control.SIZE_EXPAND_FILL
	log_text.add_theme_font_size_override("normal_font_size", 12)
	vbox.add_child(log_text)
	panel.log_text = log_text

	# Connect filter signals now that nodes exist
	filter_trade.toggled.connect(func(on): panel.active_filters["trade"] = on; panel._refresh())
	filter_economy.toggled.connect(func(on): panel.active_filters["economy"] = on; panel._refresh())
	filter_system.toggled.connect(func(on): panel.active_filters["system"] = on; panel._refresh())

	return panel

func _create_time_controls() -> TimeControls:
	var hbox := TimeControls.new()
	hbox.custom_minimum_size = Vector2(350, 40)

	var pause_btn := Button.new()
	pause_btn.unique_name_in_owner = true
	pause_btn.name = "PauseButton"
	pause_btn.text = "⏸"
	pause_btn.custom_minimum_size = Vector2(40, 35)
	hbox.add_child(pause_btn)
	hbox.pause_button = pause_btn

	for i in range(5):
		var speed_btn := Button.new()
		var speeds = [0.2, 1, 5, 20, 50]
		speed_btn.text = "%.1f×" % speeds[i] if speeds[i] < 1.0 else "%d×" % int(speeds[i])
		speed_btn.custom_minimum_size = Vector2(50, 35)
		var idx = i
		speed_btn.pressed.connect(func(): hbox._set_speed(idx))
		hbox.add_child(speed_btn)

	var speed_label := Label.new()
	speed_label.unique_name_in_owner = true
	speed_label.name = "SpeedLabel"
	speed_label.text = "0.2×"
	speed_label.add_theme_font_size_override("font_size", 16)
	speed_label.custom_minimum_size = Vector2(60, 0)
	speed_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	hbox.add_child(speed_label)
	hbox.speed_label = speed_label

	var time_label := Label.new()
	time_label.unique_name_in_owner = true
	time_label.name = "TimeLabel"
	time_label.text = "Day 1, 00:00"
	time_label.add_theme_font_size_override("font_size", 14)
	hbox.add_child(time_label)
	hbox.time_label = time_label

	return hbox

func _create_help_overlay() -> HelpOverlay:
	var panel := HelpOverlay.new()
	panel.set_anchors_preset(Control.PRESET_CENTER)
	panel.custom_minimum_size = Vector2(500, 400)
	panel.offset_left = -250
	panel.offset_top = -200
	panel.offset_right = 250
	panel.offset_bottom = 200

	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.05, 0.05, 0.1, 0.95)
	style.border_color = Color(0.4, 0.4, 0.7, 0.8)
	style.set_border_width_all(2)
	style.set_corner_radius_all(8)
	style.set_content_margin_all(20)
	panel.add_theme_stylebox_override("panel", style)

	var vbox := VBoxContainer.new()
	panel.add_child(vbox)

	var title := Label.new()
	title.text = "SpaceTrains — Controls"
	title.add_theme_font_size_override("font_size", 22)
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	vbox.add_child(title)

	var sep := HSeparator.new()
	vbox.add_child(sep)

	var help_text := RichTextLabel.new()
	help_text.bbcode_enabled = true
	help_text.size_flags_vertical = Control.SIZE_EXPAND_FILL
	help_text.text = """[b]Camera Controls[/b]
  Right Mouse + Drag — Rotate camera
  Middle Mouse + Drag — Pan camera
  Scroll Wheel — Zoom in/out
  F — Focus on selected object

[b]Selection[/b]
  Left Click — Select planet/station/ship
  Left Click (empty) — Deselect

[b]Time Controls[/b]
  Space — Pause / Resume
  0 — Speed 0.2× (slow)
  1 — Speed 1× (real-time)
  2 — Speed 5×
  3 — Speed 20×
  4 — Speed 50×

[b]Other[/b]
  H — Toggle this help overlay
  Ctrl+S — Save game
  Ctrl+L — Load game
  Ctrl+Q — Quit game"""
	vbox.add_child(help_text)

	return panel

# ============================================================
# Input Handling
# ============================================================

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		var mb = event as InputEventMouseButton
		if mb.button_index == MOUSE_BUTTON_LEFT and mb.pressed:
			_handle_click(mb.position)

	if event.is_action_pressed("focus_selected"):
		_focus_selected()
		get_viewport().set_input_as_handled()

	if event.is_action_pressed("save_game"):
		simulation.save_game()
		get_viewport().set_input_as_handled()
	elif event.is_action_pressed("load_game"):
		simulation.load_game()
		# Reinitialize renderer after load
		renderer.initialize(simulation)
		get_viewport().set_input_as_handled()
	elif event.is_action_pressed("quit_game"):
		get_tree().quit()
		get_viewport().set_input_as_handled()

func _handle_click(screen_pos: Vector2) -> void:
	if camera == null:
		return

	# Raycast from camera
	var from = camera.project_ray_origin(screen_pos)
	var to = from + camera.project_ray_normal(screen_pos) * 5000.0

	var space_state = camera.get_world_3d().direct_space_state
	var query = PhysicsRayQueryParameters3D.create(from, to)
	query.collide_with_areas = true
	query.collide_with_bodies = false
	var result = space_state.intersect_ray(query)

	if result.is_empty():
		EventBus.entity_deselected.emit()
		camera.clear_focus_target()
		return

	var collider = result.get("collider")
	if collider and collider is Area3D:
		var entity_type = collider.get_meta("entity_type", "")
		var entity_id = collider.get_meta("entity_id", -1)
		if entity_type != "" and entity_id >= 0:
			EventBus.entity_selected.emit(entity_type, entity_id)
			camera.set_focus_target(entity_type, entity_id)

func _focus_selected() -> void:
	if camera and camera.focus_target_id >= 0:
		if floating_origin:
			camera.set_world_origin_offset(floating_origin.origin_offset)
		camera.update_focus(simulation)

func _process(_delta: float) -> void:
	if camera and floating_origin:
		camera.set_world_origin_offset(floating_origin.origin_offset)

	# Keep camera focused on target
	if camera and camera.focus_target_id >= 0:
		camera.update_focus(simulation)

	# Floating origin check
	if floating_origin and camera:
		floating_origin.check_and_shift()
