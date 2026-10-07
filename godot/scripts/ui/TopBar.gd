extends PanelContainer

const UiTheme := preload("res://scripts/ui/UiTheme.gd")

signal pause_toggled
signal timewarp_changed(factor: float)

const TIMEWARP_PRESETS := {
    600.0: "10m/s",
    3600.0: "1h/s",
    21600.0: "6h/s",
    86400.0: "1d/s",
    432000.0: "5d/s",
}

var _date_label: Label
var _pause_button: Button
var _warp_buttons: Dictionary = {}
var _fleet_label: Label
var _money_label: Label

func _ready() -> void:
    var row := HBoxContainer.new()
    row.add_theme_constant_override("separation", 14)
    add_child(row)

    var title := Label.new()
    title.text = "SPACETRAINS"
    title.add_theme_font_size_override("font_size", 16)
    title.add_theme_color_override("font_color", UiTheme.ACCENT)
    row.add_child(title)

    _date_label = Label.new()
    _date_label.custom_minimum_size.x = 110.0
    row.add_child(_date_label)

    _pause_button = Button.new()
    _pause_button.text = "⏸ PAUSE"
    _pause_button.focus_mode = Control.FOCUS_NONE
    _pause_button.pressed.connect(func() -> void: pause_toggled.emit())
    row.add_child(_pause_button)

    var warp_row := HBoxContainer.new()
    warp_row.add_theme_constant_override("separation", 2)
    row.add_child(warp_row)
    for factor in TIMEWARP_PRESETS.keys():
        var button := Button.new()
        button.text = TIMEWARP_PRESETS[factor]
        button.focus_mode = Control.FOCUS_NONE
        button.pressed.connect(func() -> void: timewarp_changed.emit(factor))
        warp_row.add_child(button)
        _warp_buttons[factor] = button

    var spacer := Control.new()
    spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
    row.add_child(spacer)

    _fleet_label = Label.new()
    _fleet_label.add_theme_color_override("font_color", UiTheme.TEXT_DIM)
    row.add_child(_fleet_label)

    _money_label = Label.new()
    _money_label.add_theme_color_override("font_color", UiTheme.CREDITS)
    row.add_child(_money_label)

func update_state(state: Dictionary, paused: bool, timewarp: float) -> void:
    var day := float(state.get("game_time_days", 0.0))
    var year := int(day / 365.25)
    var day_of_year := day - float(year) * 365.25
    _date_label.text = "Y%d D%05.1f" % [year + 1, day_of_year]

    _pause_button.text = "▶ RESUME" if paused else "⏸ PAUSE"
    _pause_button.add_theme_color_override(
        "font_color", UiTheme.WARN if paused else UiTheme.TEXT_PRIMARY)

    var active_warp := float(state.get("timewarp_factor", timewarp))
    for factor in _warp_buttons.keys():
        var button: Button = _warp_buttons[factor]
        var active: bool = is_equal_approx(active_warp, factor)
        button.add_theme_color_override("font_color", UiTheme.ACCENT if active else UiTheme.TEXT_DIM)

    var hauling := 0
    var idle := 0
    var stranded := 0
    var laid_up := 0
    var refitting := 0
    var in_transit := 0
    for ship in state.get("ships", []):
        var phase := String(ship.get("phase", "idle"))
        if phase == "in_transit" or phase == "awaiting_departure":
            in_transit += 1
            if float(ship.get("cargo_units", 0.0)) > 0.0:
                hauling += 1
        elif phase == "stranded":
            stranded += 1
        elif phase == "laid_up":
            laid_up += 1
        elif phase == "refitting":
            refitting += 1
        else:
            idle += 1
    var fleet_text := "Fleet: %d hauling / %d moving / %d docked" % [hauling, in_transit, idle]
    if laid_up > 0:
        fleet_text += " / %d laid up" % laid_up
    if refitting > 0:
        fleet_text += " / %d in the yard" % refitting
    if stranded > 0:
        fleet_text += " / %d STRANDED" % stranded
    _fleet_label.text = fleet_text
    _fleet_label.add_theme_color_override(
        "font_color", UiTheme.ALERT if stranded > 0 else UiTheme.TEXT_DIM)

    _money_label.text = "Σ " + UiTheme.format_credits(float(state.get("total_credits", 0.0)))
