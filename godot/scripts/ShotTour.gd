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
    {"name": "station_mars", "id": "mars_transfer", "kind": "station", "distance": 0.004},
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

# Smoothness check: run at a timewarp for a while and count frames where the display clock
# did not move (a stall the player would see), and the longest one.
func _measure_smoothness(seconds: float, timewarp: float) -> void:
    main._on_timewarp_changed(timewarp)
    var stalled_frames := 0
    var frames := 0
    var longest_stall := 0.0
    var stall := 0.0
    var last_time := float(main.get("display_time_s"))
    var elapsed := 0.0
    var slow_frames := 0
    while elapsed < seconds:
        await get_tree().process_frame
        var delta := get_process_delta_time()
        elapsed += delta
        frames += 1
        if delta > 0.05:
            slow_frames += 1
        var now := float(main.get("display_time_s"))
        if now <= last_time:
            stalled_frames += 1
            stall += delta
            longest_stall = maxf(longest_stall, stall)
            if stall > 1.0 and stall - delta <= 1.0:
                print("[ShotTour] stall >1 s at display day %.2f: last snapshot day %.2f (%.2f s ago), pace %.0f, seq %d" % [
                    now / 86400.0, float(main.get("snapshot_game_time_s")) / 86400.0,
                    main._wall_time_s() - float(main.get("snapshot_wall_s")), float(main.get("sim_pace")),
                    int(main.get("current_snapshot_seq"))])
        else:
            stall = 0.0
        last_time = now
    print("[ShotTour] smoothness at %.0fx: %d frames in %.0f s, %d stalled, longest stall %.2f s, %d frames over 50 ms, day %.1f" % [
        timewarp, frames, seconds, stalled_frames, longest_stall, slow_frames, last_time / 86400.0])

func _run() -> void:
    # Run the simulation a while (fast) so the panels have history, then back to 1 day/s.
    var start_day := 3.0
    for arg in OS.get_cmdline_user_args():
        if arg.begins_with("--shot-day="):
            start_day = float(arg.trim_prefix("--shot-day="))
    if start_day > 10.0:
        main._on_timewarp_changed(432000.0)
    var waited := 0.0
    while float(main.get("display_time_s")) < start_day * 86400.0 and waited < 120.0:
        await get_tree().create_timer(0.5).timeout
        waited += 0.5
    main._on_timewarp_changed(86400.0)
    var rig: Node3D = main.get("camera_rig")
    _apply_debug_flags()
    for arg in OS.get_cmdline_user_args():
        if arg == "--shot-pause":
            var before := float(main.get("display_time_s"))
            main._on_pause_toggled()
            await get_tree().create_timer(3.0).timeout
            var paused_at := float(main.get("display_time_s"))
            main._on_pause_toggled()
            await get_tree().create_timer(1.0).timeout
            var after := float(main.get("display_time_s"))
            print("[ShotTour] pause: moved %.2f d while paused 3 s (from %.2f), %.2f d in the first second after resuming" % [
                (paused_at - before) / 86400.0, before / 86400.0, (after - paused_at) / 86400.0])
            get_tree().quit()
            return
        if arg.begins_with("--shot-smooth="):
            await _measure_smoothness(float(arg.trim_prefix("--shot-smooth=")), 432000.0)
            await _measure_smoothness(20.0, 86400.0)
            get_tree().quit()
            return
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
    # Quick save, run on, quick load: the clock must come back to the saved day.
    if OS.get_cmdline_user_args().has("--shot-saveload"):
        var saved_day := float(main.get("display_time_s")) / 86400.0
        main._request_save_or_load(true)
        await get_tree().create_timer(3.0).timeout
        main._request_save_or_load(false)
        await get_tree().create_timer(2.0).timeout
        print("[ShotTour] save/load: saved at day %.1f, after load display day %.1f, status '%s'" % [
            saved_day, float(main.get("display_time_s")) / 86400.0, String(main.get("bridge_status_text"))])
        await _shoot("after_load")
    # The economy overview.
    main.market_panel.toggle()
    main._refresh_ui(true)
    await get_tree().create_timer(0.6).timeout
    await _shoot("market_panel")
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
