extends PanelContainer

# The Statistics screen (S): money by sector, who pays whom, and every account's income and
# expense by category (the simulation's money journal), with monthly history. The data comes
# from the bridge's stats side file, written only while this screen is open.

signal ship_selected(ship_id: String)
signal visibility_toggled(open: bool)

const UiTheme := preload("res://scripts/ui/UiTheme.gd")
const LineChartScript := preload("res://scripts/ui/LineChart.gd")

const TABS := ["Overview", "Factions", "Stations", "Residents", "Colonies & Earth", "Fleet"]
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
    if visible and not held:
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
        3: _build_residents()
        4: _build_colonies()
        5: _build_fleet()

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

func _build_residents() -> void:
    _heading("Residents (workers) by station")
    var rows: Array = []
    var chart_series: Array = []
    var stations: Array = stats.get("stations", [])
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
