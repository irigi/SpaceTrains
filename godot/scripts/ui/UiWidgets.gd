extends RefCounted

# Building blocks shared by the inspector and the statistics screen: labels, tables with column
# headers, gauges, a strip of key numbers, fold-out section headings and one credit format.
# Everything here only builds controls; the caller adds them to its container.

const UiTheme := preload("res://scripts/ui/UiTheme.gd")

static func label(text: String, color: Color = UiTheme.TEXT_PRIMARY, font_size: int = 12, width: float = 0.0,
        align := HORIZONTAL_ALIGNMENT_LEFT, wrap := false) -> Label:
    var l := Label.new()
    l.text = text
    l.add_theme_color_override("font_color", color)
    l.add_theme_font_size_override("font_size", font_size)
    l.horizontal_alignment = align
    if width > 0.0:
        l.custom_minimum_size.x = width
    if wrap:
        l.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
    return l

static func credits(amount: float) -> String:
    return UiTheme.format_credits(amount)

static func signed_credits(amount: float) -> String:
    return ("+" if amount > 0.5 else "") + UiTheme.format_credits(amount)

static func value_color(value: float) -> Color:
    return UiTheme.GOOD if value > 0.5 else (UiTheme.ALERT if value < -0.5 else UiTheme.TEXT_DIM)

# A table: header strings, rows of cells. A cell is a String, [text, color], [text, color, tooltip],
# [text, color, tooltip, width] (clipped to width, the full text in the tooltip) or a Control. The first column is left-aligned, the others right-aligned. widths: minimum width
# per column (missing: 170 for the first, 90 for the rest; 0 = as narrow as the text).
static func table(headers: Array, rows: Array, widths: Array = [], font_size: int = 11) -> GridContainer:
    var grid := GridContainer.new()
    grid.columns = headers.size()
    grid.add_theme_constant_override("h_separation", 10)
    grid.add_theme_constant_override("v_separation", 2)
    for i in range(headers.size()):
        var width: float = widths[i] if i < widths.size() else (170.0 if i == 0 else 90.0)
        grid.add_child(label(String(headers[i]), UiTheme.TEXT_DIM, font_size, width,
            HORIZONTAL_ALIGNMENT_LEFT if i == 0 else HORIZONTAL_ALIGNMENT_RIGHT))
    for row in rows:
        for i in range(headers.size()):
            var cell = row[i] if i < row.size() else ""
            if cell is Control:
                grid.add_child(cell)
                continue
            var text := String(cell[0]) if cell is Array else String(cell)
            var color: Color = cell[1] if cell is Array and cell.size() > 1 else UiTheme.TEXT_PRIMARY
            var l := label(text, color, font_size, 0.0, HORIZONTAL_ALIGNMENT_LEFT if i == 0 else HORIZONTAL_ALIGNMENT_RIGHT)
            if cell is Array and cell.size() > 2:
                l.tooltip_text = String(cell[2]) if String(cell[2]) != "" else text
                l.mouse_filter = Control.MOUSE_FILTER_PASS
            if cell is Array and cell.size() > 3:
                l.clip_text = true
                l.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
                l.custom_minimum_size.x = float(cell[3])
            grid.add_child(l)
    return grid

# A thin bar, full at max_value, for table cells (stock against target and the like).
static func bar(value: float, max_value: float, color: Color, width: float = 70.0, tooltip := "") -> ProgressBar:
    var b := ProgressBar.new()
    b.min_value = 0.0
    b.max_value = maxf(max_value, 0.001)
    b.value = clampf(value, 0.0, b.max_value)
    b.show_percentage = false
    b.custom_minimum_size = Vector2(width, 10)
    b.size_flags_vertical = Control.SIZE_SHRINK_CENTER
    b.add_theme_stylebox_override("fill", UiTheme.bar_fill_style(color))
    b.tooltip_text = tooltip
    return b

# Title, a bar and its value text, in one row.
static func gauge(title: String, value: float, max_value: float, suffix: String, color: Color) -> HBoxContainer:
    var row := HBoxContainer.new()
    row.add_theme_constant_override("separation", 8)
    row.add_child(label(title, UiTheme.TEXT_DIM, 12, 78.0))
    var b := bar(value, max_value, color, 0.0)
    b.custom_minimum_size.y = 14
    b.size_flags_horizontal = Control.SIZE_EXPAND_FILL
    row.add_child(b)
    row.add_child(label(suffix, UiTheme.TEXT_PRIMARY, 11, 96.0, HORIZONTAL_ALIGNMENT_RIGHT))
    return row

# Key numbers side by side: [{label, value, color?, tooltip?}], each a small tile.
static func kpi_strip(items: Array) -> HBoxContainer:
    var strip := HBoxContainer.new()
    strip.add_theme_constant_override("separation", 4)
    for item in items:
        var tile := PanelContainer.new()
        var style := UiTheme.panel_style(Color(0.08, 0.12, 0.18, 0.85), Color(0.3, 0.45, 0.55, 0.35))
        style.set_content_margin_all(4)
        style.content_margin_left = 6.0
        tile.add_theme_stylebox_override("panel", style)
        tile.size_flags_horizontal = Control.SIZE_EXPAND_FILL
        tile.tooltip_text = String(item.get("tooltip", ""))
        var box := VBoxContainer.new()
        box.add_theme_constant_override("separation", 0)
        box.mouse_filter = Control.MOUSE_FILTER_IGNORE
        tile.add_child(box)
        var value_label := label(String(item.get("value", "")), item.get("color", UiTheme.TEXT_PRIMARY), 14)
        value_label.clip_text = true
        value_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
        box.add_child(value_label)
        var name_label := label(String(item.get("label", "")), UiTheme.TEXT_DIM, 10)
        name_label.clip_text = true
        name_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
        box.add_child(name_label)
        strip.add_child(tile)
    return strip

# A section heading that folds: "▾ TITLE" when open, "▸ TITLE   summary" when folded.
static func section_header(title: String, open: bool, summary: String, on_pressed: Callable) -> Button:
    var button := Button.new()
    button.focus_mode = Control.FOCUS_NONE  # keys stay with the map (Space, shortcuts)
    button.flat = true
    button.alignment = HORIZONTAL_ALIGNMENT_LEFT
    button.text = ("▾ " if open else "▸ ") + title + ("" if open or summary == "" else "    " + summary)
    button.add_theme_color_override("font_color", UiTheme.ACCENT)
    button.add_theme_color_override("font_hover_color", Color.WHITE)
    button.add_theme_font_size_override("font_size", 13)
    button.clip_text = true
    button.pressed.connect(on_pressed)
    return button

static func separator() -> HSeparator:
    var sep := HSeparator.new()
    sep.add_theme_color_override("separator", Color(0.3, 0.45, 0.55, 0.4))
    return sep
