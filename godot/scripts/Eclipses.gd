extends RefCounted

# Planets and moons shade ships and stations. Godot's shadow maps cannot resolve a 2 km
# station beside a 12,000 km planet at true scale, so each model's sunlight is worked out
# directly: the share of the Sun's disc left uncovered by every body, seen from the model
# (umbra and penumbra both). Models in shade move from render layer 1 (lit by the Sun) to
# one of the eclipse layers, each lit by a copy of the Sun light at a fixed share of its
# energy (LEVELS steps). The camera's fill light (layer 2) and ambient light still reach
# them, as on any night side.

const LEVELS := 8
const FIRST_BIT := 10  # render layers 11-17 (layer 11: 1/8 of the sunlight ... 17: 7/8)
const SUN_BIT := 1
const FILL_BIT := 2
const MASK := ((1 << (LEVELS - 1)) - 1) << FIRST_BIT
const ALL_LAYERS := (1 << 20) - 1

# Copies of the Sun light for the shaded levels 1..LEVELS-1, children of sun (so they
# follow it). The Sun light itself lights neither the eclipse layers nor the fill layer
# (2), which every model keeps: a lit model is on layer 1 as well.
static func make_lights(sun: OmniLight3D) -> void:
    sun.light_cull_mask = ALL_LAYERS & ~MASK & ~FILL_BIT
    for level in range(1, LEVELS):
        var light := sun.duplicate() as OmniLight3D
        light.name = "Eclipse%d" % level
        light.position = Vector3.ZERO
        light.light_energy = sun.light_energy * float(level) / float(LEVELS)
        light.light_cull_mask = 1 << (FIRST_BIT + level - 1)
        sun.add_child(light)

# The bodies that can shade anything: [position (m), radius (m), reach² (m²)]. reach: where
# the body covers less than a tenth of the Sun's apparent radius (under 1% of its disc).
static func occluders(sun_m: PackedFloat64Array, sun_radius_m: float, bodies: Array) -> Array:
    var result: Array = []
    for body in bodies:
        var p: PackedFloat64Array = body[0]
        var radius := float(body[1])
        if radius <= 0.0:
            continue
        var to_sun := Vector3(sun_m[0] - p[0], sun_m[1] - p[1], sun_m[2] - p[2]).length()
        var reach := 10.0 * radius * to_sun / sun_radius_m
        result.append([p, radius, reach * reach])
    return result

# The share of the Sun's disc visible from p (0 in umbra, 1 in full sunlight).
static func lit_fraction(p: PackedFloat64Array, sun_m: PackedFloat64Array, sun_radius_m: float, bodies: Array) -> float:
    var lit := 1.0
    var to_sun := Vector3(sun_m[0] - p[0], sun_m[1] - p[1], sun_m[2] - p[2])
    var sun_d := to_sun.length()
    if sun_d <= sun_radius_m:
        return 1.0
    var sun_ang := asin(sun_radius_m / sun_d)
    for body in bodies:
        var b: PackedFloat64Array = body[0]
        var to_body := Vector3(b[0] - p[0], b[1] - p[1], b[2] - p[2])
        var d2 := to_body.length_squared()
        if d2 > float(body[2]) or to_body.dot(to_sun) <= 0.0:
            continue
        var d := sqrt(d2)
        if d >= sun_d:
            continue
        var body_ang := asin(minf(float(body[1]) / d, 1.0))
        var apart := to_body.angle_to(to_sun)
        lit *= 1.0 - _overlap(sun_ang, body_ang, apart) / (PI * sun_ang * sun_ang)
    return clampf(lit, 0.0, 1.0)

# Area shared by two discs of radii r1 and r2 with centres d apart (small angles, flat).
static func _overlap(r1: float, r2: float, d: float) -> float:
    if d >= r1 + r2:
        return 0.0
    if d <= absf(r1 - r2):
        return PI * minf(r1, r2) * minf(r1, r2)
    var a1 := r1 * r1 * acos(clampf((d * d + r1 * r1 - r2 * r2) / (2.0 * d * r1), -1.0, 1.0))
    var a2 := r2 * r2 * acos(clampf((d * d + r2 * r2 - r1 * r1) / (2.0 * d * r2), -1.0, 1.0))
    var k := 0.5 * sqrt(maxf((-d + r1 + r2) * (d + r1 - r2) * (d - r1 + r2) * (d + r1 + r2), 0.0))
    return a1 + a2 - k

static func level_of(lit: float) -> int:
    return clampi(roundi(lit * LEVELS), 0, LEVELS)

# Moves a model's meshes to the layer for its level (LEVELS: the Sun's own layer). Only
# meshes on the fill layer (2) are models; others are left alone. Walks the model while
# it is shaded, so meshes added by a level-of-detail switch follow. In umbra a model keeps
# only the fill layer.
static func apply(container: Node3D, level: int) -> void:
    var was := int(container.get_meta("eclipse_level", LEVELS))
    if level == LEVELS and was == LEVELS:
        return
    container.set_meta("eclipse_level", level)
    var target := SUN_BIT
    if level < LEVELS:
        target = 0 if level == 0 else 1 << (FIRST_BIT + level - 1)
    _set_layers(container, target)

static func _set_layers(node: Node, target: int) -> void:
    for child in node.get_children():
        if child is VisualInstance3D:
            var v := child as VisualInstance3D
            if v.layers & FILL_BIT:
                v.layers = (v.layers & ~(SUN_BIT | MASK)) | target
        _set_layers(child, target)
