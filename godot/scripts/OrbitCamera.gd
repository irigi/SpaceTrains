extends Node3D

@export var rotation_sensitivity := 0.01
@export var pan_sensitivity := 0.0025
@export var zoom_sensitivity := 0.12
@export var min_distance := 0.0008
@export var max_distance := 2500.0

@onready var camera: Camera3D = $Camera3D

var pivot: Vector3 = Vector3.ZERO
var distance := 90.0
var yaw := -0.6
var pitch := -0.45
var rotating := false
var panning := false
# Spheres the camera may not enter (planets, moons, the Sun), as [center, radius] in this
# rig's parent space; Main refreshes them every frame.
var obstacles: Array = []

func _ready() -> void:
    _update_transform()

func _unhandled_input(event: InputEvent) -> void:
    if event is InputEventMouseButton:
        if event.button_index == MOUSE_BUTTON_RIGHT:
            rotating = event.pressed
        elif event.button_index == MOUSE_BUTTON_MIDDLE:
            panning = event.pressed
        elif event.button_index == MOUSE_BUTTON_WHEEL_UP and event.pressed:
            distance = max(min_distance, distance * (1.0 - zoom_sensitivity))
            _update_transform()
        elif event.button_index == MOUSE_BUTTON_WHEEL_DOWN and event.pressed:
            distance = min(max_distance, distance * (1.0 + zoom_sensitivity))
            _update_transform()
    elif event is InputEventMouseMotion:
        if rotating:
            yaw -= event.relative.x * rotation_sensitivity
            pitch = clamp(pitch - event.relative.y * rotation_sensitivity, -PI * 0.495, PI * 0.495)
            _update_transform()
        elif panning:
            var basis := camera.global_transform.basis
            pivot += (-basis.x * event.relative.x + basis.y * event.relative.y) * pan_sensitivity * distance
            _update_transform()

func focus_point(target: Vector3) -> void:
    pivot = target
    _update_transform()

func _update_transform() -> void:
    var direction := Vector3(
        cos(pitch) * sin(yaw),
        sin(pitch),
        cos(pitch) * cos(yaw)
    )
    var eye := pivot + direction * distance
    # A planet in the way pushes the camera out to just above its surface (it slides along
    # the planet, as in Kerbal Space Program). Yaw, pitch and distance stay as the user set
    # them, so turning away brings the camera back.
    var surface_gap := INF
    for _pass in range(2):
        for obstacle in obstacles:
            var center: Vector3 = obstacle[0]
            var radius: float = obstacle[1]
            if pivot.distance_to(center) <= radius:
                continue  # looking at something inside it (e.g. the focused body's own centre)
            var keep_out := radius * 1.03 + distance * 0.05
            var from_center := eye - center
            var gap := from_center.length()
            if gap < keep_out:
                var outward := from_center / gap if gap > 0.0 else direction
                eye = center + outward * keep_out
    for obstacle in obstacles:
        surface_gap = minf(surface_gap, eye.distance_to(obstacle[0]) - float(obstacle[1]))
    var offset := eye - pivot
    if offset.length() < 1.0e-15:
        offset = direction * distance
    global_position = pivot
    rotation = Vector3.ZERO
    # Build the basis from the normalized offset: look_at() rejects tiny
    # (ship-scale) offsets because its alignment check is not scale-invariant.
    camera.transform = Transform3D(Basis.looking_at(-offset.normalized(), Vector3.UP), offset)
    # Ship meshes are ~1e-8 world units at true scale; shrink the near plane as we zoom in so
    # they stay drawn however close the camera is (reverse-Z keeps depth precision fine),
    # and keep it short of the nearest planet's surface so the planet is never sliced open.
    var near := clampf(offset.length() * 0.02, 1.0e-14, 0.1)
    if surface_gap < INF:
        near = minf(near, maxf(surface_gap * 0.5, 1.0e-14))
    camera.near = near
