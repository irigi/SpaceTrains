# Dev utility: capture screenshots of the running game without manual interaction.
# Enable by appending to project.godot:
#   [autoload]
#   DebugShot="*res://scripts/DebugShot.gd"
extends Node

func _ready() -> void:
    await get_tree().create_timer(8.0).timeout
    var main := get_tree().current_scene
    main._on_timewarp_changed(432000.0)
    await get_tree().create_timer(20.0).timeout
    # Pick an in-transit ship with a trajectory, prefer interplanetary arcs.
    var best_id := ""
    var best_span := 0.0
    for ship in main.bridge_state.get("ships", []):
        if String(ship.get("phase", "")) != "in_transit":
            continue
        var span := float(ship.get("total_travel_time_s", 0.0))
        if span > best_span and (ship.get("trajectory_path", []) as Array).size() >= 2:
            best_span = span
            best_id = String(ship.get("id", ""))
    if best_id != "":
        main.select_entity(best_id, "ship", true)
        main.camera_rig.distance = 40.0
    await get_tree().create_timer(2.0).timeout
    get_viewport().get_texture().get_image().save_png("/tmp/st_transit.png")
    if best_id != "":
        main.camera_rig.distance = 0.0015
        await get_tree().create_timer(1.5).timeout
        get_viewport().get_texture().get_image().save_png("/tmp/st_ship_close.png")
    main.select_entity("earth_l1", "station", true)
    main.camera_rig.distance = 0.0012
    await get_tree().create_timer(1.5).timeout
    get_viewport().get_texture().get_image().save_png("/tmp/st_station_close.png")
    get_tree().quit()
