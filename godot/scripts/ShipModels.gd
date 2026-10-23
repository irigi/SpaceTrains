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
# Live state per ship: radiator heat pipes glowing while thrusting (false colour in the
# thermal view, T) and dark tips where worn, a hot nozzle and a plume while the engine runs,
# and cargo containers packed in the hold by commodity (DETAIL).

const MeshBuilder := preload("res://scripts/MeshBuilder.gd")
const ShipLayout := preload("res://scripts/ShipLayout.gd")
const Materials := preload("res://scripts/ModelMaterials.gd")
const UiTheme := preload("res://scripts/ui/UiTheme.gd")

const COARSE := 0
const DETAIL := 1
const HAB_RADIUS_M := ShipLayout.HAB_DIAMETER_M * 0.5
# Which container shape each commodity travels in.
const CARGO_SHAPES := {
    "water": "tank", "oxygen": "tank", "fuel": "tank", "deuterium": "tank", "helium_3": "tank",
    "metals": "bin", "platinum": "bin",
}

static var _cache := {}
# Detailed meshes take ~25 ms to build, so they are built on worker threads: cache key ->
# WorkerThreadPool task id, and the finished arrays (under _mutex) until the main thread
# turns them into meshes.
static var _pending := {}
static var _finished := {}
static var _mutex := Mutex.new()
static var _plume_meshes := {}
static var _cargo_meshes := {}

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
    # The truss stops at the hold's bulkheads (the bay frame carries the load there).
    var truss_gap := Vector2.ZERO
    for shape in layout["shapes"]:
        if String(shape["key"]) == "habitat.crew_quarters":
            crew_band = shape["rect"]
        if String(shape["kind"]) == "hold":
            truss_gap = Vector2((shape["rect"] as Rect2).position.x - mid, (shape["rect"] as Rect2).end.x - mid)
    for shape in layout["shapes"]:
        var r: Rect2 = shape["rect"]
        var comp: Dictionary = shape["comp"]
        # Layout x (bow 0 to stern) becomes z (bow at -Z); layout y becomes x.
        var z0 := r.position.x - mid
        var z1 := r.end.x - mid
        var width := r.size.y
        match String(shape["kind"]):
            "structure":
                if detail and truss_gap != Vector2.ZERO and truss_gap.x > z0 and truss_gap.y < z1:
                    _truss(b, z0, truss_gap.x)
                    _truss(b, truss_gap.y, z1)
                elif detail:
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

# An open cargo bay: four longerons with a frame at every container row and thin
# diagonals, a floor plate with ribs and a latch pair under every floor slot, and silver
# sills and corner gussets at both ends, so it reads as a bay when empty.
static func _hold_frame(b: MeshBuilder, z0: float, z1: float, width: float) -> void:
    var h := width * 0.5
    var grid := _hold_grid(z0, z1, width)
    var t: float = grid["t"]
    var bays: int = grid["along"]
    var corners := [Vector2(h, h), Vector2(-h, h), Vector2(-h, -h), Vector2(h, -h)]
    for c in corners:
        b.beam("truss", Vector3(c.x, c.y, z0), Vector3(c.x, c.y, z1), t)
    for j in range(bays + 1):
        var z := lerpf(z0, z1, float(j) / float(bays))
        for i in range(4):
            var c0: Vector2 = corners[i]
            var c1: Vector2 = corners[(i + 1) % 4]
            b.beam("truss", Vector3(c0.x, c0.y, z), Vector3(c1.x, c1.y, z), t)
            # Diagonals on the open sides (not on the floor).
            if j < bays and i != 2:
                var zn := lerpf(z0, z1, float(j + 1) / float(bays))
                var from := c0 if j % 2 == 0 else c1
                var to := c1 if j % 2 == 0 else c0
                b.beam("truss", Vector3(from.x, from.y, z), Vector3(to.x, to.y, zn), t * 0.4)
        # A rib under the floor at every row boundary.
        b.box("steel", Vector3(width, t * 1.6, t * 1.4), _at(0.0, -h - t * 1.1, z))
    var plate := maxf(t * 0.6, 0.08)
    b.box("blanket", Vector3(width, plate, z1 - z0), _at(0.0, -h - plate * 0.5, (z0 + z1) * 0.5))
    var across: int = grid["across"]
    var cell: float = grid["cell"]
    var step: float = grid["step"]
    var half: float = float(grid["inner"]) * 0.5
    var latch := Vector3(minf(cell * 0.12, 0.4), minf(cell * 0.06, 0.18), minf(cell * 0.12, 0.4))
    for row in range(bays):
        for i in range(across):
            var x := -half + cell * (float(i) + 0.5)
            for end in [-0.42, 0.42]:
                b.box("metal", latch, _at(x, -h + latch.y * 0.5, z0 + step * (float(row) + 0.5 + end)))
    # Open end frames with a low sill (solid bulkheads would hide the cargo when the
    # camera looks along the ship).
    var sill := width * 0.22
    for z in [z0, z1]:
        b.box("silver", Vector3(width - t, sill, 0.08), _at(0.0, -h + sill * 0.5, z))
        for c in corners:
            b.box("silver", Vector3(t * 2.2, t * 2.2, 0.1), _at(c.x * 0.97, c.y * 0.97, z))

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
    var sig := int(engine_on) | (int(plasma) << 1) | (lod << 2) | (int(temperature) << 4) | (int(lost * 100.0) << 24) | (int(Materials.thermal_view) << 32)
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

# The hold's container slots: `across` x `across` square cells in the cross-section
# (inside the longerons), `along` rows of about 6 m. Shared by the frame's latches and the
# cargo, so containers sit on the latches.
static func _hold_grid(z0: float, z1: float, width: float) -> Dictionary:
    var t := clampf(width * 0.03, 0.1, 0.3)
    var inner := width - 2.0 * t
    var across := clampi(int(round(inner / 2.4)), 1, 4)
    var along := maxi(1, int(ceil((z1 - z0) / 6.5)))
    return {"across": across, "along": along, "cell": inner / float(across), "step": (z1 - z0) / float(along),
        "inner": inner, "t": t}

# Unit containers (1 x 1 x 1, centred, long along Z, floor at -Y). Surfaces: cargo_body
# (light grey), cargo_mark (tinted by the instance colour: box doors and stripe, tank
# shell, bin rim) and cargo_ore.
static func _cargo_mesh(shape: String) -> ArrayMesh:
    if _cargo_meshes.has(shape):
        return _cargo_meshes[shape]
    var b := MeshBuilder.new()
    match shape:
        "tank":
            # An ISO tank: a cylinder in a square frame.
            for c in [Vector2(0.47, 0.47), Vector2(-0.47, 0.47), Vector2(-0.47, -0.47), Vector2(0.47, -0.47)]:
                b.box("cargo_body", Vector3(0.06, 0.06, 1.0), _at(c.x, c.y, 0.0))
            for z in [-0.47, 0.47]:
                b.box("cargo_body", Vector3(1.0, 0.06, 0.06), _at(0.0, 0.47, z))
                b.box("cargo_body", Vector3(1.0, 0.06, 0.06), _at(0.0, -0.47, z))
                b.box("cargo_body", Vector3(0.06, 1.0, 0.06), _at(0.47, 0.0, z))
                b.box("cargo_body", Vector3(0.06, 1.0, 0.06), _at(-0.47, 0.0, z))
            b.vessel("cargo_mark", -0.46, 0.46, 0.42, 0.12, 14)
        "bin":
            # An open-top ore bin with a heap.
            b.box("cargo_body", Vector3(1.0, 0.06, 1.0), _at(0.0, -0.47, 0.0))
            b.box("cargo_body", Vector3(0.06, 0.8, 1.0), _at(0.47, -0.07, 0.0))
            b.box("cargo_body", Vector3(0.06, 0.8, 1.0), _at(-0.47, -0.07, 0.0))
            b.box("cargo_body", Vector3(0.88, 0.8, 0.06), _at(0.0, -0.07, 0.47))
            b.box("cargo_body", Vector3(0.88, 0.8, 0.06), _at(0.0, -0.07, -0.47))
            b.box("cargo_mark", Vector3(1.02, 0.08, 1.02), _at(0.0, 0.33, 0.0))
            b.box("cargo_ore", Vector3(0.88, 0.62, 0.88), _at(0.0, -0.13, 0.0))
            b.box("cargo_ore", Vector3(0.62, 0.2, 0.66), _at(0.0, 0.25, 0.0))
            b.box("cargo_ore", Vector3(0.3, 0.12, 0.36), _at(0.05, 0.38, -0.05))
        _:
            # A box container: ribbed sides, coloured end doors and a stripe.
            b.box("cargo_body", Vector3(0.96, 0.96, 1.0))
            for k in range(7):
                var z := lerpf(-0.42, 0.42, float(k) / 6.0)
                b.box("cargo_body", Vector3(1.0, 0.9, 0.025), _at(0.0, 0.0, z))
            b.box("cargo_mark", Vector3(0.86, 0.86, 1.01))
            b.box("cargo_mark", Vector3(1.005, 0.12, 0.9), _at(0.0, 0.26, 0.0))
    var built := b.commit()
    var mesh: ArrayMesh = built["mesh"]
    var slots: Array = built["slots"]
    for i in range(slots.size()):
        mesh.surface_set_material(i, Materials.shared(String(slots[i])))
    _cargo_meshes[shape] = mesh
    return mesh

# The commodity colour as paint on the container marks.
static func _cargo_paint(commodity: String) -> Color:
    var c := UiTheme.commodity_color(commodity)
    return Color.from_hsv(c.h, minf(c.s, 0.6), minf(c.v, 0.75))

# Cargo containers in the hold, one colour per lot, the same share of the slots as of the
# capacity. Rows fill from the bow, and each row from the floor and the middle out, so a
# part load is a solid block. Hidden behind a cold-hold liner.
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
    var node := container.get_node_or_null("cargo") as Node3D
    if node != null:
        node.queue_free()
        container.remove_child(node)
    if hold.is_empty() or bool(hold.get("enclosed", false)) or sig == "":
        return
    var z0 := float(hold["z0"])
    var z1 := float(hold["z1"])
    var grid := _hold_grid(z0, z1, float(hold["width"]))
    var across: int = grid["across"]
    var cell: float = grid["cell"]
    var step: float = grid["step"]
    var half: float = float(grid["inner"]) * 0.5
    # Slot order within a row: floor first, then from the middle out.
    var row_order: Array[Vector2i] = []
    for j in range(across):
        for i in range(across):
            row_order.append(Vector2i(i, j))
    var mid := float(across - 1) * 0.5
    row_order.sort_custom(func(a: Vector2i, b: Vector2i) -> bool:
        if a.y != b.y:
            return a.y < b.y
        return absf(float(a.x) - mid) < absf(float(b.x) - mid))
    var slots := row_order.size() * int(grid["along"])
    var picks: Array = []  # [shape, colour] per slot, in fill order
    for lot in lots:
        var units := float(lot.get("units", 0.0))
        if units <= 0.0:
            continue
        var count := maxi(1, int(round(units / capacity * float(slots))))
        var id := String(lot.get("commodity_id", ""))
        for k in range(count):
            if picks.size() < slots:
                picks.append([String(CARGO_SHAPES.get(id, "box")), _cargo_paint(id)])
    node = Node3D.new()
    node.name = "cargo"
    container.add_child(node)
    var by_shape := {}
    for n in range(picks.size()):
        var slot: Vector2i = row_order[n % row_order.size()]
        var row := n / row_order.size()
        var size := Vector3(cell * 0.92, cell * 0.92, step * 0.94)
        var pos := Vector3(-half + cell * (float(slot.x) + 0.5), -half + cell * (float(slot.y) + 0.5),
            z0 + step * (float(row) + 0.5))
        var shape: String = picks[n][0]
        if not by_shape.has(shape):
            by_shape[shape] = []
        by_shape[shape].append([Transform3D(Basis.from_scale(size), pos), picks[n][1]])
    for shape in by_shape.keys():
        var items: Array = by_shape[shape]
        var mm := MultiMesh.new()
        mm.transform_format = MultiMesh.TRANSFORM_3D
        mm.use_colors = true
        mm.mesh = _cargo_mesh(shape)
        mm.instance_count = items.size()
        for i in range(items.size()):
            mm.set_instance_transform(i, items[i][0])
            mm.set_instance_color(i, items[i][1])
        var instance := MultiMeshInstance3D.new()
        instance.multimesh = mm
        instance.layers = Materials.MODEL_LAYERS
        node.add_child(instance)
