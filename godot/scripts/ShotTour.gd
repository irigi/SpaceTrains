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
    # Close-ups: the station models with docked ships, and the same paint near the Sun
    # (Mercury) and far from it (Titan) for the colour check.
    {"name": "station_leo_close", "id": "earth_orbit", "kind": "station", "distance": 0.0004},
    {"name": "station_l1_close", "id": "earth_l1", "kind": "station", "distance": 0.0004},
    {"name": "station_mercury_close", "id": "mercury_yard", "kind": "station", "distance": 0.0003},
    {"name": "station_titan_close", "id": "titan_works", "kind": "station", "distance": 0.0003},
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
    # --shot-load=<save>: start from a saved game (made from the same data files), e.g. a
    # late game with modules fitted and worn drives.
    for arg in OS.get_cmdline_user_args():
        if arg.begins_with("--shot-load="):
            main.command_request += 1
            main.command_save_path = ""
            main.command_load_path = arg.trim_prefix("--shot-load=")
            main._write_bridge_commands()
            await get_tree().create_timer(8.0).timeout
            print("[ShotTour] loaded %s: display day %.1f" % [main.command_load_path, float(main.get("display_time_s")) / 86400.0])
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
    if OS.get_cmdline_user_args().has("--shot-close"):
        await _close_zoom_shots(rig)
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
            rig.set("distance", 20.0)
            rig.call("_update_transform")
            await get_tree().create_timer(0.6).timeout
            await _shoot("ship_selected_far")
            break
    # A ship just leaving a planet, close up: it must ride the planet's rail.
    var now_s := float(main.get("display_time_s"))
    for ship_id in main.entity_details.keys():
        var ship: Dictionary = main.entity_details[ship_id]
        if main.entity_kinds.get(ship_id, "") == "ship" and String(ship.get("phase", "")) == "in_transit" \
                and now_s - float(ship.get("departure_time_s", 0.0)) < 1.5 * 86400.0 \
                and now_s > float(ship.get("departure_time_s", 0.0)):
            main.select_entity(ship_id, "ship", true)
            rig.set("distance", 0.01)
            rig.call("_update_transform")
            await get_tree().create_timer(0.6).timeout
            await _shoot("ship_departing")
            await get_tree().create_timer(2.0).timeout
            await _shoot("ship_departing_later")
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
    # The ship systems section of the inspector (schematic), for one ship of each drive: a
    # burning thermal ship if there is one, then the same with the mouse over the schematic.
    for propulsion in ["variable_isp", "nuclear_thermal"]:
        var pick := ""
        for ship_id in main.entity_details.keys():
            var ship: Dictionary = main.entity_details[ship_id]
            if main.entity_kinds.get(ship_id, "") != "ship" or String(ship.get("propulsion_type", "")) != propulsion \
                    or String(ship.get("phase", "")) != "in_transit":
                continue
            # Prefer a ship with modules fitted (tier 2), then a burning one.
            var fitted := String(ship.get("design_id", "")).contains("+")
            var pick_fitted := pick != "" and String(main.entity_details[pick].get("design_id", "")).contains("+")
            if pick == "" or (fitted and not pick_fitted) \
                    or (fitted == pick_fitted and String(ship.get("engine_state", "")) == "burning"):
                pick = ship_id
        if pick == "":
            continue
        main.select_entity(pick, "ship", true)
        rig.set("distance", 0.0005)
        rig.call("_update_transform")
        await get_tree().create_timer(0.6).timeout
        var scroll: ScrollContainer = main.inspector_panel.get("_scroll")
        scroll.scroll_vertical = 100000
        await get_tree().create_timer(0.3).timeout
        await _shoot("ship_systems_" + propulsion)
        var schematic := _find_schematic(main.inspector_panel)
        if schematic != null:
            var spot := schematic.get_global_rect().position + Vector2(schematic.size.x * 0.5, 75.0)
            get_viewport().warp_mouse(spot)
            await get_tree().create_timer(0.3).timeout
            schematic = _find_schematic(main.inspector_panel)
            await _shoot("ship_systems_%s_hover" % propulsion)
            # The hover must hold through inspector refreshes (it blinked), and the wheel over
            # the panel, already scrolled to the end, must not zoom the map.
            var lost := 0
            var frames := 0
            var started := Time.get_ticks_msec()
            while Time.get_ticks_msec() - started < 3000:
                await RenderingServer.frame_post_draw
                frames += 1
                if String(schematic.get("_hover_key")) == "":
                    lost += 1
            var distance_before := float(rig.get("distance"))
            for i in 5:
                var wheel := InputEventMouseButton.new()
                wheel.button_index = MOUSE_BUTTON_WHEEL_DOWN
                wheel.pressed = true
                wheel.position = spot
                wheel.global_position = spot
                Input.parse_input_event(wheel)
                await get_tree().process_frame
            print("[ShotTour] schematic hover lost in %d of %d frames; wheel over the inspector: map distance %.6f -> %.6f" % [
                lost, frames, distance_before, float(rig.get("distance"))])
            # Burns are short and rarely caught: the same ship drawn as if burning, before
            # the next inspector refresh replaces it.
            if propulsion == "nuclear_thermal":
                var burning: Dictionary = main.entity_details[pick].duplicate()
                burning["engine_state"] = "burning"
                var designs: Dictionary = main.get("bridge_designs")
                schematic.set_ship(designs.get(String(burning.get("design_id", burning.get("class_id", ""))), {}), burning)
                await _shoot("ship_systems_nuclear_thermal_burning")
    # The 3D models up close, one per hull that is flying (paused, so they hold still).
    main._on_pause_toggled()
    await get_tree().create_timer(0.5).timeout
    var shot_hulls := {}
    for ship_id in main.entity_details.keys():
        var ship: Dictionary = main.entity_details[ship_id]
        var hull := String(ship.get("hull_id", ""))
        if main.entity_kinds.get(ship_id, "") != "ship" or shot_hulls.has(hull) or String(ship.get("phase", "")) != "in_transit":
            continue
        shot_hulls[hull] = true
        main.select_entity(ship_id, "ship", true)
        rig.set("distance", 0.0003)
        rig.call("_update_transform")
        await get_tree().create_timer(0.6).timeout
        await _shoot("ship_model_" + hull)
        # Close enough for the detailed model.
        rig.set("distance", maxf(main._model_world_size(main.entity_nodes[ship_id]) * 3.0, float(rig.get("min_distance"))))
        rig.call("_update_transform")
        await get_tree().create_timer(0.6).timeout
        await _shoot("ship_detail_" + hull)
    main._on_pause_toggled()
    # The economy overview.
    main.market_panel.toggle()
    main._refresh_ui(true)
    await get_tree().create_timer(0.6).timeout
    await _shoot("market_panel")
    main.market_panel.toggle()
    # The Statistics screen, every tab (the bridge writes its file once the screen is open).
    main.stats_panel.toggle()
    await get_tree().create_timer(2.5).timeout
    for tab in range(6):
        main.stats_panel.set_tab(tab)
        await get_tree().create_timer(0.8).timeout
        await _shoot("stats_%d" % tab)
    main.stats_panel.toggle()
    main.help_panel.toggle()
    await get_tree().create_timer(0.3).timeout
    await _shoot("help_panel")
    get_tree().quit()

# Close zoom: a ship at full zoom, a ship's path up close (twice, a second apart, to see
# jitter), an interplanetary ship up close, and a low-orbit station with the camera aimed
# from inside the planet (it must be pushed out, not cut the sphere open).
func _close_zoom_shots(rig: Node3D) -> void:
    var local_ship := ""
    var helio_ship := ""
    for ship_id in main.entity_details.keys():
        if main.entity_kinds.get(ship_id, "") != "ship" or String(main.entity_details[ship_id].get("phase", "")) != "in_transit" \
                or not main.ship_paths.has(ship_id):
            continue
        if bool(main.ship_paths[ship_id].get("helio", false)):
            if helio_ship == "":
                helio_ship = ship_id
        elif local_ship == "":
            local_ship = ship_id
    for pick in [[local_ship, "local"], [helio_ship, "helio"]]:
        var ship_id: String = pick[0]
        if ship_id == "":
            print("[ShotTour] close: no %s ship in flight" % pick[1])
            continue
        main.select_entity(ship_id, "ship", true)
        rig.set("distance", float(rig.get("min_distance")))
        rig.call("_update_transform")
        await get_tree().create_timer(0.6).timeout
        await _shoot("close_%s_ship_min" % pick[1])
        for distance in [0.0005, 0.005]:
            rig.set("distance", distance)
            rig.call("_update_transform")
            await get_tree().create_timer(0.6).timeout
            await _shoot("close_%s_path_%s" % [pick[1], str(distance)])
        await get_tree().create_timer(1.0).timeout
        await _shoot("close_%s_path_later" % pick[1])
        var started := Time.get_ticks_usec()
        for i in 20:
            main._update_fine_paths()
        var cached_ms := float(Time.get_ticks_usec() - started) / 20000.0
        started = Time.get_ticks_usec()
        for i in 5:
            for entry in main.fine_paths.values():
                entry["camera"] = 1.0e-9  # force a rebuild
            main._update_fine_paths()
        var rebuild_ms := float(Time.get_ticks_usec() - started) / 5000.0
        print("[ShotTour] close %s: min_distance %.8f, fine paths %d drawn, %.2f ms per frame (%.2f ms when all rebuilt)" % [
            pick[1], float(rig.get("min_distance")), main.fine_paths.values().filter(func(e): return e["node"] != null and e["node"].visible).size(),
            cached_ms, rebuild_ms])
    if main.entity_nodes.has("earth_orbit") and main.entity_nodes.has("earth"):
        main.select_entity("earth_orbit", "station", true)
        var to_planet: Vector3 = (main.entity_nodes["earth"].global_position - main.entity_nodes["earth_orbit"].global_position).normalized()
        rig.set("yaw", atan2(to_planet.x, to_planet.z))
        rig.set("pitch", asin(clampf(to_planet.y, -0.9, 0.9)) - 0.1)
        # Without the push-out this would put the camera near Earth's centre.
        rig.set("distance", main.entity_nodes["earth"].global_position.distance_to(main.entity_nodes["earth_orbit"].global_position))
        rig.call("_update_transform")
        await get_tree().create_timer(0.6).timeout
        var camera: Camera3D = rig.get("camera")
        var earth: Node3D = main.entity_nodes["earth"]
        print("[ShotTour] close station: camera %.6f from Earth's centre, radius %.6f, near %.10f" % [
            camera.global_position.distance_to(earth.global_position), earth.scale.x, camera.near])
        await _shoot("close_station_blocked")

func _find_schematic(node: Node) -> Control:
    for child in node.get_children():
        if child is Control and (child as Control).has_method("set_ship"):
            return child
        var found := _find_schematic(child)
        if found != null:
            return found
    return null

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
