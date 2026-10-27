extends Control

# A donut of parts of a whole with a legend beside it (values and shares). Hovering a slice or
# its legend row highlights both. For one entity's composition; to compare several entities use
# StackedBar.

const UiTheme := preload("res://scripts/ui/UiTheme.gd")

var title := ""
# [{name: String, value: float, color: Color}]
var slices: Array = []
var value_format := "%.0f"   # how a slice's value is written in the legend
var center_text := ""        # in the hole (the total when empty)
var _hover := -1

const LEGEND_ROW := 15.0

func _init() -> void:
    custom_minimum_size = Vector2(300, 140)
    mouse_filter = Control.MOUSE_FILTER_STOP

func set_data(chart_title: String, chart_slices: Array, format := "%.0f", center := "") -> void:
    title = chart_title
    slices = []
    for s in chart_slices:
        if float(s.get("value", 0.0)) > 0.0:
            slices.append(s)
    value_format = format
    center_text = center
    custom_minimum_size.y = maxf(140.0, 26.0 + LEGEND_ROW * slices.size())
    queue_redraw()

func _total() -> float:
    var total := 0.0
    for s in slices:
        total += float(s.get("value", 0.0))
    return total

func _geometry() -> Dictionary:
    var top := 20.0 if title != "" else 4.0
    var radius := minf(56.0, (size.y - top - 6.0) * 0.5)
    return {"center": Vector2(8.0 + radius, top + radius), "radius": radius, "inner": radius * 0.58,
        "legend_x": 22.0 + 2.0 * radius, "top": top}

func _slice_at(pos: Vector2) -> int:
    var g := _geometry()
    var total := _total()
    if total <= 0.0:
        return -1
    var offset: Vector2 = pos - g.center
    var distance := offset.length()
    if distance >= g.inner and distance <= g.radius:
        var angle := fposmod(offset.angle() + PI * 0.5, TAU)
        var start := 0.0
        for i in range(slices.size()):
            var sweep := TAU * float(slices[i].get("value", 0.0)) / total
            if angle >= start and angle < start + sweep:
                return i
            start += sweep
    if pos.x >= g.legend_x:
        var row := int((pos.y - g.top) / LEGEND_ROW)
        if row >= 0 and row < slices.size():
            return row
    return -1

func _gui_input(event: InputEvent) -> void:
    if event is InputEventMouseMotion:
        var hit := _slice_at(event.position)
        if hit != _hover:
            _hover = hit
            queue_redraw()

func _notification(what: int) -> void:
    if what == NOTIFICATION_MOUSE_EXIT and _hover != -1:
        _hover = -1
        queue_redraw()

func _draw() -> void:
    var font := get_theme_default_font()
    if title != "":
        draw_string(font, Vector2(4, 13), title, HORIZONTAL_ALIGNMENT_LEFT, -1, 12, UiTheme.ACCENT)
    var g := _geometry()
    var total := _total()
    if total <= 0.0:
        draw_string(font, Vector2(4, g.top + 14), "nothing to show", HORIZONTAL_ALIGNMENT_LEFT, -1, 11, UiTheme.TEXT_DIM)
        return
    var start := -PI * 0.5
    for i in range(slices.size()):
        var s: Dictionary = slices[i]
        var sweep := TAU * float(s.get("value", 0.0)) / total
        var color: Color = s.get("color", UiTheme.ACCENT)
        var outer: float = g.radius + (3.0 if i == _hover else 0.0)
        if _hover != -1 and i != _hover:
            color = color.darkened(0.35)
        var steps := maxi(2, int(ceil(sweep / (TAU / 96.0))))
        var points := PackedVector2Array()
        for k in range(steps + 1):
            points.append(g.center + Vector2.from_angle(start + sweep * k / steps) * outer)
        for k in range(steps, -1, -1):
            points.append(g.center + Vector2.from_angle(start + sweep * k / steps) * g.inner)
        if points.size() >= 3:
            draw_colored_polygon(points, color)
        start += sweep
    var middle := center_text if center_text != "" else value_format % total
    if _hover != -1:
        middle = "%.0f%%" % (100.0 * float(slices[_hover].get("value", 0.0)) / total)
    var width := font.get_string_size(middle, HORIZONTAL_ALIGNMENT_LEFT, -1, 12).x
    draw_string(font, g.center + Vector2(-width * 0.5, 4), middle, HORIZONTAL_ALIGNMENT_LEFT, -1, 12, UiTheme.TEXT_PRIMARY)
    # Legend: swatch, name, value, share.
    var x: float = g.legend_x
    var legend_width := size.x - x - 4.0
    for i in range(slices.size()):
        var s: Dictionary = slices[i]
        var y: float = g.top + LEGEND_ROW * i
        var text_color := Color.WHITE if i == _hover else UiTheme.TEXT_PRIMARY
        draw_rect(Rect2(x, y + 3, 9, 9), s.get("color", UiTheme.ACCENT))
        draw_string(font, Vector2(x + 14, y + 12), String(s.get("name", "")), HORIZONTAL_ALIGNMENT_LEFT,
            legend_width - 120.0, 11, text_color)
        var value := float(s.get("value", 0.0))
        draw_string(font, Vector2(x + legend_width - 110.0, y + 12), value_format % value, HORIZONTAL_ALIGNMENT_RIGHT,
            66, 11, UiTheme.TEXT_DIM)
        draw_string(font, Vector2(x + legend_width - 40.0, y + 12), "%.0f%%" % (100.0 * value / total),
            HORIZONTAL_ALIGNMENT_RIGHT, 38, 11, text_color)
