extends PanelContainer

# The Statistics screen (S): money by sector, who pays whom, and every account's income and
# expense by category (the simulation's money journal), with monthly history. The data comes
# from the bridge's stats side file, written only while this screen is open.

signal ship_selected(ship_id: String)
signal visibility_toggled(open: bool)
# The Routes tab's pair may have changed: the bridge is asked for it at once, not at the next UI refresh.
signal route_changed

const UiTheme := preload("res://scripts/ui/UiTheme.gd")
const LineChartScript := preload("res://scripts/ui/LineChart.gd")
const DonutChartScript := preload("res://scripts/ui/DonutChart.gd")
const StackedBarScript := preload("res://scripts/ui/StackedBar.gd")

const TABS := ["Overview", "Factions", "Stations", "People", "Colonies & Earth", "Fleet", "Flows", "Routes"]
const ROUTES_TAB := 7
const PERIODS := [["lifetime", "Since the start"], ["last_year", "Last year"], ["last_30", "Last 30 days"]]
const SECTOR_NAMES := {"earth": "Earth", "colony": "Colonies", "faction": "Factions", "station": "Stations",
    "households": "Residents", "fleet": "Fleets"}
const SERIES_COLORS := [Color(0.35, 0.85, 0.95), Color(0.95, 0.85, 0.4), Color(0.45, 0.9, 0.55), Color(1.0, 0.55, 0.35),
    Color(0.75, 0.6, 1.0), Color(1.0, 0.45, 0.65), Color(0.6, 0.8, 0.8), Color(0.9, 0.9, 0.9), Color(0.55, 0.7, 1.0)]
# Owner transfers: not a ship's or a station's own result.
const OWNER_CATEGORIES := ["dividends", "ship_sale_cash", "working_capital", "liner_topup", "faction_tax", "faction_topup",
    "insolvency_cover", "money_controller"]

var stats: Dictionary = {}
var _tab := 0
var _period := "last_year"
var _monthly := false   # charts: balances (false) or net flow per month (true)
var _selected_station := ""
var _flow_good := ""          # Flows tab: one good, or all ("")
var _flow_value := false      # Flows tab: credits (true) or units (false)
# Routes tab: the planet pair shown (the busiest route until one is picked), the view ("fastest",
# "thermal" or "plasma"), and the porkchop file and ships from Main.
var route_origin := "earth"
var route_destination := "mars"
var _route_view := "fastest"
var _route_picked := false
var _route_other := false
var porkchops: Dictionary = {}
# The last grid seen for each pair ("earth>saturn"): switching back shows it at once while the
# bridge sends the current one (the bridge keeps every grid; only the file names the pairs asked).
var _pair_cache: Dictionary = {}
var ships: Array = []
var _route_map: Control = null   # the Routes tab's plot while it is shown
var _labels: Dictionary = {}   # category id -> label
var _tab_bar: TabBar
var _period_buttons: Array = []
var _measure_button: Button
var _content: VBoxContainer
var _scroll: ScrollContainer
var _status: Label

func _gui_input(event: InputEvent) -> void:
    if event is InputEventMouseButton and event.button_index in [MOUSE_BUTTON_WHEEL_UP, MOUSE_BUTTON_WHEEL_DOWN,
            MOUSE_BUTTON_WHEEL_LEFT, MOUSE_BUTTON_WHEEL_RIGHT]:
        accept_event()

func _ready() -> void:
    visible = false
    add_theme_stylebox_override("panel", UiTheme.panel_style(UiTheme.PANEL_BG_SOLID, UiTheme.ACCENT_DIM))
    var box := VBoxContainer.new()
    box.add_theme_constant_override("separation", 6)
    add_child(box)
    var title := Label.new()
    title.text = "STATISTICS — money by account and source   (S or Esc to close)"
    title.add_theme_color_override("font_color", UiTheme.ACCENT)
    title.add_theme_font_size_override("font_size", 14)
    box.add_child(title)
    _tab_bar = TabBar.new()
    _tab_bar.focus_mode = Control.FOCUS_NONE  # keys stay with the map (Space, shortcuts)
    for name in TABS:
        _tab_bar.add_tab(name)
    _tab_bar.tab_changed.connect(func(index: int) -> void:
        _tab = index
        _rebuild())
    box.add_child(_tab_bar)
    var options := HBoxContainer.new()
    options.add_theme_constant_override("separation", 6)
    box.add_child(options)
    for period in PERIODS:
        var button := Button.new()
        button.focus_mode = Control.FOCUS_NONE  # keys stay with the map (Space, shortcuts)
        button.text = period[1]
        button.toggle_mode = true
        button.button_pressed = period[0] == _period
        button.pressed.connect(func() -> void:
            _period = period[0]
            for b in _period_buttons:
                b.button_pressed = b == button
            _rebuild())
        options.add_child(button)
        _period_buttons.append(button)
    var spacer := Control.new()
    spacer.custom_minimum_size.x = 24
    options.add_child(spacer)
    _measure_button = Button.new()
    _measure_button.focus_mode = Control.FOCUS_NONE  # keys stay with the map (Space, shortcuts)
    _measure_button.text = "Charts: balances"
    _measure_button.pressed.connect(func() -> void:
        _monthly = not _monthly
        _measure_button.text = "Charts: net per month" if _monthly else "Charts: balances"
        _rebuild())
    options.add_child(_measure_button)
    _status = Label.new()
    _status.add_theme_color_override("font_color", UiTheme.TEXT_DIM)
    _status.add_theme_font_size_override("font_size", 11)
    options.add_child(_status)
    _scroll = ScrollContainer.new()
    _scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
    _scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_AUTO
    box.add_child(_scroll)
    _content = VBoxContainer.new()
    _content.add_theme_constant_override("separation", 10)
    _content.size_flags_horizontal = Control.SIZE_EXPAND_FILL
    _scroll.add_child(_content)

func toggle() -> void:
    visible = not visible
    if visible:
        var viewport := get_viewport_rect().size
        custom_minimum_size = Vector2(min(1280.0, viewport.x - 40.0), viewport.y - 90.0)
        _rebuild()
    visibility_toggled.emit(visible)

func set_tab(index: int) -> void:
    _tab = index
    _tab_bar.current_tab = index
    _rebuild()

func update_stats(data: Dictionary) -> void:
    stats = data
    _labels.clear()
    for category in stats.get("categories", []):
        _labels[String(category.get("id", ""))] = String(category.get("label", ""))
    # Not while the mouse is held over the panel: a rebuild between press and release frees the
    # button being clicked (the next stats file comes about a second later).
    var held := Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT) and get_global_rect().has_point(get_global_mouse_position())
    # The Routes tab does not use the statistics file (its plot follows the porkchop file).
    if visible and not held and _tab != ROUTES_TAB:
        _rebuild()

# --- building blocks ---

func _label(text: String, color: Color = UiTheme.TEXT_PRIMARY, font_size: int = 11, width: float = 0.0,
        align := HORIZONTAL_ALIGNMENT_LEFT) -> Label:
    var label := Label.new()
    label.text = text
    label.add_theme_color_override("font_color", color)
    label.add_theme_font_size_override("font_size", font_size)
    label.horizontal_alignment = align
    if width > 0.0:
        label.custom_minimum_size.x = width
    return label

func _heading(text: String) -> void:
    _content.add_child(_label(text, UiTheme.ACCENT, 13))

func _note(text: String) -> void:
    var label := _label(text, UiTheme.TEXT_DIM, 11)
    label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
    label.custom_minimum_size.x = 900
    _content.add_child(label)

func _cr(value: float) -> String:
    return UiTheme.format_credits(value)

func _value_color(value: float) -> Color:
    return UiTheme.GOOD if value > 0.5 else (UiTheme.ALERT if value < -0.5 else UiTheme.TEXT_DIM)

# A table: header strings, rows of [text or [text, color]], first column left-aligned.
func _table(headers: Array, rows: Array, widths: Array = []) -> GridContainer:
    var grid := GridContainer.new()
    grid.columns = headers.size()
    grid.add_theme_constant_override("h_separation", 10)
    grid.add_theme_constant_override("v_separation", 2)
    for i in range(headers.size()):
        var width: float = widths[i] if i < widths.size() else (170.0 if i == 0 else 90.0)
        grid.add_child(_label(String(headers[i]), UiTheme.TEXT_DIM, 11, width,
            HORIZONTAL_ALIGNMENT_LEFT if i == 0 else HORIZONTAL_ALIGNMENT_RIGHT))
    for row in rows:
        for i in range(row.size()):
            var cell = row[i]
            if cell is Control:
                grid.add_child(cell)
                continue
            var text := String(cell[0]) if cell is Array else String(cell)
            var color: Color = cell[1] if cell is Array else UiTheme.TEXT_PRIMARY
            grid.add_child(_label(text, color, 11, 0.0, HORIZONTAL_ALIGNMENT_LEFT if i == 0 else HORIZONTAL_ALIGNMENT_RIGHT))
    return grid

func _flows(account: Dictionary) -> Dictionary:
    return account.get(_period, {})

func _totals(flows: Dictionary) -> Vector2:
    var total := Vector2.ZERO
    for pair in flows.values():
        total.x += float(pair[0])
        total.y += float(pair[1])
    return total

# An account's income and expense by category: in, out, net, share of its income or expense.
func _flow_table(title: String, flows: Dictionary) -> VBoxContainer:
    var box := VBoxContainer.new()
    box.add_child(_label(title, UiTheme.TEXT_PRIMARY, 12))
    var totals := _totals(flows)
    var ids := flows.keys()
    ids.sort_custom(func(a, b) -> bool:
        var fa: Array = flows[a]
        var fb: Array = flows[b]
        return max(float(fa[0]), float(fa[1])) > max(float(fb[0]), float(fb[1])))
    var rows: Array = []
    for id in ids:
        var pair: Array = flows[id]
        var income := float(pair[0])
        var expense := float(pair[1])
        var share: float = income / totals.x if income >= expense and totals.x > 0.0 else (expense / totals.y if totals.y > 0.0 else 0.0)
        var bar := ProgressBar.new()
        bar.custom_minimum_size = Vector2(70, 10)
        bar.show_percentage = false
        bar.max_value = 1.0
        bar.value = share
        bar.add_theme_stylebox_override("fill", UiTheme.bar_fill_style(UiTheme.GOOD if income >= expense else UiTheme.WARN))
        rows.append([String(_labels.get(id, id)), [_cr(income) if income > 0.0 else "", UiTheme.GOOD],
            [_cr(expense) if expense > 0.0 else "", UiTheme.WARN], [_cr(income - expense), _value_color(income - expense)], bar])
    rows.append([["Total", UiTheme.ACCENT], [_cr(totals.x), UiTheme.GOOD], [_cr(totals.y), UiTheme.WARN],
        [_cr(totals.x - totals.y), _value_color(totals.x - totals.y)], ""])
    box.add_child(_table(["Category", "In", "Out", "Net", "Share"], rows, [150.0, 82.0, 82.0, 82.0, 74.0]))
    return box

# Side-by-side tables that wrap onto the next line when the screen is too narrow.
func _flow_row() -> HFlowContainer:
    var row := HFlowContainer.new()
    row.add_theme_constant_override("h_separation", 24)
    row.add_theme_constant_override("v_separation", 12)
    row.custom_minimum_size.x = custom_minimum_size.x - 40.0
    return row

func _station_name(station_id: String) -> String:
    for station in stats.get("stations", []):
        if String(station.get("id", "")) == station_id:
            return String(station.get("name", station_id))
    return station_id

func _accounts(kind: String) -> Array:
    var out: Array = []
    for account in stats.get("accounts", []):
        if String(account.get("kind", "")) == kind:
            out.append(account)
    return out

func _account(key: String) -> Dictionary:
    for account in stats.get("accounts", []):
        if String(account.get("key", "")) == key:
            return account
    return {}

func _history_days() -> Array:
    return stats.get("history", {}).get("days", [])

# One account's chart series: its balance, or its net flow per month (owner transfers left out
# when `own_result`).
func _series(key: String, own_result := false) -> Array:
    var history: Dictionary = stats.get("history", {})
    if not _monthly:
        return history.get("balances", {}).get(key, [])
    var flows: Dictionary = history.get("flows", {}).get(key, {})
    var net: Array = []
    net.resize(_history_days().size())
    net.fill(0.0)
    for category in flows.keys():
        if own_result and OWNER_CATEGORIES.has(category):
            continue
        var values: Array = flows[category]
        for i in range(min(values.size(), net.size())):
            net[i] = float(net[i]) + float(values[i])
    return net

func _sum_series(list: Array) -> Array:
    var total: Array = []
    for values in list:
        if total.size() < values.size():
            for i in range(total.size(), values.size()):
                total.append(0.0)
        for i in range(values.size()):
            total[i] = float(total[i]) + float(values[i])
    return total

func _chart(title: String, chart_series: Array) -> void:
    var chart := LineChartScript.new()
    chart.custom_minimum_size = Vector2(900, 220)
    _content.add_child(chart)
    chart.set_data(title + ("  (net per month)" if _monthly else "  (balance)"), _history_days(), chart_series)

func _faction_name(faction_id: String) -> String:
    for faction in stats.get("factions", []):
        if String(faction.get("id", "")) == faction_id:
            return String(faction.get("name", faction_id))
    return faction_id

func _faction_color(faction_id: String, fallback: Color) -> Color:
    for faction in stats.get("factions", []):
        if String(faction.get("id", "")) == faction_id and String(faction.get("color", "")) != "":
            return Color.html("#" + String(faction.get("color", "")))
    return fallback

func _top_category(flows: Dictionary, income: bool) -> String:
    var best := ""
    var best_value := 0.0
    for id in flows.keys():
        var value := float(flows[id][0 if income else 1])
        if value > best_value:
            best_value = value
            best = id
    return "" if best == "" else "%s %s" % [String(_labels.get(best, best)), _cr(best_value)]

# --- tabs ---

func _rebuild() -> void:
    if _content == null:
        return
    for child in _content.get_children():
        child.queue_free()
    if stats.is_empty():
        _status.text = "waiting for the simulation…"
        return
    _status.text = "  day %.0f · %d monthly samples" % [float(stats.get("day", 0.0)), _history_days().size()]
    match _tab:
        0: _build_overview()
        1: _build_factions()
        2: _build_stations()
        3: _build_people()
        4: _build_colonies()
        5: _build_fleet()
        6: _build_flows()
        7:
            _build_routes()
            route_changed.emit()

func _build_overview() -> void:
    _heading("Money by sector")
    var kinds := ["earth", "colony", "faction", "station", "households", "fleet"]
    var rows: Array = []
    var chart_series: Array = []
    for kind_index in range(kinds.size()):
        var kind: String = kinds[kind_index]
        var balance := 0.0
        var flows_in := 0.0
        var flows_out := 0.0
        var keys: Array = []
        for account in _accounts(kind):
            balance += float(account.get("balance", 0.0))
            var totals := _totals(_flows(account))
            flows_in += totals.x
            flows_out += totals.y
            keys.append(String(account.get("key", "")))
        rows.append([SECTOR_NAMES[kind], _cr(balance), [_cr(flows_in), UiTheme.GOOD], [_cr(flows_out), UiTheme.WARN],
            [_cr(flows_in - flows_out), _value_color(flows_in - flows_out)]])
        var list: Array = []
        for key in keys:
            list.append(_series(key))
        chart_series.append({"name": SECTOR_NAMES[kind], "color": SERIES_COLORS[kind_index], "values": _sum_series(list)})
    var total := float(stats.get("total", 0.0))
    var seed := float(stats.get("seed_total", 0.0))
    rows.append([["All accounts", UiTheme.ACCENT], _cr(total), "", "", ["drift %s" % _cr(total - seed), _value_color(-abs(total - seed))]])
    _content.add_child(_table(["Sector", "Balance", "Received", "Paid", "Net"], rows))
    _note("Every credit sits in one account; the total never changes (drift 0). Earth is not limited: a negative "
        + "balance is what it has paid into space. Received and paid are for the period above.")
    _heading("Who pays whom (period)")
    var sectors: Dictionary = stats.get("sectors", {})
    var matrix: Array = sectors.get(_period, [])
    var order: Array = sectors.get("kinds", [])
    if matrix.size() == order.size() * order.size():
        var header: Array = ["Payer \\ payee"]
        for kind in order:
            header.append(SECTOR_NAMES.get(kind, kind))
        var matrix_rows: Array = []
        for from in range(order.size()):
            var row: Array = [SECTOR_NAMES.get(order[from], order[from])]
            for to in range(order.size()):
                var value := float(matrix[from * order.size() + to])
                row.append([_cr(value) if value > 0.5 else "·", UiTheme.TEXT_PRIMARY if value > 0.5 else UiTheme.TEXT_DIM])
            matrix_rows.append(row)
        _content.add_child(_table(header, matrix_rows))
    _chart("Sectors", chart_series)

func _build_factions() -> void:
    _heading("Factions")
    var factions: Array = stats.get("factions", [])
    var stations: Array = stats.get("stations", [])
    var fleets: Dictionary = {}
    for fleet in stats.get("fleet", {}).get("by_faction", []):
        fleets[String(fleet.get("key", ""))] = fleet
    var rows: Array = []
    for faction in factions:
        var id := String(faction.get("id", ""))
        var account := _account("faction:" + id)
        var flows := _flows(account)
        var operating := 0.0
        var investment := 0.0
        for category in flows.keys():
            var net := float(flows[category][0]) - float(flows[category][1])
            if category in ["hull_earth", "hull_yard", "working_capital", "salvage"]:
                investment += net
            else:
                operating += net
        var station_profit := 0.0
        var owned := 0
        for station in stations:
            if String(station.get("faction", "")) == id:
                owned += 1
                station_profit += float(station.get("profit_last_year" if _period != "lifetime" else "profit", 0.0))
        var fleet: Dictionary = fleets.get(id, {})
        rows.append([String(faction.get("name", id)), _cr(float(faction.get("treasury", 0.0))),
            _cr(float(faction.get("credit_limit", 0.0))), [_cr(operating), _value_color(operating)],
            [_cr(investment), _value_color(investment)], "%d" % owned, [_cr(station_profit), _value_color(station_profit)],
            "%d" % int(fleet.get("ships", 0)), [_cr(float(fleet.get("profit", 0.0))), _value_color(float(fleet.get("profit", 0.0)))]])
    _content.add_child(_table(["Faction", "Treasury", "May borrow", "Operating", "Investment", "Stations",
        "Stations' profit", "Ships ever", "Fleet profit"], rows))
    _note("Operating: taxes and salvage in; top-ups, insolvency cover, liner top-ups, emergency premiums and interest out. "
        + "Investment: hulls and new ships' working capital. Stations' profit is before the faction's taxes and subsidies "
        + "(last year, or since the start). Fleet profit is every ship's lifetime result, sold ones too.")
    var tables := _flow_row()
    for faction in factions:
        tables.add_child(_flow_table(String(faction.get("name", "")), _flows(_account("faction:" + String(faction.get("id", ""))))))
    _content.add_child(tables)
    var chart_series: Array = []
    for i in range(factions.size()):
        var id := String(factions[i].get("id", ""))
        chart_series.append({"name": String(factions[i].get("name", id)), "color": _faction_color(id, SERIES_COLORS[i]),
            "values": _series("faction:" + id)})
    _chart("Faction treasuries", chart_series)

func _station_buttons_row(station: Dictionary) -> Button:
    var id := String(station.get("id", ""))
    var button := Button.new()
    button.focus_mode = Control.FOCUS_NONE  # keys stay with the map (Space, shortcuts)
    button.text = String(station.get("name", id))
    button.flat = true
    button.alignment = HORIZONTAL_ALIGNMENT_LEFT
    button.custom_minimum_size.x = 170
    button.add_theme_font_size_override("font_size", 11)
    button.add_theme_color_override("font_color", UiTheme.ACCENT if id == _selected_station else UiTheme.TEXT_PRIMARY)
    button.pressed.connect(func() -> void:
        _selected_station = id
        _rebuild())
    return button

func _build_stations() -> void:
    _heading("Stations (click a name for its accounts)")
    var stations: Array = stats.get("stations", [])
    if _selected_station == "" and not stations.is_empty():
        _selected_station = String(stations[0].get("id", ""))
    var rows: Array = []
    for station in stations:
        var id := String(station.get("id", ""))
        var flows := _flows(_account("station:" + id))
        var profit := float(station.get("profit" if _period == "lifetime" else "profit_last_year", 0.0))
        rows.append([_station_buttons_row(station), _faction_name(String(station.get("faction", ""))),
            _cr(float(station.get("credits", 0.0))), [_cr(profit), _value_color(profit)], _top_category(flows, true),
            _top_category(flows, false), "%.0f" % float(station.get("red_days", 0.0))])
    _content.add_child(_table(["Station", "Faction", "Cash", "Profit", "Top income", "Top expense", "Red days"], rows,
        [170.0, 150.0, 90.0, 90.0, 210.0, 210.0, 70.0]))
    _note("Profit: before the faction's taxes and subsidies, with the stock's value gain (since the start, or over the "
        + "last year for the shorter periods).")
    if _selected_station != "":
        var row := _flow_row()
        row.add_child(_flow_table("Station account", _flows(_account("station:" + _selected_station))))
        row.add_child(_flow_table("Its residents", _flows(_account("households:" + _selected_station))))
        _content.add_child(row)
        var profit_series: Array = stats.get("history", {}).get("station_profit", {}).get(_selected_station, [])
        var monthly_profit: Array = []
        for i in range(profit_series.size()):
            monthly_profit.append(float(profit_series[i]) - (float(profit_series[i - 1]) if i > 0 else 0.0))
        _chart("Selected station", [
            {"name": "cash" if not _monthly else "net flow", "color": SERIES_COLORS[0], "values": _series("station:" + _selected_station, true)},
            {"name": "profit since start" if not _monthly else "profit", "color": SERIES_COLORS[2],
                "values": profit_series if not _monthly else monthly_profit},
        ])

# --- People: who lives where and what they do (the station systems view, display only) ---

const POP_KIND_NAMES := {"labourer": "labourers", "technician": "technicians", "engineer": "engineers",
    "scientist": "scientists", "service": "service and administration"}
const POP_KINDS := ["labourer", "technician", "engineer", "scientist", "service"]
const LIFE_GOODS := ["food", "water", "oxygen", "medicine"]

static func _count(value: float) -> String:
    var n := int(round(value))
    var text := str(absi(n))
    var grouped := ""
    while text.length() > 3:
        grouped = "," + text.substr(text.length() - 3) + grouped
        text = text.substr(0, text.length() - 3)
    return ("-" if n < 0 else "") + text + grouped

func _donut(title: String, slices: Array, width := 380.0) -> Control:
    var donut := DonutChartScript.new()
    donut.custom_minimum_size.x = width
    donut.set_data(title, slices, "%.0f")
    return donut

func _build_people() -> void:
    var stations: Array = stats.get("stations", [])
    var colonies: Array = stats.get("colonies", [])
    _heading("People")
    # Where people live, and what they do (all stations).
    var where: Array = []
    for i in range(stations.size()):
        var station: Dictionary = stations[i]
        where.append({"name": String(station.get("name", "")), "value": float(station.get("population", 0.0)),
            "color": UiTheme.categorical(i)})
    for i in range(colonies.size()):
        var colony: Dictionary = colonies[i]
        where.append({"name": String(colony.get("name", "")) + " (surface)", "value": float(colony.get("people", 0.0)),
            "color": UiTheme.categorical(stations.size() + i).darkened(0.25)})
    var what := {}
    var out_of_work := 0.0
    var dependants := 0.0
    var spacers := 0.0
    var visitors := 0.0
    for station in stations:
        var systems: Dictionary = station.get("systems", {})
        var by_kind: Dictionary = systems.get("jobs_by_kind", {})
        for kind in by_kind.keys():
            what[kind] = float(what.get(kind, 0.0)) + float(by_kind[kind])
        out_of_work += maxf(0.0, float(systems.get("workers", 0.0)) - float(systems.get("employed", 0.0)))
        dependants += float(systems.get("dependants", 0.0))
        spacers += float(systems.get("spacers", 0.0))
        visitors += float(systems.get("visitors", 0.0)) + float(systems.get("waiting", 0.0))
    var doing: Array = []
    for k in range(POP_KINDS.size()):
        doing.append({"name": String(POP_KIND_NAMES[POP_KINDS[k]]), "value": float(what.get(POP_KINDS[k], 0.0)),
            "color": UiTheme.categorical(k)})
    doing.append({"name": "out of work", "value": out_of_work, "color": UiTheme.ALERT})
    doing.append({"name": "dependants", "value": dependants, "color": UiTheme.categorical(8)})
    doing.append({"name": "spacers (crews)", "value": spacers, "color": UiTheme.categorical(5)})
    doing.append({"name": "visitors and travellers", "value": visitors, "color": UiTheme.categorical(7)})
    var donuts := _flow_row()
    donuts.add_child(_donut("Where people live (stations and colony surfaces)", where, 440.0))
    donuts.add_child(_donut("What station people do (jobs by kind, typical staffing)", doing, 440.0))
    _content.add_child(donuts)
    _note("Earth (billions) is left out. Jobs by kind are a typical staffing of each station's works and services, "
        + "derived from the economy's numbers; the simulation itself counts workers and jobs only.")

    # Residents by station: who they are, against housing.
    var parts := [{"name": "workers in jobs", "color": UiTheme.categorical(0)}, {"name": "out of work", "color": UiTheme.ALERT},
        {"name": "dependants", "color": UiTheme.categorical(8)}, {"name": "spacers", "color": UiTheme.categorical(5)},
        {"name": "visitors", "color": UiTheme.categorical(4)}, {"name": "waiting to travel", "color": UiTheme.categorical(7)}]
    var rows: Array = []
    var kind_rows: Array = []
    var needs_rows: Array = []
    for station in stations:
        var systems: Dictionary = station.get("systems", {})
        if systems.is_empty():
            continue
        var name := String(station.get("name", ""))
        var workers := float(systems.get("workers", 0.0))
        var employed := float(systems.get("employed", 0.0))
        rows.append({"name": name, "values": [employed, maxf(0.0, workers - employed), float(systems.get("dependants", 0.0)),
            float(systems.get("spacers", 0.0)), float(systems.get("visitors", 0.0)), float(systems.get("waiting", 0.0))],
            "marker": float(systems.get("housing", -1.0))})
        var by_kind: Dictionary = systems.get("jobs_by_kind", {})
        var kind_values: Array = []
        for kind in POP_KINDS:
            kind_values.append(float(by_kind.get(kind, 0.0)))
        kind_rows.append({"name": name, "values": kind_values})
        # Life needs: the share the station's own farms and recycling cover.
        var cover := {}
        for need in systems.get("needs", []):
            var n: Dictionary = need
            if String(n.get("tier", "")) == "life" and float(n.get("need_per_day", 0.0)) > 0.0:
                cover[String(n.get("good", ""))] = 1.0 - float(n.get("use_per_day", 0.0)) / float(n["need_per_day"])
            elif String(n.get("tier", "")) == "life" and bool(n.get("made_here", false)):
                cover[String(n.get("good", ""))] = 1.0
        var row: Array = [_station_buttons_row(station)]
        for good in LIFE_GOODS:
            if cover.has(good):
                var c := float(cover[good])
                row.append(["%.0f%%" % (100.0 * c), UiTheme.GOOD if c >= 0.9 else (UiTheme.WARN if c >= 0.5 else UiTheme.ALERT)])
            else:
                row.append(["imported", UiTheme.TEXT_DIM])
        needs_rows.append(row)
    var residents := StackedBarScript.new()
    residents.custom_minimum_size.x = 900.0
    residents.set_data("Residents by station (white line: housing)", parts, rows, false, "%.0f", "housing")
    _content.add_child(residents)
    var kind_parts: Array = []
    for k in range(POP_KINDS.size()):
        kind_parts.append({"name": String(POP_KIND_NAMES[POP_KINDS[k]]), "color": UiTheme.categorical(k)})
    var kinds := StackedBarScript.new()
    kinds.custom_minimum_size.x = 900.0
    kinds.set_data("Jobs by kind (typical staffing)", kind_parts, kind_rows, false, "%.0f")
    _content.add_child(kinds)
    _heading("Life needs met by the station itself (own farms and recycling)")
    _content.add_child(_table(["Station", "Food", "Water", "Oxygen", "Medicine"], needs_rows, [170.0, 80.0, 80.0, 80.0, 80.0]))
    _note("The rest is imported. Water and oxygen are mostly recycled; a station without farms imports all its food.")

    # People over time, stacked: stations and colony surfaces.
    var people_series: Array = []
    for i in range(stations.size()):
        people_series.append({"name": String(stations[i].get("name", "")).left(14), "color": UiTheme.categorical(i),
            "values": stations[i].get("people_history", [])})
    for i in range(colonies.size()):
        people_series.append({"name": String(colonies[i].get("name", "")).left(14),
            "color": UiTheme.categorical(stations.size() + i).darkened(0.25), "values": colonies[i].get("people_history", [])})
    var people_chart := LineChartScript.new()
    people_chart.custom_minimum_size = Vector2(900, 300)
    people_chart.stacked = true
    people_chart.format_value = func(v: float) -> String: return _count(v)
    _content.add_child(people_chart)
    people_chart.set_data("People over time (stations, then colony surfaces)", _history_days(), people_series)

    _heading("Residents' money by station")
    rows = []
    var chart_series: Array = []
    for i in range(stations.size()):
        var station: Dictionary = stations[i]
        var id := String(station.get("id", ""))
        var flows := _flows(_account("households:" + id))
        var totals := _totals(flows)
        var population := float(station.get("population", 0.0))
        var workers := float(station.get("workers", 0.0))
        var jobs := float(station.get("jobs", 0.0))
        var market := bool(station.get("labour_market", false))
        var savings := float(station.get("household_credits", 0.0))
        var gap := ""
        if market and jobs > workers + 0.5:
            gap = "%.0f open" % (jobs - workers)
        elif market and workers > jobs + 0.5:
            gap = "%.0f idle" % (workers - jobs)
        var rate: float = (totals.x - totals.y) / totals.x if totals.x > 0.0 else 0.0
        rows.append([_station_buttons_row(station), "%.0f" % population, "%.0f" % workers, gap,
            "%.2f" % float(station.get("wage", 0.0)), _cr(savings), "%.1f" % (savings / max(1.0, population)),
            [_cr(totals.x), UiTheme.GOOD], [_cr(totals.y), UiTheme.WARN], ["%.0f%%" % (100.0 * rate), _value_color(rate)]])
        chart_series.append({"name": String(station.get("name", id)).left(12), "color": SERIES_COLORS[i % SERIES_COLORS.size()],
            "values": _series("households:" + id)})
    _content.add_child(_table(["Station", "People", "Workers", "Jobs", "Wage cr/d", "Savings", "Per head", "Income",
        "Spending", "Saved"], rows, [170.0, 70.0, 70.0, 80.0, 70.0, 90.0, 60.0, 90.0, 90.0, 60.0]))
    _note("Income: wages, crew wages, hazard pay and the savings migrants bring. Spending: goods, services, fares, "
        + "visits and the savings emigrants take. Earth's stations have no labour market (everyone works).")
    if _selected_station != "":
        _content.add_child(_flow_table("Residents of " + _station_name(_selected_station), _flows(_account("households:" + _selected_station))))
    _chart("Residents' savings", chart_series)

func _build_colonies() -> void:
    _heading("Colonies (background economies)")
    var colonies: Array = stats.get("colonies", [])
    var rows: Array = []
    var welfare_rows: Array = []
    var chart_series: Array = []
    for i in range(colonies.size()):
        var colony: Dictionary = colonies[i]
        var id := String(colony.get("id", ""))
        var credits := float(colony.get("credits", 0.0))
        var line := float(colony.get("credit_line", 0.0))
        var headroom := credits + line
        rows.append([String(colony.get("name", id)), String(colony.get("gateways", "")), [_cr(credits), _value_color(credits)],
            _cr(line), [_cr(headroom), UiTheme.ALERT if headroom < 0.05 * max(1.0, line) else UiTheme.TEXT_PRIMARY],
            "%.0f" % float(colony.get("people", 0.0)),
            "%.2f / %.2f / %.2f" % [float(colony.get("wage_p10", 0.0)), float(colony.get("wage_p50", 0.0)), float(colony.get("wage_p90", 0.0))],
            "%.0f" % float(colony.get("hired_up", 0.0)), "%.0f" % float(colony.get("gone_down", 0.0)),
            _cr(float(colony.get("earth_investment_per_year", 0.0)))])
        var satisfaction := float(colony.get("satisfaction", 1.0))
        welfare_rows.append([String(colony.get("name", id)),
            ["%.0f%%" % (100.0 * satisfaction), UiTheme.GOOD if satisfaction >= 0.8 else (UiTheme.WARN if satisfaction >= 0.5 else UiTheme.ALERT)],
            "%.2f" % float(colony.get("throttle", 1.0)), _cr(float(colony.get("income_per_year", 0.0))),
            _cr(float(colony.get("imports_per_year", 0.0))),
            "%.2f (data %.2f)" % [float(colony.get("wage_p50", 0.0)), float(colony.get("wage_data", 0.0))]])
        chart_series.append({"name": String(colony.get("name", id)), "color": SERIES_COLORS[i + 1], "values": _series("colony:" + id)})
        if not _monthly:
            var limit: Array = []
            for d in _history_days():
                limit.append(-line)
            chart_series.append({"name": "limit", "color": SERIES_COLORS[i + 1].darkened(0.35), "values": limit, "dashed": true})
    _content.add_child(_table(["Colony", "Gateway", "Account", "Credit line", "Headroom", "People", "Wage p10/p50/p90",
        "Hired up", "Gone down", "Earth invests/yr"], rows, [130.0, 110.0, 90.0, 80.0, 80.0, 70.0, 130.0, 70.0, 70.0, 100.0]))
    _note("Wages are lognormal: p10, median and p90 in cr per day. Hired up: colonists who took the gateway's "
        + "jobs; gone down: its unemployed who moved to the surface (since the start).")
    _heading("Colony welfare")
    _content.add_child(_table(["Colony", "Imports met", "Can afford", "Income/yr", "Imports wanted/yr", "Wage median"],
        welfare_rows, [130.0, 90.0, 90.0, 100.0, 120.0, 130.0]))
    _note("The surface mines and sells at its gateway; it buys its imports from what it earns and Earth invests. "
        + "Short of money, it keeps buying food and medicine and cuts machinery and electronics first (can afford: "
        + "1 = in full, 0 = at its credit line). Imports met: the share of what its people want that it bought "
        + "(90-day average); growth follows it. Its wage median follows its income per head.")
    var tables := _flow_row()
    for colony in colonies:
        tables.add_child(_flow_table(String(colony.get("name", "")), _flows(_account("colony:" + String(colony.get("id", ""))))))
    _content.add_child(tables)
    _chart("Colony accounts against their credit lines", chart_series)
    _heading("Earth")
    _content.add_child(_flow_table("Earth's flows (negative net: paid into space)", _flows(_account("earth:earth"))))
    _chart("Earth", [{"name": "Earth", "color": SERIES_COLORS[0], "values": _series("earth:earth")}])

func _fleet_rows(list: Array, name_of: Callable) -> Array:
    var rows: Array = []
    for t in list:
        var ships := int(t.get("ships", 0))
        var profit := float(t.get("profit", 0.0))
        rows.append([String(name_of.call(String(t.get("key", "")))), "%d" % ships, "%d" % int(t.get("in_service", 0)),
            "%.0f%%" % (100.0 * float(t.get("profitable", 0)) / max(1, ships)),
            _cr(float(t.get("cargo_revenue", 0.0)) - float(t.get("cargo_purchases", 0.0))), _cr(float(t.get("fares", 0.0))),
            _cr(float(t.get("fuel", 0.0))), _cr(float(t.get("wages", 0.0))), _cr(float(t.get("capital", 0.0))),
            _cr(float(t.get("refits", 0.0)) + float(t.get("provisions", 0.0))), [_cr(profit), _value_color(profit)]])
    return rows

func _ship_button(ship: Dictionary) -> Button:
    var button := Button.new()
    button.focus_mode = Control.FOCUS_NONE  # keys stay with the map (Space, shortcuts)
    var sold := bool(ship.get("sold", false))
    button.text = "%s  %s%s" % [String(ship.get("name", "")), _cr(float(ship.get("profit", 0.0))), "  (sold)" if sold else ""]
    button.flat = true
    button.alignment = HORIZONTAL_ALIGNMENT_LEFT
    button.add_theme_font_size_override("font_size", 11)
    button.disabled = sold
    var id := String(ship.get("id", ""))
    button.pressed.connect(func() -> void: ship_selected.emit(id))
    return button

func _build_fleet() -> void:
    var fleet: Dictionary = stats.get("fleet", {})
    var headers := ["", "Ships ever", "In service", "Profitable", "Cargo margin", "Fares", "Fuel", "Crew", "Capital",
        "Refits, supplies", "Profit"]
    _heading("Fleet by faction (lifetime, sold ships included)")
    _content.add_child(_table(headers, _fleet_rows(fleet.get("by_faction", []), _faction_name)))
    _heading("Fleet by ship class")
    _content.add_child(_table(headers, _fleet_rows(fleet.get("by_class", []), func(key: String) -> String: return key)))
    var ranked := HBoxContainer.new()
    ranked.add_theme_constant_override("separation", 30)
    for part in [["Best ships", "top"], ["Worst ships", "bottom"]]:
        var box := VBoxContainer.new()
        box.add_child(_label(part[0] + " (click to select)", UiTheme.TEXT_PRIMARY, 12))
        for ship in fleet.get(part[1], []):
            box.add_child(_ship_button(ship))
        ranked.add_child(box)
    _content.add_child(ranked)
    var tables := _flow_row()
    var chart_series: Array = []
    var factions: Array = stats.get("factions", [])
    for i in range(factions.size()):
        var id := String(factions[i].get("id", ""))
        tables.add_child(_flow_table(String(factions[i].get("name", id)) + " fleet", _flows(_account("fleet:" + id))))
        chart_series.append({"name": String(factions[i].get("name", id)), "color": _faction_color(id, SERIES_COLORS[i]),
            "values": _series("fleet:" + id, true)})
    _content.add_child(tables)
    _chart("Fleets' cash" if not _monthly else "Fleets' result (owner transfers left out)", chart_series)

# --- Flows: what moved from place to place (the flow record) ---

const HeatmapScript := preload("res://scripts/ui/Heatmap.gd")
const PASSENGER_GROUPS := ["settlers", "rotation", "visitors", "emigrants"]

func _node_names() -> Dictionary:
    var names := {}
    for node in stats.get("flows", {}).get("nodes", []):
        names[String(node.get("id", ""))] = String(node.get("name", ""))
    return names

# A short column title for a place: "Low Earth", "Mars surface".
static func _short_place(name: String) -> String:
    var words := name.split(" ")
    var short := words[0]
    if words.size() > 1 and (short.length() <= 4 or words[1] == "surface" or words[0] == "Earth"):
        short += " " + words[1]
    return short.left(12)

func _flow_entries() -> Array:
    return stats.get("flows", {}).get(_period, {}).get("entries", [])

func _flow_controls() -> HBoxContainer:
    var row := HBoxContainer.new()
    row.add_theme_constant_override("separation", 6)
    var measure := Button.new()
    measure.focus_mode = Control.FOCUS_NONE  # keys stay with the map (Space, shortcuts)
    measure.text = "Measure: credits" if _flow_value else "Measure: units"
    measure.pressed.connect(func() -> void:
        _flow_value = not _flow_value
        _rebuild())
    row.add_child(measure)
    var goods := OptionButton.new()
    goods.focus_mode = Control.FOCUS_NONE
    goods.add_item("All goods")
    var ids: Array = stats.get("flows", {}).get("goods", [])
    for i in range(ids.size()):
        goods.add_item(String(ids[i]).replace("_", " "))
        if String(ids[i]) == _flow_good:
            goods.select(i + 1)
    goods.item_selected.connect(func(index: int) -> void:
        _flow_good = "" if index == 0 else String(ids[index - 1])
        _rebuild())
    row.add_child(goods)
    return row

func _build_flows() -> void:
    var flows: Dictionary = stats.get("flows", {})
    if flows.is_empty():
        _note("No flow record in this statistics file.")
        return
    var names := _node_names()
    var period_name := {"lifetime": "since the start", "last_year": "over the last year", "last_30": "over the last 30 days"}
    _heading("Goods from place to place, %s" % String(period_name.get(_period, _period)))
    _content.add_child(_flow_controls())

    # From x to: goods delivered (ships' cargo as it arrived, and surface trade).
    var matrix := {}
    var froms := {}
    var tos := {}
    var total := 0.0
    for e in _flow_entries():
        if String(e[0]) != "delivered" or (_flow_good != "" and String(e[3]) != _flow_good):
            continue
        var amount := float(e[5]) if _flow_value else float(e[4])
        var key := String(e[1]) + "|" + String(e[2])
        matrix[key] = float(matrix.get(key, 0.0)) + amount
        froms[String(e[1])] = float(froms.get(String(e[1]), 0.0)) + amount
        tos[String(e[2])] = float(tos.get(String(e[2]), 0.0)) + amount
        total += amount
    var order: Array = []
    for node in flows.get("nodes", []):
        order.append(String(node.get("id", "")))
    var row_ids := order.filter(func(id): return froms.has(id))
    var col_ids := order.filter(func(id): return tos.has(id))
    if total <= 0.0:
        _note("Nothing delivered in this period.")
    else:
        var top := 0.0
        for key in matrix.keys():
            top = maxf(top, float(matrix[key]))
        var grid := GridContainer.new()
        grid.columns = col_ids.size() + 2
        grid.add_theme_constant_override("h_separation", 2)
        grid.add_theme_constant_override("v_separation", 2)
        grid.add_child(_label("from \\ to", UiTheme.TEXT_DIM, 11, 150.0))
        for id in col_ids:
            var head := _label(_short_place(String(names.get(id, id))), UiTheme.TEXT_DIM, 10, 66.0, HORIZONTAL_ALIGNMENT_CENTER)
            head.tooltip_text = String(names.get(id, id))
            head.mouse_filter = Control.MOUSE_FILTER_PASS
            grid.add_child(head)
        grid.add_child(_label("total", UiTheme.TEXT_DIM, 11, 70.0, HORIZONTAL_ALIGNMENT_RIGHT))
        var fmt := func(v: float) -> String: return _cr(v) if _flow_value else _count(v)
        for from_id in row_ids:
            grid.add_child(_label(String(names.get(from_id, from_id)), UiTheme.TEXT_PRIMARY, 11, 150.0))
            for to_id in col_ids:
                var v := float(matrix.get(String(from_id) + "|" + String(to_id), 0.0))
                var cell := PanelContainer.new()
                cell.custom_minimum_size = Vector2(66, 18)
                var bg := Color(0.06, 0.08, 0.11)
                if v > 0.0:
                    bg = HeatmapScript.scale_color(sqrt(v / top)).darkened(0.25)
                cell.add_theme_stylebox_override("panel", UiTheme.flat_style(bg))
                cell.tooltip_text = "%s → %s: %s" % [names.get(from_id, from_id), names.get(to_id, to_id), fmt.call(v)]
                if v > 0.0:
                    var text := _label(fmt.call(v), Color.WHITE if sqrt(v / top) < 0.6 else Color.BLACK, 10, 0.0,
                        HORIZONTAL_ALIGNMENT_CENTER)
                    text.mouse_filter = Control.MOUSE_FILTER_IGNORE
                    cell.add_child(text)
                grid.add_child(cell)
            grid.add_child(_label(fmt.call(float(froms[from_id])), UiTheme.TEXT_PRIMARY, 11, 70.0, HORIZONTAL_ALIGNMENT_RIGHT))
        grid.add_child(_label("total", UiTheme.TEXT_DIM, 11, 150.0))
        for to_id in col_ids:
            grid.add_child(_label(fmt.call(float(tos[to_id])), UiTheme.TEXT_PRIMARY, 10, 66.0, HORIZONTAL_ALIGNMENT_CENTER))
        grid.add_child(_label(fmt.call(total), UiTheme.ACCENT, 11, 70.0, HORIZONTAL_ALIGNMENT_RIGHT))
        _content.add_child(grid)
        _note("Ships' cargo as it arrived (after spoilage), and the trade of Earth and the colonies' surfaces with their gateways. "
            + "Units of different goods are added as they are (a unit is the game's standard lot). Hover a cell for its places.")

    # Goods balance per station: imported, exported, stock change, and what it made net of use.
    _build_goods_balance(flows, names)
    _build_passenger_flows(names)
    _build_lanes(names)

func _build_goods_balance(flows: Dictionary, names: Dictionary) -> void:
    var period: Dictionary = flows.get(_period, {})
    var goods: Array = flows.get("goods", [])
    var start: Array = period.get("stock_start", [])
    var finish: Array = period.get("stock_end", [])
    var stations: Array = (flows.get("nodes", []) as Array).filter(func(n): return String(n.get("kind", "")) == "station")
    var imported := {}
    var exported := {}
    var spoiled := {}
    for e in _flow_entries():
        var kind := String(e[0])
        if _flow_good != "" and String(e[3]) != _flow_good:
            continue
        if kind == "delivered":
            imported[String(e[2])] = float(imported.get(String(e[2]), 0.0)) + float(e[4])
            if String(e[2]).begins_with("surface:"):
                exported[String(e[1])] = float(exported.get(String(e[1]), 0.0)) + float(e[4])
            elif not String(e[1]).begins_with("surface:"):
                spoiled[String(e[1])] = float(spoiled.get(String(e[1]), 0.0)) - float(e[4])
        elif kind == "loaded":
            exported[String(e[1])] = float(exported.get(String(e[1]), 0.0)) + float(e[4])
            spoiled[String(e[1])] = float(spoiled.get(String(e[1]), 0.0)) + float(e[4])
    var rows: Array = []
    for i in range(stations.size()):
        var id := String(stations[i].get("id", ""))
        var change := 0.0
        for g in range(goods.size()):
            if _flow_good != "" and String(goods[g]) != _flow_good:
                continue
            if i < start.size() and i < finish.size():
                change += float(finish[i][g]) - float(start[i][g])
        var imp := float(imported.get(id, 0.0))
        var exp := float(exported.get(id, 0.0))
        var made := change - imp + exp
        rows.append([String(names.get(id, id)), _count(imp), _count(exp), [_count(change), _value_color(change)],
            [_count(made), _value_color(made)]])
    _heading("Goods balance by station (units%s)" % ("" if _flow_good == "" else ", " + _flow_good.replace("_", " ")))
    _content.add_child(_table(["Station", "Imported", "Exported", "Stock change", "Made net of use"], rows,
        [170.0, 90.0, 90.0, 100.0, 120.0]))
    _note("Imported: cargo arriving and surface goods bought; exported: cargo loaded and goods sold to the surface. "
        + "Made net of use = stock change - imported + exported: positive for a producer, negative for a consumer "
        + "(it includes residents' use and any spoilage in store).")

func _build_passenger_flows(names: Dictionary) -> void:
    var routes := {}
    for e in _flow_entries():
        if String(e[0]) != "passengers":
            continue
        var key := String(e[1]) + "|" + String(e[2])
        var entry: Dictionary = routes.get(key, {"fares": 0.0})
        entry[String(e[3])] = float(entry.get(String(e[3]), 0.0)) + float(e[4])
        entry["fares"] = float(entry["fares"]) + float(e[5])
        routes[key] = entry
    _heading("Passengers landed, from place to place")
    if routes.is_empty():
        _note("No passengers landed in this period.")
        return
    var keys := routes.keys()
    keys.sort_custom(func(a, b): return float(routes[a]["fares"]) > float(routes[b]["fares"]))
    var rows: Array = []
    for key in keys:
        var parts: PackedStringArray = String(key).split("|")
        var entry: Dictionary = routes[key]
        var row: Array = [String(names.get(parts[0], parts[0])), String(names.get(parts[1], parts[1]))]
        for group in PASSENGER_GROUPS:
            row.append(_count(float(entry.get(group, 0.0))) if float(entry.get(group, 0.0)) > 0.0 else "")
        row.append(_cr(float(entry["fares"])))
        rows.append(row)
    _content.add_child(_table(["From", "To", "Settlers", "Crew rotation", "Visitors", "Emigrants", "Fares"], rows,
        [170.0, 170.0, 80.0, 100.0, 80.0, 80.0, 90.0]))
    _note("Settlers recruited by a colony are shown going on to its surface.")

func _build_lanes(names: Dictionary) -> void:
    var rows: Array = []
    var lanes: Array = []
    var carried := {}
    var by_lane := {}   # lanes are recorded by class: summed here
    for e in _flow_entries():
        if String(e[0]) == "lane":
            var key := String(e[1]) + "|" + String(e[2])
            if by_lane.has(key):
                var sum: Array = by_lane[key]
                for k in range(4, mini(sum.size(), e.size())):
                    sum[k] = float(sum[k]) + float(e[k])
            else:
                by_lane[key] = (e as Array).duplicate()
                lanes.append(by_lane[key])
        elif String(e[0]) == "delivered" and not String(e[1]).begins_with("surface:") and not String(e[2]).begins_with("surface:"):
            var key := String(e[1]) + "|" + String(e[2])
            carried[key] = float(carried.get(key, 0.0)) + float(e[4])
    _heading("Lanes: transport work and propellant")
    if lanes.is_empty():
        _note("No arrivals in this period.")
        return
    lanes.sort_custom(func(a, b): return float(a[5]) > float(b[5]))
    for e in lanes.slice(0, 20):
        var units := float(carried.get(String(e[1]) + "|" + String(e[2]), 0.0))
        var propellant_kg := float(e[5])
        rows.append([String(names.get(String(e[1]), e[1])), [String(names.get(String(e[2]), e[2])), UiTheme.TEXT_PRIMARY, "", 170.0],
            "%.0f" % float(e[6]), _count(units), _count(float(e[4])), "%.1f" % (propellant_kg / 1000.0),
            ["%.0f" % (propellant_kg / units), UiTheme.TEXT_PRIMARY] if units > 0.0 else ["empty", UiTheme.WARN]])
    _content.add_child(_table(["From", "To", "Trips", "Cargo units", "Unit-days", "Propellant t", "kg per unit"], rows,
        [170.0, 170.0, 60.0, 90.0, 100.0, 100.0, 90.0]))
    _note("Every arrival on the lane, loaded or not, with the propellant planned for the trip; cargo as it arrived. "
        + "kg per unit: the propellant burned for each unit delivered (empty legs count against the lane). The 20 lanes "
        + "that burn the most are shown.")

# --- Routes: porkchop plots (trajectory/Porkchop.hpp; the bridge fills them in the background) ---
#
# What a player asks of a route is "if I leave on day X with this ship, how long will it take?":
# both planners trade propellant against time (thermal: the cheapest of the arcs by fuel and time
# cost; plasma: the fastest trip for each of four fuel loads, the load by the same cost), so the
# first view is each class's fastest trip by departure date, with the ships on the route as dots
# (a dot above its line: the planner saved fuel). The maps behind it show one grid per drive, each
# class as the line its tank reaches. Classes shown: those that flew the route in the last year.

const PLANET_NAMES := {"mercury": "Mercury", "venus": "Venus", "earth": "Earth", "mars": "Mars", "ceres": "Ceres",
    "jupiter": "Jupiter", "saturn": "Saturn", "uranus": "Uranus", "neptune": "Neptune"}
const PorkchopsScript := preload("res://scripts/ui/Porkchops.gd")
const UiWidgets := preload("res://scripts/ui/UiWidgets.gd")
const FrontierChartScript := preload("res://scripts/ui/FrontierChart.gd")
const ROUTE_VIEWS := [["fastest", "Fastest trip by departure"], ["thermal", "Nuclear-thermal: Δv map"],
    ["plasma", "Plasma: effort map"]]

# Open the Routes tab on a pair (from the inspector's windows).
func show_route(origin: String, destination: String) -> void:
    route_origin = origin
    route_destination = destination
    _route_picked = true
    if not visible:
        toggle()
    set_tab(ROUTES_TAB)

func update_porkchops(data: Dictionary, ship_list: Array) -> void:
    porkchops = data
    ships = ship_list
    for pair in porkchops.get("pairs", []):
        _pair_cache["%s>%s" % [String(pair.get("origin", "")), String(pair.get("destination", ""))]] = pair
    if not visible or _tab != ROUTES_TAB:
        return
    # Until a route is picked, show the busiest one.
    var routes := PorkchopsScript.routes(porkchops)
    if not _route_picked and not routes.is_empty():
        _route_picked = true
        if String(routes[0].get("origin", "")) != route_origin or String(routes[0].get("destination", "")) != route_destination:
            route_origin = String(routes[0].get("origin", ""))
            route_destination = String(routes[0].get("destination", ""))
            _rebuild()
            return
    # New columns: refill the plot in place; rebuild only when there was no plot yet.
    if is_instance_valid(_route_map) and _route_map.is_inside_tree():
        _fill_route_view(_route_map)
        return
    var held := Input.is_mouse_button_pressed(MOUSE_BUTTON_LEFT) and get_global_rect().has_point(get_global_mouse_position())
    if not held:
        _rebuild()

func _route_pair() -> Dictionary:
    for pair in porkchops.get("pairs", []):
        if String(pair.get("origin", "")) == route_origin and String(pair.get("destination", "")) == route_destination:
            return pair
    return _pair_cache.get("%s>%s" % [route_origin, route_destination], {})

func _planet_name(id: String) -> String:
    return String(PLANET_NAMES.get(id, id.capitalize()))

func _route_controls() -> HBoxContainer:
    var row := HBoxContainer.new()
    row.add_theme_constant_override("separation", 6)
    # The routes ships fly, busiest first; "other pair" opens free pickers.
    var routes := PorkchopsScript.routes(porkchops)
    var picker := OptionButton.new()
    picker.focus_mode = Control.FOCUS_NONE
    var selected := -1
    for i in range(routes.size()):
        var r: Dictionary = routes[i]
        var trips := float(r.get("trips", 0.0))
        picker.add_item("%s → %s · %.0f %s in the last year" % [_planet_name(String(r.get("origin", ""))),
            _planet_name(String(r.get("destination", ""))), trips, "trip" if trips == 1.0 else "trips"])
        if String(r.get("origin", "")) == route_origin and String(r.get("destination", "")) == route_destination:
            selected = i
    picker.add_item("Other pair…")
    var other := _route_other or selected == -1
    picker.select(routes.size() if other else selected)
    picker.item_selected.connect(func(index: int) -> void:
        _route_picked = true
        if index < routes.size():
            _route_other = false
            route_origin = String(routes[index].get("origin", ""))
            route_destination = String(routes[index].get("destination", ""))
        else:
            _route_other = true
        _rebuild())
    row.add_child(picker)
    if other:
        var planets: Array = []
        for station_id in porkchops.get("stations", {}).keys():
            var planet := String(porkchops["stations"][station_id])
            if not planets.has(planet):
                planets.append(planet)
        for planet in [route_origin, route_destination]:
            if not planets.has(planet):
                planets.append(planet)
        for which in ["origin", "destination"]:
            var pick := OptionButton.new()
            pick.focus_mode = Control.FOCUS_NONE
            for i in range(planets.size()):
                pick.add_item(("from " if which == "origin" else "to ") + _planet_name(String(planets[i])))
                if planets[i] == (route_origin if which == "origin" else route_destination):
                    pick.select(i)
            pick.item_selected.connect(func(index: int) -> void:
                if which == "origin":
                    route_origin = planets[index]
                else:
                    route_destination = planets[index]
                _rebuild())
            row.add_child(pick)
    var view := OptionButton.new()
    view.focus_mode = Control.FOCUS_NONE
    for i in range(ROUTE_VIEWS.size()):
        view.add_item(String(ROUTE_VIEWS[i][1]))
        if ROUTE_VIEWS[i][0] == _route_view:
            view.select(i)
    view.item_selected.connect(func(index: int) -> void:
        _route_view = String(ROUTE_VIEWS[index][0])
        _rebuild())
    row.add_child(view)
    return row

# The classes on show, each with its colour: [{class, count, flown, color}].
func _route_classes() -> Array:
    var out := PorkchopsScript.route_classes(porkchops, route_origin, route_destination, ships)
    for i in range(out.size()):
        out[i]["color"] = UiTheme.categorical(i)
    return out

func _build_routes() -> void:
    _route_map = null
    _heading("Routes: %s to %s" % [_planet_name(route_origin), _planet_name(route_destination)])
    _content.add_child(_route_controls())
    if route_origin == route_destination:
        _note("Pick two different planets: stations around one planet have no launch windows.")
        return
    var pair := _route_pair()
    if pair.is_empty() or (pair.get("columns", []) as Array).is_empty():
        _note("Computing… the first columns appear within seconds (the outer planets take about a minute for the whole window; "
            + "routes ships fly are computed ahead and kept on disk, so they open at once next time).")
        return
    var classes := _route_classes()
    var view: Control
    if _route_view == "fastest":
        view = FrontierChartScript.new()
        view.custom_minimum_size = Vector2(1000, 420)
    else:
        view = HeatmapScript.new()
        view.custom_minimum_size = Vector2(1000, 460)
        view.x_label = "departure (game day)"
        view.y_label = "flight (days)"
    _fill_route_view(view)
    _route_map = view
    _content.add_child(view)
    _content.add_child(_route_legend(classes))
    _route_note(classes)

func _route_legend(classes: Array) -> Control:
    var flow := HFlowContainer.new()
    flow.add_theme_constant_override("h_separation", 14)
    for entry in classes:
        var item := HBoxContainer.new()
        var swatch := ColorRect.new()
        swatch.color = entry["color"]
        swatch.custom_minimum_size = Vector2(14, 4)
        swatch.size_flags_vertical = Control.SIZE_SHRINK_CENTER
        item.add_child(swatch)
        var c: Dictionary = entry["class"]
        var count := float(entry["count"])
        var reachable := PorkchopsScript.frontier(porkchops, _route_pair(), c, false).any(func(v): return not is_nan(float(v)))
        var label := UiWidgets.label("%s (%s) · %.0f %s%s" % [String(c.get("name", "")),
            "plasma" if PorkchopsScript.is_plasma(c) else "thermal", count,
            ("trip" if count == 1.0 else "trips") if bool(entry["flown"]) else "in the fleet",
            "" if reachable else " · cannot make this trip in the window"], UiTheme.TEXT_PRIMARY if reachable else UiTheme.TEXT_DIM, 11)
        item.add_child(label)
        flow.add_child(item)
    return flow

# The ships on the route now (waiting for their window or flying it): [{x, y, color, class_id}].
func _route_ships(classes: Array, drive := "") -> Array:
    var stations: Dictionary = porkchops.get("stations", {})
    var out: Array = []
    for ship in ships:
        var phase := String(ship.get("phase", ""))
        if phase != "awaiting_departure" and phase != "in_transit":
            continue
        if String(stations.get(String(ship.get("origin_station_id", "")), "")) != route_origin \
                or String(stations.get(String(ship.get("destination_station_id", "")), "")) != route_destination:
            continue
        var plasma := String(ship.get("propulsion_type", "")) == "variable_isp"
        if (drive == "plasma" and not plasma) or (drive == "thermal" and plasma):
            continue
        var color := Color(0.6, 0.6, 0.6)
        for entry in classes:
            if String(entry["class"].get("id", "")) == String(ship.get("class_id", "")):
                color = entry["color"]
        var dep := float(ship.get("departure_time_s", 0.0)) / 86400.0
        var arr := float(ship.get("arrival_time_s", 0.0)) / 86400.0
        out.append({"x": dep, "y": arr - dep, "color": color, "name": String(ship.get("name", ""))})
    return out

# The plot's data from the porkchop file: refilled in place as columns arrive (no rebuild of the tab).
func _fill_route_view(view: Control) -> void:
    var pair := _route_pair()
    var classes := _route_classes()
    var columns: Array = pair.get("columns", [])
    var xs: Array = []
    for column in columns:
        xs.append(float(column.get("day", 0.0)))
    var now_day := float(porkchops.get("now_day", NAN))
    if _route_view == "fastest":
        var series: Array = []
        for entry in classes:
            var c: Dictionary = entry["class"]
            series.append({"name": String(c.get("name", "")), "color": entry["color"],
                "values": PorkchopsScript.frontier(porkchops, pair, c, true)})
            series.append({"name": String(c.get("name", "")) + ", empty", "color": Color(entry["color"], 0.7), "dashed": true,
                "values": PorkchopsScript.frontier(porkchops, pair, c, false)})
        view.set_data("Fastest trip by departure date: solid with a full hold, dashed empty (fuel kept back: the planner's 10% "
            + "reserve); dots: ships on the route", xs, series, _route_ships(classes), now_day)
        return
    var plasma := _route_view == "plasma"
    var rows: Array = pair.get("plasma_transit_days" if plasma else "transit_days", [])
    var grid: Array = []
    var lo := INF
    var hi := -INF
    for column in columns:
        var raw: Array = column.get("effort" if plasma else "dv", [])
        var values: Array = []
        for r in range(rows.size()):
            var v := NAN if r >= raw.size() or raw[r] == null else float(raw[r]) / (1.0 if plasma else 1000.0)
            values.append(v)
            if not is_nan(v):
                lo = minf(lo, v)
                hi = maxf(hi, v)
        grid.append(values)
    var contours: Array = []
    var drive_classes := classes.filter(func(e): return PorkchopsScript.is_plasma(e["class"]) == plasma)
    for entry in drive_classes:
        var level := PorkchopsScript.reach(porkchops, entry["class"], true) / (1.0 if plasma else 1000.0)
        contours.append({"level": level / (lo if plasma else 1.0), "color": entry["color"]})
    if plasma:
        # Effort relative to the window's best: class free and readable (×2: twice the best).
        for col in grid:
            for r in range(col.size()):
                if not is_nan(col[r]):
                    col[r] = float(col[r]) / lo
        hi = minf(hi / lo, 1000.0)
        view.format_value = func(v: float) -> String: return "×" + HeatmapScript.significant(v, 2)
        view.unit = "of best"
        view.y_log = true
        view.set_grid("Plasma drive effort ∫a²dt, as a multiple of the window's best (log scale); lines: what each class's tank "
            + "reaches with a full hold (inside the line it can go)", grid, xs, rows, 1.0, hi, true)
    else:
        view.value_format = "%.1f"
        view.unit = "km/s"
        view.y_log = false
        view.set_grid("Nuclear-thermal Δv (both burns, km/s); lines: what each class's tank reaches with a full hold (inside the "
            + "line it can go)", grid, xs, rows, lo, minf(hi, lo * 2.5), false)
    view.contours = contours
    view.now_x = now_day
    view.markers = _route_ships(classes, "plasma" if plasma else "thermal")
    var best := lo
    view.readout = func(col: int, row: int) -> String:
        var dep := float(xs[col])
        var flight := float(rows[row])
        var v: float = grid[col][row]
        var text := "depart day %.0f (in %.0f d) · flight %.0f d · arrive day %.0f" % [dep, dep - now_day, flight, dep + flight]
        if is_nan(v):
            return text + "\nno transfer"
        text += ("\neffort ×%s of the best" % HeatmapScript.significant(v, 3)) if plasma else ("\nΔv %.2f km/s" % v)
        for entry in drive_classes:
            var share := PorkchopsScript.propellant_share(porkchops, entry["class"], v * best if plasma else v * 1000.0, true)
            text += "\n%s: %s" % [String(entry["class"].get("name", "")),
                "%.0f%% of the tank" % (100.0 * share) if share <= 1.0 else "cannot (needs %s the tank)" % (
                    "%.1f×" % share if share < 50.0 else "far more than")]
        return text
    view.queue_redraw()

func _route_note(classes: Array) -> void:
    var pair := _route_pair()
    var text := "Classes: those that flew this route in the last year (the busiest six), else the commonest in the fleet. "
    if _route_view == "fastest":
        text += "Each line is the shortest flight the class's tank allows at each departure (gaps: it cannot go); both planners "
        text += "trade propellant against time, so a ship's dot above its line means the planner chose to save fuel. "
    elif _route_view == "thermal":
        text += "Each column is a departure date, each row a flight time. Δv is what the planner charges for the arc (against "
        text += "circular speed at both ends; it searches 0.3 to 1.5 Hohmann times); hover for each class's share of its tank. "
    else:
        text += "Each column is a departure date, each row a flight time (log scale). Effort = ∫a²dt: the drive's push "
        text += "(acceleration) squared, added up over the flight, for the cheapest path that arrives at that time. It is what a "
        text += "plasma drive (fixed power, variable exhaust speed) pays for: 1/m_end − 1/m_start = effort / (2 × drive power), so "
        text += "more power or a lighter ship carries the same path on less propellant. It depends only on the path, so one map "
        text += "serves every ship; shown as a multiple of the window's cheapest cell (×10: ten times its effort). Hover for each "
        text += "class's share of its tank. "
    text += "Full hold: cargo at 100 kg a unit. The plot covers one synodic period (at most two years) from now. Hohmann time "
    text += "%.0f d, synodic period %.0f d." % [float(pair.get("hohmann_days", 0.0)), float(pair.get("synodic_days", 0.0))]
    _note(text)
