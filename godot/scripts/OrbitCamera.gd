extends Node3D

@export var rotation_sensitivity := 0.01
@export var pan_sensitivity := 0.0025
@export var zoom_sensitivity := 0.12
@export var min_distance := 0.08
@export var max_distance := 2500.0

@onready var camera: Camera3D = $Camera3D

var pivot: Vector3 = Vector3.ZERO
var distance := 90.0
var yaw := -0.6
var pitch := -0.45
var rotating := false
var panning := false

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
    var offset := Vector3(
        cos(pitch) * sin(yaw),
        sin(pitch),
        cos(pitch) * cos(yaw)
    ) * distance
    global_position = pivot
    rotation = Vector3.ZERO
    camera.position = offset
    camera.look_at(pivot, Vector3.UP)
