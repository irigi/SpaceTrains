extends PanelContainer

const UiTheme := preload("res://scripts/ui/UiTheme.gd")

var _grid: GridContainer
var _title: Label

func _ready() -> void:
    visible = false
    add_theme_stylebox_override("panel", UiTheme.panel_style(UiTheme.PANEL_BG_SOLID, UiTheme.ACCENT_DIM))
    var box := VBoxContainer.new()
    box.add_theme_constant_override("separation", 8)
    add_child(box)
    _title = Label.new()
    _title.text = "MARKET OVERVIEW — price / stock   (M to close)"
    _title.add_theme_color_override("font_color", UiTheme.ACCENT)
    _title.add_theme_font_size_override("font_size", 14)
    box.add_child(_title)
    _grid = GridContainer.new()
    _grid.add_theme_constant_override("h_separation", 6)
    _grid.add_theme_constant_override("v_separation", 3)
    box.add_child(_grid)

func toggle() -> void:
    visible = not visible

func _cell(text: String, color: Color, min_width: float, bg: Color = Color.TRANSPARENT) -> Control:
    var label := Label.new()
    label.text = text
    label.custom_minimum_size.x = min_width
    label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
    label.add_theme_font_size_override("font_size", 11)
    label.add_theme_color_override("font_color", color)
    if bg.a > 0.0:
        var wrapper := PanelContainer.new()
        wrapper.add_theme_stylebox_override("panel", UiTheme.flat_style(bg))
        wrapper.add_child(label)
        return wrapper
    return label

func update_market(state: Dictionary) -> void:
    if not visible:
        return
    for child in _grid.get_children():
        child.queue_free()

    var commodities: Array = state.get("commodities", [])
    var stations: Array = state.get("stations", [])
    if commodities.is_empty() or stations.is_empty():
        return

    _grid.columns = stations.size() + 1
    _grid.add_child(_cell("", UiTheme.TEXT_DIM, 90.0))
    for station in stations:
        var header := _cell(String(station.get("name", "")).left(14), UiTheme.TEXT_PRIMARY, 92.0)
        _grid.add_child(header)

    for commodity in commodities:
        var commodity_id := String(commodity.get("id", ""))
        var base_price := float(commodity.get("base_price", 1.0))
        _grid.add_child(_cell(commodity_id, UiTheme.TEXT_DIM, 90.0))
        for station in stations:
            var prices: Dictionary = station.get("prices", {})
            var inventory: Dictionary = station.get("inventory", {})
            var price := float(prices.get(commodity_id, 0.0))
            var stock := float(inventory.get(commodity_id, 0.0))
            # Tint by deviation from base price: red = scarce/expensive, green = glut/cheap.
            var deviation: float = clamp(log(max(price / max(base_price, 0.001), 0.01)) / log(4.0), -1.0, 1.0)
            var bg := Color(0.5 + 0.5 * deviation, 0.5 - 0.35 * abs(deviation) + 0.4 * (-deviation if deviation < 0.0 else 0.0), 0.25, 0.22 + 0.3 * abs(deviation))
            _grid.add_child(_cell("%.0f / %.0f" % [price, stock], UiTheme.TEXT_PRIMARY, 92.0, bg))
