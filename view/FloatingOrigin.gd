class_name FloatingOrigin
extends Node3D
## Floating origin system to maintain rendering precision.
## Shifts all world-space objects to keep the camera near the origin.
## Works in coordination with OrbitCamera by adjusting its pivot_point.

const SHIFT_THRESHOLD: float = 500.0  # Distance before we shift origin

var origin_offset: Vector3 = Vector3.ZERO
var _camera: OrbitCamera

func _ready() -> void:
	pass

func set_camera(cam: OrbitCamera) -> void:
	_camera = cam

func check_and_shift() -> void:
	if _camera == null:
		return

	var cam_pos = _camera.global_position
	if cam_pos.length() > SHIFT_THRESHOLD:
		var shift = cam_pos
		origin_offset += shift

		# Shift all children of this node (the renderer and its contents)
		for child in get_children():
			if child is Node3D:
				child.global_position -= shift

		# Adjust the camera's pivot_point so _update_camera_transform
		# keeps the camera in the same relative position to the scene
		_camera.pivot_point -= shift
		_camera._update_camera_transform()

func world_to_render(world_pos: Vector3) -> Vector3:
	return world_pos - origin_offset

func render_to_world(render_pos: Vector3) -> Vector3:
	return render_pos + origin_offset
