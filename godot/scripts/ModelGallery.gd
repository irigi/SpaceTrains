# Dev utility: renders the ship (and station) models one by one, for checking how they look
# without running the game. Each design is shot coarse and detailed, from three-quarter and
# top views, with its engine running and some cargo aboard; then a contact sheet.
#
# Run (needs a designs file, e.g. from `spacetrains_headless --snapshot-json <file>`, which
# writes <file>.designs next to it):
#   godot4 --path godot -s res://scripts/ModelGallery.gd -- --designs=<file.designs> --out=<dir>
# Options: --only=<id,id,prefix*> (just these designs), --stations=<snapshot.json>,
# --thermal (everything in the thermal view). Plasma ships also get a thermal-view shot,
# and the bulk carriers and the light freighter half-full and empty shots.
extends SceneTree

const ShipModels := preload("res://scripts/ShipModels.gd")
const Materials := preload("res://scripts/ModelMaterials.gd")
const StationModels := preload("res://scripts/StationModels.gd")

const SHOT := Vector2i(640, 400)

var _viewport: SubViewport
var _camera: Camera3D
var _stage: Node3D

func _initialize() -> void:
    _run.call_deferred()

func _arg(name: String, fallback := "") -> String:
    for arg in OS.get_cmdline_user_args():
        if arg.begins_with("--%s=" % name):
            return arg.trim_prefix("--%s=" % name)
    return fallback

func _setup() -> void:
    _viewport = SubViewport.new()
    _viewport.size = SHOT
    _viewport.own_world_3d = true
    _viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
    _viewport.msaa_3d = Viewport.MSAA_4X
    root.add_child(_viewport)
    _stage = Node3D.new()
    _viewport.add_child(_stage)
    # The game's lighting: one sun-coloured light, a dim ambient, ACES and bloom.
    var sun := DirectionalLight3D.new()
    sun.light_energy = 1.9
    sun.light_color = Color(1.0, 0.96, 0.82)
    sun.rotation = Vector3(deg_to_rad(-40.0), deg_to_rad(-30.0), 0.0)
    _viewport.add_child(sun)
    var env := Environment.new()
    env.background_mode = Environment.BG_COLOR
    env.background_color = Color(0.01, 0.012, 0.025, 1.0)
    env.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
    env.ambient_light_color = Color(0.16, 0.17, 0.22, 1.0)
    env.ambient_light_energy = 0.55
    env.tonemap_mode = Environment.TONE_MAPPER_ACES
    env.glow_enabled = true
    env.glow_intensity = 0.6
    env.glow_bloom = 0.15
    env.glow_hdr_threshold = 1.1
    env.glow_blend_mode = Environment.GLOW_BLEND_MODE_ADDITIVE
    var world_env := WorldEnvironment.new()
    world_env.environment = env
    _viewport.add_child(world_env)
    _camera = Camera3D.new()
    _camera.fov = 40.0
    _camera.near = 0.05
    _camera.far = 5000.0
    _viewport.add_child(_camera)
    var fill := DirectionalLight3D.new()
    fill.light_cull_mask = Materials.FILL_LAYER
    fill.light_energy = Materials.FILL_ENERGY
    fill.light_color = Color(0.85, 0.9, 1.0)
    _camera.add_child(fill)

func _frame(radius: float, view: String, length := 0.0) -> void:
    var direction := Vector3(0.75, 0.45, 0.5).normalized()
    if view == "bow":
        # Close on the habitat end.
        direction = Vector3(-0.7, 0.45, -0.6).normalized()
        var target := Vector3(0.0, 0.0, -length * 0.5 + 6.0)
        _camera.transform = Transform3D(Basis.looking_at(-direction, Vector3.UP), target + direction * 16.0)
        return
    var up := Vector3.UP
    if view == "top":
        # Like the schematic: from above, bow to the left.
        direction = Vector3(0.0, 1.0, 0.0)
        up = Vector3(1.0, 0.0, 0.0)
    elif view == "stern":
        direction = Vector3(0.35, 0.3, 1.0).normalized()
    var distance := radius / sin(deg_to_rad(_camera.fov * 0.5)) * 1.05
    _camera.transform = Transform3D(Basis.looking_at(-direction, up), direction * distance)

# Close on the cargo hold, from above one side.
func _frame_hold(hold: Dictionary, direction := Vector3(0.8, 0.6, 0.35)) -> void:
    direction = direction.normalized()
    var target := Vector3(0.0, 0.0, (float(hold["z0"]) + float(hold["z1"])) * 0.5)
    var size := maxf(float(hold["width"]), float(hold["z1"]) - float(hold["z0"]))
    _camera.transform = Transform3D(Basis.looking_at(-direction, Vector3.UP), target + direction * size * 2.2)

func _shoot(path: String) -> Image:
    await RenderingServer.frame_post_draw
    await RenderingServer.frame_post_draw
    var image := _viewport.get_texture().get_image()
    image.save_png(path)
    return image

# fill: the share of the hold in use (metals, water and electronics at 35/25/15 of 75%).
func _ship_data(design: Dictionary, fill := 1.0) -> Dictionary:
    var temperature := 0.0
    for c in design.get("components", []):
        if String(c.get("kind", "")) == "radiator":
            temperature = float(c.get("temperature_k", 0.0))
    var capacity := float(design.get("cargo_capacity_units", 100.0))
    return {"radiator_temperature_k": temperature, "radiator_condition": 0.8, "cargo_capacity_units": capacity,
        "cargo": [{"commodity_id": "metals", "units": capacity * 0.35 * fill}, {"commodity_id": "water", "units": capacity * 0.25 * fill},
            {"commodity_id": "electronics", "units": capacity * 0.15 * fill}]}

func _run() -> void:
    var out := _arg("out", "/tmp/gallery")
    DirAccess.make_dir_recursive_absolute(out)
    _setup()
    Materials.set_thermal_view(_arg("thermal", "") != "" or OS.get_cmdline_user_args().has("--thermal"))
    var json := JSON.new()
    if json.parse(FileAccess.get_file_as_string(_arg("designs"))) != OK:
        print("[Gallery] cannot read --designs")
        quit(1)
        return
    var designs: Dictionary = json.data.get("designs", {})
    var only := _arg("only")
    var picked: Array[String] = []
    var modules_seen := {}
    for id in designs.keys():
        var key := String(id)
        if only != "":
            for part in only.split(","):
                if key == part or (part.ends_with("*") and key.begins_with(part.trim_suffix("*"))):
                    picked.append(key)
                    break
            continue
        if key.contains("+"):
            var module := key.get_slice("+", 1)
            if not modules_seen.has(module):
                modules_seen[module] = true
                picked.append(key)
        elif not RegEx.create_from_string("_tank\\d+$").search(key):
            picked.append(key)
    var tiles: Array[Image] = []
    for key in picked:
        var design: Dictionary = designs[key]
        var plasma := String(design.get("propulsion_type", "")) == "variable_isp"
        for lod in [ShipModels.COARSE, ShipModels.DETAIL]:
            var container := Node3D.new()
            _stage.add_child(container)
            ShipModels.ensure(container, key, design, lod)
            var data := _ship_data(design)
            ShipModels.update_live(container, data, true, plasma, 18.0 if not plasma else 26.0)
            var radius := float(container.get_meta("radius_m", 30.0))
            var name := "%s_%s" % [key.replace("+", "_"), "coarse" if lod == ShipModels.COARSE else "detail"]
            _frame(radius, "three_quarter")
            var image := await _shoot(out.path_join(name + ".png"))
            tiles.append(image)
            if lod == ShipModels.DETAIL:
                _frame(radius, "top")
                await _shoot(out.path_join(name + "_top.png"))
                _frame(radius, "stern")
                await _shoot(out.path_join(name + "_stern.png"))
                _frame(radius, "bow", float(container.get_meta("length_m", 40.0)))
                await _shoot(out.path_join(name + "_bow.png"))
                var hold: Dictionary = container.get_meta("hold", {})
                if not hold.is_empty():
                    _frame_hold(hold)
                    await _shoot(out.path_join(name + "_hold.png"))
                    _frame_hold(hold, Vector3(1.0, 0.05, 0.02))
                    await _shoot(out.path_join(name + "_hold_side.png"))
                if key in ["bulk_tanker", "plasma_bulk_tanker", "light_freighter"] and not hold.is_empty():
                    for fill in [0.5, 0.0]:
                        ShipModels.update_live(container, _ship_data(design, fill), true, plasma, 18.0 if not plasma else 26.0)
                        _frame_hold(hold)
                        await _shoot(out.path_join("%s_hold%02d.png" % [name, int(fill * 100.0)]))
                    ShipModels.update_live(container, data, true, plasma, 18.0 if not plasma else 26.0)
                if plasma and not Materials.thermal_view:
                    Materials.set_thermal_view(true)
                    ShipModels.update_live(container, data, true, plasma, 26.0)
                    _frame(radius, "three_quarter")
                    await _shoot(out.path_join(name + "_thermal.png"))
                    Materials.set_thermal_view(false)
            container.queue_free()
            await process_frame
        print("[Gallery] %s: radius %.1f m" % [key, float(ShipModels.model_info(key, design, ShipModels.DETAIL)["radius_m"])])
    var stations_path := _arg("stations")
    if stations_path != "" and json.parse(FileAccess.get_file_as_string(stations_path)) == OK:
        var ships: Array = []
        for key in ["light_freighter", "plasma_bulk_tanker", "deep_space_freighter", "luna_ferry"]:
            if designs.has(key):
                ships.append([key, designs[key]])
        for station in json.data.get("stations", []):
            var container := Node3D.new()
            _stage.add_child(container)
            StationModels.ensure(container, station)
            Materials.set_band(Materials.instance_materials(container)["band"], Color(0.3, 0.55, 0.9))
            # A few docked ships, as the map places them.
            var berths: Array = container.get_meta("berths")
            for i in range(mini(ships.size() * 2, berths.size())):
                var pick: Array = ships[i % ships.size()]
                var ship := Node3D.new()
                container.add_child(ship)
                ShipModels.ensure(ship, String(pick[0]), pick[1], ShipModels.DETAIL)
                var berth: Dictionary = berths[i]
                var dir: Vector3 = berth["dir"]
                ship.transform = Transform3D(Basis.looking_at(-dir, Vector3(0, 0, 1)),
                    (berth["pos"] as Vector3) + dir * (float(ship.get_meta("length_m", 50.0)) * 0.5 + 3.0))
            var radius := float(container.get_meta("radius_m", 100.0))
            var name := "station_%s" % String(station.get("id", ""))
            _frame(radius * 1.1, "three_quarter")
            tiles.append(await _shoot(out.path_join(name + ".png")))
            _frame(radius * 0.6, "three_quarter")
            await _shoot(out.path_join(name + "_close.png"))
            container.queue_free()
            await process_frame
            print("[Gallery] %s (%s): radius %.0f m" % [String(station.get("id", "")), String(station.get("economy_profile_id", "")), radius])
    _contact_sheet(tiles, out.path_join("contact_sheet.png"))
    quit()

func _contact_sheet(tiles: Array[Image], path: String) -> void:
    if tiles.is_empty():
        return
    var columns := 4
    var scale := 0.5
    var w := int(SHOT.x * scale)
    var h := int(SHOT.y * scale)
    var rows := int(ceil(float(tiles.size()) / float(columns)))
    var sheet := Image.create(w * columns, h * rows, false, Image.FORMAT_RGBA8)
    sheet.fill(Color(0.01, 0.012, 0.025, 1.0))
    for i in range(tiles.size()):
        var tile: Image = tiles[i].duplicate()
        tile.convert(Image.FORMAT_RGBA8)
        tile.resize(w, h, Image.INTERPOLATE_BILINEAR)
        sheet.blit_rect(tile, Rect2i(0, 0, w, h), Vector2i((i % columns) * w, (i / columns) * h))
    sheet.save_png(path)
