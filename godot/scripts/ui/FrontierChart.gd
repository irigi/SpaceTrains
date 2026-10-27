extends Control

# Fastest trip by departure date (the Routes tab): x the departure day, y the flight in days,
# one line per class (gaps where it cannot go), ships as dots at (their departure, their flight),
# a "now" line and a read-out at the mouse of every line's flight and arrival day. The legend is
# drawn by the caller (shared with the maps).

const UiTheme := preload("res://scripts/ui/UiTheme.gd")

var title := ""
var days: Array = []
# [{name: String, color: Color, values: Array (NAN: cannot go), dashed: bool}]
var series: Array = []
# [{x: day, y: flight days, color: Color, name: String}]
var markers: Array = []
var now_x := NAN
var _hover_x := -1.0

const PAD_LEFT := 52.0
const PAD_RIGHT := 16.0
const PAD_TOP := 40.0
const PAD_BOTTOM := 34.0

func _init() -> void:
    custom_minimum_size = Vector2(560, 300)
    mouse_filter = Control.MOUSE_FILTER_STOP

func set_data(chart_title: String, chart_days: Array, chart_series: Array, chart_markers: Array, now_day: float) -> void:
    title = chart_title
    days = chart_days
    series = chart_series
    markers = chart_markers
    now_x = now_day
    queue_redraw()

func _gui_input(event: InputEvent) -> void:
    if event is InputEventMouseMotion:
        _hover_x = event.position.x
        queue_redraw()

func _notification(what: int) -> void:
    if what == NOTIFICATION_MOUSE_EXIT:
        _hover_x = -1.0
        queue_redraw()

func _area() -> Rect2:
    return Rect2(PAD_LEFT, PAD_TOP, size.x - PAD_LEFT - PAD_RIGHT, size.y - PAD_TOP - PAD_BOTTOM)

func _draw() -> void:
    var font := get_theme_default_font()
    var area := _area()
    draw_rect(Rect2(Vector2.ZERO, size), Color(0.03, 0.05, 0.08, 0.9))
    draw_string(font, Vector2(8, 15), title, HORIZONTAL_ALIGNMENT_LEFT, size.x - 16.0, 12, UiTheme.ACCENT)
    if days.size() < 2:
        draw_string(font, Vector2(area.position.x, area.get_center().y), "computing…", HORIZONTAL_ALIGNMENT_LEFT, -1, 11,
            UiTheme.TEXT_DIM)
        return
    var hi := 1.0
    for s in series:
        for v in s.get("values", []):
            if not is_nan(float(v)):
                hi = maxf(hi, float(v))
    for m in markers:
        hi = maxf(hi, float(m.get("y", 0.0)))
    hi *= 1.08
    var d0 := float(days[0])
    var d1 := float(days[days.size() - 1])
    var span := maxf(1.0, d1 - d0)
    var to_point := func(day: float, flight: float) -> Vector2:
        return Vector2(area.position.x + (day - d0) / span * area.size.x, area.end.y - flight / hi * area.size.y)
    # Grid: flight times at round steps.
    draw_rect(area, Color(0.35, 0.85, 0.95, 0.25), false, 1.0)
    var step := _round_step(hi / 5.0)
    var tick := step
    while tick < hi:
        var y: float = to_point.call(d0, tick).y
        draw_line(Vector2(area.position.x, y), Vector2(area.end.x, y), Color(0.55, 0.63, 0.72, 0.15), 1.0)
        draw_string(font, Vector2(2, y + 4), "%.0f" % tick, HORIZONTAL_ALIGNMENT_RIGHT, PAD_LEFT - 6, 10, UiTheme.TEXT_DIM)
        tick += step
    draw_string(font, Vector2(2, area.end.y + 4), "0", HORIZONTAL_ALIGNMENT_RIGHT, PAD_LEFT - 6, 10, UiTheme.TEXT_DIM)
    draw_string(font, Vector2(8, PAD_TOP - 8), "flight (days)", HORIZONTAL_ALIGNMENT_LEFT, -1, 10, UiTheme.TEXT_DIM)
    for i in [0, days.size() / 2, days.size() - 1]:
        var x: float = to_point.call(float(days[i]), 0.0).x
        draw_string(font, Vector2(x - 30, area.end.y + 13), "%.0f" % float(days[i]), HORIZONTAL_ALIGNMENT_CENTER, 60, 10,
            UiTheme.TEXT_DIM)
    draw_string(font, Vector2(area.position.x, size.y - 5), "departure (game day)", HORIZONTAL_ALIGNMENT_LEFT, -1, 10, UiTheme.TEXT_DIM)
    if not is_nan(now_x) and now_x >= d0 and now_x <= d1:
        var nx: float = to_point.call(now_x, 0.0).x
        draw_line(Vector2(nx, area.position.y), Vector2(nx, area.end.y), Color(1, 1, 1, 0.6), 1.0)
        draw_string(font, Vector2(nx + 3, area.position.y + 11), "now", HORIZONTAL_ALIGNMENT_LEFT, -1, 10, Color.WHITE)
    # Lines, broken where the class cannot go.
    for s in series:
        var values: Array = s.get("values", [])
        var color: Color = s.get("color", UiTheme.ACCENT)
        var dashed := bool(s.get("dashed", false))
        var run := PackedVector2Array()
        for i in range(mini(values.size(), days.size()) + 1):
            var v := float(values[i]) if i < mini(values.size(), days.size()) else NAN
            if is_nan(v):
                _draw_run(run, color, dashed)
                run = PackedVector2Array()
                continue
            run.append(to_point.call(float(days[i]), v))
    for m in markers:
        var p: Vector2 = to_point.call(float(m.get("x", 0.0)), float(m.get("y", 0.0)))
        if area.grow(2.0).has_point(p):
            draw_circle(p, 4.5, Color.BLACK)
            draw_circle(p, 3.5, m.get("color", Color.WHITE))
    # Read-out at the mouse: each line's flight and arrival for the nearest departure.
    if _hover_x >= area.position.x and _hover_x <= area.end.x:
        var day := d0 + (_hover_x - area.position.x) / area.size.x * span
        var index := clampi(int(round((day - d0) / span * (days.size() - 1))), 0, days.size() - 1)
        var dep := float(days[index])
        var x: float = to_point.call(dep, 0.0).x
        draw_line(Vector2(x, area.position.y), Vector2(x, area.end.y), Color(1, 1, 1, 0.25), 1.0)
        var lines: Array = [["depart day %.0f (in %.0f d)" % [dep, dep - now_x] if not is_nan(now_x) else "depart day %.0f" % dep,
            UiTheme.TEXT_PRIMARY]]
        for s in series:
            var values: Array = s.get("values", [])
            var v := float(values[index]) if index < values.size() else NAN
            lines.append(["%s: %s" % [String(s.get("name", "")), "cannot go" if is_nan(v) else "%.0f d, arrive day %.0f" % [v, dep + v]],
                s.get("color", UiTheme.TEXT_PRIMARY)])
        var box_w := 10.0
        for line in lines:
            box_w = maxf(box_w, font.get_string_size(String(line[0]), HORIZONTAL_ALIGNMENT_LEFT, -1, 10).x + 12.0)
        var box_x: float = x + 8.0 if x + 8.0 + box_w < area.end.x else x - 8.0 - box_w
        draw_rect(Rect2(box_x, area.position.y + 4, box_w, 13 * lines.size() + 6), Color(0.06, 0.09, 0.14, 0.94))
        for i in range(lines.size()):
            draw_string(font, Vector2(box_x + 6, area.position.y + 17 + 13 * i), String(lines[i][0]), HORIZONTAL_ALIGNMENT_LEFT, -1, 10,
                lines[i][1])

func _draw_run(points: PackedVector2Array, color: Color, dashed: bool) -> void:
    if points.size() == 1:
        draw_circle(points[0], 1.5, color)
    elif points.size() >= 2:
        if dashed:
            # Dashes of about 6 px along the line.
            for i in range(points.size() - 1):
                var a := points[i]
                var b := points[i + 1]
                var length := a.distance_to(b)
                var t := 0.0
                while t < length:
                    draw_line(a.lerp(b, t / length), a.lerp(b, minf(t + 6.0, length) / length), color, 1.5)
                    t += 10.0
        else:
            draw_polyline(points, color, 2.0, true)

static func _round_step(raw: float) -> float:
    var magnitude := pow(10.0, floor(log(maxf(raw, 1e-9)) / log(10.0)))
    for f in [1.0, 2.0, 5.0, 10.0]:
        if raw <= f * magnitude:
            return f * magnitude
    return 10.0 * magnitude
