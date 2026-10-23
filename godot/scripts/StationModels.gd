extends RefCounted

# Generic station models, built in metres at real size and drawn at the ships' scale
# (Main.gd ship_metres_to_world), so docked ships are in proportion.
# - The habitat ring is sized by population, anchored on the Stanford torus (900 m radius
#   for 10,000 people, tube 65 m): radius grows with the square root of the people per ring,
#   never under 250 m (1 g at 2 rpm). Rings turn at the rate that gives 1 g.
# - Everything else (spine, docking hubs, berths, tanks, arrays, flavour) is sized for the
#   ships that dock there (40-140 m long), the same at every station.
# Two levels of detail: COARSE (plain shapes) and DETAIL (ring sectors drawn as MultiMeshes,
# lattice trusses, panels, windows and fittings; built on a worker thread, request_detail()),
# switched by on-screen size in Main.gd.
# Tier 3 (station structures) may replace this.
#
# Layout (local Z is the station's axis; Main tilts it to stand up from the ecliptic):
# - a spine with a docking hub at each end, four berthing arms per hub (docked ships sit
#   at their ends, bow to the hub);
# - one habitat ring, two counter-rotating ones a tube diameter apart above 15,000 people,
#   with spokes, windows and the faction band; rings turn in real time (timewarp would spin them);
# - storage tanks by storage capacity, solar arrays and radiators;
# - a flavour by economy profile: greenhouses (agri), fabrication blocks (industrial), a
#   sphere tank farm (chemical, and any fuel factory), a solar furnace (smelter), container
#   stacks (frontier), ore bins (belt port), shuttle pads (lunar port), extraction rigs.

const MeshBuilder := preload("res://scripts/MeshBuilder.gd")
const Materials := preload("res://scripts/ModelMaterials.gd")

const COARSE := 0
const DETAIL := 1
const TWO_RING_POPULATION := 15000.0
# The size the docking core's parts are drawn for (they were once scaled by ring radius).
const CORE_M := 110.0
const STANFORD_RADIUS_M := 900.0
const STANFORD_POPULATION := 10000.0
const MIN_RING_RADIUS_M := 250.0
const TUBE_RATIO := 65.0 / 900.0

static var _cache := {}

# Habitat ring radius for a station's (seeded) population, split over its rings.
static func ring_radius_m(population: float) -> float:
    var per_ring := maxf(population, 1000.0) / float(2 if population >= TWO_RING_POPULATION else 1)
    return maxf(MIN_RING_RADIUS_M, STANFORD_RADIUS_M * sqrt(per_ring / STANFORD_POPULATION))

# Spin that gives 1 g at the ring's rim.
static func ring_spin_rad_s(radius_m: float) -> float:
    return sqrt(9.81 / maxf(radius_m, 1.0))

static func _key(data: Dictionary) -> String:
    return "%s|%d|%d|%d" % [String(data.get("economy_profile_id", "")), int(_ring_count(data)),
        int(round(ring_radius_m(float(data.get("seeded_population", data.get("population", 10000.0)))))),
        int(_tank_count(data)) + (100 if float(data.get("fuel_factory_per_day", 0.0)) > 0.0 else 0)]

static func _ring_count(data: Dictionary) -> int:
    return 2 if float(data.get("seeded_population", data.get("population", 0.0))) >= TWO_RING_POPULATION else 1

static func _tank_count(data: Dictionary) -> int:
    return clampi(int(float(data.get("storage_capacity", 20000.0)) / 9000.0), 2, 8)

# {hull, ring (mesh infos), ring_z: [z...], radius_m (whole station), core_radius_m (without
#  the ring), ring_radius_m, spin_rad_s, berths: [{pos, dir}]}
static func model_info(data: Dictionary) -> Dictionary:
    var key := _key(data)
    if not _cache.has(key):
        _cache[key] = _build(data)
    return _cache[key]

static func _build(data: Dictionary) -> Dictionary:
    var hull := _hull(data, false)
    var ring_radius := float(hull["ring_radius_m"])
    var tube := TUBE_RATIO * ring_radius
    var ring := _build_ring(ring_radius, 0.3 * CORE_M)
    var aabb: AABB = hull["arrays"]["aabb"]
    var core_bound := 0.0
    for i in range(8):
        core_bound = maxf(core_bound, aabb.get_endpoint(i).length())
    var bound := maxf((ring_radius + tube) * 1.02, core_bound)
    return {"hull": MeshBuilder.to_mesh(hull["arrays"]), "ring": ring, "ring_z": hull["ring_z"], "radius_m": bound,
        "core_radius_m": core_bound, "ring_radius_m": ring_radius, "spin_rad_s": ring_spin_rad_s(ring_radius),
        "berths": hull["berths"]}

# Where the habitat rings sit along the axis: one at the middle, two side by side a tube
# diameter apart (counter-rotating).
static func _ring_z(data: Dictionary) -> Array:
    var tube := TUBE_RATIO * ring_radius_m(float(data.get("seeded_population", data.get("population", 10000.0))))
    return [0.0] if _ring_count(data) == 1 else [-2.0 * tube, 2.0 * tube]

# The non-spinning core: spine, hubs and berths, tanks, arrays, flavour. detail: lattice
# trusses, modules with windows, berth cradles, paneled arrays (DETAIL); arrays only, so
# it can run on a worker thread.
static func _hull(data: Dictionary, detail: bool) -> Dictionary:
    # Seeded population: the model is the station as founded, not rebuilt as people come and go.
    var ring_radius := ring_radius_m(float(data.get("seeded_population", data.get("population", 10000.0))))
    var ring_count := _ring_count(data)
    var rng := RandomNumberGenerator.new()
    rng.seed = hash(_key(data))
    # The docking core: `radius` sizes its parts. The hubs stand far enough beyond the
    # rings' spokes that docked ships clear them.
    var radius := CORE_M
    var ring_z := _ring_z(data)
    var half := maxf(1.2 * radius, absf(float(ring_z[0])) + 150.0)
    var length := 2.0 * half
    var hub_r := 0.22 * radius
    var b := MeshBuilder.new()
    # Spine: a truss core with pressurized modules along it.
    if detail:
        _lattice(b, "truss", Vector3(0, 0, -half), Vector3(0, 0, half), 0.08 * radius, 7.0, 0.7, 4)
        b.cylinder("steel", -half, half, 2.6, 12)
        _modules(b, -half * 0.75, half * 0.75, 0.14 * radius, rng)
    else:
        b.cylinder("truss", -half, half, 0.09 * radius, 12)
        b.vessel("blanket", -half * 0.75, half * 0.75, 0.14 * radius, 0.1 * radius, 20)
    # Docking hubs and berthing arms.
    var berths: Array = []
    for end in [-1.0, 1.0]:
        var zc: float = end * half
        b.vessel("blanket", zc - hub_r, zc + hub_r, hub_r, hub_r * 0.6, 32 if detail else 20)
        b.cylinder("metal", zc + end * hub_r * 0.8, zc + end * (hub_r + 4.0), 3.0, 14)
        b.torus("steel", zc + end * (hub_r + 4.0), 3.4, 0.5, Transform3D.IDENTITY, 16, 5)
        if detail:
            _hub_detail(b, zc, end, hub_r, rng)
        # Long enough that neighbouring berths' ships (up to ~200 m across the radiators) clear.
        var arm := 1.0 * radius
        for k in range(4):
            var a := PI * 0.25 + PI * 0.5 * float(k)
            var radial := Vector3(cos(a), sin(a), 0.0)
            var tip := radial * arm + Vector3(0.0, 0.0, zc)
            var root := radial * hub_r * 0.8 + Vector3(0.0, 0.0, zc)
            var tip_basis := Basis(Vector3(0, 0, 1), radial.cross(Vector3(0, 0, 1)), radial).orthonormalized()
            if detail:
                _arm_detail(b, root, tip, radial, tip_basis)
            else:
                b.beam("truss", root, tip, 3.0)
            b.cylinder("metal", -1.5, 1.5, 2.2, 10, Transform3D(tip_basis, tip))
            berths.append({"pos": tip, "dir": radial})
    # Habitat ring(s) as separate meshes (they spin); here only their bearings on the spine.
    for z in ring_z:
        if detail:
            b.cylinder("metal", float(z) - 0.1 * radius - 3.0, float(z) + 0.1 * radius + 3.0, 0.16 * radius, 32)
        else:
            b.cylinder("steel", float(z) - 0.1 * radius, float(z) + 0.1 * radius, 0.3 * radius, 24)
    # Storage tanks round the spine, between the rings and the lower hub.
    var tanks := _tank_count(data)
    var tank_r := 0.09 * radius
    var tz0 := -half + hub_r * 2.2
    var tz1 := (float(ring_z[0]) - 0.2 * radius) if ring_count == 2 else -0.25 * radius
    if tz1 - tz0 < 4.0 * tank_r:
        tz1 = tz0 + 4.0 * tank_r
    for k in range(tanks):
        var a := TAU * float(k) / float(tanks)
        var at := Vector3(cos(a), sin(a), 0.0) * (0.14 * radius + tank_r * 1.3)
        b.vessel("tank", tz0, tz1, tank_r, tank_r * 0.8, 24 if detail else 14, Transform3D(Basis.IDENTITY, at))
        if detail:
            for f in [0.2, 0.5, 0.8]:
                b.torus("steel", lerpf(tz0, tz1, f), tank_r + 0.15, 0.35, Transform3D(Basis.IDENTITY, at), 24, 4)
            for f in [0.25, 0.75]:
                var z: float = lerpf(tz0, tz1, f)
                b.beam("copper", Vector3(at.x, at.y, z) * (1.0 - tank_r / at.length()), Vector3(cos(a), sin(a), 0.0) * 0.08 * radius + Vector3(0, 0, z), 0.5)
    # Solar arrays (upper end) and radiators (lower end) on masts.
    for side in [-1.0, 1.0]:
        var mast_z := half - hub_r * 2.6
        var panel_at := Vector3(side * (0.55 * radius + 0.45 * radius), 0.0, mast_z)
        var rad_z := tz0 + 0.3 * (tz1 - tz0)
        var rad_basis := Basis(Vector3(0, 0, 1), Vector3(1, 0, 0), Vector3(0, 1, 0))
        var rad_at := Vector3(0.0, side * (0.3 * radius + 0.12 * radius), rad_z)
        if detail:
            _lattice(b, "truss", Vector3(0.0, 0.0, mast_z), Vector3(side * 0.55 * radius, 0.0, mast_z), 1.4, 3.0, 0.2, 3)
            _panel_grid(b, "solar", Vector3(0.9 * radius, 0.3, 0.38 * radius), Transform3D(Basis.IDENTITY, panel_at), 12, 5)
            _lattice(b, "truss", Vector3(0.0, 0.0, rad_z), Vector3(0.0, side * 0.3 * radius, rad_z), 1.1, 3.0, 0.18, 3)
            _panel_grid(b, "silver", Vector3(0.25 * radius, 0.25, 0.5 * radius), Transform3D(rad_basis, rad_at), 2, 8)
        else:
            b.beam("truss", Vector3(0.0, 0.0, mast_z), Vector3(side * 0.55 * radius, 0.0, mast_z), 1.2)
            b.box("solar", Vector3(0.9 * radius, 0.3, 0.38 * radius), Transform3D(Basis.IDENTITY, panel_at))
            b.beam("truss", Vector3(0.0, 0.0, rad_z), Vector3(0.0, side * 0.3 * radius, rad_z), 1.0)
            b.box("silver", Vector3(0.25 * radius, 0.25, 0.5 * radius), Transform3D(rad_basis, rad_at))
    _flavour(b, String(data.get("economy_profile_id", "")), radius, length, ring_z,
        float(data.get("fuel_factory_per_day", 0.0)) > 0.0)
    return {"arrays": b.commit_arrays(), "ring_z": ring_z, "ring_radius_m": ring_radius, "berths": berths}

# --- Detail parts (DETAIL) --------------------------------------------------------

# A lattice truss from a to c: `sides` chords round a circle of radius half_w, battens
# every ~bay metres and a diagonal per face per bay.
static func _lattice(b: MeshBuilder, slot: String, a: Vector3, c: Vector3, half_w: float, bay: float, chord: float, sides := 4) -> void:
    var along := c - a
    var length := along.length()
    if length < 1.0e-3:
        return
    var dir := along / length
    var up := Vector3(0, 0, 1) if absf(dir.z) < 0.9 else Vector3(1, 0, 0)
    var u := dir.cross(up).normalized()
    var v := dir.cross(u)
    var corners: Array[Vector3] = []
    for i in range(sides):
        var ang := TAU * (float(i) + 0.5) / float(sides)
        corners.append((u * cos(ang) + v * sin(ang)) * half_w)
    var bays := maxi(1, int(round(length / bay)))
    for i in range(sides):
        b.beam(slot, a + corners[i], c + corners[i], chord)
    for j in range(bays + 1):
        var p := a + along * (float(j) / float(bays))
        var pn := a + along * (float(j + 1) / float(bays))
        for i in range(sides):
            var q0 := p + corners[i]
            b.beam(slot, q0, p + corners[(i + 1) % sides], chord * 0.6)
            if j < bays:
                b.beam(slot, q0, pn + corners[(i + 1) % sides], chord * 0.45)

# Pressurized modules along the spine, each with frames, a row of windows and a cupola,
# joined by narrow nodes.
static func _modules(b: MeshBuilder, z0: float, z1: float, r: float, rng: RandomNumberGenerator) -> void:
    var count := maxi(1, int((z1 - z0 + 6.0) / 48.0))
    var each := (z1 - z0 - 6.0 * float(count - 1)) / float(count)
    for m in range(count):
        var a := z0 + float(m) * (each + 6.0)
        var c := a + each
        b.vessel("blanket", a, c, r, 4.0, 32)
        for z in [a + 4.5, c - 4.5]:
            b.torus("steel", z, r + 0.1, 0.35, Transform3D.IDENTITY, 32, 4)
        for row in [-0.25, 0.25]:
            _window_ring(b, (a + c) * 0.5 + row * each, r, 24, Vector2(1.6, 1.0), rng)
        var ang := rng.randf() * TAU
        var radial := Vector3(cos(ang), sin(ang), 0.0)
        var basis := Basis(radial.cross(Vector3(0, 0, 1)), Vector3(0, 0, 1), radial)
        b.vessel("blanket", r - 1.0, r + 4.0, 3.0, 1.5, 16, Transform3D(basis, Vector3(0, 0, (a + c) * 0.5)))
        b.box("window", Vector3(2.0, 2.0, 0.2), Transform3D(basis, Vector3(0, 0, (a + c) * 0.5) + radial * (r + 4.05)))
        if m < count - 1:
            b.cylinder("steel", c - 1.0, c + 7.0, 4.5, 20)

# Windows round a cylinder about Z, some lit.
static func _window_ring(b: MeshBuilder, z: float, r: float, count: int, size: Vector2, rng: RandomNumberGenerator) -> void:
    for k in range(count):
        var ang := TAU * (float(k) + 0.5) / float(count)
        var radial := Vector3(cos(ang), sin(ang), 0.0)
        var basis := Basis(radial.cross(Vector3(0, 0, 1)), Vector3(0, 0, 1), radial)
        b.box("window" if rng.randf() < 0.6 else "metal", Vector3(size.x, size.y, 0.2), Transform3D(basis, radial * (r + 0.05) + Vector3(0, 0, z)))

# A docking hub's fittings: frames, windows, antennas and beacons.
static func _hub_detail(b: MeshBuilder, zc: float, end: float, hub_r: float, rng: RandomNumberGenerator) -> void:
    b.torus("steel", zc, hub_r + 0.15, 0.5, Transform3D.IDENTITY, 32, 4)
    for row in [-6.0, 6.0]:
        var z: float = zc + row
        _window_ring(b, z, hub_r, 20, Vector2(1.4, 0.9), rng)
    for k in range(2):
        var ang := PI * float(k) + 0.3
        var radial := Vector3(cos(ang), sin(ang), 0.0)
        var base := radial * hub_r * 0.7 + Vector3(0, 0, zc + end * hub_r * 0.5)
        var top := base + radial * 14.0 + Vector3(0, 0, end * 4.0)
        b.beam("truss", base, top, 0.4)
        var dish := PackedVector2Array([Vector2(0.0, 0.0), Vector2(0.5, 1.5), Vector2(1.4, 3.0)])
        b.lathe("silver", dish, 16, Transform3D(Basis(radial.cross(Vector3(0, 0, 1)), Vector3(0, 0, 1), radial), top))
    for k in range(4):
        var ang := PI * 0.5 * float(k)
        b.box("window", Vector3(0.8, 0.8, 0.8), Transform3D(Basis.IDENTITY, Vector3(cos(ang), sin(ang), 0.0) * (hub_r * 0.55) + Vector3(0, 0, zc + end * (hub_r + 1.0))))

# A berthing arm: a triangular lattice with a propellant line, and a cradle at its tip.
static func _arm_detail(b: MeshBuilder, root: Vector3, tip: Vector3, radial: Vector3, tip_basis: Basis) -> void:
    _lattice(b, "truss", root, tip - radial * 2.0, 2.4, 4.5, 0.4, 3)
    var side := radial.cross(Vector3(0, 0, 1))
    b.beam("copper", root + side * 3.2, tip - radial * 3.0 + side * 3.2, 0.7)
    b.beam("steel", root - side * 3.2, tip - radial * 3.0 - side * 3.2, 0.5)
    # Cradle: a collar and four clamps round the docking port, floodlights.
    b.torus("steel", 0.0, 3.6, 0.45, Transform3D(tip_basis, tip), 20, 4)
    for k in range(4):
        var ang := PI * 0.5 * float(k) + PI * 0.25
        var off := tip_basis * Vector3(cos(ang) * 4.2, sin(ang) * 4.2, 1.0)
        b.box("metal", Vector3(1.0, 1.0, 3.0), Transform3D(tip_basis, tip + off))
    for s in [-1.0, 1.0]:
        b.box("window", Vector3(0.6, 0.6, 0.4), Transform3D(tip_basis, tip - radial * 6.0 + side * s * 3.0 + Vector3(0, 0, 2.6)))

# A flat panel (solar cells, radiator) as a grid of tiles in a frame. size: x across, z
# along, y thickness; xf places its centre.
static func _panel_grid(b: MeshBuilder, slot: String, size: Vector3, xf: Transform3D, nx: int, nz: int) -> void:
    var gap := 0.6
    var cell := Vector3(size.x / float(nx), size.y, size.z / float(nz))
    for i in range(nx):
        for j in range(nz):
            var c := Vector3(-size.x * 0.5 + cell.x * (float(i) + 0.5), 0.0, -size.z * 0.5 + cell.z * (float(j) + 0.5))
            b.box(slot, Vector3(cell.x - gap, size.y, cell.z - gap), xf * Transform3D(Basis.IDENTITY, c))
    for i in range(nx + 1):
        var x := -size.x * 0.5 + cell.x * float(i)
        b.beam("steel", xf * Vector3(x, 0, -size.z * 0.5), xf * Vector3(x, 0, size.z * 0.5), 0.35)
    for j in [0, nz]:
        var z := -size.z * 0.5 + cell.z * float(j)
        b.beam("steel", xf * Vector3(-size.x * 0.5, 0, z), xf * Vector3(size.x * 0.5, 0, z), 0.35)

# A habitat ring around local Z at z = 0: torus, window strips facing out, the faction
# band, spokes to the hub collar (radius collar_m).
static func _build_ring(radius: float, collar_m: float) -> Dictionary:
    var b := MeshBuilder.new()
    var tube := TUBE_RATIO * radius
    b.torus("blanket", 0.0, radius, tube, Transform3D.IDENTITY, 128, 16)
    # The faction band round the ring's outer rim, between the two rows of windows.
    b.cylinder("band", -0.22 * tube, 0.22 * tube, radius + tube * 1.01, 128, Transform3D.IDENTITY, true)
    var windows := 96
    var window_len := 0.45 * TAU * radius / float(windows)
    for k in range(windows):
        var a := TAU * (float(k) + 0.5) / float(windows)
        var radial := Vector3(cos(a), sin(a), 0.0)
        var basis := Basis(radial, Vector3(-sin(a), cos(a), 0.0), Vector3(0.0, 0.0, 1.0))
        for row in [-0.45, 0.45]:
            b.box("window", Vector3(0.6, window_len, 0.09 * tube), Transform3D(basis, radial * (radius + tube * 0.98) + Vector3(0.0, 0.0, row * tube)))
    # Spokes: lift tubes, 15 m across as on the Stanford torus.
    for k in range(6):
        var a := TAU * float(k) / 6.0
        var radial := Vector3(cos(a), sin(a), 0.0)
        b.beam("steel", radial * collar_m * 0.9, radial * (radius - tube * 0.9), 15.0)
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

# --- Detailed habitat ring (DETAIL) ----------------------------------------------
#
# The ring is cut into sectors, 6 to 10 between neighbouring spokes (~100 m of rim each),
# built once per variant and drawn as MultiMeshes round the ring. Round the tube (angle
# phi, 0 at the outer rim, +pi/2 on the +Z side): the faction band at the rim; homes with
# rows of windows (or greenhouse glazing) either side of it; shield panels top and
# bottom; on the +Z inner shoulder the sunlight windows (Stanford-style, fed by mirrors);
# services (pipes) on the inside; radiator fins underneath. Frames ring the tube at each
# sector's ends and middle; airlocks, antennas, equipment and beacons are scattered on it.

const SECTOR_VARIANTS := ["junction", "home_a", "home_b", "farm"]

static func _sectors_per_spoke(radius: float) -> int:
    return clampi(int(round(TAU * radius / 6.0 / 105.0)), 5, 10)

# The variant of sector k (sector 0 starts at a spoke).
static func _sector_variant(k: int, per_spoke: int) -> String:
    var j := k % per_spoke
    if j == 0:
        return "junction"
    if j % 3 == 2:
        return "farm"
    return "home_a" if j % 2 == 1 else "home_b"

static func _tp(radius: float, tube: float, th: float, ph: float, lift: float) -> Vector3:
    var radial := Vector3(cos(th), sin(th), 0.0)
    return radial * radius + (radial * cos(ph) + Vector3(0.0, 0.0, sin(ph))) * (tube + lift)

static func _tn(th: float, ph: float) -> Vector3:
    return Vector3(cos(th) * cos(ph), sin(th) * cos(ph), sin(ph))

# A patch of the tube's surface between two ring angles and two tube angles, lifted off it.
static func _patch(b: MeshBuilder, slot: String, radius: float, tube: float, th0: float, th1: float,
        ph0: float, ph1: float, lift := 0.0, n_th := 1, n_ph := 1) -> void:
    for i in range(n_th):
        var ta := lerpf(th0, th1, float(i) / float(n_th))
        var tb := lerpf(th0, th1, float(i + 1) / float(n_th))
        for j in range(n_ph):
            var pa := lerpf(ph0, ph1, float(j) / float(n_ph))
            var pb := lerpf(ph0, ph1, float(j + 1) / float(n_ph))
            var p00 := _tp(radius, tube, ta, pa, lift)
            var p10 := _tp(radius, tube, tb, pa, lift)
            var p11 := _tp(radius, tube, tb, pb, lift)
            var p01 := _tp(radius, tube, ta, pb, lift)
            var n00 := _tn(ta, pa)
            var n10 := _tn(tb, pa)
            var n11 := _tn(tb, pb)
            var n01 := _tn(ta, pb)
            b.tri(slot, p00, p10, p11, n00, n10, n11)
            b.tri(slot, p00, p11, p01, n00, n11, n01)

# A frame round the tube at ring angle th.
static func _tube_frame(b: MeshBuilder, slot: String, radius: float, tube: float, th: float, lift: float, thickness: float) -> void:
    var radial := Vector3(cos(th), sin(th), 0.0)
    var tangent := Vector3(-sin(th), cos(th), 0.0)
    b.torus(slot, 0.0, tube + lift, thickness, Transform3D(Basis(radial, Vector3(0, 0, 1), tangent), radial * radius), 64, 4)

# Placement on the tube's surface: z out along the normal, x along the ring, lifted.
static func _on_tube(radius: float, tube: float, th: float, ph: float, lift: float) -> Transform3D:
    var n := _tn(th, ph)
    var x := Vector3(-sin(th), cos(th), 0.0)
    return Transform3D(Basis(x, n.cross(x), n), _tp(radius, tube, th, ph, lift))

static func _ring_sector(radius: float, delta: float, variant: String) -> Dictionary:
    var tube := TUBE_RATIO * radius
    var b := MeshBuilder.new()
    var rng := RandomNumberGenerator.new()
    rng.seed = hash(variant) + int(radius)
    var steps := 8
    var farm := variant == "farm"
    # Skin, zone by zone round the tube.
    for zone in [[-0.13, 0.13, "blanket"], [0.13, 0.95, "steel" if farm else "blanket"], [-0.95, -0.13, "steel" if farm else "blanket"],
            [0.95, 2.15, "metal"], [-2.15, -0.95, "metal"], [2.15, 2.85, "metal"], [2.85, PI, "steel"],
            [-PI, -2.6, "steel"], [-2.6, -2.15, "metal"]]:
        var p0: float = zone[0]
        var p1: float = zone[1]
        _patch(b, zone[2], radius, tube, 0.0, delta, p0, p1, 0.0, steps, maxi(1, int(ceil((p1 - p0) / 0.15))))
    # Homes: rows of windows, some lit; farms: greenhouse glazing in frames.
    for sgn in [-1.0, 1.0]:
        if farm:
            var cells := maxi(2, int(round(delta * radius / 9.0)))
            for i in range(cells):
                for j in range(3):
                    var pa := 0.15 + 0.26 * float(j)
                    var ta := delta * float(i) / float(cells)
                    var tb := delta * float(i + 1) / float(cells)
                    _patch(b, "glass", radius, tube, ta + 0.4 / radius, tb - 0.4 / radius,
                        sgn * (pa + 0.4 / tube), sgn * (pa + 0.26 - 0.4 / tube), 0.25, 1, 2)
        else:
            for k in range(7):
                var ph: float = sgn * (0.2 + 0.11 * float(k))
                var rho := radius + tube * cos(ph)
                var count := int(delta * rho / 4.0)
                for i in range(count):
                    var th := delta * (float(i) + 0.5) / float(count)
                    var lit := rng.randf() < 0.55
                    _patch(b, "window" if lit else "metal", radius, tube, th - 1.2 / rho, th + 1.2 / rho,
                        ph - 0.75 / tube, ph + 0.75 / tube, 0.15)
    # Shield panels, top and bottom, with seams.
    var along := maxi(4, int(round(delta * radius / 13.0)))
    for sgn in [-1.0, 1.0]:
        for i in range(along):
            for j in range(6):
                var pa := 0.95 + 0.2 * float(j)
                var roll := rng.randf()
                var slot := "tank" if roll < 0.7 else ("blanket" if roll < 0.92 else "steel")
                _patch(b, slot, radius, tube, delta * float(i) / float(along) + 0.3 / radius, delta * float(i + 1) / float(along) - 0.3 / radius,
                    sgn * (pa + 0.3 / tube), sgn * (pa + 0.2 - 0.3 / tube), 0.35, 1, 2)
    # Sunlight windows on the +Z inner shoulder, between mullions.
    var panes := maxi(4, int(round(delta * radius / 5.0)))
    for i in range(panes):
        for j in range(2):
            var pa := 2.15 + 0.35 * float(j)
            _patch(b, "glass", radius, tube, delta * float(i) / float(panes) + 0.25 / radius, delta * float(i + 1) / float(panes) - 0.25 / radius,
                pa + 0.25 / tube, pa + 0.35 - 0.25 / tube, 0.2, 1, 2)
    # Service lines along the inside.
    for ph in [PI - 0.14, PI, -PI + 0.14]:
        for i in range(steps):
            var a := _tp(radius, tube, delta * float(i) / float(steps), ph, 1.6)
            var c := _tp(radius, tube, delta * float(i + 1) / float(steps), ph, 1.6)
            b.beam("copper" if ph == PI else "steel", a, c, 1.3)
    for i in range(steps):
        b.box("truss", Vector3(0.6, 20.0, 2.2), _on_tube(radius, tube, delta * float(i) / float(steps), PI, 0.8))
    # Radiator fins underneath.
    for i in range(3):
        var xf := _on_tube(radius, tube, delta * (float(i) + 0.5) / 3.0, -2.38, 3.0)
        b.box("silver", Vector3(delta * radius / 3.0 - 6.0, 0.3, 6.0), xf)
    # Frames: heavy at the sector's ends, light in the middle.
    _tube_frame(b, "steel", radius, tube, 0.0, 0.7, 0.9)
    _tube_frame(b, "truss", radius, tube, delta * 0.5, 0.5, 0.45)
    # Fittings on the shield: airlocks, antennas, equipment boxes, beacons.
    for k in range(12):
        var th := delta * rng.randf_range(0.06, 0.94)
        var ph := rng.randf_range(1.0, 2.1) * (1.0 if rng.randf() < 0.5 else -1.0)
        var kind := rng.randi() % 4
        match kind:
            0:
                var xf := _on_tube(radius, tube, th, ph, 1.6)
                b.box("steel", Vector3(5.0, 4.0, 2.6), xf)
                b.box("window", Vector3(1.0, 1.0, 0.2), xf * Transform3D(Basis.IDENTITY, Vector3(0, 0, 1.35)))
            1:
                var xf := _on_tube(radius, tube, th, ph, 0.3)
                b.beam("truss", xf.origin, xf * Vector3(0, 0, 9.0), 0.35)
                b.lathe("silver", PackedVector2Array([Vector2(0.0, 0.0), Vector2(0.4, 1.2), Vector2(1.1, 2.4)]), 14,
                    xf * Transform3D(Basis.IDENTITY, Vector3(0, 0, 9.0)))
            2:
                b.box("foil", Vector3(rng.randf_range(3.0, 7.0), rng.randf_range(3.0, 5.0), 2.0), _on_tube(radius, tube, th, ph, 1.3))
            _:
                b.box("window", Vector3(0.7, 0.7, 0.7), _on_tube(radius, tube, th, ph, 0.7))
    # Junction: the socket where a spoke enters the ring.
    if variant == "junction":
        var radial := Vector3(1, 0, 0)
        var basis := Basis(Vector3(0, 1, 0), Vector3(0, 0, 1), -radial)
        b.lathe("steel", PackedVector2Array([Vector2(-4.0, 15.0), Vector2(3.0, 15.0), Vector2(14.0, 9.5)]), 24,
            Transform3D(basis, radial * (radius - tube + 2.0)))
    return b.commit_arrays()

# The ring's own parts drawn once: faction band, spokes and the spinning hub collar.
static func _ring_extras(radius: float, collar_m: float) -> Dictionary:
    var tube := TUBE_RATIO * radius
    var b := MeshBuilder.new()
    var segments := 384
    for i in range(segments):
        _patch(b, "band", radius, tube, TAU * float(i) / float(segments), TAU * float(i + 1) / float(segments), -0.12, 0.12, 0.25, 1, 2)
    # Spokes: lift tubes 15 m across (as on the Stanford torus) with frames and elevator rails.
    var spoke_r := 7.5
    for k in range(6):
        var a := TAU * float(k) / 6.0
        var radial := Vector3(cos(a), sin(a), 0.0)
        var tangent := Vector3(-sin(a), cos(a), 0.0)
        var basis := Basis(tangent, Vector3(0, 0, 1), radial)
        var r0 := collar_m - 1.0
        var r1 := radius - tube + 3.0
        b.cylinder("steel", r0, r1, spoke_r, 24, Transform3D(basis, Vector3.ZERO), true)
        var frames := int((r1 - r0) / 25.0)
        for f in range(1, frames):
            b.torus("truss", 0.0, spoke_r + 0.3, 0.45, Transform3D(Basis(tangent, Vector3(0, 0, 1), radial), radial * lerpf(r0, r1, float(f) / float(frames))), 24, 4)
        for side in [-1.0, 1.0]:
            var off: Vector3 = tangent * side * (spoke_r + 2.5)
            b.beam("truss", radial * r0 + off, radial * (r1 - 10.0) + off, 0.9)
            for f in range(1, frames):
                var at := radial * lerpf(r0, r1, float(f) / float(frames))
                b.beam("truss", at + tangent * side * spoke_r, at + off, 0.4)
        # Spinning hub collar sockets.
        b.cylinder("steel", r0 - 4.0, r0 + 8.0, spoke_r + 2.0, 24, Transform3D(basis, Vector3.ZERO))
    b.cylinder("blanket", -0.1 * CORE_M, 0.1 * CORE_M, collar_m, 48)
    for z in [-0.1 * CORE_M, 0.1 * CORE_M]:
        b.torus("steel", z, collar_m + 0.2, 0.6, Transform3D.IDENTITY, 48, 4)
    var rng := RandomNumberGenerator.new()
    rng.seed = int(radius)
    for z in [-4.0, 4.0]:
        _window_ring(b, z, collar_m, 40, Vector2(1.8, 1.2), rng)
    return b.commit_arrays()

# Everything DETAIL needs, as arrays (worker thread).
static func _detail_arrays(data: Dictionary) -> Dictionary:
    var hull := _hull(data, true)
    var radius := float(hull["ring_radius_m"])
    var per_spoke := _sectors_per_spoke(radius)
    var delta := TAU / float(6 * per_spoke)
    var sectors := {}
    for variant in SECTOR_VARIANTS:
        sectors[variant] = _ring_sector(radius, delta, variant)
    return {"hull": hull["arrays"], "extras": _ring_extras(radius, 0.2 * CORE_M), "sectors": sectors,
        "per_spoke": per_spoke, "delta": delta}

# Engine resources from _detail_arrays (main thread): meshes, and one MultiMesh per sector
# variant with the sectors' turns round the ring (shared by all rings of this size).
static func _finish_detail(arrays: Dictionary) -> Dictionary:
    var per_spoke := int(arrays["per_spoke"])
    var delta := float(arrays["delta"])
    var multimeshes: Array = []
    for variant in SECTOR_VARIANTS:
        var built := MeshBuilder.to_mesh(arrays["sectors"][variant])
        var mesh: ArrayMesh = built["mesh"]
        var slots: Array = built["slots"]
        for i in range(slots.size()):
            mesh.surface_set_material(i, Materials.shared(String(slots[i])))
        var turns: Array[Transform3D] = []
        for k in range(6 * per_spoke):
            if _sector_variant(k, per_spoke) == variant:
                turns.append(Transform3D(Basis(Vector3(0, 0, 1), delta * float(k)), Vector3.ZERO))
        var mm := MultiMesh.new()
        mm.transform_format = MultiMesh.TRANSFORM_3D
        mm.mesh = mesh
        mm.instance_count = turns.size()
        for i in range(turns.size()):
            mm.set_instance_transform(i, turns[i])
        multimeshes.append(mm)
    return {"hull": MeshBuilder.to_mesh(arrays["hull"]), "extras": MeshBuilder.to_mesh(arrays["extras"]),
        "multimeshes": multimeshes}

static var _detail_cache := {}
static var _pending := {}
static var _finished := {}
static var _mutex := Mutex.new()

# The detailed model, built now (blocking).
static func detail_info(data: Dictionary) -> Dictionary:
    var key := _key(data)
    if not _detail_cache.has(key):
        _detail_cache[key] = _finish_detail(_detail_arrays(data))
    return _detail_cache[key]

# True once the detailed model is ready; otherwise starts (or keeps waiting for) a
# background build (~0.1-0.3 s of GDScript).
static func request_detail(data: Dictionary) -> bool:
    var key := _key(data)
    if _detail_cache.has(key):
        return true
    if _pending.has(key):
        var task: int = _pending[key]
        if not WorkerThreadPool.is_task_completed(task):
            return false
        WorkerThreadPool.wait_for_task_completion(task)
        _pending.erase(key)
        _mutex.lock()
        var arrays: Dictionary = _finished.get(key, {})
        _finished.erase(key)
        _mutex.unlock()
        if arrays.is_empty():
            return false
        _detail_cache[key] = _finish_detail(arrays)
        return true
    var copy := data.duplicate(true)
    _pending[key] = WorkerThreadPool.add_task(func() -> void:
        var arrays := _detail_arrays(copy)
        _mutex.lock()
        _finished[key] = arrays
        _mutex.unlock())
    return false

# --- Nodes ------------------------------------------------------------------------

# The station's model in its container: the hull and its spinning ring(s). The container
# is turned so the station's axis stands up from the ecliptic.
static func ensure(container: Node3D, data: Dictionary, lod := COARSE) -> void:
    var key := _key(data)
    if String(container.get_meta("model_key", "")) == key and int(container.get_meta("lod", COARSE)) == lod:
        return
    var info := model_info(data)
    var detail := detail_info(data) if lod == DETAIL else {}
    for child in container.get_children():
        if String(child.name).begins_with("ring") or String(child.name) == "model":
            child.queue_free()
            container.remove_child(child)
    var hull := MeshInstance3D.new()
    hull.name = "model"
    Materials.apply_mesh(hull, detail["hull"] if lod == DETAIL else info["hull"], container)
    container.add_child(hull)
    var i := 0
    for z in info["ring_z"]:
        var ring := MeshInstance3D.new()
        ring.name = "ring_%d" % i
        ring.position = Vector3(0.0, 0.0, float(z))
        ring.set_meta("spin", 1.0 if i % 2 == 0 else -1.0)
        Materials.apply_mesh(ring, detail["extras"] if lod == DETAIL else info["ring"], container)
        if lod == DETAIL:
            for mm in detail["multimeshes"]:
                var sectors := MultiMeshInstance3D.new()
                sectors.multimesh = mm
                sectors.layers = Materials.MODEL_LAYERS
                ring.add_child(sectors)
        container.add_child(ring)
        i += 1
    container.rotation = Vector3(-PI * 0.5, 0.0, 0.0)
    container.set_meta("model_key", key)
    container.set_meta("lod", lod)
    container.set_meta("radius_m", info["radius_m"])
    container.set_meta("berths", info["berths"])
    container.set_meta("core_radius_m", info["core_radius_m"])
    container.set_meta("spin_rad_s", info["spin_rad_s"])

static func spin_rings(container: Node3D, spin_time_s: float) -> void:
    var rate := float(container.get_meta("spin_rad_s", 0.1))
    for child in container.get_children():
        if child.has_meta("spin"):
            (child as Node3D).rotation = Vector3(0.0, 0.0, fmod(spin_time_s * rate, TAU) * float(child.get_meta("spin")))
