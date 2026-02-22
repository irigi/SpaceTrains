class_name FloatingOrigin
extends Node3D
## Floating origin system to maintain rendering precision.
## Shifts all world-space objects to keep the camera near the origin.

const SHIFT_THRESHOLD: float = 500.0  # Distance before we shift origin

var origin_offset: Vector3 = Vector3.ZERO
var camera: Camera3D

func _ready() -> void:
	pass

func set_camera(cam: Camera3D) -> void:
	camera = cam

func check_and_shift() -> void:
	if camera == null:
		return

	var cam_pos = camera.global_position
	if cam_pos.length() > SHIFT_THRESHOLD:
		var shift = cam_pos
		origin_offset += shift

		# Shift all children of this node
		for child in get_children():
			if child is Node3D:
				child.global_position -= shift

		camera.global_position -= shift

func world_to_render(world_pos: Vector3) -> Vector3:
	return world_pos - origin_offset

func render_to_world(render_pos: Vector3) -> Vector3:
	return render_pos + origin_offset
