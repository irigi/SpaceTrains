extends RefCounted

# Shared palette for all HUD panels. Everything is generated at runtime —
# no font or texture assets are bundled with the project.
const ACCENT := Color(0.35, 0.85, 0.95)
const ACCENT_DIM := Color(0.35, 0.85, 0.95, 0.45)
const PANEL_BG := Color(0.05, 0.08, 0.12, 0.82)
const PANEL_BG_SOLID := Color(0.06, 0.09, 0.14, 0.96)
const TEXT_PRIMARY := Color(0.88, 0.93, 0.97)
const TEXT_DIM := Color(0.55, 0.63, 0.72)
const WARN := Color(1.0, 0.72, 0.25)
const ALERT := Color(1.0, 0.32, 0.25)
const GOOD := Color(0.45, 0.9, 0.55)
const CREDITS := Color(0.95, 0.85, 0.4)

const CATEGORY_COLORS := {
    "mission": Color(0.55, 0.8, 1.0),
    "arrival": Color(0.45, 0.9, 0.55),
    "trade": Color(0.95, 0.85, 0.4),
    "fuel": Color(0.85, 0.65, 0.95),
    "alert": Color(1.0, 0.42, 0.32),
    "news": Color(0.45, 0.9, 0.85),
    "general": Color(0.7, 0.75, 0.8),
}

static func panel_style(bg := PANEL_BG, border := ACCENT_DIM) -> StyleBoxFlat:
    var style := StyleBoxFlat.new()
    style.bg_color = bg
    style.border_color = border
    style.set_border_width_all(1)
    style.set_corner_radius_all(3)
    style.set_content_margin_all(10)
    return style

static func flat_style(bg: Color) -> StyleBoxFlat:
    var style := StyleBoxFlat.new()
    style.bg_color = bg
    style.set_corner_radius_all(2)
    style.set_content_margin_all(0)
    return style

static func build() -> Theme:
    var theme := Theme.new()
    theme.default_font_size = 13

    theme.set_stylebox("panel", "PanelContainer", panel_style())

    # Buttons: dark flat, accent highlight on hover, filled when pressed/toggled.
    var button_normal := panel_style(Color(0.08, 0.12, 0.18, 0.9), Color(0.3, 0.45, 0.55, 0.6))
    button_normal.set_content_margin_all(0)
    button_normal.content_margin_left = 10.0
    button_normal.content_margin_right = 10.0
    button_normal.content_margin_top = 4.0
    button_normal.content_margin_bottom = 4.0
    var button_hover: StyleBoxFlat = button_normal.duplicate()
    button_hover.border_color = ACCENT
    button_hover.bg_color = Color(0.1, 0.16, 0.24, 0.95)
    var button_pressed: StyleBoxFlat = button_normal.duplicate()
    button_pressed.bg_color = Color(0.16, 0.38, 0.46, 0.95)
    button_pressed.border_color = ACCENT
    theme.set_stylebox("normal", "Button", button_normal)
    theme.set_stylebox("hover", "Button", button_hover)
    theme.set_stylebox("pressed", "Button", button_pressed)
    theme.set_stylebox("focus", "Button", StyleBoxEmpty.new())
    theme.set_color("font_color", "Button", TEXT_PRIMARY)
    theme.set_color("font_hover_color", "Button", Color.WHITE)
    theme.set_color("font_pressed_color", "Button", Color.WHITE)
    theme.set_font_size("font_size", "Button", 13)

    theme.set_color("font_color", "Label", TEXT_PRIMARY)

    # Entity list: translucent rows, accent selection.
    var list_bg := panel_style(Color(0.03, 0.05, 0.09, 0.6), Color(0.3, 0.45, 0.55, 0.35))
    list_bg.set_content_margin_all(4)
    theme.set_stylebox("panel", "ItemList", list_bg)
    var list_selected := flat_style(Color(0.16, 0.38, 0.46, 0.85))
    theme.set_stylebox("selected", "ItemList", list_selected)
    theme.set_stylebox("selected_focus", "ItemList", list_selected)
    theme.set_color("font_color", "ItemList", TEXT_PRIMARY)
    theme.set_color("font_selected_color", "ItemList", Color.WHITE)
    theme.set_font_size("font_size", "ItemList", 13)

    # Tabs.
    theme.set_color("font_selected_color", "TabBar", ACCENT)
    theme.set_color("font_unselected_color", "TabBar", TEXT_DIM)
    theme.set_font_size("font_size", "TabBar", 13)
    var tab_selected := flat_style(Color(0.1, 0.18, 0.26, 0.9))
    tab_selected.border_color = ACCENT
    tab_selected.border_width_bottom = 2
    tab_selected.content_margin_left = 10.0
    tab_selected.content_margin_right = 10.0
    tab_selected.content_margin_top = 4.0
    tab_selected.content_margin_bottom = 4.0
    var tab_unselected: StyleBoxFlat = tab_selected.duplicate()
    tab_unselected.bg_color = Color(0.05, 0.08, 0.12, 0.5)
    tab_unselected.border_width_bottom = 0
    theme.set_stylebox("tab_selected", "TabBar", tab_selected)
    theme.set_stylebox("tab_unselected", "TabBar", tab_unselected)
    theme.set_stylebox("tab_hovered", "TabBar", tab_unselected)

    # Progress bars: thin dark track with accent fill (recolored per-use).
    var bar_bg := flat_style(Color(0.04, 0.07, 0.1, 0.9))
    bar_bg.border_color = Color(0.3, 0.45, 0.55, 0.4)
    bar_bg.set_border_width_all(1)
    theme.set_stylebox("background", "ProgressBar", bar_bg)
    theme.set_stylebox("fill", "ProgressBar", flat_style(ACCENT_DIM))
    theme.set_font_size("font_size", "ProgressBar", 10)

    # Filter line edit.
    var edit_bg := flat_style(Color(0.04, 0.07, 0.1, 0.9))
    edit_bg.border_color = Color(0.3, 0.45, 0.55, 0.5)
    edit_bg.set_border_width_all(1)
    edit_bg.set_content_margin_all(4)
    theme.set_stylebox("normal", "LineEdit", edit_bg)
    var edit_focus: StyleBoxFlat = edit_bg.duplicate()
    edit_focus.border_color = ACCENT
    theme.set_stylebox("focus", "LineEdit", edit_focus)
    theme.set_color("font_color", "LineEdit", TEXT_PRIMARY)
    theme.set_color("font_placeholder_color", "LineEdit", TEXT_DIM)

    theme.set_stylebox("normal", "RichTextLabel", StyleBoxEmpty.new())
    theme.set_color("default_color", "RichTextLabel", TEXT_PRIMARY)
    theme.set_font_size("normal_font_size", "RichTextLabel", 12)

    return theme

static func bar_fill_style(color: Color) -> StyleBoxFlat:
    return flat_style(Color(color.r, color.g, color.b, 0.7))

static func format_credits(amount: float) -> String:
    if abs(amount) >= 1_000_000.0:
        return "%.2fM cr" % (amount / 1_000_000.0)
    if abs(amount) >= 10_000.0:
        return "%.1fk cr" % (amount / 1_000.0)
    return "%.0f cr" % amount
