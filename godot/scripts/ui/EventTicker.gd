extends PanelContainer

const UiTheme := preload("res://scripts/ui/UiTheme.gd")

const MAX_LINES := 14

var _text: RichTextLabel
var _last_signature := ""

func _ready() -> void:
    custom_minimum_size = Vector2(470, 190)
    var box := VBoxContainer.new()
    add_child(box)
    var title := Label.new()
    title.text = "COMMS LOG"
    title.add_theme_color_override("font_color", UiTheme.ACCENT)
    title.add_theme_font_size_override("font_size", 12)
    box.add_child(title)
    _text = RichTextLabel.new()
    _text.bbcode_enabled = true
    _text.scroll_active = false
    _text.fit_content = false
    _text.size_flags_vertical = Control.SIZE_EXPAND_FILL
    box.add_child(_text)

func _category_color(event: Dictionary) -> Color:
    var category := String(event.get("category", ""))
    if UiTheme.CATEGORY_COLORS.has(category):
        return UiTheme.CATEGORY_COLORS[category]
    # Fallback for snapshots from older bridges: classify by keywords.
    var text := String(event.get("text", ""))
    if text.contains("stranded") or text.contains("jettisoned"):
        return UiTheme.CATEGORY_COLORS["alert"]
    if text.contains("arrived"):
        return UiTheme.CATEGORY_COLORS["arrival"]
    if text.contains("refueled"):
        return UiTheme.CATEGORY_COLORS["fuel"]
    if text.contains("departed") or text.contains("scheduled"):
        return UiTheme.CATEGORY_COLORS["mission"]
    return UiTheme.CATEGORY_COLORS["general"]

func update_events(events: Array) -> void:
    var lines: Array[String] = []
    var start: int = max(events.size() - MAX_LINES, 0)
    for i in range(events.size() - 1, start - 1, -1):
        var event: Dictionary = events[i]
        var day := float(event.get("time_s", 0.0)) / 86400.0
        var color := _category_color(event)
        # Older entries fade toward the bottom of the list.
        var age_rank := lines.size()
        var alpha: float = clamp(1.0 - float(age_rank) * 0.055, 0.35, 1.0)
        var faded := Color(color.r, color.g, color.b, alpha)
        lines.append("[color=#%s]D%.1f  %s[/color]" % [
            faded.to_html(true), day, String(event.get("text", ""))])
    var content := "\n".join(lines)
    if content == _last_signature:
        return
    _last_signature = content
    _text.text = content
