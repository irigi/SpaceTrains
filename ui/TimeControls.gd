class_name TimeControls
extends HBoxContainer
## Time control UI: pause/play and speed buttons.

var simulation: Simulation

var pause_button: Button
var speed_label: Label
var time_label: Label

var speed_presets: Array[float] = [1.0, 5.0, 20.0, 50.0]
var current_speed_index: int = 0

func _ready() -> void:
	if pause_button:
		pause_button.pressed.connect(_on_pause_pressed)

func _process(_delta: float) -> void:
	if simulation == null:
		return

	if time_label:
		var hours = int(simulation.world.sim_time / 60.0)
		var days = hours / 24
		var remaining_hours = hours % 24
		time_label.text = "Day %d, %02d:00" % [days + 1, remaining_hours]

	if speed_label:
		if simulation.paused:
			speed_label.text = "PAUSED"
		else:
			speed_label.text = "%.0f×" % simulation.speed_multiplier

func _unhandled_input(event: InputEvent) -> void:
	if simulation == null:
		return

	if event.is_action_pressed("pause_toggle"):
		_on_pause_pressed()
	elif event.is_action_pressed("speed_1"):
		_set_speed(0)
	elif event.is_action_pressed("speed_2"):
		_set_speed(1)
	elif event.is_action_pressed("speed_3"):
		_set_speed(2)
	elif event.is_action_pressed("speed_4"):
		_set_speed(3)

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
