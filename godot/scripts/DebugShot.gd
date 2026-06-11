# Dev utility: capture screenshots of the running game without manual interaction.
# Enable by appending to project.godot:
#   [autoload]
#   DebugShot="*res://scripts/DebugShot.gd"
extends Node

func _ready() -> void:
    await get_tree().create_timer(10.0).timeout
    var main := get_tree().current_scene
    main.select_entity("earth_l1", "station", true)
    await get_tree().create_timer(2.0).timeout
    get_viewport().get_texture().get_image().save_png("/tmp/st_station.png")
    main.market_panel.toggle()
    main.market_panel.update_market(main.bridge_state)
    await get_tree().create_timer(1.0).timeout
    get_viewport().get_texture().get_image().save_png("/tmp/st_market.png")
    get_tree().quit()
