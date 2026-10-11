extends Control

# To-scale top view of a ship, laid out from its design's components (the bridge's
# .designs file; tier 1 of docs/plans/ship_internals.md), with live state from the
# snapshot: tank and hold fill, crew, false-colour temperatures of the radiators and the
# reactor, and the engine plume. Below it, a bar of the ship's current mass by part.
# Hovering a part (or a bar segment) highlights both and shows its spec line.
#
# Layout rules, bow on the left, all sizes in metres from the components:
# - habitat: a 4.5 m cylinder (NASA's Deep Space Habitat module) as long as its
#   pressurized volume needs; its rooms and systems are bands sized by mass;
# - hold: a box of its volume (1 m³ per cargo unit);
# - tanks: a cylinder of the propellant volume (LH2 is 14x bulkier than water);
# - thermal ships: a shadow shield, then reactor and nozzle; plasma ships: drive and
#   magnetic nozzle, with two radiator wings (2.5:1) of the panel area at mid-ship.

const UiTheme := preload("res://scripts/ui/UiTheme.gd")
const Thermal := preload("res://scripts/ThermalPalette.gd")

const HAB_DIAMETER_M := 4.5
const GAP_M := 1.0
const WING_OFFSET_M := 2.5
const SHIP_H := 150.0
const FOOTER_H := 20.0
const BAR_H := 16.0
const INFO_H := 30.0
const FONT_SIZE := 10
const COLD_REACTOR_K := 300.0

const PART_COLORS := {
    "engine": Color(0.88, 0.48, 0.25),
    "radiator": Color(0.79, 0.25, 0.25),
    "tank": Color(0.35, 0.63, 0.85),
    "habitat": Color(0.91, 0.89, 0.78),
    "hold": Color(0.49, 0.75, 0.42),
    "structure": Color(0.54, 0.56, 0.6),
    "propellant": Color(0.62, 0.82, 1.0),
    "cargo": Color(0.36, 0.62, 0.3),
    "provisions": Color(0.85, 0.72, 0.45),
}
# Habitat parts drawn as bands inside the hull; the hull and shielding are its frame.
const HABITAT_BANDS := ["habitat.crew_quarters", "habitat.common_rooms", "habitat.life_support",
    "habitat.thermal_control", "habitat.power_avionics", "habitat.airlock_eva"]
const HABITAT_FRAME := ["habitat.pressure_hull", "habitat.shielding"]
const BAND_SHADES := [Color(0.95, 0.86, 0.66), Color(0.86, 0.9, 0.82), Color(0.72, 0.82, 0.86),
    Color(0.8, 0.78, 0.9), Color(0.86, 0.84, 0.72), Color(0.7, 0.72, 0.76)]

var _design: Dictionary = {}
var _ship: Dictionary = {}
var _components: Dictionary = {}
# Shapes in metres: {key, kind, rect, comp}. Drawn in order; hit-tested in reverse.
var _shapes: Array = []
var _bounds := Rect2()
var _plasma := false
var _engine_rect := Rect2()
var _segments: Array = []
var _scale := 1.0
var _offset := Vector2.ZERO
var _hover_key := ""

func _init() -> void:
    custom_minimum_size = Vector2(0, SHIP_H + FOOTER_H + BAR_H + INFO_H + 10.0)
    mouse_filter = Control.MOUSE_FILTER_PASS
    # Hover is read from the mouse position when drawing, so redraw once laid out too.
    set_notify_transform(true)

func set_ship(design: Dictionary, ship: Dictionary) -> void:
    _design = design
    _ship = ship
    _components.clear()
    for component in design.get("components", []):
        _components[String(component.get("id", ""))] = component
    _layout()
    _build_mass_bar()
    queue_redraw()

func _gui_input(event: InputEvent) -> void:
    if event is InputEventMouseMotion:
        queue_redraw()

func _notification(what: int) -> void:
    if what == NOTIFICATION_MOUSE_EXIT or what == NOTIFICATION_RESIZED or what == NOTIFICATION_TRANSFORM_CHANGED:
        queue_redraw()

func _f(component: Dictionary, key: String) -> float:
    return float(component.get(key, 0.0))

# --- Layout (metres) -------------------------------------------------------------

func _add_shape(key: String, kind: String, rect: Rect2) -> void:
    _shapes.append({"key": key, "kind": kind, "rect": rect, "comp": _components.get(key, {})})

func _layout() -> void:
    _shapes.clear()
    _plasma = _components.has("radiators")
    var x := 0.0
    var habitat: Dictionary = _components.get("habitat", {})
    if not habitat.is_empty():
        var radius := HAB_DIAMETER_M * 0.5
        var length := maxf(_f(habitat, "volume_m3") / (PI * radius * radius), HAB_DIAMETER_M)
        _add_shape("habitat", "habitat", Rect2(x, -radius, length, HAB_DIAMETER_M))
        # Bands by mass, inset so the hull and shielding show as the frame around them.
        var inner := Rect2(x + radius * 0.45, -radius * 0.62, length - radius * 0.9, HAB_DIAMETER_M * 0.62)
        var total := 0.0
        for key in HABITAT_BANDS:
            total += _f(_components.get(key, {}), "mass_kg")
        var bx := inner.position.x
        for key in HABITAT_BANDS:
            var share := _f(_components.get(key, {}), "mass_kg") / maxf(total, 1.0)
            if share <= 0.0:
                continue
            _add_shape(key, "band", Rect2(bx, inner.position.y, inner.size.x * share, inner.size.y))
            bx += inner.size.x * share
        x += length + GAP_M
    var hold: Dictionary = _components.get("hold", {})
    if not hold.is_empty():
        var volume := maxf(_f(hold, "volume_m3"), 1.0)
        var width := clampf(pow(volume, 1.0 / 3.0) * 1.2, 3.0, 12.0)
        _add_shape("hold", "hold", Rect2(x, -width * 0.5, volume / (width * width), width))
        x += volume / (width * width) + GAP_M
    var tank: Dictionary = _components.get("tanks", {})
    var tank_width := 3.0
    if not tank.is_empty():
        var volume := maxf(_f(tank, "volume_m3"), 1.0)
        tank_width = clampf(pow(volume, 1.0 / 3.0) * 0.7, 2.0, 10.0)
        var length := maxf(volume / (PI * tank_width * tank_width * 0.25), tank_width)
        _add_shape("tanks", "tank", Rect2(x, -tank_width * 0.5, length, tank_width))
        x += length + GAP_M
    if not _plasma:
        var shield := maxf(3.0, tank_width * 0.6)
        _add_shape("shield", "shield", Rect2(x, -shield * 0.5, 0.6, shield))
        x += 0.6 + GAP_M
    var engine: Dictionary = _components.get("engine", {})
    var engine_length := maxf(2.0, pow(_f(engine, "mass_kg") / 1000.0, 1.0 / 3.0) * 2.2)
    _engine_rect = Rect2(x, -engine_length * 0.4, engine_length, engine_length * 0.8)
    _add_shape("engine", "engine", _engine_rect)
    var ship_end := x + engine_length
    # The truss runs the whole length, under everything.
    _shapes.push_front({"key": "structure", "kind": "structure", "rect": Rect2(0.0, -0.35, ship_end, 0.7),
        "comp": _components.get("structure", {})})
    _bounds = Rect2(0.0, -HAB_DIAMETER_M * 0.5, ship_end, HAB_DIAMETER_M)
    for shape in _shapes:
        _bounds = _bounds.merge(shape["rect"])
    if _plasma:
        var area := _f(_components["radiators"], "area_m2")
        var span := sqrt(area / 2.0 / 2.5)
        var wing_length := 2.5 * span
        var middle := ship_end * 0.5
        for side in [-1.0, 1.0]:
            var y: float = WING_OFFSET_M if side > 0.0 else -WING_OFFSET_M - span
            var wing := {"key": "radiators", "kind": "wing", "rect": Rect2(middle - wing_length * 0.5, y, wing_length, span),
                "comp": _components["radiators"]}
            _shapes.push_front(wing)
            _bounds = _bounds.merge(wing["rect"])
    # Room for the plume, so the scale doesn't jump when the engine lights.
    _bounds = _bounds.merge(Rect2(ship_end, -1.0, engine_length * 1.6, 2.0))

# --- Mass bar --------------------------------------------------------------------

func _build_mass_bar() -> void:
    _segments.clear()
    var items: Array = []
    for component in _design.get("components", []):
        if String(component.get("parent_id", "")) == "":
            items.append([String(component["id"]), PART_COLORS.get(String(component.get("kind", "")), Color.GRAY),
                _f(component, "mass_kg"), "%s %s" % [String(component.get("name", "")), fmt_mass(_f(component, "mass_kg"))]])
    var propellant := float(_ship.get("propellant_kg", 0.0))
    var cargo := float(_ship.get("cargo_mass_kg", 0.0))
    var provisions := float(_ship.get("provisions_mass_kg", 0.0))
    items.append(["tanks", PART_COLORS["propellant"], propellant, "Propellant aboard %s of %s" % [
        fmt_mass(propellant), fmt_mass(float(_design.get("propellant_capacity_kg", 0.0)))]])
    items.append(["hold", PART_COLORS["cargo"], cargo, "Cargo %s" % fmt_mass(cargo)])
    items.append(["habitat", PART_COLORS["provisions"], provisions, "Provisions %s (%.0f days)" % [
        fmt_mass(provisions), float(_ship.get("provision_days", 0.0))]])
    var total := 0.0
    for item in items:
        total += float(item[2])
    var start := 0.0
    for item in items:
        var share := float(item[2]) / maxf(total, 1.0)
        if share <= 0.0:
            continue
        _segments.append({"key": item[0], "color": item[1], "from": start, "to": start + share,
            "tip": "%s · %.0f%% of %s" % [item[3], 100.0 * share, fmt_mass(total)]})
        start += share

# --- Drawing ---------------------------------------------------------------------

func _to_px(rect: Rect2) -> Rect2:
    return Rect2(rect.position * _scale + _offset, rect.size * _scale)

func _bar_rect() -> Rect2:
    return Rect2(4.0, SHIP_H + FOOTER_H + 4.0, size.x - 8.0, BAR_H)

func _draw() -> void:
    if _shapes.is_empty() or size.x < 40.0:
        return
    _scale = minf((size.x - 12.0) / _bounds.size.x, (SHIP_H - 8.0) / _bounds.size.y)
    _offset = Vector2((size.x - _bounds.size.x * _scale) * 0.5 - _bounds.position.x * _scale,
        SHIP_H * 0.5 - (_bounds.position.y + _bounds.size.y * 0.5) * _scale)
    var tip := _find_hover()
    for shape in _shapes:
        _draw_shape(shape)
    _draw_plume()
    for shape in _shapes:
        if String(shape["key"]) == _hover_key and String(shape["kind"]) != "structure":
            draw_rect(_to_px(shape["rect"]).grow(1.0), UiTheme.ACCENT, false, 1.5)
    _draw_footer()
    _draw_mass_bar()
    var font := get_theme_default_font()
    var text := tip if tip != "" else "Hover a part for details"
    var color := UiTheme.TEXT_PRIMARY if tip != "" else UiTheme.TEXT_DIM
    draw_multiline_string(font, Vector2(4.0, SHIP_H + FOOTER_H + BAR_H + 18.0), text, HORIZONTAL_ALIGNMENT_LEFT,
        size.x - 8.0, FONT_SIZE + 1, 2, color)

func _find_hover() -> String:
    _hover_key = ""
    if not is_visible_in_tree() or not get_global_rect().has_point(get_global_mouse_position()):
        return ""
    var mouse := get_local_mouse_position()
    if _bar_rect().has_point(mouse):
        var bar := _bar_rect()
        var at := (mouse.x - bar.position.x) / bar.size.x
        for segment in _segments:
            if at >= float(segment["from"]) and at < float(segment["to"]):
                _hover_key = String(segment["key"])
                return String(segment["tip"])
        return ""
    for i in range(_shapes.size() - 1, -1, -1):
        var shape: Dictionary = _shapes[i]
        if _to_px(shape["rect"]).grow(1.0).has_point(mouse):
            _hover_key = String(shape["key"])
            return _shape_tip(shape)
    return ""

func _shape_tip(shape: Dictionary) -> String:
    var comp: Dictionary = shape["comp"]
    match String(shape["kind"]):
        "shield":
            return "Shadow shield: shades the ship from the reactor (counted in the engine's mass)"
        "habitat":
            var frame: Array[String] = []
            for key in HABITAT_FRAME:
                var part: Dictionary = _components.get(key, {})
                if not part.is_empty():
                    frame.append("%s %s" % [String(part.get("name", "")).to_lower(), fmt_mass(_f(part, "mass_kg"))])
            return "%s %s · %s · %s" % [String(comp.get("name", "")), fmt_mass(_f(comp, "mass_kg")),
                component_spec(comp), ", ".join(frame)]
        "tank":
            var capacity := float(_design.get("propellant_capacity_kg", _f(comp, "capacity_kg")))
            return "%s %s · %s · %.0f%% full" % [String(comp.get("name", "")), fmt_mass(_f(comp, "mass_kg")),
                component_spec(comp), 100.0 * float(_ship.get("propellant_kg", 0.0)) / maxf(capacity, 1.0)]
        "hold":
            var lots: Array[String] = []
            for lot in _ship.get("cargo", []):
                lots.append("%.0f %s" % [float(lot.get("units", 0.0)), String(lot.get("commodity_id", "")).replace("_", " ")])
            return "%s %s · %s · %s" % [String(comp.get("name", "")), fmt_mass(_f(comp, "mass_kg")), component_spec(comp),
                ", ".join(lots) if not lots.is_empty() else "empty"]
        "wing":
            return "%s %s · %s · now %.0f K" % [String(comp.get("name", "")), fmt_mass(_f(comp, "mass_kg")),
                component_spec(comp), float(_ship.get("radiator_temperature_k", _f(comp, "temperature_k")))]
        "engine":
            return "%s %s · %s · %s" % [String(comp.get("name", "")), fmt_mass(_f(comp, "mass_kg")), component_spec(comp),
                String(_ship.get("engine_state", "off"))]
    var spec := component_spec(comp)
    return "%s %s%s" % [String(comp.get("name", "")), fmt_mass(_f(comp, "mass_kg")), (" · " + spec) if spec != "" else ""]

func _draw_shape(shape: Dictionary) -> void:
    var r := _to_px(shape["rect"])
    var comp: Dictionary = shape["comp"]
    var outline := Color(0.0, 0.0, 0.0, 0.6)
    match String(shape["kind"]):
        "structure":
            draw_line(Vector2(r.position.x, r.get_center().y), Vector2(r.end.x, r.get_center().y),
                PART_COLORS["structure"], maxf(1.0, r.size.y))
        "wing":
            var temperature := float(_ship.get("radiator_temperature_k", _f(comp, "temperature_k")))
            draw_rect(r, Thermal.color(temperature))
            # Panel seams, and the strut back to the truss.
            var seams := clampi(int(r.size.x / 14.0), 1, 12)
            for i in range(1, seams):
                var sx := r.position.x + r.size.x * float(i) / float(seams)
                draw_line(Vector2(sx, r.position.y), Vector2(sx, r.end.y), Color(0.0, 0.0, 0.0, 0.25), 1.0)
            draw_rect(r, outline, false, 1.0)
            var axis_y := _offset.y
            var edge_y := r.end.y if r.end.y <= axis_y else r.position.y
            draw_line(Vector2(r.get_center().x, axis_y), Vector2(r.get_center().x, edge_y), PART_COLORS["structure"], 1.0)
        "habitat":
            _rounded(r, PART_COLORS["habitat"].darkened(0.25), outline)
        "band":
            var index := HABITAT_BANDS.find(String(shape["key"]))
            draw_rect(r, BAND_SHADES[maxi(index, 0) % BAND_SHADES.size()])
            draw_line(Vector2(r.end.x, r.position.y), Vector2(r.end.x, r.end.y), Color(0.0, 0.0, 0.0, 0.35), 1.0)
            if String(shape["key"]) == "habitat.crew_quarters":
                _draw_crew(r)
        "hold":
            draw_rect(r, Color(0.12, 0.16, 0.14))
            var capacity := float(_design.get("cargo_capacity_units", 0.0))
            var x := r.position.x
            for lot in _ship.get("cargo", []):
                var w := r.size.x * float(lot.get("units", 0.0)) / maxf(capacity, 1.0)
                draw_rect(Rect2(x, r.position.y, minf(w, r.end.x - x), r.size.y),
                    UiTheme.commodity_color(String(lot.get("commodity_id", ""))))
                x += w
            draw_rect(r, PART_COLORS["hold"], false, 1.0)
        "tank":
            var cold := _f(comp, "temperature_k") < 100.0
            _rounded(r, Color(0.1, 0.13, 0.18), outline)
            var fill := clampf(float(_ship.get("propellant_kg", 0.0)) / maxf(float(_design.get("propellant_capacity_kg", 1.0)), 1.0), 0.0, 1.0)
            if fill > 0.0:
                var inner := r.grow(-1.5)
                inner.size.x *= fill
                # LH2 at 20 K frosts the tank white; water stays blue.
                draw_rect(inner, Color(0.85, 0.94, 1.0) if cold else Color(0.29, 0.55, 0.85))
            _rounded(r, Color(0, 0, 0, 0), PART_COLORS["tank"].lightened(0.2))
        "shield":
            draw_rect(r, Color(0.33, 0.36, 0.4))
        "engine":
            _draw_engine(r, comp)

func _rounded(r: Rect2, fill: Color, border: Color) -> void:
    var style := StyleBoxFlat.new()
    style.bg_color = fill
    style.border_color = border
    style.set_border_width_all(1)
    style.set_corner_radius_all(int(minf(r.size.x, r.size.y) * 0.5))
    style.anti_aliasing = true
    draw_style_box(style, r)

func _draw_crew(r: Rect2) -> void:
    var crew := int(_ship.get("crew_size", _design.get("crew_size", 0)))
    if crew <= 0:
        return
    var rows := 2 if crew > 3 and r.size.y > 10.0 else 1
    var per_row := int(ceil(float(crew) / float(rows)))
    var dot := clampf(minf(r.size.x / float(per_row + 1), r.size.y / float(rows + 1)) * 0.35, 1.0, 3.0)
    for i in range(crew):
        var row := floori(float(i) / float(per_row))
        var column := i % per_row
        var center := Vector2(r.position.x + r.size.x * float(column + 1) / float(per_row + 1),
            r.position.y + r.size.y * float(row + 1) / float(rows + 1))
        draw_circle(center, dot, Color(0.2, 0.25, 0.35))

func _engine_on() -> bool:
    var state := String(_ship.get("engine_state", "off"))
    return state == "burning" or state == "thrusting"

func _draw_engine(r: Rect2, comp: Dictionary) -> void:
    var outline := Color(0.0, 0.0, 0.0, 0.6)
    var split := 0.55 if not _plasma else 0.6
    var block := Rect2(r.position.x, r.position.y + r.size.y * 0.15, r.size.x * split, r.size.y * 0.7)
    var throat := r.position.x + r.size.x * split
    var nozzle := PackedVector2Array([Vector2(throat, r.get_center().y - r.size.y * 0.2),
        Vector2(r.end.x, r.position.y), Vector2(r.end.x, r.end.y), Vector2(throat, r.get_center().y + r.size.y * 0.2)])
    if _plasma:
        draw_rect(block, PART_COLORS["engine"])
        draw_colored_polygon(nozzle, Color(0.72, 0.55, 0.94, 0.85))
        # Magnetic nozzle: field coils as rings.
        for i in range(3):
            var cx := throat + (r.end.x - throat) * (0.25 + 0.3 * float(i))
            var half := r.size.y * (0.25 + 0.25 * (0.25 + 0.3 * float(i)))
            draw_line(Vector2(cx, r.get_center().y - half), Vector2(cx, r.get_center().y + half), Color(0.95, 0.85, 1.0, 0.7), 1.0)
    else:
        # The reactor in false colour: its core temperature while burning, otherwise cold.
        var temperature := _f(comp, "temperature_k") if _engine_on() else COLD_REACTOR_K
        draw_rect(block, Thermal.color(temperature))
        draw_colored_polygon(nozzle, Color(0.55, 0.57, 0.62))
    draw_rect(block, outline, false, 1.0)
    draw_polyline(nozzle + PackedVector2Array([nozzle[0]]), outline, 1.0)

func _draw_plume() -> void:
    if not _engine_on():
        return
    var r := _to_px(_engine_rect)
    var length := r.size.x * 1.5
    var root_color := Color(0.6, 0.75, 1.0, 0.9) if _plasma else Color(1.0, 0.88, 0.6, 0.95)
    var tail_color := Color(0.45, 0.35, 1.0, 0.0) if _plasma else Color(1.0, 0.45, 0.2, 0.0)
    var points := PackedVector2Array([Vector2(r.end.x, r.position.y + r.size.y * 0.1), Vector2(r.end.x + length, r.position.y - r.size.y * 0.1),
        Vector2(r.end.x + length, r.end.y + r.size.y * 0.1), Vector2(r.end.x, r.end.y - r.size.y * 0.1)])
    draw_polygon(points, PackedColorArray([root_color, tail_color, tail_color, root_color]))

func _draw_footer() -> void:
    var font := get_theme_default_font()
    var y := SHIP_H + 4.0
    # Scale bar: the round length nearest 70 px.
    var metres := 1.0
    for candidate in [1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0]:
        if candidate * _scale <= 90.0:
            metres = candidate
    var length := metres * _scale
    draw_line(Vector2(6.0, y + 6.0), Vector2(6.0 + length, y + 6.0), UiTheme.TEXT_DIM, 1.0)
    draw_line(Vector2(6.0, y + 2.0), Vector2(6.0, y + 10.0), UiTheme.TEXT_DIM, 1.0)
    draw_line(Vector2(6.0 + length, y + 2.0), Vector2(6.0 + length, y + 10.0), UiTheme.TEXT_DIM, 1.0)
    draw_string(font, Vector2(10.0 + length, y + 10.0), "%.0f m" % metres, HORIZONTAL_ALIGNMENT_LEFT, -1, FONT_SIZE, UiTheme.TEXT_DIM)
    # Temperature legend, with a tick at the radiators' or reactor's current temperature.
    var legend := Rect2(size.x - 110.0, y + 2.0, 70.0, 8.0)
    var steps := 28
    for i in range(steps):
        var x0 := legend.position.x + legend.size.x * float(i) / float(steps)
        draw_rect(Rect2(x0, legend.position.y, legend.size.x / float(steps) + 0.5, legend.size.y), Thermal.at(0.15 + 0.85 * float(i) / float(steps - 1)))
    draw_string(font, Vector2(legend.position.x - 34.0, y + 10.0), "200 K", HORIZONTAL_ALIGNMENT_LEFT, -1, FONT_SIZE, UiTheme.TEXT_DIM)
    draw_string(font, Vector2(legend.end.x + 3.0, y + 10.0), "6000", HORIZONTAL_ALIGNMENT_LEFT, -1, FONT_SIZE, UiTheme.TEXT_DIM)
    var marked := float(_ship.get("radiator_temperature_k", 0.0))
    if not _plasma and _engine_on():
        marked = _f(_components.get("engine", {}), "temperature_k")
    if marked > 0.0:
        var mx := legend.position.x + legend.size.x * Thermal.scale_of(marked)
        draw_line(Vector2(mx, legend.position.y - 2.0), Vector2(mx, legend.end.y + 2.0), UiTheme.TEXT_PRIMARY, 1.0)

func _draw_mass_bar() -> void:
    var bar := _bar_rect()
    for segment in _segments:
        var r := Rect2(bar.position.x + bar.size.x * float(segment["from"]), bar.position.y,
            bar.size.x * (float(segment["to"]) - float(segment["from"])), bar.size.y)
        draw_rect(r, segment["color"])
        draw_line(Vector2(r.end.x, r.position.y), Vector2(r.end.x, r.end.y), Color(0.0, 0.0, 0.0, 0.5), 1.0)
        if String(segment["key"]) == _hover_key:
            draw_rect(r.grow(1.0), UiTheme.ACCENT, false, 1.5)
    draw_rect(bar, Color(0.3, 0.45, 0.55, 0.6), false, 1.0)

# --- Text helpers, shared with the inspector's parts list ------------------------

static func component_spec(c: Dictionary) -> String:
    var parts: Array[String] = []
    var power := float(c.get("power_w", 0.0))
    var heat := float(c.get("heat_w", 0.0))
    var temperature := float(c.get("temperature_k", 0.0))
    match String(c.get("kind", "")):
        "engine":
            if float(c.get("isp_s", 0.0)) > 0.0:
                parts.append("Isp %.0f s" % float(c["isp_s"]))
                parts.append("thrust %.0f kN" % (float(c.get("thrust_n", 0.0)) / 1000.0))
            parts.append("jet %s" % fmt_power(power))
            if heat > 0.0:
                parts.append("waste heat %s" % fmt_power(heat))
            if temperature > 0.0:
                parts.append("core %.0f K" % temperature)
        "radiator", "thermal_control":
            if float(c.get("area_m2", 0.0)) > 0.0:
                parts.append("%.0f m²" % float(c["area_m2"]))
            parts.append("rejects %s at %.0f K" % [fmt_power(heat), temperature])
        "tank":
            parts.append("%s propellant · %.0f m³ · %.0f K" % [fmt_mass(float(c.get("capacity_kg", 0.0))),
                float(c.get("volume_m3", 0.0)), temperature])
        "habitat":
            parts.append("%d crew · %.0f m³ pressurized · %s" % [int(c.get("crew_capacity", 0.0)),
                float(c.get("volume_m3", 0.0)), fmt_power(power)])
        "crew_quarters":
            parts.append("%.0f m³ habitable" % float(c.get("volume_m3", 0.0)))
        "hold":
            parts.append("rated %s · %.0f m³" % [fmt_mass(float(c.get("capacity_kg", 0.0))), float(c.get("volume_m3", 0.0))])
    return " · ".join(parts)

static func fmt_mass(kg: float) -> String:
    return "%.0f kg" % kg if absf(kg) < 1000.0 else "%.1f t" % (kg / 1000.0)

static func fmt_power(w: float) -> String:
    if w >= 1.0e6:
        return "%.1f MW" % (w / 1.0e6)
    return "%.1f kW" % (w / 1000.0)
