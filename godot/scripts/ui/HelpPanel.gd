# The key bindings table (H to show; H, Esc or Close to hide).
extends PanelContainer

const UiTheme := preload("res://scripts/ui/UiTheme.gd")
const KeyBindings := preload("res://scripts/ui/KeyBindings.gd")

func _ready() -> void:
    visible = false
    add_theme_stylebox_override("panel", UiTheme.panel_style(UiTheme.PANEL_BG_SOLID, UiTheme.ACCENT_DIM))
    var box := VBoxContainer.new()
    box.add_theme_constant_override("separation", 8)
    add_child(box)

    var header := HBoxContainer.new()
    box.add_child(header)
    var title := Label.new()
    title.text = "KEY BINDINGS   (H or Esc to close)"
    title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
    title.add_theme_color_override("font_color", UiTheme.ACCENT)
    title.add_theme_font_size_override("font_size", 14)
    header.add_child(title)
    var close := Button.new()
    close.text = "Close"
    close.focus_mode = Control.FOCUS_NONE
    close.pressed.connect(hide)
    header.add_child(close)

    var grid := GridContainer.new()
    grid.columns = 2
    grid.add_theme_constant_override("h_separation", 18)
    grid.add_theme_constant_override("v_separation", 3)
    box.add_child(grid)
    var group := ""
    for binding in KeyBindings.BINDINGS:
        if binding["group"] != group:
            group = binding["group"]
            grid.add_child(_label(group.to_upper(), UiTheme.TEXT_DIM, 11))
            grid.add_child(Control.new())
        grid.add_child(_label(binding["keys"], UiTheme.CREDITS, 13))
        grid.add_child(_label(binding["action"], UiTheme.TEXT_PRIMARY, 13))

func toggle() -> void:
    visible = not visible

func _label(text: String, color: Color, font_size: int) -> Label:
    var label := Label.new()
    label.text = text
    label.add_theme_color_override("font_color", color)
    label.add_theme_font_size_override("font_size", font_size)
    return label
