extends Control

# A small line chart for the Statistics screen: several series over game days, a zero line,
# credit labels on the y axis (or `format_value`), and a read-out of every series at the mouse.
# `stacked`: the series are drawn as areas on top of each other (parts of a total), the read-out
# gives each part and the total.

const UiTheme := preload("res://scripts/ui/UiTheme.gd")

var title := ""
var days: Array = []
# [{name: String, color: Color, values: Array, dashed: bool}]
var series: Array = []
var _hover_x := -1.0
var stacked := false
# func(value: float) -> String for the axis and read-out; credits when not set.
var format_value: Callable

const PAD_LEFT := 74.0
const PAD_RIGHT := 12.0
const PAD_TOP := 22.0
const PAD_BOTTOM := 20.0

func _init() -> void:
    custom_minimum_size = Vector2(560, 210)
    mouse_filter = Control.MOUSE_FILTER_STOP

func _fmt(value: float) -> String:
    return String(format_value.call(value)) if format_value.is_valid() else UiTheme.format_credits(value)

func set_data(chart_title: String, chart_days: Array, chart_series: Array) -> void:
    title = chart_title
    days = chart_days
    series = chart_series
    queue_redraw()

func _gui_input(event: InputEvent) -> void:
    if event is InputEventMouseMotion:
        _hover_x = event.position.x
        queue_redraw()
    elif event is InputEventMouseButton and event.button_index in [MOUSE_BUTTON_WHEEL_UP, MOUSE_BUTTON_WHEEL_DOWN]:
        # Let the screen's scroll container have the wheel.
        pass

func _notification(what: int) -> void:
    if what == NOTIFICATION_MOUSE_EXIT:
        _hover_x = -1.0
        queue_redraw()

func _draw() -> void:
    var font := get_theme_default_font()
    var area := Rect2(PAD_LEFT, PAD_TOP, size.x - PAD_LEFT - PAD_RIGHT, size.y - PAD_TOP - PAD_BOTTOM)
    draw_rect(Rect2(Vector2.ZERO, size), Color(0.03, 0.05, 0.08, 0.9))
    draw_string(font, Vector2(8, 15), title, HORIZONTAL_ALIGNMENT_LEFT, -1, 12, UiTheme.ACCENT)
    if days.size() < 2 or series.is_empty():
        draw_string(font, Vector2(area.position.x, area.position.y + area.size.y * 0.5),
            "History appears after the first months (a sample every 30 days).", HORIZONTAL_ALIGNMENT_LEFT, -1, 11, UiTheme.TEXT_DIM)
        return
    var lo := 0.0
    var hi := 0.0
    var tops: Array = []   # stacked: the running total under and including each series
    if stacked:
        var running: Array = []
        running.resize(days.size())
        running.fill(0.0)
        for s in series:
            var values: Array = s.get("values", [])
            var top: Array = running.duplicate()
            for i in range(mini(values.size(), days.size())):
                top[i] = float(running[i]) + maxf(0.0, float(values[i]))
                hi = max(hi, float(top[i]))
            tops.append(top)
            running = top
    else:
        for s in series:
            for v in s.get("values", []):
                lo = min(lo, float(v))
                hi = max(hi, float(v))
    if hi - lo < 1.0:
        hi = lo + 1.0
    var margin := (hi - lo) * 0.05
    lo -= margin
    hi += margin
    var d0 := float(days[0])
    var d1 := float(days[days.size() - 1])
    var span: float = max(1.0, d1 - d0)
    var to_point := func(day: float, value: float) -> Vector2:
        return Vector2(area.position.x + (day - d0) / span * area.size.x,
            area.position.y + (hi - value) / (hi - lo) * area.size.y)
    # Grid: zero line, top and bottom labels.
    draw_rect(area, Color(0.35, 0.85, 0.95, 0.15), false, 1.0)
    var zero_y: float = to_point.call(d0, 0.0).y
    for value in [0.0, hi - margin, lo + margin]:
        var y: float = to_point.call(d0, value).y
        if value != 0.0 and abs(y - zero_y) < 14.0:
            continue   # too close to the zero line's label
        draw_line(Vector2(area.position.x, y), Vector2(area.end.x, y), Color(0.55, 0.63, 0.72, 0.35 if value == 0.0 else 0.15), 1.0)
        draw_string(font, Vector2(4, y + 4), _fmt(value), HORIZONTAL_ALIGNMENT_LEFT, PAD_LEFT - 8, 10, UiTheme.TEXT_DIM)
    draw_string(font, Vector2(area.position.x, size.y - 5), "day %.0f" % d0, HORIZONTAL_ALIGNMENT_LEFT, -1, 10, UiTheme.TEXT_DIM)
    draw_string(font, Vector2(area.end.x - 70, size.y - 5), "day %.0f" % d1, HORIZONTAL_ALIGNMENT_RIGHT, 70, 10, UiTheme.TEXT_DIM)
    # Series and legend.
    var legend_x := 24.0 + font.get_string_size(title, HORIZONTAL_ALIGNMENT_LEFT, -1, 12).x
    if stacked:
        for k in range(series.size()):
            var color: Color = series[k].get("color", UiTheme.ACCENT)
            var top: Array = tops[k]
            var polygon := PackedVector2Array()
            for i in range(days.size()):
                polygon.append(to_point.call(float(days[i]), float(top[i])))
            for i in range(days.size() - 1, -1, -1):
                polygon.append(to_point.call(float(days[i]), float(tops[k - 1][i]) if k > 0 else 0.0))
            if polygon.size() >= 3:
                draw_colored_polygon(polygon, Color(color, 0.75))
    for s in series:
        var values: Array = s.get("values", [])
        var color: Color = s.get("color", UiTheme.ACCENT)
        if stacked:
            values = tops[series.find(s)]
        var points := PackedVector2Array()
        for i in range(min(values.size(), days.size())):
            points.append(to_point.call(float(days[i]), float(values[i])))
        if points.size() >= 2:
            if bool(s.get("dashed", false)):
                for i in range(0, points.size() - 1, 2):
                    draw_line(points[i], points[i + 1], color, 1.0)
            else:
                draw_polyline(points, color, 1.5, true)
        var name := String(s.get("name", ""))
        draw_rect(Rect2(legend_x, 7, 10, 3), color)
        draw_string(font, Vector2(legend_x + 13, 13), name, HORIZONTAL_ALIGNMENT_LEFT, -1, 10, UiTheme.TEXT_PRIMARY)
        legend_x += 22.0 + font.get_string_size(name, HORIZONTAL_ALIGNMENT_LEFT, -1, 10).x
    # Read-out at the mouse.
    if _hover_x >= area.position.x and _hover_x <= area.end.x:
        var day := d0 + (_hover_x - area.position.x) / area.size.x * span
        var index := 0
        for i in range(days.size()):
            if abs(float(days[i]) - day) < abs(float(days[index]) - day):
                index = i
        var x: float = to_point.call(float(days[index]), 0.0).x
        draw_line(Vector2(x, area.position.y), Vector2(x, area.end.y), Color(1, 1, 1, 0.25), 1.0)
        var lines: Array[String] = ["day %.0f" % float(days[index])]
        var total := 0.0
        for s in series:
            var values: Array = s.get("values", [])
            if index < values.size():
                lines.append("%s %s" % [String(s.get("name", "")), _fmt(float(values[index]))])
                total += float(values[index])
        if stacked:
            lines.append("total %s" % _fmt(total))
        var box_x: float = x + 8.0 if x < area.end.x - 190.0 else x - 190.0
        draw_rect(Rect2(box_x, area.position.y + 4, 182, 14 * lines.size() + 6), Color(0.06, 0.09, 0.14, 0.92))
        for i in range(lines.size()):
            draw_string(font, Vector2(box_x + 5, area.position.y + 17 + 14 * i), lines[i], HORIZONTAL_ALIGNMENT_LEFT, 176, 10,
                UiTheme.TEXT_PRIMARY)
