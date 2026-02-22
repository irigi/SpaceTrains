class_name OrbitCamera
extends Camera3D
## Orbit camera with rotate, pan, zoom. Focuses on selected objects.

@export var zoom_speed: float = 0.1
@export var rotate_speed: float = 0.005
@export var pan_speed: float = 0.5
@export var min_distance: float = 0.5
@export var max_distance: float = 5000.0

var pivot_point: Vector3 = Vector3.ZERO
var distance: float = 100.0
var yaw: float = 0.0
var pitch: float = -0.5  # Slight downward angle

var _rotating: bool = false
var _panning: bool = false

var focus_target_id: int = -1
var focus_target_type: String = ""  # "body", "station", "ship"
var world_origin_offset: Vector3 = Vector3.ZERO

func _ready() -> void:
	_update_camera_transform()

func _input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		var mb := event as InputEventMouseButton
		match mb.button_index:
			MOUSE_BUTTON_RIGHT:
				_rotating = mb.pressed
				get_viewport().set_input_as_handled()
			MOUSE_BUTTON_MIDDLE:
				_panning = mb.pressed
				get_viewport().set_input_as_handled()
			MOUSE_BUTTON_WHEEL_UP:
				distance = maxf(min_distance, distance * (1.0 - zoom_speed))
				_update_camera_transform()
				get_viewport().set_input_as_handled()
			MOUSE_BUTTON_WHEEL_DOWN:
				distance = minf(max_distance, distance * (1.0 + zoom_speed))
				_update_camera_transform()
				get_viewport().set_input_as_handled()

	elif event is InputEventMouseMotion:
		var mm := event as InputEventMouseMotion
		if _rotating:
			yaw -= mm.relative.x * rotate_speed
			pitch -= mm.relative.y * rotate_speed
			pitch = clampf(pitch, -PI * 0.49, PI * 0.49)
			_update_camera_transform()
			get_viewport().set_input_as_handled()
		elif _panning:
			var right = global_transform.basis.x
			var up = global_transform.basis.y
			var pan_amount = distance * pan_speed * 0.001
			pivot_point -= right * mm.relative.x * pan_amount
			pivot_point += up * mm.relative.y * pan_amount
			_update_camera_transform()
			get_viewport().set_input_as_handled()

func _update_camera_transform() -> void:
	var offset := Vector3.ZERO
	offset.x = cos(pitch) * sin(yaw) * distance
	offset.y = sin(pitch) * distance
	offset.z = cos(pitch) * cos(yaw) * distance
	global_position = pivot_point + offset
	look_at(pivot_point, Vector3.UP)

func focus_on(pos: Vector3) -> void:
	pivot_point = pos
	_update_camera_transform()

func set_focus_target(entity_type: String, entity_id: int) -> void:
	focus_target_type = entity_type
	focus_target_id = entity_id

func clear_focus_target() -> void:
	focus_target_type = ""
	focus_target_id = -1

func set_world_origin_offset(offset: Vector3) -> void:
	world_origin_offset = offset

func update_focus(sim: Simulation) -> void:
	if focus_target_id < 0:
		return

	match focus_target_type:
		"body":
			pivot_point = sim.get_body_position(focus_target_id) - world_origin_offset
		"station":
			pivot_point = sim.get_station_position(focus_target_id) - world_origin_offset
		"ship":
			if focus_target_id in sim.world.ships:
				pivot_point = sim.world.ships[focus_target_id].position - world_origin_offset
	_update_camera_transform()
