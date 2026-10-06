extends PanelContainer

const UiTheme := preload("res://scripts/ui/UiTheme.gd")

signal entity_selected(entity_id: String, kind: String)

const TABS := ["Ships", "Stations", "Bodies"]
const TAB_KINDS := ["ship", "station", "body"]

var _tab_bar: TabBar
var _filter: LineEdit
var _list: ItemList
var _collapse_button: Button
var _content: VBoxContainer
var _collapsed := false
var _current_signature := ""
var _row_ids: Array[String] = []
var _last_details: Dictionary = {}
var _last_kinds: Dictionary = {}
var _selected_id := ""

func _ready() -> void:
    custom_minimum_size = Vector2(290, 0)
    _content = VBoxContainer.new()
    _content.add_theme_constant_override("separation", 6)
    add_child(_content)

    var header := HBoxContainer.new()
    _content.add_child(header)
    var title := Label.new()
    title.text = "REGISTRY"
    title.add_theme_color_override("font_color", UiTheme.ACCENT)
    title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
    header.add_child(title)
    _collapse_button = Button.new()
    _collapse_button.text = "◀"
    _collapse_button.focus_mode = Control.FOCUS_NONE
    _collapse_button.pressed.connect(_toggle_collapsed)
    header.add_child(_collapse_button)

    _tab_bar = TabBar.new()
    for tab_name in TABS:
        _tab_bar.add_tab(tab_name)
    _tab_bar.tab_changed.connect(func(_index: int) -> void: _rebuild(true))
    _content.add_child(_tab_bar)

    _filter = LineEdit.new()
    _filter.placeholder_text = "filter…"
    _filter.text_changed.connect(func(_text: String) -> void: _rebuild(true))
    _content.add_child(_filter)

    _list = ItemList.new()
    _list.size_flags_vertical = Control.SIZE_EXPAND_FILL
    _list.item_selected.connect(_on_item_selected)
    _content.add_child(_list)

func _toggle_collapsed() -> void:
    _collapsed = not _collapsed
    _tab_bar.visible = not _collapsed
    _filter.visible = not _collapsed
    _list.visible = not _collapsed
    _collapse_button.text = "▶" if _collapsed else "◀"
    custom_minimum_size = Vector2(36, 0) if _collapsed else Vector2(290, 0)
    size.x = custom_minimum_size.x

func update_entities(details: Dictionary, kinds: Dictionary, selected_id: String) -> void:
    _last_details = details
    _last_kinds = kinds
    _selected_id = selected_id
    if _collapsed:
        return
    _rebuild(false)

func set_selected(entity_id: String) -> void:
    _selected_id = entity_id
    for i in range(_row_ids.size()):
        if _row_ids[i] == entity_id:
            _list.select(i)
            return
    _list.deselect_all()

func _ship_status(detail: Dictionary) -> Array:
    var phase := String(detail.get("phase", "idle"))
    match phase:
        "in_transit":
            return ["▶", UiTheme.ACCENT]
        "awaiting_departure":
            return ["◆", UiTheme.WARN]
        "stranded":
            return ["✖", UiTheme.ALERT]
        "laid_up":
            return ["◌", UiTheme.TEXT_DIM]
        _:
            return ["●", UiTheme.GOOD]

func _station_alert(detail: Dictionary) -> bool:
    # Alert when any consumed commodity is under a week of stock.
    var rates: Dictionary = detail.get("net_rates", {})
    var inventory: Dictionary = detail.get("inventory", {})
    for commodity_id in rates.keys():
        var rate := float(rates[commodity_id])
        if rate >= 0.0:
            continue
        var stock := float(inventory.get(commodity_id, 0.0))
        if stock / abs(rate) < 7.0:
            return true
    return false

func _rebuild(force: bool) -> void:
    var kind: String = TAB_KINDS[_tab_bar.current_tab]
    var filter_text := _filter.text.to_lower()
    var ids: Array = _last_details.keys()
    ids.sort()

    var rows: Array = []
    var signature_parts: Array[String] = [kind, filter_text]
    for entity_id in ids:
        if String(_last_kinds.get(entity_id, "")) != kind:
            continue
        var detail: Dictionary = _last_details[entity_id]
        var display_name := String(detail.get("name", entity_id))
        if filter_text != "" and not display_name.to_lower().contains(filter_text):
            continue
        var glyph := ""
        var color := UiTheme.TEXT_PRIMARY
        if kind == "ship":
            var status := _ship_status(detail)
            glyph = status[0]
            color = status[1]
        elif kind == "station":
            if _station_alert(detail):
                glyph = "⚠"
                color = UiTheme.WARN
            else:
                glyph = "◇"
                color = UiTheme.TEXT_PRIMARY
        else:
            glyph = "○"
        rows.append({"id": entity_id, "text": "%s  %s" % [glyph, display_name], "color": color})
        signature_parts.append("%s:%s:%s" % [entity_id, glyph, color.to_html(false)])

    var signature := "|".join(signature_parts)
    if not force and signature == _current_signature:
        return
    _current_signature = signature

    _list.clear()
    _row_ids.clear()
    for row in rows:
        var index := _list.add_item(row["text"])
        _list.set_item_custom_fg_color(index, row["color"])
        _row_ids.append(row["id"])
    set_selected(_selected_id)

func _on_item_selected(index: int) -> void:
    if index < 0 or index >= _row_ids.size():
        return
    var entity_id := _row_ids[index]
    entity_selected.emit(entity_id, String(_last_kinds.get(entity_id, "")))
