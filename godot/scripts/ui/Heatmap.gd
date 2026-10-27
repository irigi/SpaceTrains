extends Control

# A grid of coloured cells with axes, a colour legend and a read-out at the mouse; optional
# threshold contours, point markers and a vertical "now" line. Used for porkchop plots
# (departure x flight time) and for coloured from x to tables.
#
# values[col][row]: a float, or NAN for a cell with no value (drawn dark). Columns run along x,
# rows along y from the bottom up. Axis values are the cell centres.

const UiTheme := preload("res://scripts/ui/UiTheme.gd")

var title := ""
var values: Array = []
var x_values: Array = []
var y_values: Array = []
var x_label := ""
var y_label := ""
var color_lo := 0.0
var color_hi := 1.0
var log_scale := false
var value_format := "%.2f"
# Optional func(value: float) -> String instead of value_format (Godot has no %g).
var format_value: Callable
var unit := ""
# [{level: float, color: Color, name: String}]: a line along the cell edges where values cross level.
var contours: Array = []
# [{x: float, y: float, color: Color, name: String}] in axis units.
var markers: Array = []
var now_x := NAN
# Optional: func(col: int, row: int) -> String for the read-out (default: axis values and the value).
var readout: Callable
var x_format := "%.0f"
var y_format := "%.0f"
var _hover := Vector2i(-1, -1)

const PAD_LEFT := 52.0
const PAD_RIGHT := 64.0
const PAD_TOP := 34.0
const PAD_BOTTOM := 34.0

func _init() -> void:
    custom_minimum_size = Vector2(560, 300)
    mouse_filter = Control.MOUSE_FILTER_STOP

func set_grid(chart_title: String, grid: Array, xs: Array, ys: Array, lo: float, hi: float, logarithmic := false) -> void:
    title = chart_title
    values = grid
    x_values = xs
    y_values = ys
    color_lo = lo
    color_hi = hi
    log_scale = logarithmic
    queue_redraw()

# Viridis-like sequential scale, t in [0, 1].
static func scale_color(t: float) -> Color:
    const STOPS := [Color(0.267, 0.005, 0.329), Color(0.229, 0.322, 0.546), Color(0.128, 0.567, 0.551),
        Color(0.369, 0.789, 0.383), Color(0.993, 0.906, 0.144)]
    t = clampf(t, 0.0, 1.0) * (STOPS.size() - 1)
    var i := mini(int(t), STOPS.size() - 2)
    return (STOPS[i] as Color).lerp(STOPS[i + 1], t - i)

func _t(value: float) -> float:
    if log_scale:
        var lo := log(maxf(color_lo, 1e-12))
        var hi := log(maxf(color_hi, color_lo * 1.0001 + 1e-12))
        return (log(maxf(value, 1e-12)) - lo) / maxf(hi - lo, 1e-12)
    return (value - color_lo) / maxf(color_hi - color_lo, 1e-12)

func _fmt(value: float) -> String:
    return String(format_value.call(value)) if format_value.is_valid() else value_format % value

# A number with `digits` significant digits (for values spanning decades).
static func significant(value: float, digits := 3) -> String:
    if value == 0.0 or is_nan(value) or is_inf(value):
        return str(value)
    var decimals := maxi(0, digits - 1 - int(floor(log(absf(value)) / log(10.0))))
    return String.num(value, decimals)

func _area() -> Rect2:
    return Rect2(PAD_LEFT, PAD_TOP, size.x - PAD_LEFT - PAD_RIGHT, size.y - PAD_TOP - PAD_BOTTOM)

func _cell_rect(col: int, row: int) -> Rect2:
    var area := _area()
    var w := area.size.x / maxf(1.0, values.size())
    var rows := (values[0] as Array).size() if not values.is_empty() else 1
    var h := area.size.y / maxf(1.0, rows)
    return Rect2(area.position.x + col * w, area.end.y - (row + 1) * h, w, h)

func _value(col: int, row: int) -> float:
    if col < 0 or col >= values.size():
        return NAN
    var column: Array = values[col]
    return float(column[row]) if row >= 0 and row < column.size() else NAN

func _axis_to_x(x: float) -> float:
    var area := _area()
    if x_values.size() < 2:
        return area.position.x
    var x0 := float(x_values[0])
    var x1 := float(x_values[x_values.size() - 1])
    var half := (x1 - x0) / (x_values.size() - 1) * 0.5
    return area.position.x + (x - (x0 - half)) / maxf(x1 - x0 + 2.0 * half, 1e-12) * area.size.x

func _axis_to_y(y: float) -> float:
    var area := _area()
    if y_values.size() < 2:
        return area.end.y
    var y0 := float(y_values[0])
    var y1 := float(y_values[y_values.size() - 1])
    var half := (y1 - y0) / (y_values.size() - 1) * 0.5
    return area.end.y - (y - (y0 - half)) / maxf(y1 - y0 + 2.0 * half, 1e-12) * area.size.y

func _gui_input(event: InputEvent) -> void:
    if not event is InputEventMouseMotion:
        return
    var area := _area()
    var hit := Vector2i(-1, -1)
    if area.has_point(event.position) and not values.is_empty():
        var rows := (values[0] as Array).size()
        hit.x = clampi(int((event.position.x - area.position.x) / area.size.x * values.size()), 0, values.size() - 1)
        hit.y = clampi(int((area.end.y - event.position.y) / area.size.y * rows), 0, rows - 1)
    if hit != _hover:
        _hover = hit
        queue_redraw()

func _notification(what: int) -> void:
    if what == NOTIFICATION_MOUSE_EXIT and _hover.x != -1:
        _hover = Vector2i(-1, -1)
        queue_redraw()

func _draw() -> void:
    var font := get_theme_default_font()
    var area := _area()
    draw_rect(Rect2(Vector2.ZERO, size), Color(0.03, 0.05, 0.08, 0.9))
    draw_string(font, Vector2(8, 15), title, HORIZONTAL_ALIGNMENT_LEFT, size.x - 16.0, 12, UiTheme.ACCENT)
    if values.is_empty():
        draw_string(font, Vector2(area.position.x, area.get_center().y), "computing…", HORIZONTAL_ALIGNMENT_LEFT, -1, 11,
            UiTheme.TEXT_DIM)
        return
    var rows := (values[0] as Array).size()
    var empty := Color(0.07, 0.08, 0.1)
    for col in range(values.size()):
        for row in range(rows):
            var v := _value(col, row)
            var rect := _cell_rect(col, row)
            draw_rect(Rect2(rect.position, rect.size + Vector2(0.6, 0.6)), empty if is_nan(v) else scale_color(_t(v)))
    # Contours along cell edges.
    for contour in contours:
        var level := float(contour.get("level", 0.0))
        var color: Color = contour.get("color", Color.WHITE)
        for col in range(values.size()):
            for row in range(rows):
                var v := _value(col, row)
                var inside := not is_nan(v) and v <= level
                var rect := _cell_rect(col, row)
                if col + 1 < values.size():
                    var right := _value(col + 1, row)
                    if inside != (not is_nan(right) and right <= level):
                        draw_line(Vector2(rect.end.x, rect.position.y), rect.end, color, 1.5)
                if row + 1 < rows:
                    var up := _value(col, row + 1)
                    if inside != (not is_nan(up) and up <= level):
                        draw_line(rect.position, Vector2(rect.end.x, rect.position.y), color, 1.5)
    draw_rect(area, Color(0.35, 0.85, 0.95, 0.25), false, 1.0)
    if not is_nan(now_x):
        var nx := _axis_to_x(now_x)
        if nx >= area.position.x and nx <= area.end.x:
            draw_line(Vector2(nx, area.position.y), Vector2(nx, area.end.y), Color(1, 1, 1, 0.7), 1.0)
            draw_string(font, Vector2(nx + 3, area.position.y + 11), "now", HORIZONTAL_ALIGNMENT_LEFT, -1, 10, Color.WHITE)
    for m in markers:
        var p := Vector2(_axis_to_x(float(m.get("x", 0.0))), _axis_to_y(float(m.get("y", 0.0))))
        if area.grow(2.0).has_point(p):
            draw_circle(p, 4.0, Color.BLACK)
            draw_circle(p, 3.0, m.get("color", Color.WHITE))
    # Axes: first, middle and last values.
    if x_values.size() >= 1:
        for i in [0, x_values.size() / 2, x_values.size() - 1]:
            var x := _axis_to_x(float(x_values[i]))
            draw_string(font, Vector2(x - 30, area.end.y + 13), x_format % float(x_values[i]), HORIZONTAL_ALIGNMENT_CENTER, 60, 10,
                UiTheme.TEXT_DIM)
    draw_string(font, Vector2(area.position.x, size.y - 5), x_label, HORIZONTAL_ALIGNMENT_LEFT, area.size.x, 10, UiTheme.TEXT_DIM)
    if y_values.size() >= 1:
        for i in [0, y_values.size() / 2, y_values.size() - 1]:
            var y := _axis_to_y(float(y_values[i]))
            draw_string(font, Vector2(2, y + 4), y_format % float(y_values[i]), HORIZONTAL_ALIGNMENT_RIGHT, PAD_LEFT - 6, 10,
                UiTheme.TEXT_DIM)
    draw_string(font, Vector2(8, PAD_TOP - 6), y_label, HORIZONTAL_ALIGNMENT_LEFT, -1, 10, UiTheme.TEXT_DIM)
    # Colour legend on the right.
    var lx := area.end.x + 10.0
    var steps := 40
    for k in range(steps):
        var t := float(k) / (steps - 1)
        var y := area.end.y - area.size.y * (k + 1) / steps
        draw_rect(Rect2(lx, y, 10, area.size.y / steps + 0.6), scale_color(t))
    var hi_text := _fmt(color_hi)
    var lo_text := _fmt(color_lo)
    draw_string(font, Vector2(lx + 13, area.position.y + 8), hi_text, HORIZONTAL_ALIGNMENT_LEFT, PAD_RIGHT - 24, 10, UiTheme.TEXT_DIM)
    draw_string(font, Vector2(lx + 13, area.end.y), lo_text, HORIZONTAL_ALIGNMENT_LEFT, PAD_RIGHT - 24, 10, UiTheme.TEXT_DIM)
    if unit != "":
        draw_string(font, Vector2(lx, area.end.y + 13), unit, HORIZONTAL_ALIGNMENT_LEFT, PAD_RIGHT - 12, 10, UiTheme.TEXT_DIM)
    # Read-out of the hovered cell.
    if _hover.x != -1:
        var rect := _cell_rect(_hover.x, _hover.y)
        draw_rect(rect, Color.WHITE, false, 1.0)
        var text := ""
        if readout.is_valid():
            text = String(readout.call(_hover.x, _hover.y))
        else:
            var v := _value(_hover.x, _hover.y)
            text = "%s %s · %s %s · %s" % [x_label, x_format % float(x_values[_hover.x]), y_label,
                y_format % float(y_values[_hover.y]), "—" if is_nan(v) else _fmt(v) + " " + unit]
        var lines := text.split("\n")
        var box_w := 10.0
        for line in lines:
            box_w = maxf(box_w, font.get_string_size(line, HORIZONTAL_ALIGNMENT_LEFT, -1, 10).x + 10.0)
        var box := Rect2(rect.end.x + 6.0, rect.position.y - 4.0, box_w, 13.0 * lines.size() + 6.0)
        if box.end.x > area.end.x:
            box.position.x = rect.position.x - 6.0 - box_w
        box.position.y = clampf(box.position.y, area.position.y, area.end.y - box.size.y)
        draw_rect(box, Color(0.06, 0.09, 0.14, 0.94))
        for i in range(lines.size()):
            draw_string(font, box.position + Vector2(5, 13 + 13 * i), lines[i], HORIZONTAL_ALIGNMENT_LEFT, -1, 10, UiTheme.TEXT_PRIMARY)
