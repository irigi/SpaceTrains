extends PanelContainer

const UiTheme := preload("res://scripts/ui/UiTheme.gd")

var _content: VBoxContainer
var _scroll: ScrollContainer

func _ready() -> void:
    custom_minimum_size = Vector2(360, 0)
    _scroll = ScrollContainer.new()
    _scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
    add_child(_scroll)
    _content = VBoxContainer.new()
    _content.size_flags_horizontal = Control.SIZE_EXPAND_FILL
    _content.add_theme_constant_override("separation", 6)
    _scroll.add_child(_content)
    _show_help()

func _clear() -> void:
    for child in _content.get_children():
        child.queue_free()

func _add_label(text: String, color := UiTheme.TEXT_PRIMARY, font_size := 13) -> Label:
    var label := Label.new()
    label.text = text
    label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
    label.add_theme_color_override("font_color", color)
    if font_size != 13:
        label.add_theme_font_size_override("font_size", font_size)
    _content.add_child(label)
    return label

func _add_header(display_name: String, subtitle: String, faction_color: Color) -> void:
    var strip := ColorRect.new()
    strip.color = faction_color
    strip.custom_minimum_size = Vector2(0, 3)
    _content.add_child(strip)
    _add_label(display_name, UiTheme.TEXT_PRIMARY, 16)
    if subtitle != "":
        _add_label(subtitle, UiTheme.TEXT_DIM, 12)

func _add_separator() -> void:
    var sep := HSeparator.new()
    sep.add_theme_color_override("separator", Color(0.3, 0.45, 0.55, 0.4))
    _content.add_child(sep)

func _add_gauge(title: String, value: float, max_value: float, suffix: String, color: Color) -> void:
    var row := HBoxContainer.new()
    row.add_theme_constant_override("separation", 8)
    _content.add_child(row)
    var label := Label.new()
    label.text = title
    label.custom_minimum_size.x = 78.0
    label.add_theme_color_override("font_color", UiTheme.TEXT_DIM)
    label.add_theme_font_size_override("font_size", 12)
    row.add_child(label)
    var bar := ProgressBar.new()
    bar.min_value = 0.0
    bar.max_value = max(max_value, 0.001)
    bar.value = clamp(value, 0.0, bar.max_value)
    bar.show_percentage = false
    bar.custom_minimum_size = Vector2(0, 14)
    bar.size_flags_horizontal = Control.SIZE_EXPAND_FILL
    bar.size_flags_vertical = Control.SIZE_SHRINK_CENTER
    bar.add_theme_stylebox_override("fill", UiTheme.bar_fill_style(color))
    row.add_child(bar)
    var value_label := Label.new()
    value_label.text = suffix
    value_label.custom_minimum_size.x = 96.0
    value_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
    value_label.add_theme_font_size_override("font_size", 11)
    value_label.add_theme_color_override("font_color", UiTheme.TEXT_DIM)
    row.add_child(value_label)

func _show_help() -> void:
    _clear()
    _add_label("INSPECTOR", UiTheme.ACCENT, 14)
    _add_separator()
    _add_label("Click an entity in the scene or pick one from the registry.", UiTheme.TEXT_DIM)
    _add_label("RMB rotate · MMB pan · wheel zoom\nF focus · Space pause · , . warp\nM market overview", UiTheme.TEXT_DIM, 12)

# context keys: game_time_s, faction_colors (Dictionary), names (id -> display name),
# price_trends (commodity_id -> -1/0/1 for the selected station)
func update_selection(detail: Dictionary, kind: String, context: Dictionary) -> void:
    if detail.is_empty():
        _show_help()
        return
    _clear()
    match kind:
        "station":
            _build_station(detail, context)
        "ship":
            _build_ship(detail, context)
        _:
            _build_body(detail)

func _faction_color(detail: Dictionary, context: Dictionary) -> Color:
    var colors: Dictionary = context.get("faction_colors", {})
    return colors.get(String(detail.get("faction_id", "")), Color(0.5, 0.55, 0.6))

func _build_station(detail: Dictionary, context: Dictionary) -> void:
    var faction := String(detail.get("faction_id", ""))
    _add_header(
        String(detail.get("name", detail.get("id", ""))),
        "%s · pop %s" % [faction, str(detail.get("population", 0))],
        _faction_color(detail, context))

    var credits := float(detail.get("credits", 0.0))
    _add_label("Cash: " + UiTheme.format_credits(credits),
        UiTheme.CREDITS if credits >= 0.0 else UiTheme.ALERT)

    var capacity := float(detail.get("storage_capacity", 0.0))
    var used := float(detail.get("storage_used", 0.0))
    if capacity > 0.0:
        var ratio := used / capacity
        var bar_color := UiTheme.ACCENT
        if ratio > 0.9:
            bar_color = UiTheme.ALERT
        elif ratio > 0.75:
            bar_color = UiTheme.WARN
        _add_gauge("Storage", used, capacity, "%.0f / %.0f u" % [used, capacity], bar_color)

    _add_separator()
    _add_label("MARKET", UiTheme.ACCENT, 12)

    var inventory: Dictionary = detail.get("inventory", {})
    var prices: Dictionary = detail.get("prices", {})
    var rates: Dictionary = detail.get("net_rates", {})
    var trends: Dictionary = context.get("price_trends", {})
    var max_stock := 1.0
    for commodity_id in inventory.keys():
        max_stock = max(max_stock, float(inventory[commodity_id]))

    for commodity_id in inventory.keys():
        var stock := float(inventory[commodity_id])
        var rate := float(rates.get(commodity_id, 0.0))
        if stock < 0.1 and rate == 0.0:
            continue
        var row := HBoxContainer.new()
        row.add_theme_constant_override("separation", 6)
        _content.add_child(row)

        var name_label := Label.new()
        name_label.text = String(commodity_id)
        name_label.custom_minimum_size.x = 86.0
        name_label.add_theme_font_size_override("font_size", 12)
        row.add_child(name_label)

        var bar := ProgressBar.new()
        bar.min_value = 0.0
        bar.max_value = max_stock
        bar.value = stock
        bar.show_percentage = false
        bar.custom_minimum_size = Vector2(0, 12)
        bar.size_flags_horizontal = Control.SIZE_EXPAND_FILL
        bar.size_flags_vertical = Control.SIZE_SHRINK_CENTER
        var days_left: float = stock / absf(rate) if rate < 0.0 else INF
        var bar_color := UiTheme.ACCENT
        if days_left < 7.0:
            bar_color = UiTheme.ALERT
        elif days_left < 14.0:
            bar_color = UiTheme.WARN
        bar.add_theme_stylebox_override("fill", UiTheme.bar_fill_style(bar_color))
        row.add_child(bar)

        var stock_label := Label.new()
        stock_label.text = "%.0f" % stock
        stock_label.custom_minimum_size.x = 36.0
        stock_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
        stock_label.add_theme_font_size_override("font_size", 11)
        stock_label.add_theme_color_override("font_color", UiTheme.TEXT_DIM)
        row.add_child(stock_label)

        var trend := int(trends.get(commodity_id, 0))
        var trend_glyph := "▲" if trend > 0 else ("▼" if trend < 0 else "·")
        var trend_color := UiTheme.ALERT if trend > 0 else (UiTheme.GOOD if trend < 0 else UiTheme.TEXT_DIM)
        var price_label := Label.new()
        price_label.text = "%.0f %s" % [float(prices.get(commodity_id, 0.0)), trend_glyph]
        price_label.custom_minimum_size.x = 52.0
        price_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
        price_label.add_theme_font_size_override("font_size", 11)
        price_label.add_theme_color_override("font_color", trend_color)
        row.add_child(price_label)

        var rate_label := Label.new()
        rate_label.text = "%+.1f/d" % rate if rate != 0.0 else ""
        rate_label.custom_minimum_size.x = 50.0
        rate_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
        rate_label.add_theme_font_size_override("font_size", 11)
        rate_label.add_theme_color_override(
            "font_color", UiTheme.GOOD if rate > 0.0 else (UiTheme.WARN if rate < 0.0 else UiTheme.TEXT_DIM))
        row.add_child(rate_label)

func _resolve_name(context: Dictionary, entity_id: String) -> String:
    if entity_id == "":
        return "—"
    var names: Dictionary = context.get("names", {})
    return String(names.get(entity_id, entity_id))

func _trajectory_label(trajectory_type: String) -> String:
    match trajectory_type:
        "keplerian_local":
            return "Keplerian — local"
        "keplerian_lambert":
            return "Keplerian — Lambert arc"
        "keplerian_hohmann":
            return "Keplerian — Hohmann"
        "variable_isp":
            return "Variable-ISP (plasma)"
        _:
            return "—"

func _build_ship(detail: Dictionary, context: Dictionary) -> void:
    var propulsion := String(detail.get("propulsion_type", ""))
    var phase := String(detail.get("phase", "idle"))
    _add_header(
        String(detail.get("name", detail.get("id", ""))),
        "%s · %s · %s" % [String(detail.get("class_id", "ship")), propulsion, phase],
        _faction_color(detail, context))

    var propellant := float(detail.get("propellant_kg", 0.0))
    var propellant_capacity := float(detail.get("propellant_capacity_kg", 1.0))
    var fuel_ratio: float = propellant / maxf(propellant_capacity, 0.001)
    var fuel_color := UiTheme.ACCENT
    if fuel_ratio < 0.15:
        fuel_color = UiTheme.ALERT
    elif fuel_ratio < 0.4:
        fuel_color = UiTheme.WARN
    _add_gauge("Propellant", propellant, propellant_capacity,
        "%.0f / %.0f kg" % [propellant, propellant_capacity], fuel_color)

    var cargo := float(detail.get("cargo_units", 0.0))
    var cargo_capacity := float(detail.get("cargo_capacity_units", 0.0))
    if cargo_capacity > 0.0:
        _add_gauge("Cargo", cargo, cargo_capacity, "%.1f / %.0f u" % [cargo, cargo_capacity], UiTheme.GOOD)

    var credits := float(detail.get("credits", 0.0))
    var profit := float(detail.get("lifetime_profit", 0.0))
    _add_label("Wallet: %s   P&L: %+.0f cr" % [UiTheme.format_credits(credits), profit],
        UiTheme.CREDITS if credits >= 0.0 else UiTheme.ALERT)

    if phase == "in_transit" or phase == "awaiting_departure":
        _add_separator()
        _add_label("MISSION", UiTheme.ACCENT, 12)
        var origin := _resolve_name(context, String(detail.get("origin_station_id", "")))
        var destination := _resolve_name(context, String(detail.get("destination_station_id", "")))
        _add_label("%s → %s" % [origin, destination])
        _add_label(_trajectory_label(String(detail.get("trajectory_type", ""))), UiTheme.TEXT_DIM, 12)

        var commodity := String(detail.get("commodity_id", ""))
        if commodity != "" and cargo > 0.0:
            _add_label("Hauling %.1fu %s" % [cargo, commodity], UiTheme.TEXT_DIM, 12)
        var mission_value := float(detail.get("mission_value", 0.0))
        if abs(mission_value) > 0.5:
            _add_label("Expected margin: %+.0f cr" % mission_value,
                UiTheme.GOOD if mission_value > 0.0 else UiTheme.WARN, 12)

        var game_time_s := float(context.get("game_time_s", 0.0))
        var departure_s := float(detail.get("departure_time_s", 0.0))
        var arrival_s := float(detail.get("arrival_time_s", 0.0))
        if phase == "awaiting_departure":
            _add_label("Departure in %.1f d · ETA %.1f d" % [
                max(departure_s - game_time_s, 0.0) / 86400.0,
                max(arrival_s - game_time_s, 0.0) / 86400.0], UiTheme.TEXT_DIM, 12)
        else:
            var total_s: float = max(arrival_s - departure_s, 1.0)
            var progress: float = clamp((game_time_s - departure_s) / total_s, 0.0, 1.0)
            _add_gauge("Transit", progress, 1.0,
                "ETA %.1f d" % (max(arrival_s - game_time_s, 0.0) / 86400.0), UiTheme.ACCENT)
    elif String(detail.get("current_station_id", "")) != "":
        _add_label("Docked at %s" % _resolve_name(context, String(detail.get("current_station_id", ""))),
            UiTheme.TEXT_DIM, 12)

func _build_body(detail: Dictionary) -> void:
    var strip := ColorRect.new()
    strip.color = Color(0.4, 0.5, 0.6)
    strip.custom_minimum_size = Vector2(0, 3)
    _content.add_child(strip)
    _add_label(String(detail.get("name", detail.get("id", ""))), UiTheme.TEXT_PRIMARY, 16)
    _add_label("Celestial body", UiTheme.TEXT_DIM, 12)
    _add_label("Radius: %.0f km" % (float(detail.get("radius_m", 0.0)) / 1000.0), UiTheme.TEXT_DIM)
