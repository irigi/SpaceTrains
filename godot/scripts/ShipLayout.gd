extends RefCounted

# A ship's parts laid out in metres from its design's components (the bridge's .designs
# file), shared by the inspector schematic (ui/ShipSchematic.gd, a top view) and the map
# models (ShipModels.gd, the same parts in 3D).
#
# Coordinates: x runs from the bow (0) to the stern, y across the ship; each part is a
# Rect2 in that plane (the 3D model makes round parts round and boxes square in depth).
# Rules:
# - habitat: a 4.5 m cylinder (NASA's Deep Space Habitat module) as long as its
#   pressurized volume needs; its rooms and systems are bands sized by mass;
# - hold: a box of its volume (1 m³ per cargo unit);
# - tanks: a cylinder of the propellant volume (LH2 is 14x bulkier than water);
# - thermal ships: a shadow shield, then reactor and nozzle; plasma ships: drive and
#   magnetic nozzle, with two radiator wings (2.5:1) of the panel area at mid-ship.

const HAB_DIAMETER_M := 4.5
const GAP_M := 1.0
const WING_OFFSET_M := 2.5
# Habitat parts drawn as bands inside the hull; the hull and shielding are its frame.
const HABITAT_BANDS := ["habitat.crew_quarters", "habitat.common_rooms", "habitat.life_support",
    "habitat.thermal_control", "habitat.power_avionics", "habitat.airlock_eva"]
const CABIN_KEYS := ["cabins", "module.cabin", "module.shielded_cabin"]

static func _f(component: Dictionary, key: String) -> float:
    return float(component.get(key, 0.0))

static func components_of(design: Dictionary) -> Dictionary:
    var components := {}
    for component in design.get("components", []):
        components[String(component.get("id", ""))] = component
    return components

# Returns {shapes: [{key, kind, rect, comp}] in drawing order, bounds: Rect2 (all parts),
# plasma: bool, engine_rect: Rect2, ship_end: float (stern x), tank_width: float}.
static func layout(design: Dictionary) -> Dictionary:
    var components := components_of(design)
    var shapes: Array = []
    var add := func(key: String, kind: String, rect: Rect2) -> void:
        shapes.append({"key": key, "kind": kind, "rect": rect, "comp": components.get(key, {})})
    var plasma := components.has("radiators")
    var x := 0.0
    var habitat: Dictionary = components.get("habitat", {})
    if not habitat.is_empty():
        var radius := HAB_DIAMETER_M * 0.5
        var length := maxf(_f(habitat, "volume_m3") / (PI * radius * radius), HAB_DIAMETER_M)
        add.call("habitat", "habitat", Rect2(x, -radius, length, HAB_DIAMETER_M))
        # Bands by mass, inset so the hull and shielding show as the frame around them.
        var inner := Rect2(x + radius * 0.45, -radius * 0.62, length - radius * 0.9, HAB_DIAMETER_M * 0.62)
        var total := 0.0
        for key in HABITAT_BANDS:
            total += _f(components.get(key, {}), "mass_kg")
        var bx := inner.position.x
        for key in HABITAT_BANDS:
            var share := _f(components.get(key, {}), "mass_kg") / maxf(total, 1.0)
            if share <= 0.0:
                continue
            add.call(key, "band", Rect2(bx, inner.position.y, inner.size.x * share, inner.size.y))
            bx += inner.size.x * share
        # Storm shelter (tier 2): a water-walled refuge at the front of the habitat.
        var shelter: Dictionary = components.get("module.storm_shelter", {})
        if not shelter.is_empty():
            var depth := clampf(_f(shelter, "volume_m3") / (HAB_DIAMETER_M * HAB_DIAMETER_M * 0.4), 0.8, length * 0.3)
            add.call("module.storm_shelter", "shelter", Rect2(x + radius * 0.15, -radius * 0.75, depth, HAB_DIAMETER_M * 0.75))
        x += length + GAP_M
    # Passenger cabins (a liner's own, or a cabin module): a block sized by volume, the water
    # round it (a wall, or the propellant tanks wrapped round it) drawn as its jacket.
    for key in CABIN_KEYS:
        var cabin: Dictionary = components.get(key, {})
        if cabin.is_empty():
            continue
        var cabin_volume := maxf(_f(cabin, "volume_m3"), 1.0)
        var cabin_width := clampf(pow(cabin_volume, 1.0 / 3.0) * 0.9, HAB_DIAMETER_M, 14.0)
        var cabin_length := maxf(cabin_volume / (PI * cabin_width * cabin_width * 0.25), cabin_width)
        var cabin_rect := Rect2(x, -cabin_width * 0.5, cabin_length, cabin_width)
        if components.has("module.tank_wrap"):
            add.call("module.tank_wrap", "tank_jacket", cabin_rect.grow(1.2))
        if components.has("module.cabin_water_wall"):
            add.call("module.cabin_water_wall", "cabin_wall", cabin_rect.grow(0.6))
        add.call(key, "cabin", cabin_rect)
        x += cabin_length + GAP_M
        break
    var hold: Dictionary = components.get("hold", {})
    if not hold.is_empty():
        var volume := maxf(_f(hold, "volume_m3"), 1.0)
        var width := clampf(pow(volume, 1.0 / 3.0) * 1.2, 3.0, 12.0)
        add.call("hold", "hold", Rect2(x, -width * 0.5, volume / (width * width), width))
        if components.has("module.cold_hold"):
            add.call("module.cold_hold", "liner", Rect2(x, -width * 0.5, volume / (width * width), width).grow(0.35))
        x += volume / (width * width) + GAP_M
    var tank: Dictionary = components.get("tanks", {})
    var tank_width := 3.0
    if not tank.is_empty():
        var volume := maxf(_f(tank, "volume_m3"), 1.0)
        tank_width = clampf(pow(volume, 1.0 / 3.0) * 0.7, 2.0, 10.0)
        var length := maxf(volume / (PI * tank_width * tank_width * 0.25), tank_width)
        add.call("tanks", "tank", Rect2(x, -tank_width * 0.5, length, tank_width))
        for key in ["module.boiloff_shield", "module.zbo_cryocooler"]:
            if components.has(key):
                add.call(key, "tank_cooling", Rect2(x, -tank_width * 0.5, length, tank_width).grow(0.4))
        x += length + GAP_M
    if not plasma:
        var shield := maxf(3.0, tank_width * 0.6)
        add.call("shield", "shield", Rect2(x, -shield * 0.5, 0.6, shield))
        x += 0.6 + GAP_M
    var engine: Dictionary = components.get("engine", {})
    var engine_length := maxf(2.0, pow(_f(engine, "mass_kg") / 1000.0, 1.0 / 3.0) * 2.2)
    var engine_rect := Rect2(x, -engine_length * 0.4, engine_length, engine_length * 0.8)
    add.call("engine", "engine", engine_rect)
    var ship_end := x + engine_length
    # The truss runs the whole length, under everything.
    shapes.push_front({"key": "structure", "kind": "structure", "rect": Rect2(0.0, -0.35, ship_end, 0.7),
        "comp": components.get("structure", {})})
    var bounds := Rect2(0.0, -HAB_DIAMETER_M * 0.5, ship_end, HAB_DIAMETER_M)
    for shape in shapes:
        bounds = bounds.merge(shape["rect"])
    if plasma:
        var area := _f(components["radiators"], "area_m2")
        var span := sqrt(area / 2.0 / 2.5)
        var wing_length := 2.5 * span
        var middle := ship_end * 0.5
        # The wing roots clear the widest part beside them (a wide hold or a tank cluster,
        # which the 3D model draws about 25% wider than its rect), so they never cut through it.
        var offset := WING_OFFSET_M
        for shape in shapes:
            var r: Rect2 = shape["rect"]
            if r.end.x < middle - wing_length * 0.5 or r.position.x > middle + wing_length * 0.5:
                continue
            var half := maxf(absf(r.position.y), absf(r.end.y))
            if String(shape["kind"]) == "tank":
                half *= 1.25
            offset = maxf(offset, half + 0.5)
        for side in [-1.0, 1.0]:
            var y: float = offset if side > 0.0 else -offset - span
            var wing := {"key": "radiators", "kind": "wing", "rect": Rect2(middle - wing_length * 0.5, y, wing_length, span),
                "comp": components["radiators"]}
            shapes.push_front(wing)
            bounds = bounds.merge(wing["rect"])
    return {"shapes": shapes, "bounds": bounds, "plasma": plasma, "engine_rect": engine_rect,
        "ship_end": ship_end, "tank_width": tank_width, "components": components}
