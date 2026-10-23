extends RefCounted

# Generic station models, built in metres and drawn at the ships' scale (Main.gd
# SHIP_METRES_TO_WORLD), so docked ships are in proportion. Sizes are compressed: a real
# torus for 20,000 people would be ~1.8 km across; here the habitat ring's radius grows
# from ~44 m (5,000 people) to ~72 m (22,000). Tier 3 (station structures) may replace this.
#
# Layout (local Z is the station's axis; Main tilts it to stand up from the ecliptic):
# - a spine with a docking hub at each end, four berthing arms per hub (docked ships sit
#   at their ends, bow to the hub);
# - one habitat ring, two counter-rotating ones above 15,000 people, with spokes, windows
#   and the faction band; rings turn slowly in real time (timewarp would spin them);
# - storage tanks by storage capacity, solar arrays and radiators;
# - a flavour by economy profile: greenhouses (agri), fabrication blocks (industrial), a
#   sphere tank farm (chemical, and any fuel factory), a solar furnace (smelter), container
#   stacks (frontier), ore bins (belt port), shuttle pads (lunar port), extraction rigs.

const MeshBuilder := preload("res://scripts/MeshBuilder.gd")
const Materials := preload("res://scripts/ModelMaterials.gd")

const RING_SPIN_RAD_S := 0.04
const TWO_RING_POPULATION := 15000.0

static var _cache := {}

static func ring_radius_m(population: float) -> float:
    return 55.0 * pow(maxf(population, 1000.0) / 10000.0, 1.0 / 3.0)

static func _key(data: Dictionary) -> String:
    return "%s|%d|%d|%d" % [String(data.get("economy_profile_id", "")), int(_ring_count(data)),
        int(round(ring_radius_m(float(data.get("seeded_population", data.get("population", 10000.0)))))),
        int(_tank_count(data)) + (100 if float(data.get("fuel_factory_per_day", 0.0)) > 0.0 else 0)]

static func _ring_count(data: Dictionary) -> int:
    return 2 if float(data.get("seeded_population", data.get("population", 0.0))) >= TWO_RING_POPULATION else 1

static func _tank_count(data: Dictionary) -> int:
    return clampi(int(float(data.get("storage_capacity", 20000.0)) / 9000.0), 2, 8)

# {hull: mesh info, ring: mesh info, ring_z: [z...], radius_m, berths: [{pos, dir}]}
static func model_info(data: Dictionary) -> Dictionary:
    var key := _key(data)
    if not _cache.has(key):
        _cache[key] = _build(data)
    return _cache[key]

static func _build(data: Dictionary) -> Dictionary:
    # Seeded population: the model is the station as founded, not rebuilt as people come and go.
    var radius := ring_radius_m(float(data.get("seeded_population", data.get("population", 10000.0))))
    var length := 2.4 * radius
    var half := length * 0.5
    var hub_r := 0.14 * radius
    var b := MeshBuilder.new()
    # Spine: a truss core with pressurized modules along it.
    b.cylinder("truss", -half, half, 0.05 * radius, 12)
    b.vessel("blanket", -half * 0.75, half * 0.75, 0.08 * radius, 0.06 * radius, 20)
    # Docking hubs and berthing arms.
    var berths: Array = []
    for end in [-1.0, 1.0]:
        var zc: float = end * half
        b.vessel("blanket", zc - hub_r, zc + hub_r, hub_r, hub_r * 0.6, 20)
        b.cylinder("metal", zc + end * hub_r * 0.8, zc + end * (hub_r + 4.0), 3.0, 14)
        b.torus("steel", zc + end * (hub_r + 4.0), 3.4, 0.5, Transform3D.IDENTITY, 16, 5)
        var arm := 0.35 * radius
        for k in range(4):
            var a := PI * 0.25 + PI * 0.5 * float(k)
            var radial := Vector3(cos(a), sin(a), 0.0)
            var tip := radial * arm + Vector3(0.0, 0.0, zc)
            b.beam("truss", radial * hub_r * 0.8 + Vector3(0.0, 0.0, zc), tip, 1.6)
            b.cylinder("metal", -1.5, 1.5, 2.2, 10, Transform3D(Basis(Vector3(0, 0, 1), radial.cross(Vector3(0, 0, 1)), radial).orthonormalized(), tip))
            berths.append({"pos": tip, "dir": radial})
    # Habitat ring(s) as separate meshes (they spin); here only their spokes' hub collars.
    var ring_count := _ring_count(data)
    var ring_z: Array = []
    if ring_count == 1:
        ring_z = [0.0]
    else:
        ring_z = [-0.3 * length, 0.3 * length]
    for z in ring_z:
        b.cylinder("steel", float(z) - 0.05 * radius, float(z) + 0.05 * radius, 0.12 * radius, 20)
    # Storage tanks round the spine, between the rings and the lower hub.
    var tanks := _tank_count(data)
    var tank_r := 0.07 * radius
    var tz0 := -half + hub_r * 2.2
    var tz1 := (float(ring_z[0]) - 0.2 * radius) if ring_count == 2 else -0.25 * radius
    if tz1 - tz0 < 4.0 * tank_r:
        tz1 = tz0 + 4.0 * tank_r
    for k in range(tanks):
        var a := TAU * float(k) / float(tanks)
        var at := Vector3(cos(a), sin(a), 0.0) * (0.08 * radius + tank_r * 1.3)
        b.vessel("tank", tz0, tz1, tank_r, tank_r * 0.8, 14, Transform3D(Basis.IDENTITY, at))
    # Solar arrays (upper end) and radiators (lower end) on masts.
    for side in [-1.0, 1.0]:
        var mast_z := half - hub_r * 2.6
        b.beam("truss", Vector3(0.0, 0.0, mast_z), Vector3(side * 0.55 * radius, 0.0, mast_z), 1.2)
        b.box("solar", Vector3(0.9 * radius, 0.3, 0.38 * radius), Transform3D(Basis.IDENTITY, Vector3(side * (0.55 * radius + 0.45 * radius), 0.0, mast_z)))
        var rad_z := tz0 + 0.3 * (tz1 - tz0)
        b.beam("truss", Vector3(0.0, 0.0, rad_z), Vector3(0.0, side * 0.3 * radius, rad_z), 1.0)
        b.box("silver", Vector3(0.25 * radius, 0.25, 0.5 * radius), Transform3D(Basis(Vector3(0, 0, 1), Vector3(1, 0, 0), Vector3(0, 1, 0)), Vector3(0.0, side * (0.3 * radius + 0.12 * radius), rad_z)))
    _flavour(b, String(data.get("economy_profile_id", "")), radius, length, ring_z,
        float(data.get("fuel_factory_per_day", 0.0)) > 0.0)
    var hull := b.commit()
    var ring := _build_ring(radius)
    var aabb: AABB = hull["aabb"]
    var bound := radius * 1.15
    for i in range(8):
        bound = maxf(bound, aabb.get_endpoint(i).length())
    return {"hull": hull, "ring": ring, "ring_z": ring_z, "radius_m": bound, "ring_radius_m": radius, "berths": berths}

# A habitat ring around local Z at z = 0: torus, windows facing out, the faction band,
# spokes to the spine.
static func _build_ring(radius: float) -> Dictionary:
    var b := MeshBuilder.new()
    var tube := 0.13 * radius
    b.torus("blanket", 0.0, radius, tube, Transform3D.IDENTITY, 64, 12)
    # The faction band round the ring's outer rim, between the two rows of windows.
    b.cylinder("band", -0.22 * tube, 0.22 * tube, radius + tube * 1.01, 64, Transform3D.IDENTITY, true)
    var windows := 72
    for k in range(windows):
        var a := TAU * (float(k) + 0.5) / float(windows)
        var radial := Vector3(cos(a), sin(a), 0.0)
        var basis := Basis(radial, Vector3(-sin(a), cos(a), 0.0), Vector3(0.0, 0.0, 1.0))
        for row in [-0.45, 0.45]:
            b.box("window", Vector3(0.3, 1.6, 1.0), Transform3D(basis, radial * (radius + tube * 0.98) + Vector3(0.0, 0.0, row * tube)))
    for k in range(6):
        var a := TAU * float(k) / 6.0
        var radial := Vector3(cos(a), sin(a), 0.0)
        b.beam("steel", radial * 0.12 * radius, radial * (radius - tube * 0.9), 0.035 * radius)
    return b.commit()

static func _flavour(b: MeshBuilder, profile: String, radius: float, length: float, ring_z: Array, fuel_factory: bool) -> void:
    var half := length * 0.5
    var top := half - 0.14 * radius * 2.6
    var mid_z := 0.0 if ring_z.size() == 2 else 0.3 * length
    match profile:
        "agri_hub":
            # Greenhouse cylinders with mirrors, parallel to the spine.
            for k in range(4):
                var a := PI * 0.25 + PI * 0.5 * float(k)
                var at := Vector3(cos(a), sin(a), 0.0) * 0.3 * radius
                b.vessel("glass", mid_z - 0.22 * radius, mid_z + 0.22 * radius, 0.07 * radius, 0.05 * radius, 16, Transform3D(Basis.IDENTITY, at))
                b.box("silver", Vector3(0.16 * radius, 0.2, 0.4 * radius), Transform3D(Basis.IDENTITY.rotated(Vector3(0, 0, 1), a + PI * 0.5), Vector3(cos(a), sin(a), 0.0) * 0.42 * radius + Vector3(0.0, 0.0, mid_z)))
        "industrial_hub":
            # Fabrication blocks clustered round the spine.
            for k in range(6):
                var a := TAU * float(k) / 6.0
                var size := Vector3(0.14, 0.12, 0.2) * radius * (1.0 + 0.25 * float(k % 2))
                b.box("foil" if k % 2 == 0 else "steel", size, Transform3D(Basis.IDENTITY.rotated(Vector3(0, 0, 1), a), Vector3(cos(a), sin(a), 0.0) * 0.2 * radius + Vector3(0.0, 0.0, mid_z)))
        "chem_hub":
            _sphere_farm(b, radius, mid_z, 6)
        "smelter_hub":
            # A solar furnace: a big concentrator dish on a boom, the furnace at its focus.
            var dish_z := top + 0.1 * radius
            var at := Vector3(0.0, 0.75 * radius, dish_z)
            b.beam("truss", Vector3(0.0, 0.0, dish_z), at, 1.5)
            var dish := PackedVector2Array()
            for i in range(7):
                var rr := 0.35 * radius * float(i) / 6.0
                dish.append(Vector2(0.12 * radius * pow(rr / (0.35 * radius), 2.0), rr))
            var basis := Basis(Vector3(1, 0, 0), Vector3(0, 0, 1), Vector3(0, -1, 0))
            b.lathe("silver", dish, 24, Transform3D(basis, at + Vector3(0.0, 0.12 * radius, 0.0)))
            b.lathe("metal", dish, 24, Transform3D(basis, at + Vector3(0.0, 0.12 * radius, 0.0)), true)
            b.box("metal", Vector3(0.12, 0.12, 0.16) * radius, Transform3D(Basis.IDENTITY, Vector3(0.0, 0.3 * radius, dish_z)))
        "frontier_hub":
            # Container stacks on open racks along the spine.
            for k in range(4):
                var a := PI * 0.5 * float(k)
                var basis := Basis.IDENTITY.rotated(Vector3(0, 0, 1), a)
                var at := Vector3(cos(a), sin(a), 0.0) * 0.2 * radius + Vector3(0.0, 0.0, mid_z)
                b.box("truss", Vector3(0.1, 0.16, 0.5) * radius, Transform3D(basis, at))
                for j in range(4):
                    b.box("foil" if (j + k) % 2 == 0 else "steel", Vector3(0.08, 0.08, 0.1) * radius,
                        Transform3D(basis, at + basis * Vector3(0.0, 0.0, (float(j) - 1.5) * 0.12 * radius)))
        "belt_port":
            # Ore bins: big dark boxes in a frame.
            for k in range(3):
                var a := TAU * float(k) / 3.0
                b.box("metal", Vector3(0.18, 0.18, 0.28) * radius, Transform3D(Basis.IDENTITY.rotated(Vector3(0, 0, 1), a), Vector3(cos(a), sin(a), 0.0) * 0.25 * radius + Vector3(0.0, 0.0, mid_z)))
        "lunar_port":
            # Shuttle pads for the surface run.
            for side in [-1.0, 1.0]:
                var at := Vector3(side * 0.4 * radius, 0.0, mid_z)
                b.beam("truss", Vector3(0.0, 0.0, mid_z), at, 1.4)
                b.cylinder("steel", -0.6, 0.6, 0.16 * radius, 24, Transform3D(Basis(Vector3(0, 0, 1), Vector3(0, 1, 0), Vector3(1, 0, 0)), at + Vector3(side * 0.02 * radius, 0.0, 0.0)))
        "extractor_hub":
            # Processing rigs: long trusses with spherical tanks.
            for side in [-1.0, 1.0]:
                var at := Vector3(0.0, side * 0.35 * radius, mid_z)
                b.beam("truss", Vector3(0.0, 0.0, mid_z), at, 1.4)
                b.beam("truss", at - Vector3(0.0, 0.0, 0.3 * radius), at + Vector3(0.0, 0.0, 0.3 * radius), 1.2)
                for j in range(3):
                    var r := 0.07 * radius
                    b.vessel("tank", -r, r, r, r, 14, Transform3D(Basis.IDENTITY, at + Vector3(0.0, side * r * 1.1, (float(j) - 1.0) * 0.22 * radius)))
    if fuel_factory and profile != "chem_hub":
        _sphere_farm(b, radius, -mid_z if ring_z.size() == 1 else mid_z, 4)

static func _sphere_farm(b: MeshBuilder, radius: float, z: float, count: int) -> void:
    for k in range(count):
        var a := TAU * (float(k) + 0.5) / float(count)
        var r := 0.08 * radius
        var at := Vector3(cos(a), sin(a), 0.0) * 0.24 * radius + Vector3(0.0, 0.0, z)
        b.vessel("tank", -r, r, r, r, 16, Transform3D(Basis.IDENTITY, at))
        b.beam("steel", Vector3(0.0, 0.0, z), at, 0.9)

# --- Nodes ------------------------------------------------------------------------

# The station's model in its container: the hull and its spinning ring(s). The container
# is turned so the station's axis stands up from the ecliptic.
static func ensure(container: Node3D, data: Dictionary) -> void:
    var key := _key(data)
    if String(container.get_meta("model_key", "")) == key:
        return
    var info := model_info(data)
    for child in container.get_children():
        if String(child.name).begins_with("ring") or String(child.name) == "model":
            child.queue_free()
            container.remove_child(child)
    var hull := MeshInstance3D.new()
    hull.name = "model"
    Materials.apply_mesh(hull, info["hull"], container)
    container.add_child(hull)
    var i := 0
    for z in info["ring_z"]:
        var ring := MeshInstance3D.new()
        ring.name = "ring_%d" % i
        ring.position = Vector3(0.0, 0.0, float(z))
        ring.set_meta("spin", 1.0 if i % 2 == 0 else -1.0)
        Materials.apply_mesh(ring, info["ring"], container)
        container.add_child(ring)
        i += 1
    container.rotation = Vector3(-PI * 0.5, 0.0, 0.0)
    container.set_meta("model_key", key)
    container.set_meta("radius_m", info["radius_m"])
    container.set_meta("berths", info["berths"])

static func spin_rings(container: Node3D, real_time_s: float) -> void:
    for child in container.get_children():
        if child.has_meta("spin"):
            (child as Node3D).rotation = Vector3(0.0, 0.0, fmod(real_time_s * RING_SPIN_RAD_S, TAU) * float(child.get_meta("spin")))
