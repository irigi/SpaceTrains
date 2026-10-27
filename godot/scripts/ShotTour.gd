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
    # "size": the distance in model sizes (the model's world radius), whatever the model scale.
    {"name": "station_leo_close", "id": "earth_orbit", "kind": "station", "size": 3.0},
    {"name": "station_leo_inside", "id": "earth_orbit", "kind": "station", "size": 1.1},
    {"name": "station_leo_core", "id": "earth_orbit", "kind": "station", "size": 0.3},
    {"name": "station_l1_close", "id": "earth_l1", "kind": "station", "size": 3.0},
    {"name": "station_luna_close", "id": "luna_base", "kind": "station", "size": 3.0},
    {"name": "station_mercury_close", "id": "mercury_yard", "kind": "station", "size": 3.0},
    {"name": "station_titan_close", "id": "titan_works", "kind": "station", "size": 3.0},
    # The Low Earth station with Earth behind it, and from further out (how big it is next to its orbit).
    {"name": "station_leo_earth", "id": "earth_orbit", "kind": "station", "size": 12.0},
    {"name": "station_leo_far", "id": "earth_orbit", "kind": "station", "size": 200.0},
]

# A stop's camera distance: fixed world units, or model sizes ("size").
func _stop_distance(stop: Dictionary) -> float:
    if stop.has("size"):
        return float(stop["size"]) * main.map_icons.model_world_size(main.entity_nodes[stop["id"]])
    return float(stop["distance"])

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
            await get_tree().create_timer(0.5).timeout
            var spin_before := float(main.get("spin_clock_s"))
            await get_tree().create_timer(2.5).timeout
            print("[ShotTour] pause: station spin clock moved %.3f s while paused" % (float(main.get("spin_clock_s")) - spin_before))
            var paused_at := float(main.get("display_time_s"))
            main._on_pause_toggled()
            await get_tree().create_timer(1.0).timeout
            var after := float(main.get("display_time_s"))
            print("[ShotTour] pause: moved %.2f d while paused 3 s (from %.2f), %.2f d in the first second after resuming" % [
                (paused_at - before) / 86400.0, before / 86400.0, (after - paused_at) / 86400.0])
            get_tree().quit()
            return
        if arg.begins_with("--shot-smooth="):
            if OS.get_cmdline_user_args().has("--shot-with-routes"):
                # The porkchop worker busy on a slow pair while the game runs at speed.
                main.stats_panel.show_route("earth", "jupiter")
                main._update_porkchop_request()
                await get_tree().create_timer(1.0).timeout
            for a in OS.get_cmdline_user_args():
                # --shot-select=<station id>: keep a station in the inspector (it rebuilds on refreshes).
                if a.begins_with("--shot-select="):
                    var id := a.trim_prefix("--shot-select=")
                    main.select_entity(id, "station", not OS.get_cmdline_user_args().has("--shot-nofocus"))
            await _measure_smoothness(float(arg.trim_prefix("--shot-smooth=")), 432000.0)
            await _measure_smoothness(20.0, 86400.0)
            get_tree().quit()
            return
    for arg in OS.get_cmdline_user_args():
        if arg.begins_with("--shot-stats"):
            await _stats_shots(arg.trim_prefix("--shot-stats=") if arg.contains("=") else "")
            get_tree().quit()
            return
    if OS.get_cmdline_user_args().has("--shot-routes"):
        await _routes_shots(rig)
        get_tree().quit()
        return
    if OS.get_cmdline_user_args().has("--shot-flows"):
        await _flow_overlay_shots(rig)
        get_tree().quit()
        return
    if OS.get_cmdline_user_args().has("--shot-inspector"):
        await _inspector_shots(rig)
        get_tree().quit()
        return
    if OS.get_cmdline_user_args().has("--shot-station-systems"):
        await _station_systems_shots(rig)
        get_tree().quit()
        return
    if OS.get_cmdline_user_args().has("--shot-arrival"):
        await _arrival_shots(rig)
        get_tree().quit()
        return
    if OS.get_cmdline_user_args().has("--shot-cargo"):
        await _cargo_shots(rig)
        get_tree().quit()
        return
    if OS.get_cmdline_user_args().has("--shot-close"):
        _report_path_end_gaps()
        await _docked_close_shot(rig)
        await _close_zoom_shots(rig)
        get_tree().quit()
        return
    for stop in STOPS:
        if not main.entity_nodes.has(stop["id"]):
            print("[ShotTour] skip %s (no %s)" % [stop["name"], stop["id"]])
            continue
        main.select_entity(stop["id"], stop["kind"], true)
        rig.set("distance", maxf(_stop_distance(stop), float(rig.get("min_distance"))))
        rig.call("_update_transform")
        await get_tree().create_timer(0.6).timeout
        await _shoot(stop["name"])
        if OS.get_cmdline_user_args().has("--shot-fps"):
            await _report_fps(stop["name"])
        if OS.get_cmdline_user_args().has("--shot-big-meshes"):
            _report_big_meshes(main.get_tree().root, stop["name"])
    await _docked_close_shot(rig)
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
        rig.set("distance", maxf(main.map_icons.model_world_size(main.entity_nodes[ship_id]) * 40.0, float(rig.get("min_distance"))))
        rig.call("_update_transform")
        await get_tree().create_timer(0.6).timeout
        await _shoot("ship_model_" + hull)
        # Close enough for the detailed model.
        rig.set("distance", maxf(main.map_icons.model_world_size(main.entity_nodes[ship_id]) * 3.0, float(rig.get("min_distance"))))
        rig.call("_update_transform")
        await get_tree().create_timer(0.6).timeout
        await _shoot("ship_detail_" + hull)
        # Plasma ships again in the thermal view (T), through the real key handler.
        if String(ship.get("propulsion_type", "")) == "variable_isp":
            for on in [true, false]:
                var key := InputEventKey.new()
                key.keycode = KEY_T
                key.pressed = true
                Input.parse_input_event(key)
                await get_tree().create_timer(0.3).timeout
                if on:
                    await _shoot("ship_detail_%s_thermal" % hull)
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
    for tab in range(int(main.stats_panel.get("TABS").size())):
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

# The next ship to dock, watched from its destination station at a slow timewarp: it
# should ease into a berth over the ring, without a jump.
func _arrival_shots(rig: Node3D) -> void:
    var pick := ""
    var soonest := INF
    var waited := 0
    while waited < 240:
        var now := float(main.get("display_time_s"))
        soonest = INF
        for ship_id in main.entity_details.keys():
            var ship: Dictionary = main.entity_details[ship_id]
            if main.entity_kinds.get(ship_id, "") != "ship" or String(ship.get("phase", "")) != "in_transit":
                continue
            var arrival := float(ship.get("arrival_time_s", 0.0))
            if arrival > now + 4.0 * 3600.0 and arrival < soonest and main.entity_nodes.has(String(ship.get("destination_station_id", ""))):
                soonest = arrival
                pick = ship_id
        if soonest - now < 2.0 * 86400.0:
            break
        await get_tree().create_timer(0.5).timeout
        waited += 1
    if pick == "" or soonest == INF:
        print("[ShotTour] arrival: no ship in flight")
        return
    var station_id := String(main.entity_details[pick].get("destination_station_id", ""))
    print("[ShotTour] arrival: %s to %s in %.2f d" % [pick, station_id, (soonest - float(main.get("display_time_s"))) / 86400.0])
    main.select_entity(station_id, "station", true)
    var station: Node3D = main.entity_nodes[station_id]
    rig.set("distance", main.map_icons.model_world_size(station) * 6.0)
    rig.set("pitch", -0.6)
    rig.call("_update_transform")
    main._on_timewarp_changed(21600.0)
    while float(main.get("display_time_s")) < float(main.entity_details[pick].get("arrival_time_s", 0.0)) - 3.0 * 3600.0:
        await get_tree().process_frame
    main._on_timewarp_changed(600.0)
    var frame := 0
    var last_offset := Vector3.ZERO
    var docked_at := -1
    var started := Time.get_ticks_msec()
    while Time.get_ticks_msec() - started < 200000 and (docked_at < 0 or frame < docked_at + 120):
        await get_tree().process_frame
        var ship: Dictionary = main.entity_details[pick]
        var node: Node3D = main.entity_nodes[pick]
        var offset := (node.global_position - station.global_position) / float(main.get("ship_metres_to_world"))
        var step := offset.distance_to(last_offset)
        if docked_at < 0 and String(ship.get("phase", "")) != "in_transit":
            docked_at = frame
        # A step is a jump only when the ship is near the station (it covers ~30 km per frame
        # on approach even at 600x: arrivals are at speed, the burns are impulsive).
        if frame % 120 == 0 or (step > 60.0 and offset.length() < 5000.0 and last_offset.length() < 5000.0):
            print("[ShotTour] arrival t%+.0f s: %.0f m from the station centre, moved %.0f m this frame (%s)" % [
                float(main.get("display_time_s")) - float(ship.get("arrival_time_s", 0.0)), offset.length(), step, String(ship.get("phase", ""))])
        if frame % 90 == 0 and offset.length() < 6000.0:
            await _shoot("arrival_%04d" % frame)
        last_offset = offset
        frame += 1
    await _shoot("arrival_docked")
    main._on_timewarp_changed(86400.0)

# How far each planned path's ends are from its stations (a ship jumps by this much when
# it docks or leaves; it matters at true scale).
func _report_path_end_gaps() -> void:
    for ship_id in main.ship_paths.keys():
        var ship: Dictionary = main.entity_details.get(ship_id, {})
        var path: Dictionary = main.ship_paths[ship_id]
        var gaps := []
        for end in [["origin_station_id", float(path["departure"])], ["destination_station_id", float(path["arrival"])]]:
            var orbit: Dictionary = main.station_orbits.get(String(ship.get(end[0], "")), {})
            if orbit.is_empty():
                gaps.append(-1.0)
                continue
            var base: PackedFloat64Array = main._body_at_m(String(orbit["body"]), end[1])
            var p: PackedFloat64Array = main._rail_point_m(path, end[1])
            var dx: float = p[0] - (base[0] + cos(float(orbit["theta"])) * float(orbit["r"]))
            var dy: float = p[1] - base[1]
            var dz: float = p[2] - (base[2] + sin(float(orbit["theta"])) * float(orbit["r"]))
            gaps.append(sqrt(dx * dx + dy * dy + dz * dz))
        print("[ShotTour] path ends %s (%s): departure gap %.0f m, arrival gap %.0f m" % [ship_id, "helio" if bool(path.get("helio", false)) else "local", gaps[0], gaps[1]])

# A docked ship at its berth, close up, with its station behind.
func _docked_close_shot(rig: Node3D) -> void:
    for ship_id in main.entity_details.keys():
        if main.entity_kinds.get(ship_id, "") == "ship" and String(main.entity_details[ship_id].get("phase", "")) != "in_transit" \
                and main.entity_nodes.has(ship_id):
            main.select_entity(ship_id, "ship", true)
            rig.set("distance", maxf(main.map_icons.model_world_size(main.entity_nodes[ship_id]) * 2.5, float(rig.get("min_distance"))))
            rig.call("_update_transform")
            await get_tree().create_timer(1.5).timeout
            await _shoot("ship_docked_close")
            # Picking up close: the ship on its body off the icon, then its station's ring.
            var camera: Camera3D = rig.get("camera")
            var node: Node3D = main.entity_nodes[ship_id]
            var at := camera.unproject_position(node.global_position)
            var off := Vector2(main.map_icons.projected_model_pixels(node) * 0.6, 0.0)
            main._pick_entity(at + off)
            var on_ship := String(main.get("selected_id"))
            main.select_entity(String(main.entity_details[ship_id].get("current_station_id", "")), "station", true)
            rig.set("distance", main.map_icons.model_world_size(main.entity_nodes[main.get("selected_id")]) * 2.5)
            rig.call("_update_transform")
            await get_tree().create_timer(0.3).timeout
            var station: Node3D = main.entity_nodes[main.get("selected_id")]
            var ring_at := camera.unproject_position(station.global_position) + Vector2(main.map_icons.projected_model_pixels(station) * 0.7, 0.0)
            main.select_entity(ship_id, "ship")
            main._pick_entity(ring_at)
            print("[ShotTour] pick: ship body off the icon -> %s (want %s); inside the ring -> %s" % [on_ship, ship_id, String(main.get("selected_id"))])
            return

# Frame rate at the current view: mean and worst frame over two seconds.
func _report_fps(stop_name: String) -> void:
    var frames := 0
    var worst := 0.0
    var started := Time.get_ticks_usec()
    var last := started
    while Time.get_ticks_usec() - started < 2000000:
        await get_tree().process_frame
        var now := Time.get_ticks_usec()
        worst = maxf(worst, float(now - last) / 1000.0)
        last = now
        frames += 1
    print("[ShotTour] fps at %s: %.1f (worst frame %.1f ms)" % [stop_name, frames / (float(Time.get_ticks_usec() - started) / 1.0e6), worst])

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

# Loaded ships in flight, close up: the hold must show their cargo (prints the hold state).
func _cargo_shots(rig: Node3D) -> void:
    var shot := 0
    for ship_id in main.entity_details.keys():
        var ship: Dictionary = main.entity_details[ship_id]
        if main.entity_kinds.get(ship_id, "") != "ship" or String(ship.get("phase", "")) != "in_transit" \
                or (ship.get("cargo", []) as Array).is_empty():
            continue
        main.select_entity(ship_id, "ship", true)
        rig.set("distance", 1.5 * main.map_icons.model_world_size(main.entity_nodes[ship_id]))
        rig.call("_update_transform")
        await get_tree().create_timer(1.5).timeout
        var node: Node3D = main.entity_nodes[ship_id]
        var cargo := node.get_node_or_null("cargo")
        var vertices := 0
        if cargo is MeshInstance3D:
            for k in range((cargo as MeshInstance3D).mesh.get_surface_count()):
                vertices += (cargo as MeshInstance3D).mesh.surface_get_array_len(k)
        print("[ShotTour] cargo %s (%s): lod %d, lots %s, hold %s, cargo mesh %s, %d vertices" % [
            ship_id, String(ship.get("design_id", "")), int(node.get_meta("lod", -1)), str(ship.get("cargo")),
            str(node.get_meta("hold", {})), cargo != null, vertices])
        await _shoot("cargo_%s" % ship_id)
        if OS.get_cmdline_user_args().has("--shot-cargo-angles"):
            for k in range(4):
                rig.set("yaw", float(k) * PI * 0.5)
                rig.set("pitch", 0.5 if k % 2 == 0 else -0.5)
                rig.set("distance", 0.7 * main.map_icons.model_world_size(node))
                rig.call("_update_transform")
                await get_tree().create_timer(0.4).timeout
                await _shoot("cargo_%s_%d" % [ship_id, k])
        shot += 1
        if shot >= 4:
            break
    print("[ShotTour] cargo: %d loaded ships in flight shot" % shot)

# The station systems view in the inspector (display only: people, works and needs), for stations
# of each kind of site: scrolled so the people section's header is at the top of the panel.
func _station_systems_shots(rig: Node3D) -> void:
    for station_id in ["earth_l1", "earth_orbit", "mars_transfer", "ganymede_depot", "venus_cloud", "luna_base"]:
        if not main.entity_nodes.has(station_id):
            continue
        main.select_entity(station_id, "station", true)
        rig.set("distance", 0.004)
        rig.call("_update_transform")
        await get_tree().create_timer(0.6).timeout
        var scroll: ScrollContainer = main.inspector_panel.get("_scroll")
        var content: VBoxContainer = main.inspector_panel.get("_content")
        scroll.scroll_vertical = 0
        await get_tree().process_frame
        await get_tree().process_frame
        for child in content.get_children():
            if child is Button and String(child.text).contains("PEOPLE"):
                scroll.scroll_vertical = int(child.position.y)
                print("[ShotTour] %s: section at %.0f of %.0f px" % [station_id, child.position.y, content.size.y])
        await get_tree().process_frame
        await _shoot("station_systems_" + station_id)

# The inspector panel, page by page (cropped to the panel), for a docked and a flying ship of
# each drive and for a few stations: to judge how the panels read.
func _inspector_shots(rig: Node3D) -> void:
    var picks: Array = []
    for station_id in ["earth_orbit", "mars_transfer", "ganymede_depot"]:
        if main.entity_nodes.has(station_id):
            picks.append([station_id, "station"])
    var seen := {}
    for ship_id in main.entity_details.keys():
        if main.entity_kinds.get(ship_id, "") != "ship":
            continue
        var detail: Dictionary = main.entity_details[ship_id]
        var key := "%s/%s" % [String(detail.get("propulsion_type", "")),
            "flying" if String(detail.get("phase", "")) == "in_transit" else "docked"]
        if seen.has(key):
            continue
        seen[key] = true
        picks.append([ship_id, "ship"])
        if picks.size() >= 8:
            break
    # Every section open, also those folded by default.
    main.inspector_panel.set("_folded", {"station/works": false, "station/orders": false, "ship/money": false})
    for pick in picks:
        main.select_entity(pick[0], pick[1], true)
        rig.set("distance", 0.004)
        rig.call("_update_transform")
        await get_tree().create_timer(0.8).timeout
        var panel: Control = main.inspector_panel
        var scroll: ScrollContainer = panel.get("_scroll")
        var content: Control = panel.get("_content")
        var page := 0
        var y := 0
        while page < 6:
            scroll.scroll_vertical = y
            await get_tree().process_frame
            await get_tree().process_frame
            await get_tree().process_frame
            var image := get_viewport().get_texture().get_image()
            var rect := Rect2i(panel.get_global_rect())
            image = image.get_region(rect.intersection(Rect2i(Vector2i.ZERO, image.get_size())))
            var name := "inspector_%s_%s_%d" % [pick[1], pick[0], page]
            image.save_png(out_dir.path_join(name + ".png"))
            print("[ShotTour] saved %s (content %.0f x %.0f px, view %.0f x %.0f px)" % [name, content.size.x, content.size.y,
                scroll.size.x, scroll.size.y])
            if page == 0:
                for child in content.get_children():
                    if (child as Control).get_combined_minimum_size().x > scroll.size.x - 12.0:
                        print("[ShotTour]   wide: %s %s min %.0f" % [child.get_class(), String(child.get("text")) if child is Label or child is Button else "",
                            (child as Control).get_combined_minimum_size().x])
            y += int(scroll.size.y * 0.9)
            page += 1
            if y >= content.size.y - scroll.size.y * 0.1:
                break

# The Statistics screen, page by page: every tab, or only those listed (--shot-stats=3,4).
func _stats_shots(only: String) -> void:
    main.stats_panel.toggle()
    await get_tree().create_timer(2.5).timeout
    var panel: Control = main.stats_panel
    var scroll: ScrollContainer = panel.get("_scroll")
    var content: Control = panel.get("_content")
    for tab in range(int(panel.get("TABS").size())):
        if only != "" and not only.split(",").has(str(tab)):
            continue
        main.stats_panel.set_tab(tab)
        await get_tree().create_timer(0.8).timeout
        var y := 0
        var page := 0
        while page < 6:
            scroll.scroll_vertical = y
            await get_tree().process_frame
            await get_tree().process_frame
            await get_tree().process_frame
            var image := get_viewport().get_texture().get_image()
            var rect := Rect2i(panel.get_global_rect())
            image = image.get_region(rect.intersection(Rect2i(Vector2i.ZERO, image.get_size())))
            image.save_png(out_dir.path_join("stats_tab%d_%d.png" % [tab, page]))
            print("[ShotTour] saved stats_tab%d_%d (content %.0f px, view %.0f px)" % [tab, page, content.size.y, scroll.size.y])
            y += int(scroll.size.y * 0.9)
            page += 1
            if y >= content.size.y - scroll.size.y * 0.1:
                break
    main.stats_panel.toggle()

# The flow overlay (G) over the inner system and the Earth-Moon system.
func _flow_overlay_shots(rig: Node3D) -> void:
    var key := InputEventKey.new()
    key.keycode = KEY_G
    key.pressed = true
    Input.parse_input_event(key)
    await get_tree().create_timer(3.0).timeout
    print("[ShotTour] flow overlay: %d lines" % (main.map_icons.get("flow_lines") as Array).size())
    for stop in [["sun", "body", 30.0, "flows_inner"], ["earth", "body", 0.12, "flows_earth"]]:
        main.select_entity(stop[0], stop[1], true)
        rig.set("distance", stop[2])
        rig.call("_update_transform")
        await get_tree().create_timer(0.8).timeout
        await _shoot(stop[3])

# Porkchop plots: Earth to Mars by Δv, plasma effort and a class of each drive, Earth to Jupiter;
# then the launch windows in Low Earth Logistics' inspector. Prints how long the windows took.
func _routes_shots(rig: Node3D) -> void:
    var panel: Control = main.stats_panel
    var started := Time.get_ticks_msec()
    panel.show_route("earth", "mars")
    main._update_porkchop_request()
    var waited := 0.0
    while waited < 60.0:
        await get_tree().create_timer(0.5).timeout
        waited += 0.5
        var pair: Dictionary = panel.call("_route_pair")
        if (pair.get("columns", []) as Array).size() >= 96:
            break
    print("[ShotTour] earth>mars: %d columns after %.1f s" % [(panel.call("_route_pair").get("columns", []) as Array).size(),
        (Time.get_ticks_msec() - started) / 1000.0])
    var routes: Array = main.porkchop_data.get("routes", [])
    print("[ShotTour] %d routes in use, busiest %s" % [routes.size(), ", ".join(routes.slice(0, 6).map(func(r):
        return "%s>%s %.0f" % [r.get("origin", ""), r.get("destination", ""), float(r.get("trips", 0.0))]))])
    for view in ["fastest", "thermal", "plasma"]:
        panel.set("_route_view", view)
        panel.call("_rebuild")
        await get_tree().create_timer(0.5).timeout
        await _shoot("routes_earth_mars_" + view)
    for far in ["jupiter", "saturn"]:
        started = Time.get_ticks_msec()
        panel.show_route("earth", far)
        main._update_porkchop_request()
        waited = 0.0
        while waited < 150.0:
            await get_tree().create_timer(1.0).timeout
            waited += 1.0
            if (panel.call("_route_pair").get("columns", []) as Array).size() >= 96:
                break
        print("[ShotTour] earth>%s: %d columns after %.1f s" % [far, (panel.call("_route_pair").get("columns", []) as Array).size(),
            (Time.get_ticks_msec() - started) / 1000.0])
        for view in ["fastest", "plasma"]:
            panel.set("_route_view", view)
            panel.call("_rebuild")
            await get_tree().create_timer(0.5).timeout
            await _shoot("routes_earth_%s_%s" % [far, view])
    panel.toggle()
    main.inspector_panel.set("_folded", {"station/windows": false, "station/market": true, "station/people": true})
    main.select_entity("earth_orbit", "station", true)
    await get_tree().create_timer(20.0).timeout
    main._refresh_ui(true)
    await get_tree().create_timer(1.0).timeout
    var image := get_viewport().get_texture().get_image()
    var rect := Rect2i(main.inspector_panel.get_global_rect())
    image.get_region(rect.intersection(Rect2i(Vector2i.ZERO, image.get_size()))).save_png(out_dir.path_join("routes_inspector.png"))
    print("[ShotTour] saved routes_inspector")
