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
    {"name": "station_leo", "id": "earth_orbit", "kind": "station", "distance": 0.002},
    {"name": "station_l1", "id": "earth_l1", "kind": "station", "distance": 0.004},
]

func _ready() -> void:
    for arg in OS.get_cmdline_user_args():
        if arg.begins_with("--shot-tour="):
            out_dir = arg.trim_prefix("--shot-tour=")
    main = get_parent()
    _run.call_deferred()

func _apply_debug_flags() -> void:
    var args := OS.get_cmdline_user_args()
    if args.has("--shot-no-glow"):
        for node in main.get_children():
            if node is WorldEnvironment:
                (node as WorldEnvironment).environment.glow_enabled = false
    if args.has("--shot-no-ships"):
        for entity_id in main.entity_nodes.keys():
            if main.entity_kinds.get(entity_id, "") == "ship":
                (main.entity_nodes[entity_id] as Node3D).visible = false

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
    _apply_debug_flags()
    for stop in STOPS:
        if not main.entity_nodes.has(stop["id"]):
            print("[ShotTour] skip %s (no %s)" % [stop["name"], stop["id"]])
            continue
        main.select_entity(stop["id"], stop["kind"], true)
        rig.set("distance", maxf(float(stop["distance"]), float(rig.get("min_distance"))))
        rig.call("_update_transform")
        await get_tree().create_timer(0.6).timeout
        await _shoot(stop["name"])
        if OS.get_cmdline_user_args().has("--shot-big-meshes"):
            _report_big_meshes(main.get_tree().root, stop["name"])
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

# Debug aid: meshes that cover a large part of the view (a glow or overlay out of scale).
func _report_big_meshes(node: Node, stop_name: String) -> void:
    var camera: Camera3D = get_viewport().get_camera_3d()
    for child in node.get_children():
        if child is GeometryInstance3D and (child as GeometryInstance3D).is_visible_in_tree():
            var aabb: AABB = (child as GeometryInstance3D).get_aabb()
            var world_size: float = aabb.size.length() * (child as Node3D).global_transform.basis.get_scale().length() / sqrt(3.0)
            var distance: float = maxf(camera.global_position.distance_to((child as Node3D).global_position), 1.0e-9)
            if world_size / distance > 0.8 and world_size < 1000.0:
                print("[ShotTour] %s: big mesh %s size %.6f at distance %.6f" % [stop_name, child.get_path(), world_size, distance])
        _report_big_meshes(child, stop_name)
