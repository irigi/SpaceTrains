# Dev utility: capture screenshots of the running game without manual interaction.
# Enable by appending to project.godot:
#   [autoload]
#   DebugShot="*res://scripts/DebugShot.gd"
extends Node

func _ready() -> void:
    await get_tree().create_timer(10.0).timeout
    var main := get_tree().current_scene
    get_viewport().get_texture().get_image().save_png("/tmp/st_overview.png")
    main.select_entity("saturn", "body", true)
    main.camera_rig.distance = 0.25
    await get_tree().create_timer(2.0).timeout
    get_viewport().get_texture().get_image().save_png("/tmp/st_saturn.png")
    main.select_entity("sun", "body", true)
    main.camera_rig.distance = 0.4
    await get_tree().create_timer(2.0).timeout
    get_viewport().get_texture().get_image().save_png("/tmp/st_sun.png")
    get_tree().quit()
