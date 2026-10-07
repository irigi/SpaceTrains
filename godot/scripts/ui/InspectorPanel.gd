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
    _add_label("RMB rotate · MMB pan · wheel zoom\nF focus · Space pause · , . warp\nM economy and markets\nF5 quick save · F9 quick load", UiTheme.TEXT_DIM, 12)

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
        "%s · pop %s · orbits %s" % [faction, str(detail.get("population", 0)),
            _resolve_name(context, String(detail.get("parent_body_id", "")))],
        _faction_color(detail, context))

    var fuel_factory := float(detail.get("fuel_factory_per_day", 0.0))
    var exports: Array = detail.get("export_market", [])
    if fuel_factory > 0.0:
        _add_label("Fuel factory: %.0f u/day" % fuel_factory, UiTheme.GOOD, 12)
    if not exports.is_empty():
        _add_label("Export market for Earth's economy: %s" % ", ".join(exports), UiTheme.CREDITS, 12)

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
    _add_label("MARKET   stock vs target · price · rate · cover", UiTheme.ACCENT, 12)

    var inventory: Dictionary = detail.get("inventory", {})
    var prices: Dictionary = detail.get("prices", {})
    var rates: Dictionary = detail.get("net_rates", {})
    var targets: Dictionary = detail.get("target_stock", {})
    var demand: Dictionary = detail.get("demand_units", {})
    var unmet: Dictionary = detail.get("unmet_units", {})
    var trends: Dictionary = context.get("price_trends", {})

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
        name_label.custom_minimum_size.x = 80.0
        name_label.add_theme_font_size_override("font_size", 12)
        row.add_child(name_label)

        # The bar is full at the station's target stock (where the price is the base price).
        var target := float(targets.get(commodity_id, maxf(stock, 1.0)))
        var bar := ProgressBar.new()
        bar.min_value = 0.0
        bar.max_value = maxf(target, 1.0)
        bar.value = minf(stock, bar.max_value)
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
        bar.tooltip_text = "%.0f of target %.0f" % [stock, target]
        row.add_child(bar)

        var stock_label := Label.new()
        stock_label.text = "%.0f" % stock
        stock_label.custom_minimum_size.x = 38.0
        stock_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
        stock_label.add_theme_font_size_override("font_size", 11)
        stock_label.add_theme_color_override("font_color", UiTheme.TEXT_DIM)
        row.add_child(stock_label)

        var trend := int(trends.get(commodity_id, 0))
        var trend_glyph := "▲" if trend > 0 else ("▼" if trend < 0 else "·")
        var trend_color := UiTheme.ALERT if trend > 0 else (UiTheme.GOOD if trend < 0 else UiTheme.TEXT_DIM)
        var price_label := Label.new()
        price_label.text = "%.0f %s" % [float(prices.get(commodity_id, 0.0)), trend_glyph]
        price_label.custom_minimum_size.x = 50.0
        price_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
        price_label.add_theme_font_size_override("font_size", 11)
        price_label.add_theme_color_override("font_color", trend_color)
        row.add_child(price_label)

        var rate_label := Label.new()
        rate_label.text = "%+.1f/d" % rate if rate != 0.0 else ""
        rate_label.custom_minimum_size.x = 48.0
        rate_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
        rate_label.add_theme_font_size_override("font_size", 11)
        rate_label.add_theme_color_override(
            "font_color", UiTheme.GOOD if rate > 0.0 else (UiTheme.WARN if rate < 0.0 else UiTheme.TEXT_DIM))
        row.add_child(rate_label)

        # Consumers: days of stock left, and the share of demand that went short so far.
        var cover_label := Label.new()
        cover_label.custom_minimum_size.x = 64.0
        cover_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
        cover_label.add_theme_font_size_override("font_size", 11)
        if rate < 0.0:
            var asked := float(demand.get(commodity_id, 0.0))
            var short := float(unmet.get(commodity_id, 0.0)) / asked if asked > 0.0 else 0.0
            cover_label.text = ("%.0f d" % days_left if days_left < 999.0 else "")
            if short >= 0.01:
                cover_label.text += " %d%%✗" % int(round(100.0 * short))
            cover_label.add_theme_color_override("font_color", bar_color if short < 0.01 else UiTheme.ALERT)
        row.add_child(cover_label)

    _add_station_orders(detail, context)
    _add_station_ships(detail, context)

    var ledger: Dictionary = detail.get("ledger", {})
    if not ledger.is_empty():
        _add_separator()
        _add_label("MONEY SINCE START", UiTheme.ACCENT, 12)
        _add_label("Residents paid %s · producers paid %s" % [
            UiTheme.format_credits(float(ledger.get("household_sales", 0.0))),
            UiTheme.format_credits(float(ledger.get("producer_purchases", 0.0)))], UiTheme.TEXT_DIM, 12)
        _add_label("Ship dividends %s · subsidies %s · taxes %s" % [
            UiTheme.format_credits(float(ledger.get("dividends", 0.0))),
            UiTheme.format_credits(float(ledger.get("subsidies", 0.0))),
            UiTheme.format_credits(float(ledger.get("taxes", 0.0)))], UiTheme.TEXT_DIM, 12)

# What the station still wants of each good it consumes: up to its target stock (which
# covers its resupply time), less its stock and the cargo already on the way. These are the
# prices ships see: the bigger the gap, the higher the station bids.
func _add_station_orders(detail: Dictionary, context: Dictionary) -> void:
    var station_id := String(detail.get("id", ""))
    var inventory: Dictionary = detail.get("inventory", {})
    var targets: Dictionary = detail.get("target_stock", {})
    var rates: Dictionary = detail.get("net_rates", {})
    var prices: Dictionary = detail.get("prices", {})
    var inbound := {}
    for ship in context.get("ships", []):
        var phase := String(ship.get("phase", ""))
        if (phase == "in_transit" or phase == "awaiting_departure") and String(ship.get("destination_station_id", "")) == station_id:
            for lot in ship.get("cargo", []):
                var id := String(lot.get("commodity_id", ""))
                inbound[id] = float(inbound.get(id, 0.0)) + float(lot.get("units", 0.0))
    var orders: Array = []
    for commodity_id in targets.keys():
        if float(rates.get(commodity_id, 0.0)) >= 0.0:
            continue
        var stock := float(inventory.get(commodity_id, 0.0))
        var coming := float(inbound.get(commodity_id, 0.0))
        var wanted := float(targets[commodity_id]) - stock - coming
        if wanted >= 1.0:
            orders.append({"id": commodity_id, "wanted": wanted, "coming": coming, "price": float(prices.get(commodity_id, 0.0))})
    if orders.is_empty():
        return
    orders.sort_custom(func(a, b): return float(a["wanted"]) * float(a["price"]) > float(b["wanted"]) * float(b["price"]))
    _add_separator()
    _add_label("ORDERS   wanted up to target · on the way · paying now", UiTheme.ACCENT, 12)
    for order in orders:
        _add_label("%s: %.0f u wanted · %.0f u on the way · %.0f cr/u" % [order["id"], order["wanted"], order["coming"], order["price"]],
            UiTheme.TEXT_DIM, 12)

# Ships on their way here (with cargo and arrival) and ships docked here.
func _add_station_ships(detail: Dictionary, context: Dictionary) -> void:
    var station_id := String(detail.get("id", ""))
    var game_time_s := float(context.get("game_time_s", 0.0))
    var inbound: Array = []
    var docked: Array = []
    for ship in context.get("ships", []):
        var phase := String(ship.get("phase", ""))
        if (phase == "in_transit" or phase == "awaiting_departure") and String(ship.get("destination_station_id", "")) == station_id:
            inbound.append(ship)
        elif phase != "in_transit" and String(ship.get("current_station_id", "")) == station_id:
            docked.append(ship)
    if inbound.is_empty() and docked.is_empty():
        return
    _add_separator()
    if not inbound.is_empty():
        inbound.sort_custom(func(a, b): return float(a.get("arrival_time_s", 0.0)) < float(b.get("arrival_time_s", 0.0)))
        _add_label("INBOUND", UiTheme.ACCENT, 12)
        for ship in inbound:
            var what := _cargo_text(ship)
            if what == "":
                what = "empty"
            _add_label("%s · %s · in %.0f d" % [String(ship.get("name", "")), what,
                maxf(float(ship.get("arrival_time_s", 0.0)) - game_time_s, 0.0) / 86400.0], UiTheme.TEXT_DIM, 12)
    if not docked.is_empty():
        _add_label("DOCKED", UiTheme.ACCENT, 12)
        var names: Array[String] = []
        for ship in docked:
            names.append("%s (%s)" % [String(ship.get("name", "")), _phase_word(String(ship.get("phase", "")))])
        _add_label(", ".join(names), UiTheme.TEXT_DIM, 12)

func _phase_word(phase: String) -> String:
    match phase:
        "idle":
            return "looking for work"
        "awaiting_departure":
            return "waiting for its window"
        "laid_up":
            return "laid up"
        "refitting":
            return "in the yard"
        "stranded":
            return "stranded"
        "refueling":
            return "refuelling"
    return phase

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
        "keplerian_planet_system":
            return "Keplerian — transfer within a planet's system"
        "variable_isp", "variable_isp/refined":
            return "Plasma drive — continuous thrust"
        "variable_isp_planet_system":
            return "Plasma drive — spiral within a planet's system"
        _:
            return "—"

# "230 u food + 140 u medicine" from the snapshot's cargo list (largest lot first).
func _cargo_text(detail: Dictionary) -> String:
    var lots: Array = (detail.get("cargo", []) as Array).duplicate()
    if lots.is_empty():
        var units := float(detail.get("cargo_units", 0.0))
        var commodity := String(detail.get("commodity_id", ""))
        return "%.0f u %s" % [units, commodity] if units > 0.0 and commodity != "" else ""
    lots.sort_custom(func(a, b): return float(a.get("units", 0.0)) > float(b.get("units", 0.0)))
    var parts: Array[String] = []
    for lot in lots:
        parts.append("%.0f u %s" % [float(lot.get("units", 0.0)), String(lot.get("commodity_id", ""))])
    return " + ".join(parts)

# What the ship is doing, in words: "Carrying 120 u platinum to Earth L1 Terminal".
func _mission_sentence(detail: Dictionary, context: Dictionary) -> String:
    var phase := String(detail.get("phase", "idle"))
    var game_time_s := float(context.get("game_time_s", 0.0))
    var here := _resolve_name(context, String(detail.get("current_station_id", "")))
    var destination := _resolve_name(context, String(detail.get("destination_station_id", "")))
    var load := _cargo_text(detail)
    match phase:
        "in_transit", "awaiting_departure":
            var text := ""
            if load != "":
                text = "Carrying %s to %s to sell" % [load, destination]
            elif String(detail.get("pickup_commodity_id", "")) != "":
                text = "Flying empty to %s" % destination
            else:
                text = "Repositioning to %s" % destination
            var pickup := String(detail.get("pickup_commodity_id", ""))
            var pickup_units := float(detail.get("pickup_units", 0.0))
            if pickup != "" and pickup_units > 0.0:
                text += ", then load %.0f u %s there" % [pickup_units, pickup]
            if phase == "awaiting_departure":
                text = "Waiting %.0f d for the launch window. %s" % [
                    maxf(float(detail.get("departure_time_s", 0.0)) - game_time_s, 0.0) / 86400.0, text]
            return text + "."
        "idle", "refueling":
            return "Docked at %s, looking for work." % here
        "laid_up":
            return "Laid up at %s: no paying work, crew discharged." % here
        "refitting":
            return "In the yard at %s for new tanks, %.0f d to go." % [here,
                maxf(float(detail.get("refit_done_s", 0.0)) - game_time_s, 0.0) / 86400.0]
        "stranded":
            return "Stranded at %s: not enough fuel for sale to leave." % here
    return phase

func _build_ship(detail: Dictionary, context: Dictionary) -> void:
    var phase := String(detail.get("phase", "idle"))
    var game_time_s := float(context.get("game_time_s", 0.0))
    _add_header(
        String(detail.get("name", detail.get("id", ""))),
        "%s · %s" % [String(detail.get("class_name", detail.get("class_id", "ship"))), String(detail.get("faction_id", ""))],
        _faction_color(detail, context))
    _add_label(_mission_sentence(detail, context), UiTheme.TEXT_PRIMARY, 13)

    var propellant := float(detail.get("propellant_kg", 0.0))
    var propellant_capacity := float(detail.get("propellant_capacity_kg", 1.0))
    var fuel_ratio: float = propellant / maxf(propellant_capacity, 0.001)
    var fuel_color := UiTheme.ACCENT
    if fuel_ratio < 0.15:
        fuel_color = UiTheme.ALERT
    elif fuel_ratio < 0.4:
        fuel_color = UiTheme.WARN
    _add_gauge("Propellant", propellant, propellant_capacity,
        "%.1f / %.0f t (%d%%)" % [propellant / 1000.0, propellant_capacity / 1000.0, int(round(100.0 * fuel_ratio))], fuel_color)

    var cargo := float(detail.get("cargo_units", 0.0))
    var cargo_capacity := float(detail.get("cargo_capacity_units", 0.0))
    if cargo_capacity > 0.0:
        _add_gauge("Cargo", cargo, cargo_capacity, "%.0f / %.0f u" % [cargo, cargo_capacity], UiTheme.GOOD)

    var provision_days := float(detail.get("provision_days", 0.0))
    _add_gauge("Provisions", provision_days, 730.0, "%.0f days" % provision_days,
        UiTheme.ALERT if provision_days < 60.0 else UiTheme.ACCENT)

    if phase == "in_transit":
        var departure_s := float(detail.get("departure_time_s", 0.0))
        var arrival_s := float(detail.get("arrival_time_s", 0.0))
        var total_s: float = maxf(arrival_s - departure_s, 1.0)
        var progress: float = clampf((game_time_s - departure_s) / total_s, 0.0, 1.0)
        _add_gauge("Transit", progress, 1.0,
            "arrives in %.1f d" % (maxf(arrival_s - game_time_s, 0.0) / 86400.0), UiTheme.ACCENT)
        _add_label(_trajectory_label(String(detail.get("trajectory_type", ""))), UiTheme.TEXT_DIM, 12)

    var mission_value := float(detail.get("mission_value", 0.0))
    if (phase == "in_transit" or phase == "awaiting_departure") and absf(mission_value) > 0.5:
        _add_label("Trip expected to earn %+.0f cr (sale %s, cargo %s)" % [mission_value,
            UiTheme.format_credits(float(detail.get("expected_revenue", 0.0))),
            UiTheme.format_credits(float(detail.get("purchase_cost", 0.0)))],
            UiTheme.GOOD if mission_value > 0.0 else UiTheme.WARN, 12)

    _add_separator()
    _add_label("Crew %d · home %s" % [int(detail.get("crew_size", 0)),
        _resolve_name(context, String(detail.get("home_station_id", "")))], UiTheme.TEXT_DIM, 12)
    var route_until_s := float(detail.get("route_until_s", 0.0))
    if route_until_s > game_time_s and String(detail.get("route_destination_id", "")) != "":
        _add_label("Bought to carry %s to %s, %.0f more days" % [String(detail.get("route_commodity_id", "")),
            _resolve_name(context, String(detail.get("route_destination_id", ""))),
            (route_until_s - game_time_s) / 86400.0], UiTheme.TEXT_DIM, 12)

    var credits := float(detail.get("credits", 0.0))
    var profit := float(detail.get("lifetime_profit", 0.0))
    _add_label("Cash %s · lifetime profit %+.0f cr" % [UiTheme.format_credits(credits), profit],
        UiTheme.CREDITS if profit >= 0.0 else UiTheme.ALERT)
    var ledger: Dictionary = detail.get("ledger", {})
    if not ledger.is_empty():
        # Money in (+) and out (−) since the ship was commissioned; fuel can be net income
        # (a tanker sells fuel at the depots it serves).
        var parts: Array[String] = ["sales %+.0f" % float(ledger.get("cargo_revenue", 0.0))]
        for key in ["cargo_purchases", "fuel", "wages", "capital", "provisions", "refits"]:
            parts.append("%s %+.0f" % [String(key).replace("cargo_purchases", "cargo").replace("_", " "), -float(ledger.get(key, 0.0))])
        _add_label(" · ".join(parts) + " cr", UiTheme.TEXT_DIM, 11)

func _build_body(detail: Dictionary) -> void:
    var strip := ColorRect.new()
    strip.color = Color(0.4, 0.5, 0.6)
    strip.custom_minimum_size = Vector2(0, 3)
    _content.add_child(strip)
    _add_label(String(detail.get("name", detail.get("id", ""))), UiTheme.TEXT_PRIMARY, 16)
    _add_label("Celestial body", UiTheme.TEXT_DIM, 12)
    _add_label("Radius: %.0f km" % (float(detail.get("radius_m", 0.0)) / 1000.0), UiTheme.TEXT_DIM)
