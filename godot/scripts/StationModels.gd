extends RefCounted

# Station models, built in metres at real size and drawn at the ships' scale (Main.gd
# ship_metres_to_world), so docked ships are in proportion.
#
# Every station has a design (DESIGN_BY_STATION, else by economy profile, else the plain
# torus): a spinning habitat of its own kind round a shared non-spinning docking core.
# Habitats give their design's gravity at the rim (GRAVITY_M_S2 and GRAVITY_LOW_M_S2; spin from radius) and are sized by the seeded population:
# - torus: the Stanford torus (900 m radius, 65 m tube, 10,000 people), radius with the
#   square root of the people per ring, at least 250 m; two counter-rotating rings a tube
#   diameter apart from 15,000 people. garden_torus adds 45° mirror crowns that send
#   sunlight into the rings' sunlight windows and more greenhouse sectors; yard_torus adds
#   two dry docks with hulls being built.
# - sphere: a Bernal sphere (radius from ~70 m² of shell per person), polar glazing fed by
#   conical mirrors round each pole.
# - shaded_drum: a drum (280 m radius, length from the people) behind a sun-tracking heat
#   shield, its radiators edge-on behind it.
# - shielded_drum: a drum armoured in ice blocks inside three magnetic-shield coils.
# - bola: habitat pods on long tethers (450 m), two crossed counter-rotating bolas from
#   15,000 people, their planes far enough apart for the pods to pass. A pod is three
#   drums standing on the tether, twelve flat decks each, sized by the people.
# - rock: a hollowed asteroid spinning about its long axis, a lit window trench round
#   its waist.
# - module_wheel: twelve modules joined into a wheel (250 m), four spokes.
# - carousel: six habitat cans on lattice arms (300 m).
# Outer stations (rock, shielded_drum, carousel) get a reactor on a boom instead of solar
# arrays.
#
# The core (local Z is the station's axis; Main tilts it to stand up from the ecliptic):
# a spine with a docking hub at each end, far enough beyond the habitat that docked ships
# clear it, four berthing arms per hub (docked ships sit at their ends, bow to the hub);
# storage tanks by capacity, arrays and radiators; a flavour by economy profile
# (greenhouses, fabrication blocks, sphere tank farm, solar furnace, container stacks, ore
# bins, shuttle pads, extraction rigs).
#
# Two levels of detail, the same shapes in both so the switch is quiet: COARSE (plain
# shapes) and DETAIL (repeated parts drawn as MultiMeshes of turns about the axis, lattice
# trusses, panels, windows and fittings; built on a worker thread, request_detail()).
# Main.gd builds every station's DETAIL in the background after start-up (prefetch()).

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
# Shell area per person for the sphere and drums (the Stanford torus gives about this).
const AREA_PER_PERSON_M2 := 70.0
# How far the docking hubs stand beyond the habitat along the axis.
const HUB_CLEARANCE_M := 200.0
# Bola pods: drums with their axis along the tether, decks stacked towards the hub, the
# lowest deck at the 1 g rim. Domes as a share of the drum radius; the gap between drums.
const BOLA_DECKS := 12
const BOLA_DECK_M := 5.0
const BOLA_DOME := 0.3
const BOLA_GAP_M := 16.0

const DESIGN_BY_STATION := {
    "earth_l1": "garden_torus",
    "earth_orbit": "yard_torus",
    "venus_cloud": "sphere",
    "mercury_yard": "shaded_drum",
    "mars_transfer": "bola",
    "ceres_depot": "rock",
    "luna_base": "module_wheel",
    "ganymede_depot": "shielded_drum",
    "titan_works": "carousel",
}
const DESIGN_BY_PROFILE := {
    "agri_hub": "garden_torus",
    "industrial_hub": "yard_torus",
    "chem_hub": "sphere",
    "smelter_hub": "shaded_drum",
    "frontier_hub": "bola",
    "belt_port": "rock",
    "lunar_port": "module_wheel",
    "extractor_hub": "shielded_drum",
}
const TORUS_DESIGNS := ["torus", "garden_torus", "yard_torus"]
const REACTOR_DESIGNS := ["rock", "shielded_drum", "carousel"]

static var _cache := {}

static func design_of(data: Dictionary) -> String:
    var id := String(data.get("id", ""))
    if DESIGN_BY_STATION.has(id):
        return DESIGN_BY_STATION[id]
    return String(DESIGN_BY_PROFILE.get(String(data.get("economy_profile_id", "")), "torus"))

# Seeded population: the model is the station as founded, not rebuilt as people come and go.
static func _population(data: Dictionary) -> float:
    return maxf(float(data.get("seeded_population", data.get("population", 10000.0))), 1000.0)

# Habitat ring radius for a station's population, split over its rings.
static func ring_radius_m(population: float) -> float:
    var per_ring := maxf(population, 1000.0) / float(2 if population >= TWO_RING_POPULATION else 1)
    return maxf(MIN_RING_RADIUS_M, STANFORD_RADIUS_M * sqrt(per_ring / STANFORD_POPULATION))

# The gravity each design's habitat gives at its rim: 1 g for the torus, sphere and bola (people
# live in them for the long run); Mars surface gravity for the compact industrial habitats
# (drums, rock, module wheel, carousel). Their rims are 240-300 m: at 1 g they would spin at
# 1.9 rpm, at the edge of comfort; at 0.38 g they spin at 1.1-1.2 rpm. Visual only.
const GRAVITY_M_S2 := 9.81
const GRAVITY_LOW_M_S2 := 3.73
const LOW_GRAVITY_DESIGNS := ["shaded_drum", "shielded_drum", "rock", "module_wheel", "carousel"]

# Spin that gives gravity_m_s2 at the rim.
static func ring_spin_rad_s(radius_m: float, gravity_m_s2: float = 9.81) -> float:
    return sqrt(gravity_m_s2 / maxf(radius_m, 1.0))

static func _key(data: Dictionary) -> String:
    return "%s|%s|%d|%d" % [design_of(data), String(data.get("economy_profile_id", "")), int(round(_population(data))),
        int(_tank_count(data)) + (100 if float(data.get("fuel_factory_per_day", 0.0)) > 0.0 else 0)]

static func _tank_count(data: Dictionary) -> int:
    return clampi(int(float(data.get("storage_capacity", 20000.0)) / 9000.0), 2, 8)

# The habitat's kind and size:
# {design, kind, rim_m (the radius that gives the design's gravity), spinners: [[z, spin sign, phase, flipped]], reach_m
#  (how far along the axis it reaches), clear_m (the spine is free beyond this), solid (it
#  encloses the spine), extent_m (its radius), and per kind: tube_m, half_m (drum half
#  length), dome_m, rock_half_m}
static func _habitat(data: Dictionary) -> Dictionary:
    var design := design_of(data)
    var pop := _population(data)
    var two := pop >= TWO_RING_POPULATION
    var h := {"design": design, "solid": false}
    match design:
        "sphere":
            var r := maxf(MIN_RING_RADIUS_M, sqrt(pop * AREA_PER_PERSON_M2 / (4.0 * PI)))
            h.merge({"kind": "sphere", "rim_m": r, "spinners": [[0.0, 1.0, 0.0, false]], "reach_m": 1.22 * r,
                "clear_m": r + 20.0, "solid": true, "extent_m": r * 1.02, "surface_m": r})
        "shaded_drum", "shielded_drum":
            var r := 280.0 if design == "shaded_drum" else 260.0
            var half := 0.5 * clampf(pop * AREA_PER_PERSON_M2 / (TAU * r), 140.0, 3.0 * r)
            var dome := 0.1 * r
            h.merge({"kind": "drum", "rim_m": r, "spinners": [[0.0, 1.0, 0.0, false]], "half_m": half, "dome_m": dome,
                "reach_m": half + dome, "clear_m": half + dome + 20.0, "solid": true, "extent_m": r * 1.03,
                "surface_m": half + dome})
        "rock":
            var r := maxf(300.0, 1.25 * sqrt(pop * AREA_PER_PERSON_M2 / (4.0 * PI)))
            var half := 1.35 * r
            h.merge({"kind": "rock", "rim_m": r - 60.0, "spinners": [[0.0, 1.0, 0.0, false]], "rock_r_m": r,
                "rock_half_m": half, "reach_m": half + 10.0, "clear_m": half + 20.0, "solid": true, "extent_m": r * 1.2,
                "surface_m": half * 0.98})
        "bola":
            var r := 450.0
            # Each pod: three drums standing on the tether, BOLA_DECKS flat decks each.
            var pods := 4.0 if two else 2.0
            var dr := clampf(sqrt(pop * AREA_PER_PERSON_M2 / pods / (3.0 * BOLA_DECKS * PI)), 30.0, 70.0)
            # The two bolas' planes far enough apart that their pods pass each other.
            var off := dr + 20.0 if two else 0.0
            var spinners := [[-off, 1.0, 0.0, false], [off, -1.0, PI * 0.5, false]] if two else [[0.0, 1.0, 0.0, false]]
            var dome := BOLA_DOME * dr
            var span := 3.0 * dr + BOLA_GAP_M + 100.0
            h.merge({"kind": "bola", "rim_m": r, "drum_r_m": dr, "spinners": spinners, "reach_m": off + dr,
                "clear_m": off + dr + 0.25 * CORE_M, "extent_m": Vector2(r + dome + 5.0, span).length()})
        "module_wheel":
            var r := MIN_RING_RADIUS_M
            h.merge({"kind": "wheel", "rim_m": r, "spinners": [[0.0, 1.0, 0.0, false]], "reach_m": 0.0,
                "clear_m": 0.25 * CORE_M, "extent_m": r + 24.0})
        "carousel":
            var r := 300.0
            h.merge({"kind": "carousel", "rim_m": r, "spinners": [[0.0, 1.0, 0.0, false]], "reach_m": 0.0,
                "clear_m": 0.25 * CORE_M, "extent_m": r + 30.0})
        _:
            var r := ring_radius_m(pop)
            var tube := TUBE_RATIO * r
            # Two rings: the lower one is flipped so both rings' sunlight windows (and
            # mirror crowns) face outwards.
            var spinners := [[-2.0 * tube, 1.0, 0.0, true], [2.0 * tube, -1.0, 0.0, false]] if two else [[0.0, 1.0, 0.0, false]]
            h.merge({"kind": "torus", "rim_m": r, "tube_m": tube, "spinners": spinners,
                "reach_m": 2.0 * tube if two else 0.0, "clear_m": (2.0 * tube + 0.2 * CORE_M) if two else 0.25 * CORE_M,
                "extent_m": (r + tube) * 1.02})
    return h

# {hull, sun (or {}), spinner, spinners: [[z, sign, phase, flipped]], radius_m (whole
#  station), core_radius_m (without the habitat), rim_m, berths: [{pos, dir}]}
static func model_info(data: Dictionary) -> Dictionary:
    var key := _key(data)
    if not _cache.has(key):
        _cache[key] = _build(data)
    return _cache[key]

static func _build(data: Dictionary) -> Dictionary:
    var hab := _habitat(data)
    var hull := _hull(data, hab, false)
    var spinner := _spinner(hab, false)
    var core_bound := _bound(hull["arrays"]["aabb"])
    if not hull["sun"].is_empty():
        core_bound = maxf(core_bound, _bound(hull["sun"]["aabb"]))
    for panel in hull["solar"]:
        core_bound = maxf(core_bound, (panel["at"] as Vector3).length() + _bound(panel["arrays"]["aabb"]))
    return {"hull": MeshBuilder.to_mesh(hull["arrays"]), "sun": MeshBuilder.to_mesh(hull["sun"]) if not hull["sun"].is_empty() else {},
        "solar": _solar_meshes(hull["solar"]),
        "spinner": MeshBuilder.to_mesh(spinner["arrays"]), "spinners": hab["spinners"],
        "radius_m": maxf(float(hab["extent_m"]), core_bound), "core_radius_m": core_bound, "rim_m": hab["rim_m"],
        "berths": hull["berths"]}

# The farthest corner of a box from the origin (a bound that holds under turns about Z).
static func _bound(aabb: AABB) -> float:
    var bound := 0.0
    for i in range(8):
        bound = maxf(bound, aabb.get_endpoint(i).length())
    return bound

# The non-spinning core: spine, hubs and berths, tanks, arrays, flavour, the design's own
# fixed parts; and a sun-tracking part (shaded_drum). detail: lattice trusses, modules
# with windows, berth cradles, paneled arrays (DETAIL); arrays only, so it can run on a
# worker thread.
static func _hull(data: Dictionary, hab: Dictionary, detail: bool) -> Dictionary:
    var design := String(hab["design"])
    var rng := RandomNumberGenerator.new()
    rng.seed = hash(_key(data))
    var radius := CORE_M
    var half := maxf(1.2 * radius, float(hab["reach_m"]) + HUB_CLEARANCE_M)
    var clear := float(hab["clear_m"])
    var hub_r := 0.22 * radius
    var b := MeshBuilder.new()
    # Spine: a truss core with pressurized modules along it (not inside a solid habitat).
    var segments := [[-half * 0.75, half * 0.75]]
    if bool(hab["solid"]):
        segments = [[-half * 0.75, -clear], [clear, half * 0.75]]
    if detail:
        _lattice(b, "truss", Vector3(0, 0, -half), Vector3(0, 0, half), 0.08 * radius, 7.0, 0.7, 4)
        b.cylinder("steel", -half, half, 2.6, 12)
        for seg in segments:
            if float(seg[1]) - float(seg[0]) > 30.0:
                _modules(b, float(seg[0]), float(seg[1]), 0.14 * radius, rng)
    else:
        b.cylinder("truss", -half, half, 0.09 * radius, 12)
        for seg in segments:
            if float(seg[1]) - float(seg[0]) > 30.0:
                b.vessel("blanket", float(seg[0]), float(seg[1]), 0.14 * radius, 0.1 * radius, 20)
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
    # Bearings where the habitat turns on the spine: at each ring, or where the spine
    # enters a solid habitat.
    if bool(hab["solid"]):
        var surface := float(hab["surface_m"])
        for s in [-1.0, 1.0]:
            var z0: float = s * (surface - 6.0)
            var z1: float = s * (surface + 18.0)
            b.cylinder("metal", minf(z0, z1), maxf(z0, z1), 0.16 * radius, 32 if detail else 16)
            b.torus("steel", s * (surface + 16.0), 0.16 * radius + 0.4, 0.8, Transform3D.IDENTITY, 32 if detail else 16, 4)
    else:
        for spinner in hab["spinners"]:
            var z := float(spinner[0])
            if detail:
                b.cylinder("metal", z - 0.1 * radius - 3.0, z + 0.1 * radius + 3.0, 0.16 * radius, 32)
            else:
                b.cylinder("steel", z - 0.1 * radius, z + 0.1 * radius, 0.3 * radius, 24)
    # Storage tanks round the spine, between the habitat and the lower hub.
    var tanks := _tank_count(data)
    var tank_r := 0.09 * radius
    var tz0 := -half + hub_r * 2.2
    var tz1 := -clear
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
    # Power: solar arrays near the upper hub, or (outer stations) a reactor on a boom;
    # radiators in the lower part. Venus, close to the Sun, has twice the radiator.
    var mast_z := half - hub_r * 2.6
    var rad_scale := 2.0 if design == "sphere" else 1.0
    var solar_list: Array = []
    if design in REACTOR_DESIGNS:
        _reactor(b, half - 75.0, detail)
    for side in [-1.0, 1.0]:
        var panel_at := Vector3(side * (0.55 * radius + 0.45 * radius), 0.0, mast_z)
        var rad_z := tz0 + 0.3 * (tz1 - tz0)
        var rad_basis := Basis(Vector3(0, 0, 1), Vector3(1, 0, 0), Vector3(0, 1, 0))
        var rad_size := Vector3(0.25 * radius, 0.25, 0.5 * radius * rad_scale)
        var rad_at := Vector3(0.0, side * (0.3 * radius + 0.12 * radius * rad_scale), rad_z)
        var solar := not design in REACTOR_DESIGNS
        if detail:
            if solar:
                _lattice(b, "truss", Vector3(0.0, 0.0, mast_z), Vector3(side * 0.55 * radius, 0.0, mast_z), 1.4, 3.0, 0.2, 3)
                var sb := MeshBuilder.new()
                _panel_grid(sb, "solar", Vector3(0.9 * radius, 0.3, 0.38 * radius), Transform3D.IDENTITY, 12, 5)
                solar_list.append({"arrays": sb.commit_arrays(), "at": panel_at})
            _lattice(b, "truss", Vector3(0.0, 0.0, rad_z), Vector3(0.0, side * 0.3 * radius, rad_z), 1.1, 3.0, 0.18, 3)
            _panel_grid(b, "silver", rad_size, Transform3D(rad_basis, rad_at), 2, int(8 * rad_scale))
        else:
            if solar:
                b.beam("truss", Vector3(0.0, 0.0, mast_z), Vector3(side * 0.55 * radius, 0.0, mast_z), 1.2)
                var sb := MeshBuilder.new()
                sb.box("solar", Vector3(0.9 * radius, 0.3, 0.38 * radius), Transform3D.IDENTITY)
                solar_list.append({"arrays": sb.commit_arrays(), "at": panel_at})
            b.beam("truss", Vector3(0.0, 0.0, rad_z), Vector3(0.0, side * 0.3 * radius, rad_z), 1.0)
            b.box("silver", rad_size, Transform3D(rad_basis, rad_at))
    # The flavour sits between the rings (two rings) or just beyond the habitat.
    var two_rings: bool = hab["kind"] == "torus" and hab["spinners"].size() == 2
    var flavour_z := 0.0 if two_rings else clear + 35.0
    _flavour(b, String(data.get("economy_profile_id", "")), radius, flavour_z, 0.5 * (tz0 + tz1),
        float(data.get("fuel_factory_per_day", 0.0)) > 0.0, detail)
    # The design's own fixed parts.
    match design:
        "yard_torus":
            for side in [-1.0, 1.0]:
                _dry_dock(b, side, flavour_z, detail, side > 0.0)
        "shielded_drum":
            _shield_coils(b, hab, detail)
    var sun := {}
    if design == "shaded_drum":
        sun = _sun_shield(hab, detail)
    return {"arrays": b.commit_arrays(), "sun": sun, "berths": berths, "solar": solar_list}

# --- Core parts -------------------------------------------------------------------

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

# Windows round a cylinder about Z, some lit; xf places the cylinder.
static func _window_ring(b: MeshBuilder, z: float, r: float, count: int, size: Vector2, rng: RandomNumberGenerator, xf := Transform3D.IDENTITY) -> void:
    for k in range(count):
        var ang := TAU * (float(k) + 0.5) / float(count)
        var radial := Vector3(cos(ang), sin(ang), 0.0)
        var basis := Basis(radial.cross(Vector3(0, 0, 1)), Vector3(0, 0, 1), radial)
        b.box("window" if rng.randf() < 0.6 else "metal", Vector3(size.x, size.y, 0.2), xf * Transform3D(basis, radial * (r + 0.05) + Vector3(0, 0, z)))

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

# Local Z turned to point along +X (for parts lathed about Z that lie along X).
static func _z_to_x() -> Basis:
    return Basis(Vector3(0, 1, 0), Vector3(0, 0, 1), Vector3(1, 0, 0))

# A reactor on a boom along +X at height z: shadow shield, reactor, and radiator wings
# above and below the boom, edge-on to the reactor.
static func _reactor(b: MeshBuilder, z: float, detail: bool) -> void:
    var x1 := 150.0
    if detail:
        _lattice(b, "truss", Vector3(18.0, 0.0, z), Vector3(x1, 0.0, z), 2.5, 6.0, 0.35, 3)
        b.beam("copper", Vector3(18.0, 3.2, z), Vector3(x1 - 4.0, 3.2, z), 0.6)
    else:
        b.beam("truss", Vector3(18.0, 0.0, z), Vector3(x1, 0.0, z), 3.0)
    var at := Transform3D(_z_to_x(), Vector3(x1, 0.0, z))
    b.lathe("metal", PackedVector2Array([Vector2(0.0, 0.0), Vector2(0.0, 5.0), Vector2(4.0, 9.0), Vector2(4.0, 0.0)]), 24 if detail else 12, at)
    b.vessel("foil", 4.0, 22.0, 6.0, 3.0, 24 if detail else 12, at)
    if detail:
        for f in [8.0, 12.0, 16.0]:
            b.torus("steel", f, 6.15, 0.3, at, 24, 4)
    for s in [-1.0, 1.0]:
        var xf := Transform3D(Basis.IDENTITY, Vector3(80.0, 0.0, z + s * 26.0))
        if detail:
            _panel_grid(b, "silver", Vector3(110.0, 0.25, 40.0), xf, 10, 3)
        else:
            b.box("silver", Vector3(110.0, 0.25, 40.0), xf)

static func _flavour(b: MeshBuilder, profile: String, radius: float, mid_z: float, low_z: float, fuel_factory: bool, detail: bool) -> void:
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
            _sphere_farm(b, radius, mid_z, 6, 0.24)
        "smelter_hub":
            # A solar furnace: a big concentrator dish on a boom, the furnace at its focus.
            var at := Vector3(0.0, 0.75 * radius, mid_z)
            b.beam("truss", Vector3(0.0, 0.0, mid_z), at, 1.5)
            var dish := PackedVector2Array()
            for i in range(7):
                var rr := 0.35 * radius * float(i) / 6.0
                dish.append(Vector2(0.12 * radius * pow(rr / (0.35 * radius), 2.0), rr))
            var basis := Basis(Vector3(1, 0, 0), Vector3(0, 0, 1), Vector3(0, -1, 0))
            b.lathe("silver", dish, 24, Transform3D(basis, at + Vector3(0.0, 0.12 * radius, 0.0)))
            b.lathe("metal", dish, 24, Transform3D(basis, at + Vector3(0.0, 0.12 * radius, 0.0)), true)
            b.box("metal", Vector3(0.12, 0.12, 0.16) * radius, Transform3D(Basis.IDENTITY, Vector3(0.0, 0.3 * radius, mid_z)))
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
            # Shuttle pads for the surface run, a lander on one.
            for side in [-1.0, 1.0]:
                var at := Vector3(side * 0.4 * radius, 0.0, mid_z)
                b.beam("truss", Vector3(0.0, 0.0, mid_z), at, 1.4)
                b.cylinder("steel", -0.6, 0.6, 0.16 * radius, 24, Transform3D(Basis(Vector3(0, 0, 1), Vector3(0, 1, 0), Vector3(1, 0, 0)), at + Vector3(side * 0.02 * radius, 0.0, 0.0)))
            _lander(b, Vector3(0.42 * radius + 0.8, 0.0, mid_z), detail)
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
        _sphere_farm(b, radius, low_z, 4, 0.46)

static func _sphere_farm(b: MeshBuilder, radius: float, z: float, count: int, out: float) -> void:
    for k in range(count):
        var a := TAU * (float(k) + 0.5) / float(count)
        var r := 0.08 * radius
        var at := Vector3(cos(a), sin(a), 0.0) * out * radius + Vector3(0.0, 0.0, z)
        b.vessel("tank", -r, r, r, r, 16, Transform3D(Basis.IDENTITY, at))
        b.beam("steel", Vector3(0.0, 0.0, z), at, 0.9)

# A surface lander standing on a pad (its feet at `at`, facing +X).
static func _lander(b: MeshBuilder, at: Vector3, detail: bool) -> void:
    var xf := Transform3D(_z_to_x(), at)
    b.vessel("foil", 3.0, 11.0, 5.0, 2.0, 16 if detail else 10, xf)
    b.lathe("metal", PackedVector2Array([Vector2(1.0, 0.0), Vector2(1.0, 1.2), Vector2(3.0, 2.5), Vector2(3.0, 0.0)]), 12, xf)
    for k in range(4):
        var a := PI * 0.5 * float(k) + PI * 0.25
        var foot := at + Vector3(0.0, cos(a) * 7.0, sin(a) * 7.0)
        b.beam("truss", at + Vector3(5.0, cos(a) * 4.5, sin(a) * 4.5), foot, 0.5)
        b.box("metal", Vector3(0.3, 1.6, 1.6), Transform3D(Basis.IDENTITY, foot))
    if detail:
        b.box("window", Vector3(0.3, 1.2, 0.8), Transform3D(Basis.IDENTITY, at + Vector3(10.0, 0.0, 4.7)))

# A dry dock along +X (side +1) or -X at height z: an open lattice frame with a gantry,
# floodlights and a hull: complete (engine fitted) or half built (ribs and keel).
static func _dry_dock(b: MeshBuilder, side: float, z: float, detail: bool, half_built: bool) -> void:
    var turn := Basis(Vector3(0, 0, 1), 0.0 if side > 0.0 else PI)
    var x0 := 60.0
    var x1 := 270.0
    var hy := 36.0
    var hz := 30.0
    var p := func(x: float, y: float, zz: float) -> Vector3: return turn * Vector3(x, y, zz) + Vector3(0.0, 0.0, z)
    for cy in [-hy, hy]:
        for cz in [-hz, hz]:
            if detail:
                _lattice(b, "truss", p.call(x0, cy, cz), p.call(x1, cy, cz), 1.6, 5.0, 0.3, 3)
            else:
                b.beam("truss", p.call(x0, cy, cz), p.call(x1, cy, cz), 2.0)
    var frames := 7
    for i in range(frames + 1):
        var x := lerpf(x0, x1, float(i) / float(frames))
        var corners := [p.call(x, -hy, -hz), p.call(x, hy, -hz), p.call(x, hy, hz), p.call(x, -hy, hz)]
        for k in range(4):
            b.beam("truss", corners[k], corners[(k + 1) % 4], 1.2 if detail else 1.6)
        if detail and i % 2 == 0:
            for k in range(4):
                b.box("window", Vector3(1.2, 1.2, 1.2), Transform3D(Basis.IDENTITY, corners[k]))
    # Struts to the spine.
    for cy in [-hy, hy]:
        for cz in [-hz, hz]:
            b.beam("truss", p.call(18.0, 0.0, cz * 0.3), p.call(x0, cy, cz), 1.4 if detail else 2.0)
    # A gantry across the top, its hoist over the hull.
    var gx := 165.0 if half_built else 210.0
    b.box("steel", Vector3(6.0, 2.0 * hy + 6.0, 4.0), Transform3D(turn, p.call(gx, 0.0, hz + 3.0)))
    b.box("foil", Vector3(8.0, 8.0, 6.0), Transform3D(turn, p.call(gx, 0.0, hz - 3.0)))
    if detail:
        b.beam("steel", p.call(gx, 0.0, hz - 6.0), p.call(gx, 0.0, 16.0), 0.3)
    # The hull along the dock, lathed about X.
    var axis := Basis(turn * Vector3(0, 1, 0), Vector3(0, 0, 1), turn * Vector3(1, 0, 0))
    var hull_xf := Transform3D(axis, Vector3(0.0, 0.0, z))
    if half_built:
        b.vessel("blanket", 90.0, 150.0, 13.0, 8.0, 24 if detail else 14, hull_xf)
        b.beam("steel", p.call(150.0, 0.0, -13.0), p.call(240.0, 0.0, -13.0), 2.0)
        var ribs := 11 if detail else 5
        for i in range(ribs):
            var x := lerpf(155.0, 238.0, float(i) / float(ribs - 1))
            b.torus("steel", x, 13.0, 0.6 if detail else 0.9, hull_xf, 24 if detail else 12, 4)
        if detail:
            for k in range(6):
                var a := TAU * float(k) / 6.0
                b.beam("truss", p.call(150.0, 13.0 * cos(a), 13.0 * sin(a)), p.call(238.0, 13.0 * cos(a), 13.0 * sin(a)), 0.4)
    else:
        b.vessel("blanket", 85.0, 215.0, 13.0, 8.0, 24 if detail else 14, hull_xf)
        b.vessel("tank", 120.0, 175.0, 16.0, 8.0, 24 if detail else 14, hull_xf)
        b.lathe("metal", PackedVector2Array([Vector2(215.0, 0.0), Vector2(215.0, 5.0), Vector2(240.0, 10.0), Vector2(240.0, 0.0)]), 20 if detail else 12, hull_xf)
        if detail:
            for x in [100.0, 190.0, 205.0]:
                b.torus("steel", x, 13.2, 0.4, hull_xf, 24, 4)

# Ganymede: three superconducting coils round the drum (a magnetic shield against
# Jupiter's radiation), held by struts from the spine beyond the drum's ends.
static func _shield_coils(b: MeshBuilder, hab: Dictionary, detail: bool) -> void:
    var r := float(hab["rim_m"]) + 70.0
    var half := float(hab["half_m"])
    var end := float(hab["surface_m"]) + 14.0
    for zc in [-0.6 * half, 0.0, 0.6 * half]:
        b.torus("copper", zc, r, 3.5, Transform3D.IDENTITY, 128 if detail else 64, 8 if detail else 5)
        if detail:
            b.torus("foil", zc, r, 4.2, Transform3D.IDENTITY, 128, 6)
            for k in range(12):
                var a := TAU * (float(k) + 0.5) / 12.0
                b.box("steel", Vector3(6.0, 10.0, 12.0), Transform3D(Basis(Vector3(0, 0, 1), a), Vector3(cos(a), sin(a), 0.0) * r + Vector3(0, 0, zc)))
    for k in range(4):
        var a := PI * 0.25 + PI * 0.5 * float(k)
        var radial := Vector3(cos(a), sin(a), 0.0)
        for s in [-1.0, 1.0]:
            if detail:
                _lattice(b, "truss", Vector3(0, 0, s * end), radial * r + Vector3(0, 0, s * end), 2.0, 8.0, 0.4, 3)
            else:
                b.beam("truss", Vector3(0, 0, s * end), radial * r + Vector3(0, 0, s * end), 2.4)
        if detail:
            _lattice(b, "truss", radial * r + Vector3(0, 0, -end), radial * r + Vector3(0, 0, end), 2.0, 8.0, 0.4, 3)
        else:
            b.beam("truss", radial * r + Vector3(0, 0, -end), radial * r + Vector3(0, 0, end), 2.4)

# Mercury: a heat shield between the drum and the Sun, and radiators edge-on behind the
# drum, on booms above and below it; built facing +X and turned to the Sun (spin_rings).
static func _sun_shield(hab: Dictionary, detail: bool) -> Dictionary:
    var b := MeshBuilder.new()
    var r := float(hab["rim_m"])
    var reach := float(hab["reach_m"])
    var boom_z := reach + 15.0
    var shield_x := r + 160.0
    var back_x := -(r + 30.0)
    var hy := r + 80.0
    var hz := reach + 40.0
    for s in [-1.0, 1.0]:
        var z: float = s * boom_z
        if detail:
            _lattice(b, "truss", Vector3(0, 0, z), Vector3(shield_x - 4.0, 0, z), 3.0, 10.0, 0.5, 3)
            _lattice(b, "truss", Vector3(0, 0, z), Vector3(back_x, 0, z), 3.0, 10.0, 0.5, 3)
            for t in [-1.0, 1.0]:
                b.beam("truss", Vector3(0.35 * shield_x, 0, z), Vector3(shield_x - 4.0, t * 0.6 * hy, z), 0.9)
        else:
            b.beam("truss", Vector3(0, 0, z), Vector3(shield_x, 0, z), 4.0)
            b.beam("truss", Vector3(0, 0, z), Vector3(back_x, 0, z), 4.0)
    # The shield: three layers, gold to the Sun, in a frame.
    var shield_basis := Basis(Vector3(0, 1, 0), Vector3(1, 0, 0), Vector3(0, 0, 1))
    for layer in range(3 if detail else 2):
        var x := shield_x + 6.0 * float(layer)
        var slot := "foil" if layer == 2 or not detail and layer == 1 else "silver"
        if detail:
            _panel_grid(b, slot, Vector3(2.0 * hy, 0.3, 2.0 * hz), Transform3D(shield_basis, Vector3(x, 0, 0)), 12, 6)
        else:
            b.box(slot, Vector3(2.0 * hy, 0.3, 2.0 * hz), Transform3D(shield_basis, Vector3(x, 0, 0)))
    if detail:
        for t in [-1.0, 1.0]:
            b.beam("truss", Vector3(shield_x - 4.0, t * hy, -hz), Vector3(shield_x - 4.0, t * hy, hz), 1.5)
            b.beam("truss", Vector3(shield_x - 4.0, -hy, t * hz), Vector3(shield_x - 4.0, hy, t * hz), 1.5)
    # Radiators behind: a spar joining the booms, panels reaching away from the Sun.
    b.beam("truss", Vector3(back_x, 0, -boom_z), Vector3(back_x, 0, boom_z), 3.0 if detail else 4.0)
    var wing := Transform3D(Basis.IDENTITY, Vector3(back_x - 140.0, 0.0, 0.0))
    var size := Vector3(270.0, 0.3, 1.2 * reach)
    if detail:
        _panel_grid(b, "silver", size, wing, 9, 8)
        b.beam("copper", Vector3(back_x, 0.6, -0.6 * reach), Vector3(back_x - 275.0, 0.6, -0.6 * reach), 0.8)
        b.beam("copper", Vector3(back_x, 0.6, 0.6 * reach), Vector3(back_x - 275.0, 0.6, 0.6 * reach), 0.8)
    else:
        b.box("silver", size, wing)
    return b.commit_arrays()

# --- Habitats (spinning) -----------------------------------------------------------
#
# _spinner(hab, detail) -> {arrays (drawn once per spinner), instanced: [{arrays, turns}]}:
# the instanced parts are built once and drawn as a MultiMesh of turns about the axis
# (DETAIL only; turns only, no offsets, which Godot 4.3 MultiMeshes under scaled nodes
# would misplace).

static func _spinner(hab: Dictionary, detail: bool) -> Dictionary:
    match String(hab["kind"]):
        "sphere":
            return _sphere(hab, detail)
        "drum":
            return _drum(hab, detail)
        "rock":
            return _rock(hab, detail)
        "bola":
            return _bola(hab, detail)
        "wheel":
            return _wheel(hab, detail)
        "carousel":
            return _carousel(hab, detail)
    return _torus(hab, detail)

# Turns about Z for instances k = 0..count-1 where pick(k) is true.
static func _turns(count: int, pick: Callable) -> Array[Transform3D]:
    var turns: Array[Transform3D] = []
    for k in range(count):
        if pick.call(k):
            turns.append(Transform3D(Basis(Vector3(0, 0, 1), TAU * float(k) / float(count)), Vector3.ZERO))
    return turns

# A patch of a parametric surface s(u, v) -> [position, normal].
static func _surf(b: MeshBuilder, slot: String, s: Callable, u0: float, u1: float, v0: float, v1: float, nu := 1, nv := 1) -> void:
    for i in range(nu):
        var ua := lerpf(u0, u1, float(i) / float(nu))
        var ub := lerpf(u0, u1, float(i + 1) / float(nu))
        for j in range(nv):
            var va := lerpf(v0, v1, float(j) / float(nv))
            var vb := lerpf(v0, v1, float(j + 1) / float(nv))
            var p00: Array = s.call(ua, va)
            var p10: Array = s.call(ub, va)
            var p11: Array = s.call(ub, vb)
            var p01: Array = s.call(ua, vb)
            b.tri(slot, p00[0], p10[0], p11[0], p00[1], p10[1], p11[1])
            b.tri(slot, p00[0], p11[0], p01[0], p00[1], p11[1], p01[1])

# A frame on a parametric surface at (u, v): z out along the normal, x along u, lifted.
static func _surf_frame(s: Callable, u: float, v: float, lift: float) -> Transform3D:
    var a: Array = s.call(u, v)
    var c: Array = s.call(u + 1.0e-3, v)
    var n: Vector3 = a[1]
    var x: Vector3 = c[0] - a[0]
    x = (x - n * x.dot(n)).normalized()
    return Transform3D(Basis(x, n.cross(x), n), (a[0] as Vector3) + n * lift)

# A fitting on a surface frame: an airlock, an antenna, an equipment box or a beacon.
static func _fitting(b: MeshBuilder, xf: Transform3D, rng: RandomNumberGenerator) -> void:
    match rng.randi() % 4:
        0:
            b.box("steel", Vector3(5.0, 4.0, 2.6), xf * Transform3D(Basis.IDENTITY, Vector3(0, 0, 1.3)))
            b.box("window", Vector3(1.0, 1.0, 0.2), xf * Transform3D(Basis.IDENTITY, Vector3(0, 0, 2.65)))
        1:
            b.beam("truss", xf.origin, xf * Vector3(0, 0, 9.0), 0.35)
            b.lathe("silver", PackedVector2Array([Vector2(0.0, 0.0), Vector2(0.4, 1.2), Vector2(1.1, 2.4)]), 14,
                xf * Transform3D(Basis.IDENTITY, Vector3(0, 0, 9.0)))
        2:
            b.box("foil", Vector3(rng.randf_range(3.0, 7.0), rng.randf_range(3.0, 5.0), 2.0), xf * Transform3D(Basis.IDENTITY, Vector3(0, 0, 1.0)))
        _:
            b.box("window", Vector3(0.7, 0.7, 0.7), xf * Transform3D(Basis.IDENTITY, Vector3(0, 0, 0.35)))

# A 45° conical mirror band about Z (sunlight from the side is sent along the axis),
# from (z0, r0) up and out by w; struts down to (strut_z, strut_r). sgn -1 mirrors it.
static func _crown(b: MeshBuilder, z0: float, r0: float, w: float, strut_z: float, strut_r: float, detail: bool, sgn := 1.0) -> void:
    var prof := PackedVector2Array([Vector2(sgn * z0, r0), Vector2(sgn * (z0 + w), r0 + w)])
    var segs := 160 if detail else 64
    b.lathe("mirror", prof, segs)
    b.lathe("truss", prof, segs, Transform3D(Basis.IDENTITY, Vector3(0, 0, -sgn * 0.5)), true)
    var struts := 48 if detail else 12
    for k in range(struts):
        var a := TAU * (float(k) + 0.5) / float(struts)
        var radial := Vector3(cos(a), sin(a), 0.0)
        b.beam("truss", radial * strut_r + Vector3(0, 0, sgn * strut_z), radial * (r0 + 0.5 * w) + Vector3(0, 0, sgn * (z0 + 0.5 * w - 0.6)), 1.2 if detail else 1.8)
    if detail:
        for k in range(segs / 2):
            var a := TAU * float(k) / float(segs / 2)
            var radial := Vector3(cos(a), sin(a), 0.0)
            b.beam("steel", radial * (r0 + 0.3) + Vector3(0, 0, sgn * (z0 + 0.4)), radial * (r0 + w + 0.3) + Vector3(0, 0, sgn * (z0 + w + 0.4)), 0.4)

# --- Torus -------------------------------------------------------------------------
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

# The variant of sector k (sector 0 starts at a spoke); farm_heavy: two in three are farms.
static func _sector_variant(k: int, per_spoke: int, farm_heavy := false) -> String:
    var j := k % per_spoke
    if j == 0:
        return "junction"
    if farm_heavy:
        return "home_a" if j % 3 == 1 else "farm"
    if j % 3 == 2:
        return "farm"
    return "home_a" if j % 2 == 1 else "home_b"

static func _torus(hab: Dictionary, detail: bool) -> Dictionary:
    var radius := float(hab["rim_m"])
    var tube := float(hab["tube_m"])
    var garden: bool = hab["design"] == "garden_torus"
    var collar := 0.2 * CORE_M
    var b := MeshBuilder.new()
    if detail:
        _ring_extras(b, radius, collar)
    else:
        _ring_coarse(b, radius, collar)
    if garden:
        _crown(b, 1.5 * tube, radius - 1.5 * tube, 1.3 * tube, 0.85 * tube, radius - 0.5 * tube, detail)
    var result := {"arrays": b.commit_arrays(), "instanced": []}
    if detail:
        var per_spoke := _sectors_per_spoke(radius)
        var count := 6 * per_spoke
        var delta := TAU / float(count)
        for variant in SECTOR_VARIANTS:
            result["instanced"].append({"arrays": _ring_sector(radius, delta, variant),
                "turns": _turns(count, func(k: int) -> bool: return _sector_variant(k, per_spoke, garden) == variant)})
    return result

# A plain habitat ring: torus, window strips facing out, the faction band, spokes to the
# hub collar (radius collar_m).
static func _ring_coarse(b: MeshBuilder, radius: float, collar_m: float) -> void:
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
        _fitting(b, _on_tube(radius, tube, th, ph, 0.3), rng)
    # Junction: the socket where a spoke enters the ring.
    if variant == "junction":
        var radial := Vector3(1, 0, 0)
        var basis := Basis(Vector3(0, 1, 0), Vector3(0, 0, 1), -radial)
        b.lathe("steel", PackedVector2Array([Vector2(-4.0, 15.0), Vector2(3.0, 15.0), Vector2(14.0, 9.5)]), 24,
            Transform3D(basis, radial * (radius - tube + 2.0)))
    return b.commit_arrays()

# The ring's own parts drawn once: faction band, spokes and the spinning hub collar.
static func _ring_extras(b: MeshBuilder, radius: float, collar_m: float) -> void:
    var tube := TUBE_RATIO * radius
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
    _collar(b, collar_m, 0.1 * CORE_M, true, int(radius))

# The spinning collar round the spine that a habitat hangs from.
static func _collar(b: MeshBuilder, r: float, half: float, detail: bool, seed: int) -> void:
    b.cylinder("blanket", -half, half, r, 48 if detail else 24)
    for z in [-half, half]:
        b.torus("steel", z, r + 0.2, 0.6, Transform3D.IDENTITY, 48 if detail else 24, 4)
    if detail:
        var rng := RandomNumberGenerator.new()
        rng.seed = seed
        for z in [-0.35 * half, 0.35 * half]:
            _window_ring(b, z, r, 40, Vector2(1.8, 1.2), rng)

# --- Sphere (Bernal) ----------------------------------------------------------------
#
# Latitude zones (degrees): polar caps to 72, glazing 58-72 (lit by the mirror crowns),
# shield panels 26-58, rows of homes' windows 6-26, the faction band at the equator.
# DETAIL: 64 gores drawn as MultiMeshes.

static func _sph(r: float, lift: float) -> Callable:
    return func(th: float, lat: float) -> Array:
        var n := Vector3(cos(lat) * cos(th), cos(lat) * sin(th), sin(lat))
        return [n * (r + lift), n]

static func _sphere(hab: Dictionary, detail: bool) -> Dictionary:
    var r := float(hab["rim_m"])
    var b := MeshBuilder.new()
    var g := deg_to_rad(1.0)
    if not detail:
        var prof := PackedVector2Array()
        for i in range(33):
            var lat := -PI * 0.5 + PI * float(i) / 32.0
            prof.append(Vector2(r * sin(lat), r * cos(lat)))
        b.lathe("blanket", prof, 64)
        for sgn in [-1.0, 1.0]:
            _surf(b, "glass", _sph(r, 0.4), 0.0, TAU, sgn * 58.0 * g, sgn * 72.0 * g, 64, 2)
            _surf(b, "tank", _sph(r, 0.3), 0.0, TAU, sgn * 30.0 * g, sgn * 56.0 * g, 64, 4)
            for lat in [10.0, 16.0, 22.0]:
                var count := 96
                for k in range(count):
                    var th := TAU * (float(k) + 0.5) / float(count)
                    b.box("window", Vector3(0.5 * TAU * r * cos(sgn * lat * g) / float(count), 1.8, 0.4), _surf_frame(_sph(r, 0.1), th, sgn * lat * g, 0.0))
    _surf(b, "band", _sph(r, 0.5), 0.0, TAU, -3.0 * g, 3.0 * g, 192 if detail else 64, 1)
    for sgn in [-1.0, 1.0]:
        var cap_z := sqrt(maxf(r * r - 30.0 * 30.0, 0.0))
        var z0: float = sgn * (cap_z - 8.0)
        var z1: float = sgn * (r + 6.0)
        b.cylinder("steel", minf(z0, z1), maxf(z0, z1), 30.0, 48 if detail else 24)
        # Struts from the shell (lat 74°) to the crown.
        _crown(b, 0.97 * r, 0.28 * r, 0.25 * r, r * sin(74.0 * g), r * cos(74.0 * g), detail, sgn)
    var result := {"arrays": b.commit_arrays(), "instanced": []}
    if detail:
        var count := 64
        var delta := TAU / float(count)
        for variant in ["a", "b"]:
            result["instanced"].append({"arrays": _sphere_gore(r, delta, variant),
                "turns": _turns(count, func(k: int) -> bool: return (k % 2 == 0) == (variant == "a"))})
    return result

static func _sphere_gore(r: float, delta: float, variant: String) -> Dictionary:
    var b := MeshBuilder.new()
    var rng := RandomNumberGenerator.new()
    rng.seed = hash(variant) + int(r)
    var g := deg_to_rad(1.0)
    var s0 := _sph(r, 0.0)
    for sgn in [-1.0, 1.0]:
        var lo := func(a: float, c: float) -> Array: return [minf(a, c), maxf(a, c)]
        # Skin.
        for zone in [[0.0, 26.0, "blanket"], [26.0, 58.0, "metal"], [58.0, 72.0, "steel"], [72.0, 89.5, "metal"]]:
            var span: Array = lo.call(sgn * float(zone[0]) * g, sgn * float(zone[1]) * g)
            _surf(b, zone[2], s0, 0.0, delta, span[0], span[1], 3, maxi(1, int((float(zone[1]) - float(zone[0])) / 2.0)))
        # Windows: rows from 6° to 26°.
        for row in range(10):
            var lat: float = sgn * (7.0 + 2.0 * float(row)) * g
            var rho := r * cos(lat)
            var count := maxi(2, int(delta * rho / 4.0))
            for i in range(count):
                var th := delta * (float(i) + 0.5) / float(count)
                _surf(b, "window" if rng.randf() < 0.55 else "metal", _sph(r, 0.15), th - 1.2 / rho, th + 1.2 / rho, lat - 0.7 / r, lat + 0.7 / r)
        # Shield panels 26-58°.
        for i in range(2):
            for j in range(10):
                var la := 26.0 + 3.2 * float(j)
                var span: Array = lo.call(sgn * (la + 0.15) * g, sgn * (la + 3.05) * g)
                var roll := rng.randf()
                _surf(b, "tank" if roll < 0.7 else ("blanket" if roll < 0.92 else "steel"), _sph(r, 0.35),
                    delta * float(i) / 2.0 + 0.3 / r, delta * float(i + 1) / 2.0 - 0.3 / r, span[0], span[1], 1, 2)
        # Glazing 58-72° between mullions.
        for i in range(3):
            for j in range(3):
                var la := 58.0 + 4.67 * float(j)
                var span: Array = lo.call(sgn * (la + 0.2) * g, sgn * (la + 4.47) * g)
                _surf(b, "glass", _sph(r, 0.25), delta * float(i) / 3.0 + 0.25 / r, delta * float(i + 1) / 3.0 - 0.25 / r, span[0], span[1], 1, 2)
        # Hoops at the zone edges.
        for lat_d in [26.0, 58.0, 72.0]:
            var lat: float = sgn * lat_d * g
            for i in range(3):
                var a: Array = _sph(r, 0.7).call(delta * float(i) / 3.0, lat)
                var c: Array = _sph(r, 0.7).call(delta * float(i + 1) / 3.0, lat)
                b.beam("steel", a[0], c[0], 0.9)
        # Fittings on the shield.
        for k in range(5):
            var lat: float = sgn * rng.randf_range(28.0, 56.0) * g
            _fitting(b, _surf_frame(s0, delta * rng.randf_range(0.1, 0.9), lat, 0.3), rng)
    # A meridian rib along the gore's edge.
    for i in range(36):
        var a: Array = _sph(r, 0.6).call(0.0, (-87.0 + 174.0 * float(i) / 36.0) * g)
        var c: Array = _sph(r, 0.6).call(0.0, (-87.0 + 174.0 * float(i + 1) / 36.0) * g)
        b.beam("truss", a[0], c[0], 0.7)
    # Radiator fins on the equator's underside of half the gores.
    if variant == "b":
        b.box("silver", Vector3(delta * r - 4.0, 5.0, 0.3), _surf_frame(s0, delta * 0.5, -40.0 * g, 3.0))
    return b.commit_arrays()

# --- Drum ---------------------------------------------------------------------------
#
# A closed cylinder (radius rim_m, half length half_m) with shallow domed ends.
# shaded_drum: white blanket, three belts of windows, hoops, glazing rings on the ends.
# shielded_drum: dark hull under ice blocks, window slits between them.
# DETAIL: 48 staves drawn as MultiMeshes.

static func _cyl(r: float, lift: float) -> Callable:
    return func(th: float, z: float) -> Array:
        var n := Vector3(cos(th), sin(th), 0.0)
        return [n * (r + lift) + Vector3(0, 0, z), n]

# The end cap at z sign sgn: z = sgn (half + dome (1 - (rho/r)^2)).
static func _cap(r: float, half: float, dome: float, sgn: float, lift: float) -> Callable:
    return func(th: float, rho: float) -> Array:
        var radial := Vector3(cos(th), sin(th), 0.0)
        var n := (radial * (2.0 * dome * rho / (r * r)) + Vector3(0, 0, sgn)).normalized()
        return [radial * rho + Vector3(0, 0, sgn * (half + dome * (1.0 - (rho / r) * (rho / r)))) + n * lift, n]

static func _drum_belts(half: float) -> Array:
    return [-0.6 * half, 0.0, 0.6 * half]

static func _drum(hab: Dictionary, detail: bool) -> Dictionary:
    var r := float(hab["rim_m"])
    var half := float(hab["half_m"])
    var dome := float(hab["dome_m"])
    var shaded: bool = hab["design"] == "shaded_drum"
    var b := MeshBuilder.new()
    if not detail:
        var prof := PackedVector2Array()
        for i in range(9):
            var rho := r * float(i) / 8.0
            prof.append(Vector2(-(half + dome * (1.0 - (rho / r) * (rho / r))), rho))
        for i in range(9):
            var rho := r * float(8 - i) / 8.0
            prof.append(Vector2(half + dome * (1.0 - (rho / r) * (rho / r)), rho))
        b.lathe("blanket" if shaded else "ice", prof, 96)
        if shaded:
            for zc in _drum_belts(half):
                var count := 120
                for k in range(count):
                    var th := TAU * (float(k) + 0.5) / float(count)
                    for row in [-6.0, 0.0, 6.0]:
                        b.box("window", Vector3(0.5 * TAU * r / float(count), 2.0, 0.4), _surf_frame(_cyl(r, 0.1), th, float(zc) + float(row), 0.0))
            for zc in [-0.8 * half, -0.3 * half, 0.3 * half, 0.8 * half]:
                b.torus("steel", zc, r + 0.6, 1.2, Transform3D.IDENTITY, 96, 4)
            for sgn in [-1.0, 1.0]:
                _surf(b, "glass", _cap(r, half, dome, sgn, 0.4), 0.0, TAU, 0.45 * r, 0.75 * r, 64, 2)
        else:
            for zc in [-half * 0.5, half * 0.5]:
                b.torus("metal", zc, r + 3.5, 1.0, Transform3D.IDENTITY, 96, 4)
    # The faction band near each end, the collars where the spine enters.
    for sgn in [-1.0, 1.0]:
        var zb: float = sgn * (half - 10.0)
        _surf(b, "band", _cyl(r, 0.6 if shaded else 5.5), 0.0, TAU, zb - 5.0, zb + 5.0, 192 if detail else 96, 1)
        var z0: float = sgn * (half + dome - 4.0)
        var z1: float = sgn * (half + dome + 8.0)
        b.cylinder("steel", minf(z0, z1), maxf(z0, z1), 32.0, 48 if detail else 24)
    var result := {"arrays": b.commit_arrays(), "instanced": []}
    if detail:
        var count := 48
        var delta := TAU / float(count)
        for variant in ["a", "b"]:
            result["instanced"].append({"arrays": _drum_stave(hab, delta, variant),
                "turns": _turns(count, func(k: int) -> bool: return (k % 2 == 0) == (variant == "a"))})
    return result

static func _drum_stave(hab: Dictionary, delta: float, variant: String) -> Dictionary:
    var r := float(hab["rim_m"])
    var half := float(hab["half_m"])
    var dome := float(hab["dome_m"])
    var shaded: bool = hab["design"] == "shaded_drum"
    var b := MeshBuilder.new()
    var rng := RandomNumberGenerator.new()
    rng.seed = hash(variant) + int(r)
    var side := _cyl(r, 0.0)
    var nz := maxi(4, int(2.0 * half / 12.0))
    _surf(b, "blanket" if shaded else "metal", side, 0.0, delta, -half, half, 3, nz)
    for sgn in [-1.0, 1.0]:
        var cap := _cap(r, half, dome, sgn, 0.0)
        _surf(b, "metal", cap, 0.0, delta, 0.0, r, 1, 12)
    if shaded:
        var belts := _drum_belts(half)
        # Window belts, three rows each over a darker band.
        for zc in belts:
            _surf(b, "metal", _cyl(r, 0.08), 0.0, delta, float(zc) - 9.0, float(zc) + 9.0, 3, 1)
            for row in [-6.0, -2.0, 2.0, 6.0]:
                var count := int(delta * r / 4.0)
                for i in range(count):
                    var th := delta * (float(i) + 0.5) / float(count)
                    var z: float = float(zc) + float(row)
                    _surf(b, "window" if rng.randf() < 0.55 else "metal", _cyl(r, 0.15), th - 1.2 / r, th + 1.2 / r, z - 0.8, z + 0.8)
        # Shield tiles elsewhere, with seams.
        var tiles_z := int(2.0 * half / 10.0)
        for i in range(3):
            for j in range(tiles_z):
                var z0 := -half + 10.0 * float(j)
                var z1 := z0 + 10.0
                var in_belt := false
                for zc in belts:
                    if z1 > float(zc) - 10.0 and z0 < float(zc) + 10.0:
                        in_belt = true
                if in_belt or z1 > half - 16.0 or z0 < -half + 16.0:
                    continue
                var roll := rng.randf()
                _surf(b, "tank" if roll < 0.7 else ("blanket" if roll < 0.92 else "steel"), _cyl(r, 0.35),
                    delta * float(i) / 3.0 + 0.3 / r, delta * float(i + 1) / 3.0 - 0.3 / r, z0 + 0.3, z1 - 0.3)
        # Hoops.
        for zc in [-0.8 * half, -0.3 * half, 0.3 * half, 0.8 * half]:
            for i in range(3):
                var a: Array = _cyl(r, 0.8).call(delta * float(i) / 3.0, zc)
                var c: Array = _cyl(r, 0.8).call(delta * float(i + 1) / 3.0, zc)
                b.beam("steel", a[0], c[0], 1.2)
        # Glazing rings on the ends, between mullions.
        for sgn in [-1.0, 1.0]:
            for j in range(3):
                var rho0 := (0.45 + 0.1 * float(j)) * r
                _surf(b, "glass", _cap(r, half, dome, sgn, 0.3), 0.3 / r, delta - 0.3 / r, rho0 + 0.5, rho0 + 0.1 * r - 0.5, 1, 2)
        for k in range(6):
            var z := rng.randf_range(-half + 20.0, half - 20.0)
            var near_belt := false
            for zc in belts:
                if absf(z - float(zc)) < 14.0:
                    near_belt = true
            if not near_belt:
                _fitting(b, _surf_frame(side, delta * rng.randf_range(0.1, 0.9), z, 0.3), rng)
    else:
        # Ice blocks: rows along the drum, three across a stave, slits of window between some rows.
        var rows := int(2.0 * half / 9.0)
        for j in range(rows):
            var z := -half + 9.0 * (float(j) + 0.5)
            if j % 7 == 3:
                for i in range(int(delta * r / 4.0)):
                    var th := delta * (float(i) + 0.5) / float(int(delta * r / 4.0))
                    _surf(b, "window" if rng.randf() < 0.6 else "metal", _cyl(r, 0.2), th - 1.0 / r, th + 1.0 / r, z - 1.0, z + 1.0)
                continue
            for i in range(3):
                var th := delta * (float(i) + 0.5) / 3.0
                var depth := rng.randf_range(2.5, 5.5)
                b.box("ice", Vector3(delta * r / 3.0 - 0.8, 8.2, depth), _surf_frame(side, th, z, depth * 0.5))
        # The ends: blocks in rings.
        for sgn in [-1.0, 1.0]:
            var cap := _cap(r, half, dome, sgn, 0.0)
            for j in range(int((r - 40.0) / 12.0)):
                var rho := 40.0 + 12.0 * (float(j) + 0.5)
                var across := maxi(1, int(delta * rho / 11.0))
                for i in range(across):
                    var th := delta * (float(i) + 0.5) / float(across)
                    var depth := rng.randf_range(2.0, 4.5)
                    b.box("ice", Vector3(delta * rho / float(across) - 0.8, 11.2, depth), _surf_frame(cap, th, rho, depth * 0.5))
        for k in range(4):
            _fitting(b, _surf_frame(side, delta * rng.randf_range(0.1, 0.9), rng.randf_range(-half + 15.0, half - 15.0), 5.5), rng)
    return b.commit_arrays()

# --- Rock (hollowed asteroid) ---------------------------------------------------------
#
# An ellipsoid (rock_r across, rock_half along the spin axis) roughened by a seeded sum of
# waves, with craters, a trench round the waist holding rows of windows, and flattened
# poles where the spine enters. COARSE and DETAIL share the shape (different grids).

static func _rock_shape() -> Dictionary:
    var rng := RandomNumberGenerator.new()
    rng.seed = 4242
    # Waves: a few broad lumps (a potato, not an egg), then finer relief.
    var waves: Array = []
    for band in [[4, 0.8, 1.4, 0.07, 0.11], [8, 2.0, 4.0, 0.03, 0.05], [10, 6.0, 10.0, 0.01, 0.02], [10, 12.0, 20.0, 0.004, 0.008]]:
        for i in range(int(band[0])):
            var k := Vector3(rng.randf_range(-1, 1), rng.randf_range(-1, 1), rng.randf_range(-1, 1)).normalized()
            waves.append([k * rng.randf_range(band[1], band[2]), rng.randf_range(band[3], band[4]), rng.randf() * TAU])
    # Craters, clear of the poles and of the waist trench.
    var craters: Array = []
    while craters.size() < 22:
        var d := Vector3(rng.randf_range(-1, 1), rng.randf_range(-1, 1), rng.randf_range(-0.8, 0.8)).normalized()
        var rad := rng.randf_range(0.06, 0.22)
        var lat := absf(asin(d.z))
        if lat + rad > 1.1 or lat - 1.35 * rad < 0.08:
            continue
        craters.append([d, rad, rng.randf_range(0.03, 0.07), cos(1.7 * rad)])
    return {"waves": waves, "craters": craters}

# The rock's surface point along the unit direction dir.
static func _rock_point(hab: Dictionary, shape: Dictionary, dir: Vector3) -> Vector3:
    var a := float(hab["rock_r_m"])
    var c := float(hab["rock_half_m"])
    var base := 1.0 / sqrt((dir.x * dir.x + dir.y * dir.y) / (a * a) + dir.z * dir.z / (c * c))
    var f := 1.0
    for w in shape["waves"]:
        f += float(w[1]) * sin((w[0] as Vector3).dot(dir) + float(w[2]))
    # Craters: a bowl and a smooth raised rim (no step at the edge, so the grid resolves it).
    for cr in shape["craters"]:
        var cosine := dir.dot(cr[0])
        if cosine < float(cr[3]):
            continue
        var t := acos(minf(cosine, 1.0)) / float(cr[1])
        var depth := float(cr[2])
        if t < 1.0:
            f -= depth * (1.0 - t * t)
        var rim := (t - 0.95) / 0.25
        f += 0.3 * depth * exp(-rim * rim)
    var lat := asin(clampf(dir.z, -1.0, 1.0))
    # The poles flatten towards the bare ellipsoid, where the spine's bearings sit.
    f = lerpf(f, 0.98, smoothstep(1.15, 1.4, absf(lat)))
    var rr := base * f
    # The waist trench: 14 m deep, ~24 m wide.
    var t := absf(lat) * a / 12.0
    if t < 1.0:
        rr -= 14.0 * (1.0 - pow(t, 6.0))
    return dir * rr

static func _rock(hab: Dictionary, detail: bool) -> Dictionary:
    var shape := _rock_shape()
    var b := MeshBuilder.new()
    var n_lat := 160 if detail else 40
    var n_lon := 360 if detail else 80
    var lat_max := deg_to_rad(84.0)
    # The grid, rows of latitude; the trench's walls get rows of their own.
    var lats: Array[float] = []
    var a := float(hab["rock_r_m"])
    for i in range(n_lat + 1):
        lats.append(-lat_max + 2.0 * lat_max * float(i) / float(n_lat))
    for x in [-14.0, -11.0, -6.0, 6.0, 11.0, 14.0]:
        lats.append(x / a)
    lats.sort()
    var grid: Array = []
    for lat in lats:
        var row: Array[Vector3] = []
        for j in range(n_lon):
            var lon := TAU * float(j) / float(n_lon)
            row.append(_rock_point(hab, shape, Vector3(cos(lat) * cos(lon), cos(lat) * sin(lon), sin(lat))))
        grid.append(row)
    var rows := grid.size()
    var normals: Array = []
    for i in range(rows):
        var row: Array[Vector3] = []
        for j in range(n_lon):
            var east: Vector3 = grid[i][(j + 1) % n_lon] - grid[i][(j - 1 + n_lon) % n_lon]
            var north: Vector3 = grid[mini(i + 1, rows - 1)][j] - grid[maxi(i - 1, 0)][j]
            var n := east.cross(north).normalized()
            if n.dot(grid[i][j]) < 0.0:
                n = -n
            row.append(n)
        normals.append(row)
    for i in range(rows - 1):
        for j in range(n_lon):
            var j1 := (j + 1) % n_lon
            b.tri("rock", grid[i][j], grid[i][j1], grid[i + 1][j1], normals[i][j], normals[i][j1], normals[i + 1][j1])
            b.tri("rock", grid[i][j], grid[i + 1][j1], grid[i + 1][j], normals[i][j], normals[i + 1][j1], normals[i + 1][j])
    # Polar plates under the bearings.
    for end in [0, rows - 1]:
        var centre := Vector3(0, 0, 0)
        for j in range(n_lon):
            centre += grid[end][j]
        centre /= float(n_lon)
        var up := Vector3(0, 0, signf(centre.z))
        for j in range(n_lon):
            b.tri("rock", centre, grid[end][j], grid[end][(j + 1) % n_lon], up, up, up)
    # Windows on the trench floor, two rows, and rails along its edges.
    var count := 320 if detail else 96
    for k in range(count):
        var lon := TAU * (float(k) + 0.5) / float(count)
        var dir := Vector3(cos(lon), sin(lon), 0.0)
        var p := _rock_point(hab, shape, dir)
        var basis := Basis(Vector3(-sin(lon), cos(lon), 0.0), Vector3(0, 0, 1), dir)
        var w := 0.5 * TAU * p.length() / float(count)
        for row in ([-3.0, 3.0] if detail else [0.0]):
            var lit := not detail or absi(hash(k * 7 + int(row))) % 10 < 6
            b.box("window" if lit else "metal", Vector3(w, 2.2 if detail else 7.0, 0.4), Transform3D(basis, p + Vector3(0, 0, row)))
    if detail:
        for sgn in [-1.0, 1.0]:
            var segs := 160
            for k in range(segs):
                var l0 := TAU * float(k) / float(segs)
                var l1 := TAU * float(k + 1) / float(segs)
                var z: float = sgn * 12.5
                var p0 := _rock_point(hab, shape, Vector3(cos(l0), sin(l0), z / a).normalized())
                var p1 := _rock_point(hab, shape, Vector3(cos(l1), sin(l1), z / a).normalized())
                b.beam("steel", p0 + Vector3(0, 0, sgn * 1.0), p1 + Vector3(0, 0, sgn * 1.0), 1.2)
        # Surface works: domes, radiator fins, antennas, pit heads, out of the trench and off the poles.
        var rng := RandomNumberGenerator.new()
        rng.seed = 99
        for k in range(36):
            var lat := rng.randf_range(0.12, 0.95) * (1.0 if rng.randf() < 0.5 else -1.0)
            var lon := rng.randf() * TAU
            var dir := Vector3(cos(lat) * cos(lon), cos(lat) * sin(lon), sin(lat))
            var p := _rock_point(hab, shape, dir)
            var n := dir
            var x := Vector3(-sin(lon), cos(lon), 0.0)
            var xf := Transform3D(Basis(x, n.cross(x), n), p - n * 2.0)
            match k % 4:
                0:
                    b.lathe("blanket", PackedVector2Array([Vector2(0.0, 12.0), Vector2(5.0, 10.5), Vector2(9.0, 6.0), Vector2(11.0, 0.0)]), 20, xf)
                    _window_ring(b, 3.0, 11.2, 16, Vector2(1.4, 1.0), rng, xf)
                1:
                    b.box("silver", Vector3(30.0, 0.4, 14.0), xf * Transform3D(Basis.IDENTITY, Vector3(0, 0, 9.0)))
                    b.beam("truss", xf.origin, xf * Vector3(0, 0, 3.0), 2.0)
                2:
                    b.box("metal", Vector3(16.0, 16.0, 7.0), xf * Transform3D(Basis.IDENTITY, Vector3(0, 0, 3.5)))
                    b.box("window", Vector3(2.0, 2.0, 0.3), xf * Transform3D(Basis.IDENTITY, Vector3(0, 0, 7.1)))
                _:
                    _fitting(b, xf * Transform3D(Basis.IDENTITY, Vector3(0, 0, 2.0)), rng)
    # Spinning collars at the poles, round the bearings.
    var pole := float(hab["surface_m"])
    for sgn in [-1.0, 1.0]:
        b.cylinder("steel", minf(sgn * (pole - 20.0), sgn * (pole + 4.0)), maxf(sgn * (pole - 20.0), sgn * (pole + 4.0)), 34.0, 48 if detail else 24)
        b.torus("band", sgn * (pole - 6.0), 34.3, 2.0, Transform3D.IDENTITY, 48 if detail else 24, 6)
    return {"arrays": b.commit_arrays(), "instanced": []}

# --- Bola ---------------------------------------------------------------------------
#
# Two habitat pods on tethers from the spinning collar, one either side. A pod is three
# squat drums in a row along the direction of motion, each standing on the tether (its
# axis radial): flat decks stacked towards the hub, the lowest at the 1 g rim, so every
# deck is a floor. Domed ends (ballast and plant below, the tether bridle above), a
# corridor between the drums, radiators beyond the end drums. DETAIL: the pod and its
# tether drawn as a MultiMesh of the two turns.

static func _bola(hab: Dictionary, detail: bool) -> Dictionary:
    var r := float(hab["rim_m"])
    var dr := float(hab["drum_r_m"])
    var b := MeshBuilder.new()
    _collar(b, 0.24 * CORE_M, 14.0, detail, 7)
    var mid := r - 0.5 * BOLA_DECKS * BOLA_DECK_M
    for turn in [0.0, PI]:
        var xf := Transform3D(Basis(Vector3(0, 0, 1), turn), Vector3.ZERO)
        if not detail:
            _bola_pod(b, r, dr, xf, false)
        # Faction bands round each drum, at mid height.
        for y in _bola_drums(dr):
            b.cylinder("band", mid - 3.0, mid + 3.0, dr + 0.4, 64 if detail else 32, xf * Transform3D(_drum_up(), Vector3(0, y, 0)), true)
    var result := {"arrays": b.commit_arrays(), "instanced": []}
    if detail:
        var pod := MeshBuilder.new()
        _bola_pod(pod, r, dr, Transform3D.IDENTITY, true)
        result["instanced"].append({"arrays": pod.commit_arrays(), "turns": _turns(2, func(_k: int) -> bool: return true)})
    return result

# The drums' places along the direction of motion (local Y).
static func _bola_drums(dr: float) -> Array:
    var step := 2.0 * dr + BOLA_GAP_M
    return [-step, 0.0, step]

# Local Z along +Y (a can lying along the direction of motion).
static func _can_basis() -> Basis:
    return Basis(Vector3(0, 0, 1), Vector3(1, 0, 0), Vector3(0, 1, 0))

# Local Z along +X (radially outwards): a drum's decks lie across it.
static func _drum_up() -> Basis:
    return Basis(Vector3(0, 1, 0), Vector3(0, 0, 1), Vector3(1, 0, 0))

static func _bola_pod(b: MeshBuilder, r: float, dr: float, xf: Transform3D, detail: bool) -> void:
    var rng := RandomNumberGenerator.new()
    rng.seed = 31
    var height := BOLA_DECKS * BOLA_DECK_M
    var top := r - height
    var dome := BOLA_DOME * dr
    var mid := r - 0.5 * height
    var drums := _bola_drums(dr)
    var segs := 64 if detail else 32
    for y in drums:
        var up := xf * Transform3D(_drum_up(), Vector3(0, y, 0))
        b.vessel("blanket", top - dome, r + dome, dr, dome, segs, up)
        if detail:
            # A hoop every three decks; a row of windows on every deck (not behind the band).
            for k in range(0, BOLA_DECKS + 1, 3):
                b.torus("steel", top + k * BOLA_DECK_M, dr + 0.2, 0.6, up, segs, 4)
            for k in range(BOLA_DECKS):
                var z := top + (k + 0.55) * BOLA_DECK_M
                if absf(z - mid) > 4.0:
                    _window_ring(b, z, dr, int(TAU * dr / 4.0), Vector2(2.2, 1.6), rng, up)
            # The bridle's anchor on the upper dome, and plant on the lower one.
            b.cylinder("metal", top - dome - 4.0, top - dome + 1.0, 5.0, 16, up)
            for k in range(4):
                var ang := TAU * (float(k) + 0.5) / 4.0
                var at := Vector3(cos(ang) * 0.45 * dr, sin(ang) * 0.45 * dr, r + 0.6 * dome)
                b.box("metal", Vector3(10.0, 10.0, 6.0), up * Transform3D(Basis.IDENTITY, at))
    # Corridors between the drums, two decks up and near the top.
    for x in [r - 2.5 * BOLA_DECK_M, top + 2.5 * BOLA_DECK_M]:
        for k in range(2):
            var a := Vector3(x, drums[k], 0.0)
            var c := Vector3(x, drums[k + 1], 0.0)
            b.cylinder("steel", 0.0, (c - a).length(), 5.0, 20 if detail else 10,
                xf * Transform3D(Basis(Vector3(0, 0, 1), Vector3(1, 0, 0), Vector3(0, 1, 0)), a))
    # Radiators beyond the end drums, edge-on to the direction of motion.
    var reach := float(drums[2]) + dr
    for s in [-1.0, 1.0]:
        var at := xf * Transform3D(Basis.IDENTITY.rotated(Vector3(1, 0, 0), PI * 0.5), Vector3(mid, s * (reach + 50.0), 0.0))
        if detail:
            _panel_grid(b, "silver", Vector3(height, 0.3, 84.0), at, 3, 5)
        else:
            b.box("silver", Vector3(height, 0.3, 84.0), at)
        b.beam("truss", xf * Vector3(mid, s * reach, 0.0), xf * Vector3(mid, s * (reach + 10.0), 0.0), 3.0)
    # The tether: two cables with spreaders to a bridle above the pod; climber cars on it.
    var root := 0.24 * CORE_M
    var bridle := top - dome - 120.0
    for s in [-1.0, 1.0]:
        b.beam("steel", xf * Vector3(root, 0, s * 3.0), xf * Vector3(bridle, 0, s * 3.0), 1.2 if detail else 2.0)
    for y in drums:
        b.beam("steel", xf * Vector3(bridle, 0, 0), xf * Vector3(top - dome - 2.0, y, 0.0), 0.8 if detail else 1.4)
    if detail:
        var spreaders := int((bridle - root) / 30.0)
        for i in range(1, spreaders):
            var x := lerpf(root, bridle, float(i) / float(spreaders))
            b.beam("truss", xf * Vector3(x, 0, -3.0), xf * Vector3(x, 0, 3.0), 0.5)
        for f in [0.35, 0.7]:
            var x := lerpf(root, bridle, f)
            b.box("foil", Vector3(8.0, 6.0, 5.0), xf * Transform3D(Basis.IDENTITY, Vector3(x, 0, 0)))
            b.box("window", Vector3(0.3, 4.0, 1.0), xf * Transform3D(Basis.IDENTITY, Vector3(x + 4.1, 0, 1.0)))
        b.box("metal", Vector3(10.0, 10.0, 10.0), xf * Transform3D(Basis.IDENTITY, Vector3(bridle, 0, 0)))

# --- Module wheel --------------------------------------------------------------------
#
# Twelve modules round a 250 m wheel, joined at spherical nodes; four spokes. Modules
# alternate between homes (windows) and greenhouses (glazing). DETAIL: the module (with
# its node) drawn as MultiMeshes of turns.

const WHEEL_MODULES := 12

static func _wheel(hab: Dictionary, detail: bool) -> Dictionary:
    var r := float(hab["rim_m"])
    var b := MeshBuilder.new()
    var collar := 0.24 * CORE_M
    _collar(b, collar, 12.0, detail, 11)
    for k in range(4):
        var a := PI * 0.5 * float(k)
        var radial := Vector3(cos(a), sin(a), 0.0)
        var tangent := Vector3(-sin(a), cos(a), 0.0)
        var basis := Basis(tangent, Vector3(0, 0, 1), radial)
        b.cylinder("steel", collar - 1.0, r - 19.0, 5.0, 24 if detail else 12, Transform3D(basis, Vector3.ZERO), true)
        if detail:
            for f in range(1, 9):
                b.torus("truss", 0.0, 5.3, 0.4, Transform3D(Basis(tangent, Vector3(0, 0, 1), radial), radial * lerpf(collar, r - 19.0, float(f) / 9.0)), 20, 4)
            for s in [-1.0, 1.0]:
                b.beam("truss", radial * collar + tangent * s * 7.0, radial * (r - 22.0) + tangent * s * 7.0, 0.6)
    for k in range(WHEEL_MODULES):
        var a := TAU * float(k) / float(WHEEL_MODULES)
        var node := Vector3(cos(a), sin(a), 0.0) * r
        b.torus("band", 0.0, 20.3, 1.6, Transform3D(Basis.IDENTITY, node), 32 if detail else 16, 6)
        if not detail:
            _wheel_module(b, r, Transform3D(Basis(Vector3(0, 0, 1), a), Vector3.ZERO), k % 2 == 1, false)
    var result := {"arrays": b.commit_arrays(), "instanced": []}
    if detail:
        for farm in [false, true]:
            var m := MeshBuilder.new()
            _wheel_module(m, r, Transform3D.IDENTITY, farm, true)
            result["instanced"].append({"arrays": m.commit_arrays(),
                "turns": _turns(WHEEL_MODULES, func(k: int) -> bool: return (k % 2 == 1) == farm)})
    return result

# The node at angle 0 and the module from it to the next node.
static func _wheel_module(b: MeshBuilder, r: float, xf: Transform3D, farm: bool, detail: bool) -> void:
    var p0 := Vector3(r, 0, 0)
    var a1 := TAU / float(WHEEL_MODULES)
    var p1 := Vector3(cos(a1), sin(a1), 0.0) * r
    b.vessel("blanket", -20.0, 20.0, 20.0, 20.0, 32 if detail else 16, xf * Transform3D(Basis.IDENTITY, p0))
    var dir := (p1 - p0).normalized()
    var up := Vector3(0, 0, 1)
    var module := xf * Transform3D(Basis(up.cross(dir), up, dir), p0)
    var length := (p1 - p0).length()
    b.cylinder("steel", 15.0, length - 15.0, 6.0, 16, module)
    b.vessel("glass" if farm else "blanket", 22.0, length - 22.0, 14.0, 6.0, 32 if detail else 16, module)
    if detail:
        var rng := RandomNumberGenerator.new()
        rng.seed = 5 + int(farm)
        var z := 30.0
        while z < length - 28.0:
            b.torus("steel", z, 14.15, 0.5, module, 32, 4)
            if not farm:
                _window_ring(b, z + 4.0, 14.0, 20, Vector2(1.6, 1.1), rng, module)
            z += 9.0
        if farm:
            for k in range(8):
                var ang := TAU * float(k) / 8.0
                var radial := Vector3(cos(ang), sin(ang), 0.0)
                b.beam("steel", module * (radial * 14.3 + Vector3(0, 0, 26.0)), module * (radial * 14.3 + Vector3(0, 0, length - 26.0)), 0.4)
        # A radiator on the inside of the wheel, a dish and an airlock on the node.
        var inward := -Vector3(cos(a1 * 0.5), sin(a1 * 0.5), 0.0)
        var mid := (p0 + p1) * 0.5
        b.box("silver", Vector3(length - 60.0, 16.0, 0.3), xf * Transform3D(Basis(Vector3(0, 0, 1), atan2(dir.y, dir.x)), mid + inward * 24.0))
        _fitting(b, xf * Transform3D(Basis(Vector3(0, 1, 0), Vector3(0, 0, 1), Vector3(1, 0, 0)), p0 + Vector3(20.0, 0, 0)), rng)
        _window_ring(b, 0.0, 20.0, 18, Vector2(1.4, 1.0), rng, xf * Transform3D(Basis.IDENTITY, p0))

# --- Carousel -----------------------------------------------------------------------
#
# Six habitat cans on lattice arms (300 m), each can lying along the direction of motion,
# a greenhouse in its middle; a light tie ring through the cans. DETAIL: arm and can drawn
# as a MultiMesh of six turns.

const CAROUSEL_ARMS := 6

static func _carousel(hab: Dictionary, detail: bool) -> Dictionary:
    var r := float(hab["rim_m"])
    var b := MeshBuilder.new()
    _collar(b, 0.28 * CORE_M, 15.0, detail, 13)
    b.torus("steel", 0.0, r, 1.5, Transform3D.IDENTITY, 192 if detail else 96, 4)
    for k in range(CAROUSEL_ARMS):
        var xf := Transform3D(Basis(Vector3(0, 0, 1), TAU * float(k) / float(CAROUSEL_ARMS)), Vector3.ZERO)
        for y in [-58.0, 58.0]:
            b.cylinder("band", y - 4.0, y + 4.0, 22.5, 32 if detail else 16, xf * Transform3D(_can_basis(), Vector3(r, 0, 0)), true)
        if not detail:
            _carousel_arm(b, r, xf, false)
    var result := {"arrays": b.commit_arrays(), "instanced": []}
    if detail:
        var arm := MeshBuilder.new()
        _carousel_arm(arm, r, Transform3D.IDENTITY, true)
        result["instanced"].append({"arrays": arm.commit_arrays(), "turns": _turns(CAROUSEL_ARMS, func(_k: int) -> bool: return true)})
    return result

static func _carousel_arm(b: MeshBuilder, r: float, xf: Transform3D, detail: bool) -> void:
    var root := 0.28 * CORE_M
    var can := xf * Transform3D(_can_basis(), Vector3(r, 0, 0))
    b.vessel("blanket", -70.0, -18.0, 22.0, 10.0, 32 if detail else 16, can)
    b.vessel("blanket", 18.0, 70.0, 22.0, 10.0, 32 if detail else 16, can)
    b.cylinder("glass", -20.0, 20.0, 21.0, 32 if detail else 16, can)
    if detail:
        _lattice(b, "truss", xf * Vector3(root, 0, 0), xf * Vector3(r - 22.0, 0, 0), 5.0, 12.0, 0.8, 3)
        b.cylinder("steel", root, r - 22.0, 3.5, 16, xf * Transform3D(Basis(Vector3(0, 1, 0), Vector3(0, 0, 1), Vector3(1, 0, 0)), Vector3(0, 0, 9.0)), true)
        var rng := RandomNumberGenerator.new()
        rng.seed = 17
        for z in [-64.0, -24.0, 24.0, 64.0]:
            b.torus("steel", z, 22.2, 0.6, can, 32, 4)
        for z in range(-60, -24, 8):
            _window_ring(b, float(z), 22.0, 28, Vector2(1.8, 1.2), rng, can)
        for z in range(28, 64, 8):
            _window_ring(b, float(z), 22.0, 28, Vector2(1.8, 1.2), rng, can)
        for k in range(10):
            var ang := TAU * float(k) / 10.0
            var radial := Vector3(cos(ang), sin(ang), 0.0)
            b.beam("steel", can * (radial * 21.3 + Vector3(0, 0, -20.0)), can * (radial * 21.3 + Vector3(0, 0, 20.0)), 0.5)
        b.box("silver", Vector3(50.0, 0.3, 24.0), xf * Transform3D(Basis.IDENTITY, Vector3(r - 50.0, 0, 30.0)))
        b.beam("truss", xf * Vector3(r - 50.0, 0, 9.0), xf * Vector3(r - 50.0, 0, 18.0), 1.0)
    else:
        b.beam("truss", xf * Vector3(root, 0, 0), xf * Vector3(r - 22.0, 0, 0), 8.0)

# --- Detail builds -------------------------------------------------------------------

# Everything DETAIL needs, as arrays (worker thread).
static func _detail_arrays(data: Dictionary) -> Dictionary:
    var hab := _habitat(data)
    var hull := _hull(data, hab, true)
    return {"hull": hull["arrays"], "sun": hull["sun"], "solar": hull["solar"], "spinner": _spinner(hab, true)}

# Engine resources from _detail_arrays (main thread): meshes, and one MultiMesh per
# instanced part with its turns round the axis (shared by all spinners of this station).
static func _finish_detail(arrays: Dictionary) -> Dictionary:
    var multimeshes: Array = []
    for part in arrays["spinner"]["instanced"]:
        var built := MeshBuilder.to_mesh(part["arrays"])
        var mesh: ArrayMesh = built["mesh"]
        var slots: Array = built["slots"]
        for i in range(slots.size()):
            mesh.surface_set_material(i, Materials.shared(String(slots[i])))
        var turns: Array = part["turns"]
        var mm := MultiMesh.new()
        mm.transform_format = MultiMesh.TRANSFORM_3D
        mm.mesh = mesh
        mm.instance_count = turns.size()
        for i in range(turns.size()):
            mm.set_instance_transform(i, turns[i])
        multimeshes.append(mm)
    var sun: Dictionary = arrays["sun"]
    return {"hull": MeshBuilder.to_mesh(arrays["hull"]), "sun": MeshBuilder.to_mesh(sun) if not sun.is_empty() else {},
        "solar": _solar_meshes(arrays["solar"]), "extras": MeshBuilder.to_mesh(arrays["spinner"]["arrays"]), "multimeshes": multimeshes}

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

static func detail_ready(data: Dictionary) -> bool:
    return _detail_cache.has(_key(data))

# True once the detailed model is ready; otherwise starts (or keeps waiting for) a
# background build (~0.2-0.8 s of GDScript).
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

# Builds every station's detailed model in the background, one at a time (so start-up
# and play are not starved of worker threads). Call each frame; true once all are ready.
static func prefetch(stations: Array) -> bool:
    for data in stations:
        if _detail_cache.has(_key(data)):
            continue
        request_detail(data)
        return false
    return true

# --- Nodes ------------------------------------------------------------------------

# The station's model in its container: the hull, its spinning habitat(s) and any
# sun-tracking part. The container is turned so the station's axis stands up from the
# ecliptic.
static func ensure(container: Node3D, data: Dictionary, lod := COARSE) -> void:
    var key := _key(data)
    if String(container.get_meta("model_key", "")) == key and int(container.get_meta("lod", COARSE)) == lod:
        return
    var info := model_info(data)
    var detail := detail_info(data) if lod == DETAIL else {}
    var sun_angle := 0.0
    for child in container.get_children():
        var name := String(child.name)
        if name == "sun":
            sun_angle = (child as Node3D).rotation.z
        if name.begins_with("ring") or name.begins_with("solar") or name == "model" or name == "sun":
            child.queue_free()
            container.remove_child(child)
    var hull := MeshInstance3D.new()
    hull.name = "model"
    Materials.apply_mesh(hull, detail["hull"] if lod == DETAIL else info["hull"], container)
    container.add_child(hull)
    var sun_mesh: Dictionary = detail["sun"] if lod == DETAIL else info["sun"]
    if not sun_mesh.is_empty():
        var sun := MeshInstance3D.new()
        sun.name = "sun"
        sun.rotation = Vector3(0.0, 0.0, sun_angle)
        Materials.apply_mesh(sun, sun_mesh, container)
        container.add_child(sun)
    # Solar arrays: each on its own pivot, turned to face the Sun in spin_rings.
    var solar_meshes: Array = detail["solar"] if lod == DETAIL else info["solar"]
    for k in range(solar_meshes.size()):
        var panel := MeshInstance3D.new()
        panel.name = "solar_%d" % k
        panel.position = solar_meshes[k]["at"]
        Materials.apply_mesh(panel, solar_meshes[k]["mesh"], container)
        container.add_child(panel)
    var i := 0
    for spinner in info["spinners"]:
        var ring := MeshInstance3D.new()
        ring.name = "ring_%d" % i
        ring.position = Vector3(0.0, 0.0, float(spinner[0]))
        ring.set_meta("spin", float(spinner[1]))
        ring.set_meta("phase", float(spinner[2]))
        ring.set_meta("flip", bool(spinner[3]))
        ring.basis = _spin_basis(float(spinner[2]), bool(spinner[3]))
        Materials.apply_mesh(ring, detail["extras"] if lod == DETAIL else info["spinner"], container)
        if lod == DETAIL:
            for mm in detail["multimeshes"]:
                var parts := MultiMeshInstance3D.new()
                parts.multimesh = mm
                parts.layers = Materials.MODEL_LAYERS
                ring.add_child(parts)
        container.add_child(ring)
        i += 1
    container.rotation = Vector3(-PI * 0.5, 0.0, 0.0)
    container.set_meta("model_key", key)
    container.set_meta("lod", lod)
    container.set_meta("radius_m", info["radius_m"])
    container.set_meta("berths", info["berths"])
    container.set_meta("core_radius_m", info["core_radius_m"])
    container.set_meta("rim_m", info["rim_m"])
    # The spin follows the design's gravity; set here (ensure) rather than in the worker-built info.
    var gravity := GRAVITY_LOW_M_S2 if design_of(data) in LOW_GRAVITY_DESIGNS else GRAVITY_M_S2
    container.set_meta("spin_rad_s", ring_spin_rad_s(float(info["rim_m"]), gravity))

static func _solar_meshes(panels: Array) -> Array:
    var out: Array = []
    for panel in panels:
        out.append({"mesh": MeshBuilder.to_mesh(panel["arrays"]), "at": panel["at"]})
    return out

static func _spin_basis(angle: float, flip: bool) -> Basis:
    var basis := Basis(Vector3(0, 0, 1), angle)
    return Basis(Vector3(1, 0, 0), PI) * basis if flip else basis

# Turns the habitats (their gravity's spin, real time) and points the sun-tracking part at sun_world
# (a global position), when given.
static func spin_rings(container: Node3D, spin_time_s: float, sun_world: Variant = null) -> void:
    var rate := float(container.get_meta("spin_rad_s", 0.1))
    for child in container.get_children():
        if child.has_meta("spin"):
            var angle := float(child.get_meta("phase", 0.0)) + fmod(spin_time_s * rate, TAU) * float(child.get_meta("spin"))
            (child as Node3D).basis = _spin_basis(angle, bool(child.get_meta("flip", false)))
        elif sun_world != null and String(child.name) == "sun":
            var local: Vector3 = container.global_transform.affine_inverse() * (sun_world as Vector3)
            (child as Node3D).rotation = Vector3(0.0, 0.0, atan2(local.y, local.x))
        elif sun_world != null and String(child.name).begins_with("solar"):
            # Each array turns about its own axis (the station's, Z) so that its face points at
            # the Sun. The Sun stays near the ecliptic, so its direction is in the XY plane.
            var local: Vector3 = container.global_transform.affine_inverse() * (sun_world as Vector3)
            if Vector2(local.x, local.y).length() > 1.0e-6:
                (child as Node3D).basis = Basis(Vector3(0, 0, 1), atan2(-local.x, local.y))
