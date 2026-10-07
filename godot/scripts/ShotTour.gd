# Dev utility: a scripted camera tour that saves screenshots, for checking the map without
# sitting at the screen. Run: godot4 --path godot -- --shot-tour=/some/dir
# Each stop focuses an entity at a camera distance (world units, 1 = 8e9 m) and saves
# <dir>/<name>.png, then the game quits.
extends Node

var out_dir := ""
var main: Node

const STOPS := [
    {"name": "overview", "id": "sun", "kind": "body", "distance": 90.0},
    {"name": "inner", "id": "sun", "kind": "body", "distance": 30.0},
    {"name": "earth_system", "id": "earth", "kind": "body", "distance": 0.12},
    {"name": "earth_close", "id": "earth", "kind": "body", "distance": 0.004},
    {"name": "earth_min", "id": "earth", "kind": "body", "distance": 0.0008},
    {"name": "luna_close", "id": "luna", "kind": "body", "distance": 0.002},
    {"name": "jupiter_system", "id": "jupiter", "kind": "body", "distance": 0.3},
    {"name": "ganymede_close", "id": "ganymede", "kind": "body", "distance": 0.002},
    {"name": "saturn_close", "id": "saturn", "kind": "body", "distance": 0.06},
    {"name": "station_leo", "id": "low_earth_logistics", "kind": "station", "distance": 0.003},
]

func _ready() -> void:
    for arg in OS.get_cmdline_user_args():
        if arg.begins_with("--shot-tour="):
            out_dir = arg.trim_prefix("--shot-tour=")
    main = get_parent()
    _run.call_deferred()

func _shoot(name: String) -> void:
    await get_tree().process_frame
    await get_tree().process_frame
    var image := get_viewport().get_texture().get_image()
    image.save_png(out_dir.path_join(name + ".png"))
    print("[ShotTour] saved %s" % name)

func _run() -> void:
    # Wait for the simulation to run a few days.
    var waited := 0.0
    while float(main.get("display_time_s")) < 3.0 * 86400.0 and waited < 60.0:
        await get_tree().create_timer(0.5).timeout
        waited += 0.5
    var rig: Node3D = main.get("camera_rig")
    for stop in STOPS:
        if not main.entity_nodes.has(stop["id"]):
            print("[ShotTour] skip %s (no %s)" % [stop["name"], stop["id"]])
            continue
        main.select_entity(stop["id"], stop["kind"], true)
        rig.set("distance", float(stop["distance"]))
        rig.call("_update_transform")
        await get_tree().create_timer(0.6).timeout
        await _shoot(stop["name"])
    # A ship in flight, if any.
    for ship_id in main.entity_details.keys():
        if main.entity_kinds.get(ship_id, "") == "ship" and String(main.entity_details[ship_id].get("phase", "")) == "in_transit":
            main.select_entity(ship_id, "ship", true)
            rig.set("distance", 0.05)
            rig.call("_update_transform")
            await get_tree().create_timer(0.6).timeout
            await _shoot("ship_in_transit")
            break
    get_tree().quit()
