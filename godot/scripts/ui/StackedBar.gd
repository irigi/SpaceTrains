extends Control

# Horizontal stacked bars, one row per entity (station), the same parts in each: for comparing
# compositions across entities. A row may carry a marker (e.g. housing against residents).
# Bars share one scale (the largest row or marker), or each is shown as shares (normalized).

const UiTheme := preload("res://scripts/ui/UiTheme.gd")

var title := ""
# [{name: String, color: Color}]
var parts: Array = []
# [{name: String, values: Array (one per part), marker: float (optional, <0 = none)}]
var rows: Array = []
var normalized := false
var value_format := "%.0f"
var marker_name := ""
var _hover_row := -1
var _hover_part := -1

const LABEL_WIDTH := 150.0
const ROW_HEIGHT := 17.0
const TOP := 34.0
const RIGHT := 64.0

func _init() -> void:
    custom_minimum_size = Vector2(560, 80)
    mouse_filter = Control.MOUSE_FILTER_STOP

func set_data(chart_title: String, chart_parts: Array, chart_rows: Array, shares := false, format := "%.0f",
        marker_label := "") -> void:
    title = chart_title
    parts = chart_parts
    rows = chart_rows
    normalized = shares
    value_format = format
    marker_name = marker_label
    custom_minimum_size.y = TOP + ROW_HEIGHT * rows.size() + 18.0
    queue_redraw()

func _row_total(row: Dictionary) -> float:
    var total := 0.0
    for v in row.get("values", []):
        total += maxf(0.0, float(v))
    return total

func _scale() -> float:
    var top := 0.0
    for row in rows:
        top = maxf(top, _row_total(row))
        top = maxf(top, float(row.get("marker", -1.0)))
    return maxf(top, 1e-9)

func _bar_width() -> float:
    return maxf(40.0, size.x - LABEL_WIDTH - RIGHT)

func _gui_input(event: InputEvent) -> void:
    if not event is InputEventMouseMotion:
        return
    var pos: Vector2 = event.position
    var row := int((pos.y - TOP) / ROW_HEIGHT) if pos.y >= TOP else -1
    var part := -1
    if row >= 0 and row < rows.size() and pos.x >= LABEL_WIDTH:
        var r: Dictionary = rows[row]
        var total := _row_total(r)
        var scale := total if normalized else _scale()
        var x := LABEL_WIDTH
        var values: Array = r.get("values", [])
        for i in range(values.size()):
            var w := _bar_width() * maxf(0.0, float(values[i])) / maxf(scale, 1e-9)
            if pos.x >= x and pos.x < x + w:
                part = i
                break
            x += w
    else:
        row = -1
    if row != _hover_row or part != _hover_part:
        _hover_row = row
        _hover_part = part
        queue_redraw()

func _notification(what: int) -> void:
    if what == NOTIFICATION_MOUSE_EXIT and _hover_row != -1:
        _hover_row = -1
        _hover_part = -1
        queue_redraw()

func _draw() -> void:
    var font := get_theme_default_font()
    draw_rect(Rect2(Vector2.ZERO, size), Color(0.03, 0.05, 0.08, 0.9))
    draw_string(font, Vector2(8, 15), title, HORIZONTAL_ALIGNMENT_LEFT, -1, 12, UiTheme.ACCENT)
    # Legend of the parts on the second line.
    var lx := 8.0
    for i in range(parts.size()):
        var p: Dictionary = parts[i]
        var name := String(p.get("name", ""))
        draw_rect(Rect2(lx, 22, 9, 9), p.get("color", UiTheme.ACCENT))
        draw_string(font, Vector2(lx + 12, 31), name, HORIZONTAL_ALIGNMENT_LEFT, -1, 10,
            Color.WHITE if i == _hover_part else UiTheme.TEXT_PRIMARY)
        lx += 22.0 + font.get_string_size(name, HORIZONTAL_ALIGNMENT_LEFT, -1, 10).x
    if marker_name != "":
        draw_line(Vector2(lx, 22), Vector2(lx, 31), Color.WHITE, 2.0)
        draw_string(font, Vector2(lx + 5, 31), marker_name, HORIZONTAL_ALIGNMENT_LEFT, -1, 10, UiTheme.TEXT_PRIMARY)
    var shared_scale := _scale()
    for r_index in range(rows.size()):
        var r: Dictionary = rows[r_index]
        var y := TOP + ROW_HEIGHT * r_index
        var total := _row_total(r)
        var scale := total if normalized else shared_scale
        draw_string(font, Vector2(8, y + 12), String(r.get("name", "")), HORIZONTAL_ALIGNMENT_LEFT, LABEL_WIDTH - 14.0, 11,
            Color.WHITE if r_index == _hover_row else UiTheme.TEXT_PRIMARY)
        var x := LABEL_WIDTH
        var values: Array = r.get("values", [])
        for i in range(mini(values.size(), parts.size())):
            var w := _bar_width() * maxf(0.0, float(values[i])) / maxf(scale, 1e-9)
            var color: Color = parts[i].get("color", UiTheme.ACCENT)
            if _hover_row == r_index and _hover_part != -1 and _hover_part != i:
                color = color.darkened(0.35)
            draw_rect(Rect2(x, y + 2, w, ROW_HEIGHT - 5), color)
            x += w
        var marker := float(r.get("marker", -1.0))
        if marker >= 0.0 and not normalized:
            var mx := LABEL_WIDTH + _bar_width() * marker / shared_scale
            draw_line(Vector2(mx, y), Vector2(mx, y + ROW_HEIGHT - 2), Color.WHITE, 2.0)
        draw_string(font, Vector2(size.x - RIGHT + 4, y + 12), value_format % total, HORIZONTAL_ALIGNMENT_RIGHT, RIGHT - 8, 11,
            UiTheme.TEXT_DIM)
    # Read-out of the hovered part.
    if _hover_row != -1 and _hover_part != -1:
        var r: Dictionary = rows[_hover_row]
        var value := float(r.get("values", [])[_hover_part])
        var total := _row_total(r)
        var text := "%s · %s: %s (%.0f%%)" % [String(r.get("name", "")), String(parts[_hover_part].get("name", "")),
            value_format % value, 100.0 * value / maxf(total, 1e-9)]
        if float(r.get("marker", -1.0)) >= 0.0 and marker_name != "":
            text += " · %s %s" % [marker_name, value_format % float(r.get("marker", 0.0))]
        draw_string(font, Vector2(8, size.y - 4), text, HORIZONTAL_ALIGNMENT_LEFT, size.x - 16.0, 10, UiTheme.TEXT_PRIMARY)
