extends RefCounted

# 3D ship models built from a design's components, with the inspector schematic's layout
# (ShipLayout.gd): the same parts in the same order and proportions, in metres. The bow
# points to -Z (look_at() aims ships along their velocity), the radiator wings lie along
# ±X, and the model is centred on its length.
#
# Two levels of detail, each one cached mesh per design (MeshBuilder: one surface per
# material slot, so a ship is a few draw calls however detailed):
# - COARSE (mid zoom): about a dozen primitives, enough to tell a bulk tanker from a
#   courier at 40 px.
# - DETAIL (close up): truss lattice, domed habitat with windows, docking port, antenna
#   and RCS, open hold frame with cargo, tank clusters, shadow shield, reactor and bell
#   nozzle, or plasma drive with magnetic-nozzle coils and radiator wings on booms.
# Live state per ship: radiators in false colour while thrusting and dark where worn, a hot
# nozzle and a plume while the engine runs, and cargo containers in the hold (DETAIL).

const MeshBuilder := preload("res://scripts/MeshBuilder.gd")
const ShipLayout := preload("res://scripts/ShipLayout.gd")
const Materials := preload("res://scripts/ModelMaterials.gd")
const UiTheme := preload("res://scripts/ui/UiTheme.gd")

const COARSE := 0
const DETAIL := 1
const HAB_RADIUS_M := ShipLayout.HAB_DIAMETER_M * 0.5

static var _cache := {}
# Detailed meshes take ~25 ms to build, so they are built on worker threads: cache key ->
# WorkerThreadPool task id, and the finished arrays (under _mutex) until the main thread
# turns them into meshes.
static var _pending := {}
static var _finished := {}
static var _mutex := Mutex.new()
static var _plume_meshes := {}
static var _cargo_mesh: BoxMesh

# --- Cached meshes ----------------------------------------------------------------

# {mesh, slots, radius_m, length_m, stern_z, nozzle_r, plasma, hold: {z0, z1, width, enclosed}}
static func model_info(key: String, design: Dictionary, lod: int) -> Dictionary:
    var cache_key := "%s|%d" % [key, lod]
    if not _cache.has(cache_key):
        _cache[cache_key] = _finish(_build_arrays(design, lod))
    return _cache[cache_key]

static func is_cached(key: String, lod: int) -> bool:
    return _cache.has("%s|%d" % [key, lod])

# True once the model is ready; otherwise starts (or keeps waiting for) a background build.
static func request(key: String, design: Dictionary, lod: int) -> bool:
    var cache_key := "%s|%d" % [key, lod]
    if _cache.has(cache_key):
        return true
    if _pending.has(cache_key):
        var task: int = _pending[cache_key]
        if not WorkerThreadPool.is_task_completed(task):
            return false
        WorkerThreadPool.wait_for_task_completion(task)
        _pending.erase(cache_key)
        _mutex.lock()
        var arrays: Dictionary = _finished.get(cache_key, {})
        _finished.erase(cache_key)
        _mutex.unlock()
        if arrays.is_empty():
            return false
        _cache[cache_key] = _finish(arrays)
        return true
    var copy := design.duplicate(true)
    _pending[cache_key] = WorkerThreadPool.add_task(func() -> void:
        var arrays := _build_arrays(copy, lod)
        _mutex.lock()
        _finished[cache_key] = arrays
        _mutex.unlock())
    return false

# A stand-in design until the designs file has been read: a habitat, hold, tanks and an
# engine of a typical freighter (with radiators for a plasma ship).
static func fallback_design(propulsion: String) -> Dictionary:
    var components: Array = [
        {"id": "habitat", "kind": "habitat", "volume_m3": 145.0, "mass_kg": 24000.0},
        {"id": "hold", "kind": "hold", "volume_m3": 200.0},
        {"id": "tanks", "kind": "tank", "volume_m3": 630.0, "temperature_k": 20.0},
        {"id": "engine", "kind": "engine", "mass_kg": 2400.0},
    ]
    if propulsion == "variable_isp":
        components[2] = {"id": "tanks", "kind": "tank", "volume_m3": 60.0, "temperature_k": 293.0}
        components[3] = {"id": "engine", "kind": "engine", "mass_kg": 14000.0}
        components.append({"id": "radiators", "kind": "radiator", "area_m2": 600.0})
    return {"components": components, "propulsion_type": propulsion}

# --- Building ---------------------------------------------------------------------

static func _finish(arrays: Dictionary) -> Dictionary:
    var info := arrays.duplicate()
    var built := MeshBuilder.to_mesh(arrays["built"])
    info.erase("built")
    info["mesh"] = built["mesh"]
    info["slots"] = built["slots"]
    return info

# Everything but the engine resources (safe on a worker thread).
static func _build_arrays(design: Dictionary, lod: int) -> Dictionary:
    var layout := ShipLayout.layout(design)
    var components: Dictionary = layout["components"]
    var b := MeshBuilder.new()
    var detail := lod == DETAIL
    var ship_end: float = layout["ship_end"]
    var mid := ship_end * 0.5
    var hold_info := {}
    var nozzle_r := 1.0
    var jacketed := components.has("module.tank_wrap") or components.has("module.cabin_water_wall")
    var crew_band := Rect2()
    for shape in layout["shapes"]:
        if String(shape["key"]) == "habitat.crew_quarters":
            crew_band = shape["rect"]
    for shape in layout["shapes"]:
        var r: Rect2 = shape["rect"]
        var comp: Dictionary = shape["comp"]
        # Layout x (bow 0 to stern) becomes z (bow at -Z); layout y becomes x.
        var z0 := r.position.x - mid
        var z1 := r.end.x - mid
        var width := r.size.y
        match String(shape["kind"]):
            "structure":
                if detail:
                    _truss(b, z0, z1)
                else:
                    b.box("truss", Vector3(0.7, 0.7, z1 - z0), _at(0.0, 0.0, (z0 + z1) * 0.5))
            "habitat":
                _habitat(b, z0, z1, detail, crew_band, mid)
            "shelter":
                if detail:
                    b.lathe("silver", PackedVector2Array([Vector2(z0, HAB_RADIUS_M), Vector2(z0, HAB_RADIUS_M + 0.22),
                        Vector2(z1, HAB_RADIUS_M + 0.22), Vector2(z1, HAB_RADIUS_M)]), 20)
            "cabin":
                _cabin(b, z0, z1, width * 0.5, detail, jacketed)
            "tank_jacket":
                b.vessel("tank", z0, z1, width * 0.5, width * 0.15, 24 if detail else 10)
            "cabin_wall":
                b.vessel("steel", z0, z1, width * 0.5, width * 0.15, 24 if detail else 10)
            "hold":
                var enclosed := components.has("module.cold_hold")
                hold_info = {"z0": z0, "z1": z1, "width": width, "enclosed": enclosed}
                if detail:
                    _hold_frame(b, z0, z1, width)
                else:
                    b.box("truss", Vector3(width, width, z1 - z0) * 0.98, _at(0.0, 0.0, (z0 + z1) * 0.5))
            "liner":
                b.box("silver", Vector3(width, width, z1 - z0), _at(0.0, 0.0, (z0 + z1) * 0.5))
            "tank":
                _tanks(b, z0, z1, width * 0.5, float(comp.get("temperature_k", 293.0)) < 100.0, detail)
            "tank_cooling":
                _tank_cooling(b, String(shape["key"]), z0, z1, float(layout["tank_width"]) * 0.5, detail)
            "shield":
                b.lathe("steel", PackedVector2Array([Vector2(z0, 0.0), Vector2(z0, width * 0.5),
                    Vector2(z1, width * 0.18), Vector2(z1, 0.0)]), 20 if detail else 10)
            "engine":
                if bool(layout["plasma"]):
                    nozzle_r = _plasma_engine(b, z0, z1, detail)
                else:
                    nozzle_r = _thermal_engine(b, z0, z1, detail)
            "wing":
                _wing(b, r, mid, detail)
    var built := b.commit_arrays()
    var aabb: AABB = built["aabb"]
    var radius := 0.0
    for i in range(8):
        radius = maxf(radius, aabb.get_endpoint(i).length())
    return {"built": built, "radius_m": radius, "length_m": ship_end,
        "stern_z": ship_end - mid, "nozzle_r": nozzle_r, "plasma": bool(layout["plasma"]), "hold": hold_info}

static func _at(x: float, y: float, z: float) -> Transform3D:
    return Transform3D(Basis.IDENTITY, Vector3(x, y, z))

# A basis whose X points along `radial` (in the XY plane) and Z along the ship.
static func _radial_basis(angle: float) -> Basis:
    var radial := Vector3(cos(angle), sin(angle), 0.0)
    var tangent := Vector3(-sin(angle), cos(angle), 0.0)
    return Basis(radial, tangent, Vector3(0.0, 0.0, 1.0))

# Triangular lattice truss along the ship's axis.
static func _truss(b: MeshBuilder, z0: float, z1: float) -> void:
    var radius := 0.5
    var bays := maxi(1, int((z1 - z0) / 1.6))
    var corners: Array[Vector2] = []
    for i in range(3):
        var a := PI * 0.5 + TAU * float(i) / 3.0
        corners.append(Vector2(cos(a), sin(a)) * radius)
    for i in range(3):
        b.beam("truss", Vector3(corners[i].x, corners[i].y, z0), Vector3(corners[i].x, corners[i].y, z1), 0.09)
    for j in range(bays + 1):
        var z := lerpf(z0, z1, float(j) / float(bays))
        for i in range(3):
            var c0 := corners[i]
            var c1 := corners[(i + 1) % 3]
            b.beam("truss", Vector3(c0.x, c0.y, z), Vector3(c1.x, c1.y, z), 0.06)
            if j < bays:
                var zn := lerpf(z0, z1, float(j + 1) / float(bays))
                b.beam("truss", Vector3(c0.x, c0.y, z), Vector3(c1.x, c1.y, zn), 0.05)

static func _habitat(b: MeshBuilder, z0: float, z1: float, detail: bool, crew_band: Rect2, mid: float) -> void:
    var radius := HAB_RADIUS_M
    var dome := minf(radius * 0.55, (z1 - z0) * 0.3)
    if not detail:
        b.vessel("blanket", z0, z1, radius, dome, 10)
        b.cylinder("band", z1 - dome - 0.9, z1 - dome - 0.2, radius + 0.05, 10)
        return
    b.vessel("blanket", z0, z1, radius, dome, 24)
    # Faction band near the back of the habitat.
    b.cylinder("band", z1 - dome - 0.9, z1 - dome - 0.2, radius + 0.04, 24)
    # Windows round the crew quarters, two rows.
    var zc := (z0 + z1) * 0.5
    if crew_band.size.x > 0.0:
        zc = crew_band.get_center().x - mid
    for row in [-0.35, 0.35]:
        for k in range(12):
            var a := TAU * (float(k) + 0.5) / 12.0
            var basis := _radial_basis(a)
            b.box("window", Vector3(0.05, 0.42, 0.34), Transform3D(basis, Vector3(cos(a), sin(a), 0.0) * (radius + 0.01) + Vector3(0.0, 0.0, zc + row)))
    # Docking port and its ring at the bow.
    b.cylinder("metal", z0 - 0.8, z0 + dome * 0.5, 0.55, 14)
    b.torus("steel", z0 - 0.8, 0.62, 0.09, Transform3D.IDENTITY, 16, 5)
    # High-gain antenna on a mast, dish facing aft and up.
    var mast_z := zc
    b.beam("truss", Vector3(0.0, radius, mast_z), Vector3(0.0, radius + 1.5, mast_z), 0.1)
    # Basis.looking_at aims -Z; the dish opens along its +Z, aft and up.
    var dish_basis := Basis.looking_at(-Vector3(0.0, 0.6, 0.8).normalized(), Vector3.UP)
    var dish_xf := Transform3D(dish_basis, Vector3(0.0, radius + 1.5, mast_z))
    var dish := PackedVector2Array()
    for i in range(6):
        var rr := 0.9 * float(i) / 5.0
        dish.append(Vector2(0.28 * (rr / 0.9) * (rr / 0.9), rr))
    b.lathe("blanket", dish, 16, dish_xf)
    b.lathe("blanket", dish, 16, dish_xf, true)
    # RCS quads at the back of the habitat.
    for k in range(4):
        var a := PI * 0.25 + PI * 0.5 * float(k)
        b.box("metal", Vector3(0.35, 0.35, 0.35), Transform3D(_radial_basis(a), Vector3(cos(a), sin(a), 0.0) * (radius + 0.15) + Vector3(0.0, 0.0, z1 - dome - 1.4)))

static func _cabin(b: MeshBuilder, z0: float, z1: float, radius: float, detail: bool, jacketed: bool) -> void:
    var dome := minf(radius * 0.3, (z1 - z0) * 0.25)
    b.vessel("blanket", z0, z1, radius, dome, 24 if detail else 10)
    if not detail or jacketed:
        return
    var rows := maxi(1, int((z1 - z0 - 2.0 * dome) / 1.4))
    for side in [0.0, PI]:
        for degrees in [-50.0, -20.0, 20.0, 50.0]:
            var a: float = side + deg_to_rad(degrees)
            var basis := _radial_basis(a)
            for i in range(rows):
                var z := lerpf(z0 + dome + 0.7, z1 - dome - 0.7, (float(i) + 0.5) / float(rows)) if rows > 1 else (z0 + z1) * 0.5
                b.box("window", Vector3(0.05, 0.45, 0.7), Transform3D(basis, Vector3(cos(a), sin(a), 0.0) * (radius + 0.01) + Vector3(0.0, 0.0, z)))

# Open frame: four longerons, a square frame at every bay and a diagonal on each side.
static func _hold_frame(b: MeshBuilder, z0: float, z1: float, width: float) -> void:
    var h := width * 0.5
    var t := clampf(width * 0.03, 0.1, 0.3)
    var bays := maxi(1, int(round((z1 - z0) / maxf(width * 0.8, 2.0))))
    var corners := [Vector2(h, h), Vector2(-h, h), Vector2(-h, -h), Vector2(h, -h)]
    for c in corners:
        b.beam("truss", Vector3(c.x, c.y, z0), Vector3(c.x, c.y, z1), t)
    for j in range(bays + 1):
        var z := lerpf(z0, z1, float(j) / float(bays))
        for i in range(4):
            var c0: Vector2 = corners[i]
            var c1: Vector2 = corners[(i + 1) % 4]
            b.beam("truss", Vector3(c0.x, c0.y, z), Vector3(c1.x, c1.y, z), t)
            if j < bays:
                var zn := lerpf(z0, z1, float(j + 1) / float(bays))
                var from := c0 if j % 2 == 0 else c1
                var to := c1 if j % 2 == 0 else c0
                b.beam("truss", Vector3(from.x, from.y, z), Vector3(to.x, to.y, zn), t * 0.6)

# Propellant tanks: one vessel, or for a wide one a cluster of three of the same total
# cross-section (the volume is kept; the cluster is about 25% wider than the schematic).
static func _tanks(b: MeshBuilder, z0: float, z1: float, radius: float, cryogenic: bool, detail: bool) -> void:
    var slot := "tank" if cryogenic else "steel"
    if not detail:
        b.vessel(slot, z0, z1, radius, radius * 0.4, 10)
        return
    if radius * 2.0 >= 6.0:
        var small := radius / sqrt(3.0)
        var spread := 2.0 * small / sqrt(3.0)
        for k in range(3):
            var a := PI * 0.5 + TAU * float(k) / 3.0
            var xf := _at(cos(a) * spread, sin(a) * spread, 0.0)
            b.vessel(slot, z0, z1, small, small * 0.6, 18, xf)
            _tank_rings(b, z0, z1, small, small * 0.6, xf)
    else:
        b.vessel(slot, z0, z1, radius, radius * 0.5, 22)
        _tank_rings(b, z0, z1, radius, radius * 0.5, Transform3D.IDENTITY)

static func _tank_rings(b: MeshBuilder, z0: float, z1: float, radius: float, dome: float, xf: Transform3D) -> void:
    var span := z1 - z0 - 2.0 * dome
    var rings := int(span / 4.0)
    for i in range(1, rings + 1):
        var z := z0 + dome + span * float(i) / float(rings + 1)
        b.lathe("truss", PackedVector2Array([Vector2(z - 0.12, radius), Vector2(z - 0.12, radius + 0.07),
            Vector2(z + 0.12, radius + 0.07), Vector2(z + 0.12, radius)]), 18, xf)

static func _tank_cluster_radius(radius: float) -> float:
    return radius * 1.25 if radius * 2.0 >= 6.0 else radius

static func _tank_cooling(b: MeshBuilder, key: String, z0: float, z1: float, tank_radius: float, detail: bool) -> void:
    var outer := _tank_cluster_radius(tank_radius) + 0.4
    if key == "module.boiloff_shield":
        # An aluminised sunshade wrapped round the tanks.
        b.lathe("silver", PackedVector2Array([Vector2(z0 + 0.3, outer - 0.1), Vector2(z0 + 0.3, outer),
            Vector2(z1 - 0.3, outer), Vector2(z1 - 0.3, outer - 0.1)]), 24 if detail else 10)
    elif detail:
        # Cryocooler radiator fins along the tanks.
        for k in range(4):
            var a := PI * 0.25 + PI * 0.5 * float(k)
            var basis := _radial_basis(a)
            b.box("steel", Vector3(1.6, 0.08, (z1 - z0) * 0.6), Transform3D(basis, Vector3(cos(a), sin(a), 0.0) * (outer + 0.7) + Vector3(0.0, 0.0, (z0 + z1) * 0.5)))

# Reactor in gold insulation, then a bell nozzle. Returns the nozzle exit radius.
static func _thermal_engine(b: MeshBuilder, z0: float, z1: float, detail: bool) -> float:
    var length := z1 - z0
    var height := length * 0.8
    var reactor_r := 0.35 * height
    var z_throat := z0 + 0.55 * length
    var z_conv := z0 + 0.45 * length
    var throat_r := 0.14 * height
    var exit_r := 0.5 * height
    var segments := 24 if detail else 10
    b.vessel("foil", z0, z_conv, reactor_r, reactor_r * 0.3, segments)
    b.lathe("metal", PackedVector2Array([Vector2(z_conv, 0.0), Vector2(z_conv, reactor_r * 0.85), Vector2(z_throat, throat_r)]), segments)
    var bell := PackedVector2Array()
    var steps := 8 if detail else 3
    for i in range(steps + 1):
        var s := float(i) / float(steps)
        bell.append(Vector2(lerpf(z_throat, z1, s), throat_r + (exit_r - throat_r) * sqrt(s)))
    b.lathe("metal", bell, segments)
    if detail:
        # The inside of the bell, which glows while the engine burns.
        var inner := PackedVector2Array()
        for p in bell:
            inner.append(Vector2(p.x, p.y * 0.97))
        b.lathe("nozzle_hot", inner, segments, Transform3D.IDENTITY, true)
        # Turbopumps beside the reactor.
        for side in [-1.0, 1.0]:
            b.cylinder("metal", z0 + 0.15 * length, z_conv, 0.12 * height, 10, _at(side * (reactor_r + 0.18 * height), 0.0, 0.0))
    else:
        b.lathe("nozzle_hot", bell, segments, Transform3D.IDENTITY, true)
    return exit_r

# Drive block in gold insulation, a glowing throat and the magnetic nozzle's field coils.
# Returns the nozzle exit radius.
static func _plasma_engine(b: MeshBuilder, z0: float, z1: float, detail: bool) -> float:
    var length := z1 - z0
    var height := length * 0.8
    var z_throat := z0 + 0.6 * length
    b.box("foil", Vector3(0.7 * height, 0.7 * height, 0.6 * length), _at(0.0, 0.0, (z0 + z_throat) * 0.5))
    b.vessel("nozzle_hot", z_throat - 0.1 * height, z_throat + 0.1 * height, 0.12 * height, 0.1 * height, 12 if detail else 6)
    var last_r := 0.0
    var last_z := z_throat
    for i in range(3):
        var t := 0.25 + 0.3 * float(i)
        var z := z_throat + (z1 - z_throat) * t
        var ring_r := height * (0.25 + 0.25 * t)
        b.torus("copper", z, ring_r, 0.05 * height, Transform3D.IDENTITY, 24 if detail else 12, 6 if detail else 4)
        last_r = ring_r
        last_z = z
    if detail:
        for k in range(4):
            var a := PI * 0.25 + PI * 0.5 * float(k)
            var corner := Vector3(cos(a), sin(a), 0.0)
            b.beam("truss", corner * 0.3 * height * sqrt(2.0) + Vector3(0.0, 0.0, z_throat), corner * last_r + Vector3(0.0, 0.0, last_z), 0.04 * height)
    return last_r

# A radiator wing (one of two): the panel, a boom from the truss and the manifold at its
# root. UV.x runs root to tip for the radiator shader.
static func _wing(b: MeshBuilder, r: Rect2, mid: float, detail: bool) -> void:
    var side := 1.0 if r.position.y > 0.0 else -1.0
    var span := r.size.y
    var length := r.size.x
    var zc := r.get_center().x - mid
    var xc := r.get_center().y
    b.box("radiator", Vector3(span, 0.12, length), _at(xc, 0.0, zc), side < 0.0)
    var root_x := side * minf(absf(r.position.y), absf(r.end.y))
    b.beam("truss", Vector3(0.0, 0.0, zc), Vector3(root_x, 0.0, zc), 0.3)
    if not detail:
        return
    for f in [-0.3, 0.3]:
        b.beam("truss", Vector3(0.0, 0.0, zc + f * length), Vector3(root_x, 0.0, zc), 0.15)
    b.beam("steel", Vector3(root_x, 0.0, zc - length * 0.5), Vector3(root_x, 0.0, zc + length * 0.5), 0.28)

# --- Nodes ------------------------------------------------------------------------

# Puts the model for `key` at `lod` into the ship's container (a child "model"), with the
# shared slot materials and the ship's own band, radiator and nozzle materials. Cheap when
# nothing changed.
static func ensure(container: Node3D, key: String, design: Dictionary, lod: int) -> void:
    if String(container.get_meta("model_key", "")) == key and int(container.get_meta("lod", -1)) == lod:
        return
    var info := model_info(key, design, lod)
    var model := container.get_node_or_null("model") as MeshInstance3D
    if model == null:
        model = MeshInstance3D.new()
        model.name = "model"
        container.add_child(model)
    Materials.apply_mesh(model, info, container)
    container.set_meta("model_key", key)
    container.set_meta("lod", lod)
    container.set_meta("radius_m", info["radius_m"])
    container.set_meta("length_m", info["length_m"])
    container.set_meta("stern_z", info["stern_z"])
    container.set_meta("nozzle_r", info["nozzle_r"])
    container.set_meta("hold", info["hold"])
    container.set_meta("live_sig", -1)
    var glow := container.get_node_or_null("engine_glow") as Node3D
    if glow != null:
        glow.position = Vector3(0.0, 0.0, float(info["stern_z"]) + 2.0)
    var plume := container.get_node_or_null("plume") as Node3D
    if plume != null and lod != DETAIL:
        plume.visible = false
    var cargo := container.get_node_or_null("cargo") as Node3D
    if cargo != null:
        cargo.queue_free()
        container.remove_child(cargo)
    container.set_meta("cargo_sig", "")

static func instance_materials(container: Node3D) -> Dictionary:
    return Materials.instance_materials(container)

# Live state, every frame (cheap: materials are only touched when something changed).
# engine_on: burning (thermal) or thrusting (plasma); plume_m: plume length in metres.
static func update_live(container: Node3D, data: Dictionary, engine_on: bool, plasma: bool, plume_m: float) -> void:
    var lod := int(container.get_meta("lod", COARSE))
    var temperature := float(data.get("radiator_temperature_k", 0.0))
    var lost := 1.0 - float(data.get("radiator_condition", 1.0))
    var sig := int(engine_on) | (int(plasma) << 1) | (lod << 2) | (int(temperature) << 4) | (int(lost * 100.0) << 24)
    if int(container.get_meta("live_sig", -1)) != sig:
        container.set_meta("live_sig", sig)
        var own := instance_materials(container)
        Materials.set_radiator(own["radiator"], engine_on and plasma, temperature, lost)
        if engine_on:
            Materials.set_nozzle(own["nozzle_hot"], Color(0.55, 0.62, 1.0) if plasma else Color(1.0, 0.55, 0.22), 2.2)
        else:
            Materials.set_nozzle(own["nozzle_hot"], Color.BLACK, 0.0)
    _update_plume(container, engine_on and lod == DETAIL, plasma, plume_m)
    if lod == DETAIL:
        _update_cargo(container, data)

static func _plume_mesh(plasma: bool) -> ArrayMesh:
    var key := "plasma" if plasma else "thermal"
    if _plume_meshes.has(key):
        return _plume_meshes[key]
    # Unit plume: radius 1 at the nozzle exit (z = 0), length 1 along +Z.
    var widen := 1.4 if plasma else 1.9
    var profile := PackedVector2Array()
    for i in range(9):
        var s := float(i) / 8.0
        profile.append(Vector2(s, 1.0 + (widen - 1.0) * sqrt(s)))
    var b := MeshBuilder.new()
    b.lathe("plume", profile, 20)
    var mesh: ArrayMesh = b.commit()["mesh"]
    _plume_meshes[key] = mesh
    return mesh

static func _update_plume(container: Node3D, show: bool, plasma: bool, length_m: float) -> void:
    var plume := container.get_node_or_null("plume") as MeshInstance3D
    if not show:
        if plume != null:
            plume.visible = false
        return
    if plume == null or bool(plume.get_meta("plasma", not plasma)) != plasma:
        if plume != null:
            plume.queue_free()
            container.remove_child(plume)
        plume = MeshInstance3D.new()
        plume.name = "plume"
        plume.mesh = _plume_mesh(plasma)
        plume.material_override = Materials.plume_material(Color(0.45, 0.55, 1.0) if plasma else Color(1.0, 0.78, 0.5))
        plume.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
        plume.set_meta("plasma", plasma)
        container.add_child(plume)
    var radius := float(container.get_meta("nozzle_r", 1.0)) * (0.6 if plasma else 0.95)
    plume.position = Vector3(0.0, 0.0, float(container.get_meta("stern_z", 0.0)))
    plume.scale = Vector3(radius, radius, maxf(length_m, 1.0))
    plume.visible = true

# Cargo containers in the hold, one colour per lot, filling it in the same proportion as
# the schematic (lots in order from the bow). Hidden behind a cold-hold liner.
static func _update_cargo(container: Node3D, data: Dictionary) -> void:
    var hold: Dictionary = container.get_meta("hold", {})
    var lots: Array = data.get("cargo", [])
    var capacity := maxf(float(data.get("cargo_capacity_units", 0.0)), 1.0)
    var sig := ""
    for lot in lots:
        sig += "%s:%.0f;" % [String(lot.get("commodity_id", "")), float(lot.get("units", 0.0))]
    if String(container.get_meta("cargo_sig", "")) == sig and container.has_node("cargo") == (sig != ""):
        return
    container.set_meta("cargo_sig", sig)
    var node := container.get_node_or_null("cargo") as MultiMeshInstance3D
    if hold.is_empty() or bool(hold.get("enclosed", false)) or sig == "":
        if node != null:
            node.queue_free()
            container.remove_child(node)
        return
    if node == null:
        node = MultiMeshInstance3D.new()
        node.name = "cargo"
        node.layers = Materials.MODEL_LAYERS
        node.material_override = Materials.shared("cargo")
        container.add_child(node)
    if _cargo_mesh == null:
        _cargo_mesh = BoxMesh.new()
        _cargo_mesh.size = Vector3.ONE
    var z0 := float(hold["z0"])
    var z1 := float(hold["z1"])
    var width := float(hold["width"])
    # A 2 x 2 grid of containers across the hold, as many as fit along it.
    var cell := width * 0.42
    var box_len := clampf(width * 0.6, 2.0, 6.0)
    var along := maxi(1, int((z1 - z0) / (box_len * 1.05)))
    var slots := along * 4
    var colors: Array[Color] = []
    for lot in lots:
        var count := int(round(float(lot.get("units", 0.0)) / capacity * float(slots)))
        if float(lot.get("units", 0.0)) > 0.0:
            count = maxi(count, 1)
        var color := UiTheme.commodity_color(String(lot.get("commodity_id", "")))
        # The UI's pastel as paint: less saturated and darker.
        var grey := color.get_luminance()
        color = color.lerp(Color(grey, grey, grey), 0.3) * 0.5
        for i in range(count):
            if colors.size() < slots:
                colors.append(color)
    var mm := MultiMesh.new()
    mm.transform_format = MultiMesh.TRANSFORM_3D
    mm.use_colors = true
    mm.mesh = _cargo_mesh
    mm.instance_count = colors.size()
    var step := (z1 - z0) / float(along)
    for i in range(colors.size()):
        var k := i / 4
        var q := i % 4
        var x := (-0.5 if q % 2 == 0 else 0.5) * width * 0.46
        var y := (-0.5 if q < 2 else 0.5) * width * 0.46
        var xf := Transform3D(Basis.from_scale(Vector3(cell, cell, step * 0.92)), Vector3(x, y, z0 + step * (float(k) + 0.5)))
        mm.set_instance_transform(i, xf)
        mm.set_instance_color(i, colors[i])
    node.multimesh = mm
