# The key bindings table (H to show; H, Esc or Close to hide), and the credits (music, planet maps,
# fonts) from assets/CREDITS.txt, written by tools/package/make_credits.py.
extends PanelContainer

const UiTheme := preload("res://scripts/ui/UiTheme.gd")
const KeyBindings := preload("res://scripts/ui/KeyBindings.gd")
const CREDITS_PATH := "res://assets/CREDITS.txt"

var title: Label
var grid: GridContainer
var credits: ScrollContainer
var credits_button: Button

func _ready() -> void:
    visible = false
    add_theme_stylebox_override("panel", UiTheme.panel_style(UiTheme.PANEL_BG_SOLID, UiTheme.ACCENT_DIM))
    var box := VBoxContainer.new()
    box.add_theme_constant_override("separation", 8)
    add_child(box)

    var header := HBoxContainer.new()
    box.add_child(header)
    title = Label.new()
    title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
    title.add_theme_color_override("font_color", UiTheme.ACCENT)
    title.add_theme_font_size_override("font_size", 14)
    header.add_child(title)
    credits_button = Button.new()
    credits_button.focus_mode = Control.FOCUS_NONE
    credits_button.pressed.connect(func() -> void: _show_credits(not credits.visible))
    header.add_child(credits_button)
    var close := Button.new()
    close.text = "Close"
    close.focus_mode = Control.FOCUS_NONE
    close.pressed.connect(hide)
    header.add_child(close)

    grid = GridContainer.new()
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

    credits = ScrollContainer.new()
    credits.custom_minimum_size = Vector2(760, 520)
    credits.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
    box.add_child(credits)
    var text := _label(_credits_text(), UiTheme.TEXT_PRIMARY, 12)
    text.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
    text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
    credits.add_child(text)
    _show_credits(false)

func toggle() -> void:
    visible = not visible
    if visible:
        _show_credits(false)

func _show_credits(on: bool) -> void:
    credits.visible = on
    grid.visible = not on
    title.text = ("CREDITS" if on else "KEY BINDINGS") + "   (H or Esc to close)"
    credits_button.text = "Keys" if on else "Credits"

func _credits_text() -> String:
    var file := FileAccess.open(CREDITS_PATH, FileAccess.READ)
    if file == null:
        return "Credits: %s is missing." % CREDITS_PATH
    return file.get_as_text()

func _label(text: String, color: Color, font_size: int) -> Label:
    var label := Label.new()
    label.text = text
    label.add_theme_color_override("font_color", color)
    label.add_theme_font_size_override("font_size", font_size)
    return label
