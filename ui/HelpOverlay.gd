class_name HelpOverlay
extends PanelContainer
## Displays controls and help information.

func _ready() -> void:
	visible = false

func _unhandled_input(event: InputEvent) -> void:
	if event.is_action_pressed("toggle_help"):
		visible = not visible
		get_viewport().set_input_as_handled()
