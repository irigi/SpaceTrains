extends PanelContainer

const UiTheme := preload("res://scripts/ui/UiTheme.gd")

var _grid: GridContainer
var _title: Label
var _summary: Label

func _ready() -> void:
    visible = false
    add_theme_stylebox_override("panel", UiTheme.panel_style(UiTheme.PANEL_BG_SOLID, UiTheme.ACCENT_DIM))
    var box := VBoxContainer.new()
    box.add_theme_constant_override("separation", 8)
    add_child(box)
    _title = Label.new()
    _title.text = "ECONOMY & MARKETS — price / stock   (M to close)"
    _title.add_theme_color_override("font_color", UiTheme.ACCENT)
    _title.add_theme_font_size_override("font_size", 14)
    box.add_child(_title)
    _summary = Label.new()
    _summary.add_theme_color_override("font_color", UiTheme.TEXT_PRIMARY)
    _summary.add_theme_font_size_override("font_size", 12)
    box.add_child(_summary)
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
    _summary.text = _economy_summary(state, commodities, stations)

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

# Unmet demand, money, exports, fleet, and the station goods that go shortest.
func _economy_summary(state: Dictionary, commodities: Array, stations: Array) -> String:
    var economy: Dictionary = state.get("economy", {})
    var demand := float(economy.get("demand_value", 0.0))
    var unmet := float(economy.get("unmet_value", 0.0))
    var lines: Array[String] = []
    var unmet_text := "Unmet demand: %d%% since start" % int(round(100.0 * unmet / demand)) if demand > 0.0 else "Unmet demand: —"
    if state.has("unmet_30d"):
        unmet_text += ", %d%% over the last 30 days" % int(round(100.0 * float(state["unmet_30d"])))
    lines.append(unmet_text)
    var target := float(economy.get("money_supply_target", 0.0))
    var money := float(state.get("total_credits", 0.0))
    var treasuries := 0.0
    for value in (state.get("faction_treasuries", {}) as Dictionary).values():
        treasuries += float(value)
    lines.append("Money in stations and ships %s (%+.1f%% of target) · treasuries %s · exported to Earth's economy %s" % [
        UiTheme.format_credits(money), 100.0 * (money / target - 1.0) if target > 0.0 else 0.0,
        UiTheme.format_credits(treasuries), UiTheme.format_credits(float(economy.get("exports_value", 0.0)))])
    var ships: Array = state.get("ships", [])
    var laid_up := 0
    for ship in ships:
        if String(ship.get("phase", "")) == "laid_up":
            laid_up += 1
    lines.append("Fleet %d ships (%d bought, %d sold since start, %d laid up)" % [
        ships.size(), int(economy.get("ships_commissioned", 0)), int(economy.get("ships_sold", 0)), laid_up])
    # Worst-supplied goods, by value gone short since the start.
    var base_prices := {}
    for commodity in commodities:
        base_prices[String(commodity.get("id", ""))] = float(commodity.get("base_price", 0.0))
    var shortages: Array = []
    for station in stations:
        var demand_units: Dictionary = station.get("demand_units", {})
        var unmet_units: Dictionary = station.get("unmet_units", {})
        for commodity_id in unmet_units.keys():
            var asked := float(demand_units.get(commodity_id, 0.0))
            if asked <= 0.0:
                continue
            var short := float(unmet_units[commodity_id])
            shortages.append({"text": "%s %s %d%%" % [String(station.get("name", "")), commodity_id, int(round(100.0 * short / asked))],
                "value": short * float(base_prices.get(commodity_id, 0.0))})
    shortages.sort_custom(func(a, b): return float(a["value"]) > float(b["value"]))
    var worst: Array[String] = []
    for i in range(mini(5, shortages.size())):
        worst.append(String(shortages[i]["text"]))
    if not worst.is_empty():
        lines.append("Shortest supplied: " + " · ".join(worst))
    return "\n".join(lines)
