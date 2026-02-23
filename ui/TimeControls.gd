class_name TimeControls
extends HBoxContainer
## Time control UI: pause/play and speed buttons.

var simulation: Simulation

var pause_button: Button
var speed_label: Label
var time_label: Label

var speed_presets: Array[float] = [0.2, 1.0, 5.0, 20.0, 50.0]
var current_speed_index: int = 1

func _ready() -> void:
	if pause_button:
		pause_button.pressed.connect(_on_pause_pressed)

func _process(_delta: float) -> void:
	if simulation == null:
		return

	if time_label:
		var total_minutes: int = int(simulation.world.sim_time)
		@warning_ignore("integer_division")
		var day_index: int = total_minutes / 1440
		var year_index: int = day_index / 365
		var day_of_year: int = day_index % 365
		@warning_ignore("integer_division")
		var hour_of_day: int = (total_minutes % 1440) / 60
		time_label.text = "Y%02d D%03d %02d:00" % [year_index + 1, day_of_year + 1, hour_of_day]

	if speed_label:
		if simulation.paused:
			speed_label.text = "PAUSED"
		else:
			speed_label.text = _format_speed_label(simulation.speed_multiplier)

func _unhandled_input(event: InputEvent) -> void:
	if simulation == null:
		return

	if event.is_action_pressed("pause_toggle"):
		_on_pause_pressed()
	elif event.is_action_pressed("speed_0"):
		_set_speed(0)
	elif event.is_action_pressed("speed_1"):
		_set_speed(1)
	elif event.is_action_pressed("speed_2"):
		_set_speed(2)
	elif event.is_action_pressed("speed_3"):
		_set_speed(3)
	elif event.is_action_pressed("speed_4"):
		_set_speed(4)

func _on_pause_pressed() -> void:
	if simulation:
		simulation.set_paused(not simulation.paused)
		if pause_button:
			pause_button.text = "▶" if simulation.paused else "⏸"

func _set_speed(index: int) -> void:
	if index < 0 or index >= speed_presets.size():
		return
	current_speed_index = index
	if simulation:
		simulation.set_speed(speed_presets[index])
		if simulation.paused:
			simulation.set_paused(false)
			if pause_button:
				pause_button.text = "⏸"

func _format_speed_label(speed: float) -> String:
	if speed < 1.0:
		return "%.1f×" % speed
	return "%.0f×" % speed
