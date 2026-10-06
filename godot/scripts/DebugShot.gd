# Dev utility: capture screenshots of the running game without manual interaction.
# Enable by appending to project.godot:
#   [autoload]
#   DebugShot="*res://scripts/DebugShot.gd"
extends Node

func _ready() -> void:
    await get_tree().create_timer(8.0).timeout
    var main := get_tree().current_scene
    main._on_timewarp_changed(432000.0)
    await get_tree().create_timer(12.0).timeout
    # Overview: starfield subtlety + sun halo.
    main.camera_rig.distance = 220.0
    await get_tree().create_timer(1.5).timeout
    get_viewport().get_texture().get_image().save_png("/tmp/st2_overview.png")
    # Earth close-up: texture + no oversized engine glow nearby.
    main.select_entity("earth", "body", true)
    # View from the sunward side so the day side faces the camera.
    var earth_node: Node3D = main.entity_nodes.get("earth")
    var sun_node: Node3D = main.entity_nodes.get("sun")
    if earth_node != null and sun_node != null:
        var to_sun: Vector3 = (sun_node.global_position - earth_node.global_position).normalized()
        main.camera_rig.yaw = atan2(to_sun.x, to_sun.z)
        main.camera_rig.pitch = -0.15
    main.camera_rig.distance = 0.004
    await get_tree().create_timer(1.5).timeout
    get_viewport().get_texture().get_image().save_png("/tmp/st2_earth.png")
    # Saturn: texture + procedural ring.
    main.select_entity("saturn", "body", true)
    main.camera_rig.distance = 0.04
    await get_tree().create_timer(1.5).timeout
    get_viewport().get_texture().get_image().save_png("/tmp/st2_saturn.png")
    # In-transit ship close-up: capped engine glow.
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
        main.camera_rig.distance = 0.0015
        await get_tree().create_timer(1.5).timeout
        get_viewport().get_texture().get_image().save_png("/tmp/st2_ship.png")
    get_tree().quit()
