class_name EventLog
extends PanelContainer
## Displays recent simulation events in a scrollable log.

const MAX_ENTRIES: int = 100

var log_text: RichTextLabel
var filter_trade: CheckBox
var filter_economy: CheckBox
var filter_system: CheckBox

var entries: Array = []  # [{category, message, time}]
var active_filters: Dictionary = {
	"trade": true,
	"economy": true,
	"system": true,
	"security": true,
}

func _ready() -> void:
	EventBus.log_event.connect(_on_log_event)

	if filter_trade:
		filter_trade.toggled.connect(func(on): active_filters["trade"] = on; _refresh())
	if filter_economy:
		filter_economy.toggled.connect(func(on): active_filters["economy"] = on; _refresh())
	if filter_system:
		filter_system.toggled.connect(func(on): active_filters["system"] = on; _refresh())

func _on_log_event(category: String, message: String) -> void:
	entries.push_back({
		"category": category,
		"message": message,
		"time": Time.get_ticks_msec(),
	})
	if entries.size() > MAX_ENTRIES:
		entries.pop_front()
	_refresh()

func _refresh() -> void:
	if log_text == null:
		return

	var text := ""
	for entry in entries:
		if not active_filters.get(entry["category"], true):
			continue
		var color = _category_color(entry["category"])
		text += "[color=%s][%s][/color] %s\n" % [color, entry["category"].to_upper(), entry["message"]]

	log_text.text = text
	log_text.scroll_following = true

func _category_color(category: String) -> String:
	match category:
		"trade":
			return "#44aaff"
		"economy":
			return "#ffaa44"
		"security":
			return "#ff4444"
		"system":
			return "#88ff88"
		_:
			return "#cccccc"
