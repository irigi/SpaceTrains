extends PanelContainer

# The inspector on the right: the selected ship, station or body. Ships and stations share one
# layout: a header, a strip of key numbers, alerts, then sections that fold (and stay folded
# across selections, per kind): tables with column headers instead of packed number strings.

const UiTheme := preload("res://scripts/ui/UiTheme.gd")
const UiWidgets := preload("res://scripts/ui/UiWidgets.gd")
const ShipSchematicScript := preload("res://scripts/ui/ShipSchematic.gd")
const DonutChartScript := preload("res://scripts/ui/DonutChart.gd")

# A ship or station named in a table was clicked.
signal entity_selected(entity_id: String, kind: String)

var _content: VBoxContainer
var _scroll: ScrollContainer
# Folded sections by "kind/section"; missing = the section's default.
var _folded: Dictionary = {}
var _parts_open := false
var _schematic: ShipSchematicScript
var _schematic_used := false
# The last selection, rebuilt when a section is folded or unfolded.
var _last_detail: Dictionary = {}
var _last_kind := ""
var _last_context: Dictionary = {}

# The wheel scrolls this panel only: once its list is at the end, the event would
# otherwise fall through to the camera and zoom the map behind the panel.
func _gui_input(event: InputEvent) -> void:
    if event is InputEventMouseButton and event.button_index in [MOUSE_BUTTON_WHEEL_UP, MOUSE_BUTTON_WHEEL_DOWN,
            MOUSE_BUTTON_WHEEL_LEFT, MOUSE_BUTTON_WHEEL_RIGHT]:
        accept_event()

func _ready() -> void:
    custom_minimum_size = Vector2(390, 0)
    _scroll = ScrollContainer.new()
    _scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
    add_child(_scroll)
    # A little room between the text and the scroll bar.
    var margin := MarginContainer.new()
    margin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
    margin.add_theme_constant_override("margin_right", 6)
    _scroll.add_child(margin)
    _content = VBoxContainer.new()
    _content.size_flags_horizontal = Control.SIZE_EXPAND_FILL
    _content.add_theme_constant_override("separation", 5)
    margin.add_child(_content)
    _show_help()

func _exit_tree() -> void:
    if _schematic != null and _schematic.get_parent() == null:
        _schematic.free()

func _clear() -> void:
    # The schematic stays in the tree across refreshes (removing it would drop its hover and
    # make the hovered part blink); it is detached only if the rebuilt view has no schematic.
    _schematic_used = false
    for child in _content.get_children():
        if child != _schematic:
            child.queue_free()
    if _schematic != null and _schematic.get_parent() == _content:
        _park_schematic.call_deferred()

func _park_schematic() -> void:
    if not _schematic_used and _schematic != null and _schematic.get_parent() == _content:
        _content.remove_child(_schematic)

# --- building blocks -----------------------------------------------------------

func _add(control: Control) -> Control:
    _content.add_child(control)
    return control

func _add_label(text: String, color := UiTheme.TEXT_PRIMARY, font_size := 12) -> Label:
    return _add(UiWidgets.label(text, color, font_size, 0.0, HORIZONTAL_ALIGNMENT_LEFT, true)) as Label

func _add_note(text: String) -> Label:
    return _add_label(text, UiTheme.TEXT_DIM, 11)

func _add_header(display_name: String, subtitle: String, faction_color: Color) -> void:
    var strip := ColorRect.new()
    strip.color = faction_color
    strip.custom_minimum_size = Vector2(0, 3)
    _add(strip)
    _add_label(display_name, UiTheme.TEXT_PRIMARY, 16)
    if subtitle != "":
        _add_label(subtitle, UiTheme.TEXT_DIM, 12)

func _add_gauge(title: String, value: float, max_value: float, suffix: String, color: Color) -> void:
    _add(UiWidgets.gauge(title, value, max_value, suffix, color))

# A section heading; returns whether the section is open (then the caller adds its body).
func _section(id: String, title: String, summary := "", open_by_default := true) -> bool:
    var key := "%s/%s" % [_last_kind, id]
    var open := not bool(_folded.get(key, not open_by_default))
    _add(UiWidgets.separator())
    _add(UiWidgets.section_header(title, open, summary, func() -> void:
        _folded[key] = open
        _rebuild_last()))
    return open

func _rebuild_last() -> void:
    _clear()
    _build(_last_detail, _last_kind, _last_context)

func _table(headers: Array, rows: Array, widths: Array) -> void:
    _add(UiWidgets.table(headers, rows, widths))

# A flat button naming an entity, for table cells: a click selects it.
func _entity_button(text: String, entity_id: String, kind: String, width: float) -> Button:
    var button := Button.new()
    button.focus_mode = Control.FOCUS_NONE  # keys stay with the map (Space, shortcuts)
    button.flat = true
    button.text = text
    button.clip_text = true
    button.alignment = HORIZONTAL_ALIGNMENT_LEFT
    button.custom_minimum_size.x = width
    button.add_theme_font_size_override("font_size", 11)
    button.add_theme_color_override("font_color", UiTheme.ACCENT)
    button.add_theme_constant_override("h_separation", 0)
    var empty := StyleBoxEmpty.new()
    for state in ["normal", "hover", "pressed"]:
        button.add_theme_stylebox_override(state, empty)
    button.pressed.connect(func() -> void: entity_selected.emit(entity_id, kind))
    return button

func _show_help() -> void:
    _clear()
    _add_label("INSPECTOR", UiTheme.ACCENT, 14)
    _add(UiWidgets.separator())
    _add_label("Click an entity in the scene or pick one from the registry.", UiTheme.TEXT_DIM)
    _add_label("RMB rotate · MMB pan · wheel zoom\nF focus · Space pause · , . warp\nM economy · S statistics · H keys\nF5 quick save · F9 quick load",
        UiTheme.TEXT_DIM, 12)

# A click on a button here takes a press and a release; a rebuild in between frees the button
# and the click is lost. While the mouse is held over the panel, refreshes wait (the next one
# comes half a second later).
func _pointer_held_here() -> bool:
    return Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT) and get_global_rect().has_point(get_global_mouse_position())

# context keys: game_time_s, faction_colors (Dictionary), names (id -> display name),
# price_trends (commodity_id -> -1/0/1 for the selected station), ships, bodies, stations, designs
func update_selection(detail: Dictionary, kind: String, context: Dictionary) -> void:
    if _pointer_held_here():
        return
    if detail.is_empty():
        _show_help()
        return
    _clear()
    _build(detail, kind, context)

func _build(detail: Dictionary, kind: String, context: Dictionary) -> void:
    _last_detail = detail
    _last_kind = kind
    _last_context = context
    match kind:
        "station":
            _build_station(detail, context)
        "ship":
            _build_ship(detail, context)
        _:
            _build_body(detail, context)

# --- words and numbers ---------------------------------------------------------

func _faction_color(detail: Dictionary, context: Dictionary) -> Color:
    var colors: Dictionary = context.get("faction_colors", {})
    return colors.get(String(detail.get("faction_id", "")), Color(0.5, 0.55, 0.6))

static func _faction_name(faction_id: String) -> String:
    match faction_id:
        "sol_fed":
            return "Sol Federation"
        "mars_corp":
            return "Mars Corporation"
        "independent":
            return "Independent"
    return faction_id.replace("_", " ").capitalize()

static func _good_name(good: String) -> String:
    return "reactor consumables" if good == "reactor_fuel" else good.replace("_", " ")

# Short form for narrow table columns.
static func _good_short(good: String) -> String:
    match good:
        "reactor_fuel":
            return "reactor fuel"
        "science_samples":
            return "samples"
        "electronics":
            return "electronics"
    return good.replace("_", " ")

static func _count(value: float) -> String:
    var n := int(round(value))
    var text := str(absi(n))
    var grouped := ""
    while text.length() > 3:
        grouped = "," + text.substr(text.length() - 3) + grouped
        text = text.substr(0, text.length() - 3)
    return ("-" if n < 0 else "") + text + grouped

static func _rate(value: float) -> String:
    return ("%.0f" if value >= 10.0 else ("%.1f" if value >= 1.0 else "%.2f")) % value

static func _days(days: float) -> String:
    return "%.1f d" % days if days < 10.0 else "%.0f d" % days

func _hours(h: float) -> String:
    return "%.0f" % h if h >= 100.0 else "%.1f" % h

func _resolve_name(context: Dictionary, entity_id: String) -> String:
    if entity_id == "":
        return "—"
    var names: Dictionary = context.get("names", {})
    return String(names.get(entity_id, entity_id))

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
        return "%.0f u %s" % [units, _good_name(commodity)] if units > 0.0 and commodity != "" else ""
    lots.sort_custom(func(a, b): return float(a.get("units", 0.0)) > float(b.get("units", 0.0)))
    var parts: Array[String] = []
    for lot in lots:
        parts.append("%.0f u %s" % [float(lot.get("units", 0.0)), _good_name(String(lot.get("commodity_id", "")))])
    return " + ".join(parts)

# --- station -------------------------------------------------------------------

func _build_station(detail: Dictionary, context: Dictionary) -> void:
    var faction := String(detail.get("faction_id", ""))
    var systems: Dictionary = detail.get("systems", {})
    var site := String(systems.get("site", "orbit"))
    var site_word := {"orbit": "orbital station", "surface": "surface base", "underground": "underground base",
        "aerostat": "aerostat"}
    _add_header(String(detail.get("name", detail.get("id", ""))),
        "%s · %s at %s" % [_faction_name(faction), String(site_word.get(site, site)),
            _resolve_name(context, String(detail.get("parent_body_id", "")))],
        _faction_color(detail, context))

    # Key numbers.
    var population := float(detail.get("population", 0.0))
    var seeded := float(detail.get("seeded_population", 0.0))
    var change := population / seeded - 1.0 if seeded > 0.0 else 0.0
    var credits := float(detail.get("credits", 0.0))
    var demand: Dictionary = detail.get("demand_units", {})
    var unmet: Dictionary = detail.get("unmet_units", {})
    var asked := 0.0
    var missed := 0.0
    for good in demand.keys():
        asked += float(demand[good])
        missed += float(unmet.get(good, 0.0))
    var short_share := missed / asked if asked > 0.0 else 0.0
    var capacity := float(detail.get("storage_capacity", 0.0))
    var used := float(detail.get("storage_used", 0.0))
    var fill := used / capacity if capacity > 0.0 else 0.0
    _add(UiWidgets.kpi_strip([
        {"label": "people %+.0f%%" % (100.0 * change), "value": _count(population),
            "tooltip": "Residents; change since founding (%s)" % _count(seeded)},
        {"label": "cash", "value": UiWidgets.credits(credits), "color": UiTheme.CREDITS if credits >= 0.0 else UiTheme.ALERT},
        {"label": "demand short", "value": "%.1f%%" % (100.0 * short_share),
            "color": UiTheme.GOOD if short_share < 0.01 else (UiTheme.WARN if short_share < 0.05 else UiTheme.ALERT),
            "tooltip": "Share of what the station consumed that found no stock, since the start (units)"},
        {"label": "storage", "value": "%.0f%%" % (100.0 * fill),
            "color": UiTheme.ALERT if fill > 0.9 else (UiTheme.WARN if fill > 0.75 else UiTheme.TEXT_PRIMARY),
            "tooltip": "%s of %s u" % [_count(used), _count(capacity)]},
    ]))
    _add_station_alerts(detail)

    _add_station_market(detail, context)
    _add_station_people(detail, context)
    _add_station_works(detail, context)
    _add_station_orders(detail)
    _add_station_ships(detail, context)
    _add_station_money(detail)

# Things that need attention now: events, emergencies, people leaving.
func _add_station_alerts(detail: Dictionary) -> void:
    var supply := float(detail.get("supply_index", 1.0))
    if float(detail.get("seeded_population", 0.0)) > 0.0 and supply < 0.95:
        _add_label("Supply %.0f%%: %s" % [100.0 * supply, "short, people leaving" if supply < 0.7 else "growth held"],
            UiTheme.ALERT if supply < 0.7 else UiTheme.WARN, 12)
    for event in detail.get("events", []):
        _add_label("⚑ %s (%.0f more days)" % [String(event.get("headline", "")), float(event.get("days_left", 0.0))],
            UiTheme.WARN, 12)
    for emergency in detail.get("emergencies", []):
        _add_label("EMERGENCY: %s running out · the faction pays %.1f× · %.0f u open · %.0f d" % [
            _good_name(String(emergency.get("commodity_id", ""))), float(emergency.get("premium", 0.0)),
            float(emergency.get("units_open", 0.0)), float(emergency.get("days", 0.0))], UiTheme.ALERT, 12)

# Stock against target, price and trend, net rate and cover, one row per good; production below.
func _add_station_market(detail: Dictionary, context: Dictionary) -> void:
    var inventory: Dictionary = detail.get("inventory", {})
    var prices: Dictionary = detail.get("prices", {})
    var rates: Dictionary = detail.get("net_rates", {})
    var targets: Dictionary = detail.get("target_stock", {})
    var demand: Dictionary = detail.get("demand_units", {})
    var unmet: Dictionary = detail.get("unmet_units", {})
    var trends: Dictionary = context.get("price_trends", {})
    var exports: Array = detail.get("export_market", [])
    var rows: Array = []
    var low := 0
    for commodity_id in inventory.keys():
        var stock := float(inventory[commodity_id])
        var rate := float(rates.get(commodity_id, 0.0))
        if stock < 0.1 and rate == 0.0:
            continue
        var target := float(targets.get(commodity_id, maxf(stock, 1.0)))
        var days_left: float = stock / absf(rate) if rate < 0.0 else INF
        var bar_color := UiTheme.ACCENT
        if days_left < 7.0 and not exports.has(commodity_id):
            bar_color = UiTheme.ALERT
            low += 1
        elif days_left < 14.0:
            bar_color = UiTheme.WARN
        var trend := int(trends.get(commodity_id, 0))
        var glyph := "▲" if trend > 0 else ("▼" if trend < 0 else " ")
        var cover := ["", UiTheme.TEXT_DIM]
        if exports.has(commodity_id):
            # Sold on to Earth's economy as it arrives: no stock is kept, nothing goes short.
            bar_color = UiTheme.CREDITS
            cover = ["export", UiTheme.CREDITS, "Export market: bought for Earth's economy as it arrives"]
        elif rate < 0.0:
            var want := float(demand.get(commodity_id, 0.0))
            var short := float(unmet.get(commodity_id, 0.0)) / want if want > 0.0 else 0.0
            var text := _days(days_left) if days_left < 999.0 else "> 999 d"
            if short >= 0.01:
                cover = [text + " !", UiTheme.ALERT, "%.0f%% of demand went short since the start" % (100.0 * short)]
            else:
                cover = [text, bar_color if days_left < 14.0 else UiTheme.TEXT_PRIMARY]
        rows.append([
            [_good_short(String(commodity_id)), UiTheme.TEXT_PRIMARY, _good_name(String(commodity_id)), 74.0],
            UiWidgets.bar(stock, target, bar_color, 46.0, "%.0f of target %.0f" % [stock, target]),
            ["%.0f" % stock, UiTheme.TEXT_PRIMARY, "target %.0f" % target],
            ["%.0f%s" % [float(prices.get(commodity_id, 0.0)), glyph],
                UiTheme.ALERT if trend > 0 else (UiTheme.GOOD if trend < 0 else UiTheme.TEXT_PRIMARY)],
            ["%+.1f" % rate if rate != 0.0 else "", UiTheme.GOOD if rate > 0.0 else UiTheme.WARN],
            cover,
        ])
    var summary := "%d goods" % rows.size() + (" · %d low" % low if low > 0 else "")
    if not _section("market", "MARKET", summary):
        return
    _table(["Good", "", "Stock", "Price", "/day", "Cover"], rows, [74.0, 46.0, 40.0, 46.0, 40.0, 50.0])
    _add_note("Bar: stock against the target stock (where the price is the base price). Cover: days of stock at the net rate.")

    var fuel_factory := float(detail.get("fuel_factory_per_day", 0.0))
    if fuel_factory > 0.0:
        _add_label("Fuel factory: %.0f u/day" % fuel_factory, UiTheme.GOOD, 12)
    # Surface trade behind a gateway: what the surface economy sells (its ask) and buys (its bid);
    # the prices are in the tooltip.
    var trades: Array = detail.get("surface_trade", [])
    if not trades.is_empty():
        var sells: Array[String] = []
        var buys: Array[String] = []
        var tip: Array[String] = []
        for trade in trades:
            var good := _good_name(String(trade.get("good", "")))
            var ask := float(trade.get("ask", 0.0))
            var bid := float(trade.get("bid", 0.0))
            if ask > 0.0:
                sells.append(good)
                tip.append("sells %s at %.1f cr/u" % [good, ask])
            if bid > 0.0:
                buys.append(good)
                tip.append("buys %s at %.1f cr/u" % [good, bid])
        var line := _add_label("%s sells %s; buys %s" % [String(trades[0].get("economy", "")),
            ", ".join(sells) if not sells.is_empty() else "nothing", ", ".join(buys) if not buys.is_empty() else "nothing"],
            UiTheme.CREDITS, 11)
        line.tooltip_text = "\n".join(tip)
        line.mouse_filter = Control.MOUSE_FILTER_PASS

    # Production: outputs after upkeep, and what limits them.
    var factors: Dictionary = detail.get("output_factor", {})
    if not factors.is_empty():
        var upkeep := float(detail.get("upkeep_multiplier", 1.0))
        var limited: Dictionary = detail.get("output_limited_by", {})
        var outputs: Array[String] = []
        for output_id in factors.keys():
            var text := "%s %d%%" % [_good_name(String(output_id)), int(round(100.0 * float(factors[output_id])))]
            if limited.has(output_id):
                text += " (short of %s)" % _good_name(String(limited[output_id]))
            outputs.append(text)
        _add_label("Production %d%% after upkeep: %s" % [int(round(100.0 * upkeep)), ", ".join(outputs)],
            UiTheme.TEXT_PRIMARY if upkeep >= 0.9 else (UiTheme.WARN if upkeep >= 0.5 else UiTheme.ALERT), 12)
        var short: Array[String] = []
        var availability: Dictionary = detail.get("upkeep_availability", {})
        for good in availability.keys():
            if float(availability[good]) < 0.999:
                short.append("%s %d%%" % [_good_name(String(good)), int(round(100.0 * float(availability[good])))])
        if not short.is_empty():
            _add_label("Upkeep short (stock against a week's use): " + ", ".join(short), UiTheme.WARN, 11)

# Residents: housing, who they are (a donut), labour, jobs by kind, passengers and visitors.
const POP_KIND_NAMES := {"labourer": "labourers", "technician": "technicians", "engineer": "engineers",
    "scientist": "scientists", "service": "service, admin"}

func _add_station_people(detail: Dictionary, context: Dictionary) -> void:
    var systems: Dictionary = detail.get("systems", {})
    var workers := float(systems.get("workers", detail.get("workers", 0.0)))
    var jobs := float(systems.get("jobs", detail.get("jobs", 0.0)))
    var employed := float(systems.get("employed", minf(workers, jobs)))
    var summary := "%s residents" % _count(float(detail.get("population", 0.0)))
    if jobs - employed >= 0.5:
        summary += " · %s jobs open" % _count(jobs - employed)
    elif workers - employed >= 0.5:
        summary += " · %s out of work" % _count(workers - employed)
    if not _section("people", "PEOPLE", summary):
        return
    if not systems.is_empty():
        var residents := float(systems.get("residents", 0.0))
        var housing := float(systems.get("housing", 0.0))
        _add_gauge("Housing", residents, maxf(housing, 1.0), "%s / %s" % [_count(residents), _count(housing)],
            UiTheme.WARN if residents > 0.97 * housing else UiTheme.ACCENT)
        var donut := DonutChartScript.new()
        donut.set_data("", [
            {"name": "workers in jobs", "value": employed, "color": UiTheme.categorical(0)},
            {"name": "out of work", "value": maxf(0.0, workers - employed), "color": UiTheme.ALERT},
            {"name": "dependants", "value": float(systems.get("dependants", 0.0)), "color": UiTheme.categorical(1)},
            {"name": "spacers (crews)", "value": float(systems.get("spacers", 0.0)), "color": UiTheme.categorical(2)},
            {"name": "visitors", "value": float(systems.get("visitors", 0.0)), "color": UiTheme.categorical(4)},
            {"name": "waiting to travel", "value": float(systems.get("waiting", 0.0)), "color": UiTheme.categorical(5)},
        ], "%.0f")
        _add(donut)
    if detail.has("household_credits"):
        var wage := float(detail.get("wage_cr_per_day", 0.0))
        _add_label("Wage %.2f cr/day ($%.0fk/yr) · savings %s" % [wage, wage * 365.0 / 0.00078 / 1000.0,
            UiWidgets.credits(float(detail.get("household_credits", 0.0)))], UiTheme.TEXT_PRIMARY, 12)
    var by_kind: Dictionary = systems.get("jobs_by_kind", {})
    if not by_kind.is_empty():
        var kinds: Array = by_kind.keys()
        kinds.sort_custom(func(a, b) -> bool: return float(by_kind[a]) > float(by_kind[b]))
        var total := 0.0
        for kind in kinds:
            total += float(by_kind[kind])
        var rows: Array = []
        for kind in kinds:
            if float(by_kind[kind]) >= 0.5:
                rows.append([String(POP_KIND_NAMES.get(kind, kind)), _count(float(by_kind[kind])),
                    "%.0f%%" % (100.0 * float(by_kind[kind]) / maxf(total, 1.0))])
        _table(["Jobs by kind (typical)", "Jobs", "Share"], rows, [170.0, 60.0, 60.0])
    # Passengers waiting, visitors, passages the station pays for.
    var waiting: Array = detail.get("passengers_waiting", [])
    if not waiting.is_empty():
        # By kind; where they are going in the tooltip.
        var waiting_by_kind: Dictionary = {}
        for group in waiting:
            var kind := String(group.get("kind", ""))
            var entry: Array = waiting_by_kind.get(kind, [0.0, []])
            entry[0] += float(group.get("count", 0.0))
            (entry[1] as Array).append("%.0f to %s" % [float(group.get("count", 0.0)), _resolve_name(context, String(group.get("to", "")))])
            waiting_by_kind[kind] = entry
        var rows: Array = []
        for kind in waiting_by_kind.keys():
            var entry: Array = waiting_by_kind[kind]
            var places: Array = entry[1]
            var tip := "\n".join(places)
            rows.append([[kind, UiTheme.TEXT_PRIMARY, tip], ["%d place%s" % [places.size(), "" if places.size() == 1 else "s"],
                UiTheme.TEXT_DIM, tip], "%.0f" % entry[0]])
        _table(["Waiting to travel", "Going to", "People"], rows, [150.0, 90.0, 60.0])
    var tour := float(detail.get("rotation_tour_days", 0.0))
    var paid := float(detail.get("passages_paid", 0.0))
    if tour > 0.0 or paid > 0.0:
        var line := "Passages paid %s, %s still to recover through prices" % [UiWidgets.credits(paid),
            UiWidgets.credits(float(detail.get("passage_debt_cr", 0.0)))]
        if tour > 0.0:
            line = "Staff tours %.0f days · " % tour + line
        _add_note(line)

# The station systems view (display only; docs/plans/station_structures.md, "Light view"): the
# site, the works and their jobs, and the needs in Victoria 2's tiers. Derived from the economy's
# numbers; the simulation ignores it.
func _add_station_works(detail: Dictionary, context: Dictionary) -> void:
    var systems: Dictionary = detail.get("systems", {})
    if systems.is_empty():
        return
    var works: Array = systems.get("works", [])
    var short_needs := 0
    for need in systems.get("needs", []):
        if float(need.get("availability", 1.0)) < 0.999 and float(need.get("use_per_day", 0.0)) > 0.0:
            short_needs += 1
    var summary := "%d works" % works.size() + (" · %d needs short" % short_needs if short_needs > 0 else "")
    if not _section("works", "WORKS AND NEEDS", summary, false):
        return
    var body := _resolve_name(context, String(detail.get("parent_body_id", "")))
    var site := String(systems.get("site", "orbit"))
    var g := float(systems.get("gravity_g", 1.0))
    var where := {"orbit": "Orbital station, spin rings at 1 g",
        "surface": "Surface base on %s, %.2f g" % [body, g],
        "underground": "Underground base on %s, %.2f g" % [body, g],
        "aerostat": "Aerostat in the clouds of %s, %.2f g" % [body, g]}
    var shield := float(systems.get("shield_g_cm2", 0.0))
    _add_note(String(where.get(site, site)) + (" · shielding %.0f g/cm²" % shield if shield > 0.0 else ""))

    var rows: Array = []
    for entry in works:
        var w: Dictionary = entry
        var capacity: Dictionary = w.get("capacity", {})
        var output: Dictionary = w.get("output", {})
        var inputs: Dictionary = w.get("inputs", {})
        var made: Array[String] = []
        var cap_total := 0.0
        var out_total := 0.0
        for good in capacity.keys():
            made.append("%s %s" % [_good_short(String(good)), _rate(float(capacity[good]))])
            cap_total += float(capacity[good])
            out_total += float(output.get(good, 0.0))
        var used: Array[String] = []
        for good in inputs.keys():
            used.append("%s %s/day" % [_good_name(String(good)), _rate(float(inputs[good]))])
        var tip := "Makes per day: " + ", ".join(made) + ("\nUses: " + ", ".join(used) if not used.is_empty() else "")
        var running := ["100%", UiTheme.TEXT_PRIMARY]
        if cap_total > 0.0 and out_total < 0.995 * cap_total:
            if out_total <= 0.0 and String(w.get("kind", "")) == "fuel_works":
                running = ["idle", UiTheme.TEXT_DIM, "Depot full"]
            else:
                running = ["%d%%" % int(round(100.0 * out_total / cap_total)), UiTheme.WARN]
        rows.append([[String(w.get("name", "")), UiTheme.TEXT_PRIMARY, tip, 120.0], _count(float(w.get("jobs", 0.0))),
            [", ".join(made), UiTheme.TEXT_DIM, tip, 100.0], running])
    rows.append(["Life support, port, admin", _count(float(systems.get("services_jobs", 0.0))), "", ""])
    _table(["Works", "Jobs", "Makes /day", "Runs"], rows, [120.0, 46.0, 100.0, 40.0])

    # Needs in tiers: use per day, how much the station's own farms and recycling cover, stock.
    var need_rows: Array = []
    var titles := {"life": "Life needs", "upkeep": "Station upkeep", "everyday": "Everyday goods"}
    for tier in ["life", "upkeep", "everyday"]:
        var tier_rows: Array = []
        for need in systems.get("needs", []):
            var n: Dictionary = need
            if String(n.get("tier", "everyday")) != tier:
                continue
            var use := float(n.get("use_per_day", 0.0))
            var own := ""
            if n.has("need_per_day") and float(n.get("need_per_day", 0.0)) > 0.0:
                own = "%d%%" % int(floor(100.0 * (1.0 - use / float(n["need_per_day"]))))
            elif bool(n.get("made_here", false)):
                own = "made here"
            if use <= 0.0 and own == "":
                continue
            var availability := float(n.get("availability", 1.0))
            var stock := ["ok", UiTheme.TEXT_DIM]
            if use > 0.0 and availability < 0.999:
                stock = ["%d%%" % int(round(100.0 * availability)), UiTheme.WARN, "Stock against a week's use"]
            tier_rows.append(["  " + _good_short(String(n.get("good", ""))), _rate(use) if use > 0.0 else "—", own, stock])
        if not tier_rows.is_empty():
            need_rows.append([[String(titles[tier]), UiTheme.ACCENT], "", "", ""])
            need_rows.append_array(tier_rows)
    if not need_rows.is_empty():
        _table(["Needs", "Use /day", "Own cover", "Stock"], need_rows, [120.0, 60.0, 70.0, 50.0])

# The station's purchase orders (step 29): one row per good it buys; what it has learnt about
# its resupply (use, lead time, interval and spread, the stock it orders up to, its ceiling) and
# the competing asks in the row's tooltip.
func _add_station_orders(detail: Dictionary) -> void:
    var orders: Dictionary = detail.get("orders", {})
    if orders.is_empty():
        return
    var ids := orders.keys()
    ids.sort_custom(func(a, b): return float(orders[a].get("open_units", 0.0)) * float(orders[a].get("order_price", 0.0)) \
        > float(orders[b].get("open_units", 0.0)) * float(orders[b].get("order_price", 0.0)))
    var rows: Array = []
    var soon := 0
    for commodity_id in ids:
        var o: Dictionary = orders[commodity_id]
        var short_in := float(o.get("days_to_safety", -1.0))
        if short_in >= 0.0:
            soon += 1
        var tip := "%s · %.2f u/day · lead %.0f d · a delivery every %.0f ± %.0f d\norders up to %.0f u (extra %.0f at a discount) · worth at most %.0f cr/u · %d deliveries so far\nlanded reference %.1f · going rate %.1f" % [
            String(o.get("kind", "")), float(o.get("use_per_day", 0.0)), float(o.get("lead_days", 0.0)),
            float(o.get("interval_days", 0.0)), float(o.get("spread_days", 0.0)), float(o.get("order_up_to_units", 0.0)),
            float(o.get("max_units", 0.0)), float(o.get("ceiling", 0.0)), int(o.get("deliveries", 0)),
            float(o.get("reference_price", 0.0)), float(o.get("going_rate", 0.0))]
        var market_price := float(o.get("market_price", 0.0))
        var bidders := int(o.get("market_bidders", 0))
        if market_price > 0.0:
            tip += "\nlowest competing ask %.1f cr/u (%d ship%s near a source)" % [market_price, bidders, "" if bidders == 1 else "s"]
        rows.append([
            [_good_short(String(commodity_id)), UiTheme.TEXT_PRIMARY, tip],
            ["%.1f" % float(o.get("order_price", 0.0)), UiTheme.CREDITS, tip],
            "%.0f" % float(o.get("open_units", 0.0)),
            "%.1f" % float(o.get("reference_price", 0.0)),
            ["%.0f d" % short_in, UiTheme.WARN] if short_in >= 0.0 else ["", UiTheme.TEXT_DIM],
        ])
    if not _section("orders", "ORDERS", "%d goods" % rows.size() + (" · %d short soon" % soon if soon > 0 else ""), false):
        return
    _table(["Good", "Bid", "Open u", "Ref", "Short in"], rows, [90.0, 56.0, 56.0, 50.0, 56.0])
    _add_note("Bid: cr/u for delivery one lead time from now; ref: the landed reference price. Hover a row for what the station has learnt about its resupply.")

# Ships on their way here (cargo, arrival, contract value) and ships docked here; a name selects the ship.
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
    if not _section("ships", "SHIPS", "%d inbound · %d docked" % [inbound.size(), docked.size()]):
        return
    if not inbound.is_empty():
        inbound.sort_custom(func(a, b): return float(a.get("arrival_time_s", 0.0)) < float(b.get("arrival_time_s", 0.0)))
        var rows: Array = []
        for ship in inbound:
            var what := _cargo_text(ship)
            var agreed := 0.0
            for lot in ship.get("cargo", []):
                agreed += float(lot.get("contract_value", 0.0))
            rows.append([_entity_button(String(ship.get("name", "")), String(ship.get("id", "")), "ship", 110.0),
                [what if what != "" else "empty", UiTheme.TEXT_PRIMARY if what != "" else UiTheme.TEXT_DIM, what, 100.0],
                _days(maxf(float(ship.get("arrival_time_s", 0.0)) - game_time_s, 0.0) / 86400.0),
                UiWidgets.credits(agreed) if agreed > 0.0 else ""])
        _table(["Inbound", "Cargo", "In", "Agreed"], rows, [110.0, 100.0, 40.0, 56.0])
    if not docked.is_empty():
        var rows: Array = []
        for ship in docked:
            var phase := String(ship.get("phase", ""))
            rows.append([_entity_button(String(ship.get("name", "")), String(ship.get("id", "")), "ship", 150.0),
                [_phase_word(phase), UiTheme.WARN if phase in ["laid_up", "stranded"] else UiTheme.TEXT_PRIMARY]])
        _table(["Docked", "Doing"], rows, [150.0, 150.0])

# Money since the start: the station's profit before the faction, incomes, and its ledger.
func _add_station_money(detail: Dictionary) -> void:
    var profit := float(detail.get("lifetime_profit", 0.0))
    var last_year := float(detail.get("profit_last_year", profit))
    if not _section("money", "MONEY", "profit %s last year" % UiWidgets.signed_credits(last_year)):
        return
    var rows: Array = []
    if detail.has("lifetime_profit"):
        rows.append(["Profit before faction", [UiWidgets.signed_credits(profit), UiWidgets.value_color(profit)],
            [UiWidgets.signed_credits(last_year), UiWidgets.value_color(last_year)]])
    for income in detail.get("incomes", []):
        rows.append(["Income: %s" % String(income.get("kind", "")), "",
            [UiWidgets.credits(float(income.get("cr_per_year", 0.0))) + "/yr", UiTheme.GOOD,
                "Paid by %s" % String(income.get("payer", ""))]])
    var ledger: Dictionary = detail.get("ledger", {})
    for entry in [["household_sales", "Residents paid"], ["dividends", "Ship dividends"], ["subsidies", "Subsidies"],
            ["taxes", "Taxes"]]:
        if ledger.has(entry[0]):
            rows.append([entry[1], UiWidgets.credits(float(ledger.get(entry[0], 0.0))), ""])
    _table(["", "Since start", "Last year"], rows, [140.0, 80.0, 80.0])
    _add_note("Residents pay for what they use at the station's import cost. S: statistics for the full accounts.")

# --- ship ----------------------------------------------------------------------

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
                text += ", then load %.0f u %s there" % [pickup_units, _good_name(pickup)]
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
    _add_header(String(detail.get("name", detail.get("id", ""))),
        "%s · %s" % [String(detail.get("class_name", detail.get("class_id", "ship"))),
            _faction_name(String(detail.get("faction_id", "")))],
        _faction_color(detail, context))

    # Key numbers.
    var propellant := float(detail.get("propellant_kg", 0.0))
    var propellant_capacity := maxf(float(detail.get("propellant_capacity_kg", 1.0)), 0.001)
    var fuel_ratio := propellant / propellant_capacity
    var cargo := float(detail.get("cargo_units", 0.0))
    var cargo_capacity := float(detail.get("cargo_capacity_units", 0.0))
    var profit := float(detail.get("lifetime_profit", 0.0))
    var status := ""
    var status_color := UiTheme.TEXT_PRIMARY
    match phase:
        "in_transit":
            status = _days(maxf(float(detail.get("arrival_time_s", 0.0)) - game_time_s, 0.0) / 86400.0)
        "awaiting_departure":
            status = "dep. " + _days(maxf(float(detail.get("departure_time_s", 0.0)) - game_time_s, 0.0) / 86400.0)
        "laid_up", "stranded":
            status = _phase_word(phase)
            status_color = UiTheme.WARN
        _:
            status = "docked"
    _add(UiWidgets.kpi_strip([
        {"label": "to arrival" if phase == "in_transit" else "status", "value": status, "color": status_color},
        {"label": "propellant", "value": "%.0f%%" % (100.0 * fuel_ratio),
            "color": UiTheme.ALERT if fuel_ratio < 0.15 else (UiTheme.WARN if fuel_ratio < 0.4 else UiTheme.TEXT_PRIMARY),
            "tooltip": "%.1f of %.0f t" % [propellant / 1000.0, propellant_capacity / 1000.0]},
        {"label": "hold", "value": "%.0f%%" % (100.0 * cargo / cargo_capacity) if cargo_capacity > 0.0 else "—",
            "tooltip": "%.0f of %.0f u" % [cargo, cargo_capacity]},
        {"label": "profit to date", "value": UiWidgets.signed_credits(profit), "color": UiWidgets.value_color(profit)},
    ]))
    _add_label(_mission_sentence(detail, context), UiTheme.TEXT_PRIMARY, 13)
    if phase == "stranded":
        _add_label("Stranded: needs fuel delivered or sold to it here.", UiTheme.ALERT, 12)

    _add_ship_voyage(detail, context)
    _add_ship_load(detail, context)
    _add_ship_money(detail, context)
    _add_ship_systems(detail, context)

# Where it is going, how, and what the trip should earn; liner loops and contracts.
func _add_ship_voyage(detail: Dictionary, context: Dictionary) -> void:
    var phase := String(detail.get("phase", "idle"))
    var game_time_s := float(context.get("game_time_s", 0.0))
    var stops: Array = detail.get("liner_stops", [])
    var loop: Array = detail.get("contract_stops", [])
    var moving := phase == "in_transit" or phase == "awaiting_departure"
    var route_until_s := float(detail.get("route_until_s", 0.0))
    var committed := route_until_s > game_time_s and String(detail.get("route_destination_id", "")) != ""
    if not moving and stops.is_empty() and loop.is_empty() and not committed:
        return
    var summary := _trajectory_label(String(detail.get("trajectory_type", ""))) if moving else ""
    if not _section("voyage", "VOYAGE", summary):
        return
    if phase == "in_transit":
        var departure_s := float(detail.get("departure_time_s", 0.0))
        var arrival_s := float(detail.get("arrival_time_s", 0.0))
        var total_s: float = maxf(arrival_s - departure_s, 1.0)
        var progress: float = clampf((game_time_s - departure_s) / total_s, 0.0, 1.0)
        _add_gauge("Transit", progress, 1.0, "%s of %s" % [_days((game_time_s - departure_s) / 86400.0), _days(total_s / 86400.0)],
            UiTheme.ACCENT)
    if moving:
        _add_note(_trajectory_label(String(detail.get("trajectory_type", ""))))
        var mission_value := float(detail.get("mission_value", 0.0))
        var revenue := float(detail.get("expected_revenue", 0.0))
        if revenue > 0.0:
            _table(["This trip", "Credits"], [
                ["Contracted sale", UiWidgets.credits(revenue)],
                ["Cargo bought", UiWidgets.credits(-float(detail.get("purchase_cost", 0.0)))],
                ["Expected to earn", [UiWidgets.signed_credits(mission_value), UiWidgets.value_color(mission_value)]],
            ], [170.0, 90.0])
        elif absf(mission_value) > 0.5:
            _add_note("Running this leg costs about %s (no cargo sold on it)." % UiWidgets.credits(-mission_value))
    if not stops.is_empty():
        var names: Array[String] = []
        for stop in stops:
            names.append(_resolve_name(context, String(stop)))
        _add_label("Scheduled liner: " + " → ".join(names) + " → …", UiTheme.ACCENT, 12)
    if not loop.is_empty():
        var loop_names: Array[String] = []
        for stop in loop:
            loop_names.append(_resolve_name(context, String(stop)))
        _add_label("Rotation contract until day %d: %s → …" % [int(float(detail.get("contract_until_s", 0.0)) / 86400.0),
            " → ".join(loop_names)], UiTheme.ACCENT, 12)
        var rows: Array = []
        for delivery in detail.get("contract_deliveries", []):
            rows.append([_good_short(String(delivery.get("commodity_id", ""))),
                [_resolve_name(context, String(delivery.get("to", ""))), UiTheme.TEXT_PRIMARY, "", 130.0],
                "%.0f" % float(delivery.get("units", 0.0)), "%.0f" % float(delivery.get("unit_price", 0.0))])
        if not rows.is_empty():
            _table(["Good", "To", "Units", "cr/u"], rows, [80.0, 130.0, 46.0, 46.0])
    if committed:
        _add_note("Bought to carry %s to %s, %.0f more days" % [_good_name(String(detail.get("route_commodity_id", ""))),
            _resolve_name(context, String(detail.get("route_destination_id", ""))),
            (route_until_s - game_time_s) / 86400.0])

# Propellant, hold, passengers and provisions; the cargo lots.
func _add_ship_load(detail: Dictionary, context: Dictionary) -> void:
    var propellant := float(detail.get("propellant_kg", 0.0))
    var propellant_capacity := maxf(float(detail.get("propellant_capacity_kg", 1.0)), 0.001)
    var fuel_ratio := propellant / propellant_capacity
    var provision_days := float(detail.get("provision_days", 0.0))
    var load := _cargo_text(detail)
    if not _section("load", "LOAD", load if load != "" else "hold empty"):
        return
    _add_gauge("Propellant", propellant, propellant_capacity, "%.1f / %.0f t" % [propellant / 1000.0, propellant_capacity / 1000.0],
        UiTheme.ALERT if fuel_ratio < 0.15 else (UiTheme.WARN if fuel_ratio < 0.4 else UiTheme.ACCENT))
    var cargo := float(detail.get("cargo_units", 0.0))
    var cargo_capacity := float(detail.get("cargo_capacity_units", 0.0))
    if cargo_capacity > 0.0:
        _add_gauge("Cargo", cargo, cargo_capacity, "%.0f / %.0f u" % [cargo, cargo_capacity], UiTheme.GOOD)
    var berths := float(detail.get("berths", 0.0))
    if berths > 0.0:
        var aboard := float(detail.get("passengers_aboard", 0.0))
        _add_gauge("Passengers", aboard, berths, "%.0f / %.0f" % [aboard, berths], UiTheme.ACCENT)
    _add_gauge("Provisions", provision_days, maxf(730.0, provision_days), "%.0f days" % provision_days,
        UiTheme.ALERT if provision_days < 60.0 else UiTheme.ACCENT)
    var lots: Array = detail.get("cargo", [])
    if not lots.is_empty():
        var rows: Array = []
        for lot in lots:
            var value := float(lot.get("contract_value", 0.0))
            rows.append([_good_short(String(lot.get("commodity_id", ""))), "%.0f" % float(lot.get("units", 0.0)),
                UiWidgets.credits(value) if value > 0.0 else "at market"])
        _table(["Cargo", "Units", "Sale agreed"], rows, [140.0, 60.0, 90.0])
    # Passengers by kind and destination; cabin shielding.
    if berths > 0.0:
        var groups: Dictionary = {}
        for lot in detail.get("passenger_lots", []):
            var key := "%s|%s" % [String(lot.get("kind", "")), _resolve_name(context, String(lot.get("to", "")))]
            var entry: Array = groups.get(key, [0.0, 0.0])
            entry[0] += float(lot.get("count", 0.0))
            entry[1] += float(lot.get("count", 0.0)) * float(lot.get("fare_cr", 0.0))
            groups[key] = entry
        if not groups.is_empty():
            var rows: Array = []
            for key in groups.keys():
                var parts: PackedStringArray = String(key).split("|")
                var entry: Array = groups[key]
                rows.append([parts[0], [parts[1], UiTheme.TEXT_PRIMARY, "", 130.0], "%.0f" % entry[0], "%.0f" % (entry[1] / maxf(entry[0], 1.0))])
            _table(["Passengers", "To", "People", "Fare"], rows, [80.0, 130.0, 50.0, 50.0])
        var shield := float(detail.get("cabin_shield_g_cm2", 0.0))
        if shield > 0.0:
            _add_note("Cabins shielded by %.0f g/cm² of water" % shield)

# Crew and home; money since commissioning by category.
func _add_ship_money(detail: Dictionary, context: Dictionary) -> void:
    var credits := float(detail.get("credits", 0.0))
    var profit := float(detail.get("lifetime_profit", 0.0))
    if not _section("money", "CREW AND MONEY", "cash %s" % UiWidgets.credits(credits), false):
        return
    _add_label("Crew %d · home %s" % [int(detail.get("crew_size", 0)),
        _resolve_name(context, String(detail.get("home_station_id", "")))], UiTheme.TEXT_PRIMARY, 12)
    var ledger: Dictionary = detail.get("ledger", {})
    var rows: Array = []
    rows.append(["Cash", [UiWidgets.credits(credits), UiTheme.CREDITS if credits >= 0.0 else UiTheme.ALERT]])
    if not ledger.is_empty():
        # Money in (+) and out (−) since the ship was commissioned; fuel can be net income
        # (a tanker sells fuel at the depots it serves).
        rows.append(["Sales", [UiWidgets.signed_credits(float(ledger.get("cargo_revenue", 0.0))), UiTheme.GOOD]])
        var fares := float(detail.get("fares_cr", 0.0))
        if fares > 0.5:
            rows.append(["Fares", [UiWidgets.signed_credits(fares), UiTheme.GOOD]])
        for entry in [["cargo_purchases", "Cargo bought"], ["fuel", "Fuel"], ["wages", "Wages"], ["capital", "Capital charge"],
                ["provisions", "Provisions"], ["refits", "Refits and repairs"]]:
            var amount := -float(ledger.get(entry[0], 0.0))
            if absf(amount) >= 0.5:
                rows.append([entry[1], [UiWidgets.signed_credits(amount), UiWidgets.value_color(amount)]])
    rows.append(["Profit to date", [UiWidgets.signed_credits(profit), UiWidgets.value_color(profit)]])
    _table(["Since commissioning", "Credits"], rows, [170.0, 100.0])

# --- Ship systems (docs/modules/ships_and_fleet.md) ----------------------------
# The ship's parts from the bridge's designs file: a to-scale schematic with its live state
# (ShipSchematic), whether the engine runs, and the parts as a list (folded by default).
func _add_ship_systems(detail: Dictionary, context: Dictionary) -> void:
    var designs: Dictionary = context.get("designs", {})
    var design: Dictionary = designs.get(String(detail.get("design_id", detail.get("class_id", ""))), {})
    if design.is_empty():
        return
    if not _section("systems", "SHIP SYSTEMS", _engine_line(detail, design)):
        return
    _add_label(_engine_line(detail, design), UiTheme.TEXT_PRIMARY, 12)
    _add_tier2_lines(detail, design)
    if _schematic == null:
        _schematic = ShipSchematicScript.new()
        _schematic.size_flags_horizontal = Control.SIZE_EXPAND_FILL
    if _schematic.get_parent() == _content:
        _content.move_child(_schematic, -1)
    else:
        _content.add_child(_schematic)
    _schematic_used = true
    _schematic.set_ship(design, detail)

    var toggle := UiWidgets.section_header("Parts list", _parts_open, "", func() -> void:
        _parts_open = not _parts_open
        _rebuild_last())
    toggle.add_theme_color_override("font_color", UiTheme.TEXT_DIM)
    toggle.add_theme_font_size_override("font_size", 11)
    _add(toggle)
    if not _parts_open:
        return
    for component in design.get("components", []):
        var c: Dictionary = component
        var child := String(c.get("parent_id", "")) != ""
        var row := HBoxContainer.new()
        row.add_theme_constant_override("separation", 8)
        _add(row)
        var name_label := UiWidgets.label(("    " if child else "") + String(c.get("name", "")),
            UiTheme.TEXT_DIM if child else UiTheme.TEXT_PRIMARY, 11 if child else 12, 0.0, HORIZONTAL_ALIGNMENT_LEFT, true)
        name_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
        row.add_child(name_label)
        row.add_child(UiWidgets.label(ShipSchematicScript.fmt_mass(float(c.get("mass_kg", 0.0))), UiTheme.TEXT_DIM, 11, 64.0,
            HORIZONTAL_ALIGNMENT_RIGHT))
        var spec := ShipSchematicScript.component_spec(c)
        if spec != "":
            _add_note(("      " if child else "  ") + spec)

# Tier 2: modules fitted, the crew's dose, boil-off and wear (fields sent only when switched on).
func _add_tier2_lines(detail: Dictionary, design: Dictionary) -> void:
    var modules: Array[String] = []
    for component in design.get("components", []):
        if String(component.get("kind", "")) == "module":
            modules.append(String(component.get("name", "")))
    if not modules.is_empty():
        _add_note("Fitted: " + ", ".join(modules))
    if detail.has("leg_dose_msv"):
        var dose := float(detail.get("leg_dose_msv", 0.0))
        var limit := maxf(float(detail.get("dose_limit_msv", 1000.0)), 1.0)
        var color := UiTheme.ALERT if dose > limit else (UiTheme.WARN if dose > 0.8 * limit else UiTheme.TEXT_DIM)
        _add_label("Crew dose this leg %.0f of %.0f mSv · now %.2f mSv/day" % [dose, limit,
            float(detail.get("dose_rate_msv_day", 0.0))], color, 11)
    if float(detail.get("boiloff_kg_day", 0.0)) > 0.0:
        _add_note("Hydrogen boiling off %.0f kg/day" % float(detail.get("boiloff_kg_day", 0.0)))
    if detail.has("engine_wear"):
        var wear := float(detail.get("engine_wear", 0.0))
        var life := float(detail.get("drive_life_h", 0.0))
        var text := "Drive life left %.0f%% (%s of %s h)" % [100.0 * (1.0 - wear),
            _hours((1.0 - wear) * life), _hours(life)]
        var radiators := float(detail.get("radiator_condition", 1.0))
        if String(detail.get("propulsion_type", design.get("propulsion_type", ""))) == "variable_isp":
            text += " · radiators %.1f%%" % (100.0 * radiators)
        _add_label(text, UiTheme.WARN if wear > 0.7 else UiTheme.TEXT_DIM, 11)

func _engine_line(detail: Dictionary, design: Dictionary) -> String:
    var state := String(detail.get("engine_state", "off"))
    var text := "Engine %s" % state
    var radiator_k := float(detail.get("radiator_temperature_k", 0.0))
    if radiator_k > 0.0:
        text += " · drive radiators %.0f K" % radiator_k
        if state != "thrusting":
            text += " (idle, sunlit)"
    elif state == "burning":
        for component in design.get("components", []):
            if String(component.get("kind", "")) == "engine" and float(component.get("temperature_k", 0.0)) > 0.0:
                text += " · reactor core %.0f K" % float(component["temperature_k"])
    return text

# --- body ----------------------------------------------------------------------

func _build_body(detail: Dictionary, context: Dictionary = {}) -> void:
    var body_id := String(detail.get("id", ""))
    var parent_id := String(detail.get("parent_id", ""))
    var a_m := float(detail.get("semi_major_axis_m", 0.0))
    var period_d := float(detail.get("orbital_period_s", 0.0)) / 86400.0
    var subtitle := "Star"
    if parent_id == "sun":
        subtitle = "Planet"
    elif parent_id != "":
        subtitle = "Moon of %s" % _resolve_name(context, parent_id)
    _add_header(String(detail.get("name", body_id)), subtitle, Color(0.4, 0.5, 0.6))
    var tiles: Array = [{"label": "radius", "value": "%s km" % _count(float(detail.get("radius_m", 0.0)) / 1000.0)}]
    if parent_id == "sun":
        tiles.append({"label": "from the Sun", "value": "%.2f AU" % (a_m / 1.495978707e11)})
        tiles.append({"label": "year", "value": "%.0f d" % period_d})
    elif parent_id != "":
        tiles.append({"label": "orbit", "value": "%s km" % _count(a_m / 1000.0)})
        tiles.append({"label": "month", "value": "%.1f d" % period_d})
    _add(UiWidgets.kpi_strip(tiles))
    var moons: Array = []
    for body in context.get("bodies", []):
        if String(body.get("parent_id", "")) == body_id:
            moons.append([_entity_button(String(body.get("name", "")), String(body.get("id", "")), "body", 150.0)])
    if not moons.is_empty():
        _table(["Planets" if parent_id == "" else "Moons"], moons, [150.0])
    var stations: Array = []
    for station in context.get("stations", []):
        if String(station.get("parent_body_id", "")) == body_id:
            stations.append([_entity_button(String(station.get("name", "")), String(station.get("id", "")), "station", 200.0),
                _count(float(station.get("population", 0.0)))])
    if not stations.is_empty():
        _table(["Stations", "People"], stations, [200.0, 70.0])
