class_name SelectionPanel
extends PanelContainer
## Left panel showing details of the selected entity.

var simulation: Simulation

var selected_type: String = ""
var selected_id: int = -1

var title_label: Label
var type_label: Label
var details_text: RichTextLabel

func _ready() -> void:
	visible = false
	EventBus.entity_selected.connect(_on_entity_selected)
	EventBus.entity_deselected.connect(_on_entity_deselected)
	if details_text != null:
		details_text.meta_clicked.connect(_on_details_meta_clicked)

func _on_entity_selected(entity_type: String, entity_id: int) -> void:
	selected_type = entity_type
	selected_id = entity_id
	visible = true
	_update_display()

func _on_entity_deselected() -> void:
	selected_type = ""
	selected_id = -1
	visible = false

func _process(_delta: float) -> void:
	if visible and simulation != null:
		_update_display()

func _update_display() -> void:
	if simulation == null or simulation.world == null:
		return
	if title_label == null or type_label == null or details_text == null:
		return

	match selected_type:
		"body":
			_display_body()
		"station":
			_display_station()
		"ship":
			_display_ship()

func _display_body() -> void:
	if selected_id not in simulation.world.bodies:
		return
	var body: WorldState.CelestialBodyData = simulation.world.bodies[selected_id]
	title_label.text = body.entity_name
	type_label.text = body.body_type.capitalize()

	var text := ""
	text += "[b]Orbital Radius:[/b] %.2f AU\n" % body.orbital_radius
	if body.orbital_period > 0:
		text += "[b]Orbital Period:[/b] %.0f hours\n" % (body.orbital_period / 60.0)
	text += "\n[b]Stations:[/b] %d\n" % body.station_ids.size()
	for sid in body.station_ids:
		if sid in simulation.world.stations:
			var s = simulation.world.stations[sid]
			var station_link := _make_entity_link("station", sid, s.entity_name)
			text += "  • %s (%s)\n" % [station_link, s.faction_name]
	details_text.text = text

func _display_station() -> void:
	if selected_id not in simulation.world.stations:
		return
	var station: WorldState.StationData = simulation.world.stations[selected_id]
	title_label.text = station.entity_name
	type_label.text = "%s Station" % station.orbit_type.capitalize()

	var text := ""
	text += "[b]Faction:[/b] %s\n" % station.faction_name
	text += "[b]Population:[/b] %d\n" % station.population
	text += "[b]Docks:[/b] %d/%d in use\n" % [station.docked_ship_ids.size(), station.max_docks]
	text += "\n[b]Modules:[/b]\n"
	for m in station.modules:
		text += "  • %s\n" % m
	text += "\n[b]Inventory:[/b]\n"
	for commodity in station.inventory:
		text += "  %s: %d\n" % [commodity, station.inventory[commodity]]
	text += "\n[b]Ships (%d):[/b]\n" % station.ship_ids.size()
	for ship_id in station.ship_ids:
		if ship_id in simulation.world.ships:
			var ship = simulation.world.ships[ship_id]
			var ship_link := _make_entity_link("ship", ship_id, ship.entity_name)
			text += "  • %s [%s]\n" % [ship_link, ship.state]
	details_text.text = text

func _display_ship() -> void:
	if selected_id not in simulation.world.ships:
		return
	var ship: WorldState.ShipData = simulation.world.ships[selected_id]
	title_label.text = ship.entity_name
	type_label.text = "%s (%s)" % [ship.ship_type, ship.role]

	var text := ""
	text += "[b]Faction:[/b] %s\n" % ship.faction_name
	text += "[b]State:[/b] %s\n" % ship.state
	text += "[b]Hull:[/b] %.0f / %.0f\n" % [ship.hull, ship.hull_max]
	text += "[b]Fuel:[/b] %.0f / %.0f\n" % [ship.fuel, ship.fuel_max]
	text += "[b]Speed:[/b] %.1f\n" % ship.base_speed

	if ship.mission_type != "":
		text += "\n[b]Mission:[/b] %s\n" % ship.mission_type
		if ship.mission_commodity != "":
			text += "  Cargo: %d %s\n" % [ship.mission_amount, ship.mission_commodity]
		if ship.mission_dest_id >= 0 and ship.mission_dest_id in simulation.world.stations:
			text += "  Destination: %s\n" % simulation.world.stations[ship.mission_dest_id].entity_name
		if ship.state == "traveling" and ship.travel_duration > 0:
			var eta = ship.travel_duration * (1.0 - ship.travel_progress)
			text += "  ETA: %.0f min\n" % eta
			text += "  Progress: %.0f%%\n" % (ship.travel_progress * 100.0)

	if not ship.cargo.is_empty():
		text += "\n[b]Cargo:[/b]\n"
		for commodity in ship.cargo:
			text += "  %s: %d\n" % [commodity, ship.cargo[commodity]]
	elif ship.mission_type == "":
		text += "\n[i]No cargo[/i]\n"

	if ship.docked_station_id >= 0 and ship.docked_station_id in simulation.world.stations:
		var docked_station = simulation.world.stations[ship.docked_station_id]
		text += "\n[b]Docked at:[/b] %s\n" % _make_entity_link("station", ship.docked_station_id, docked_station.entity_name)

	details_text.text = text

func _on_details_meta_clicked(meta: Variant) -> void:
	var meta_text := str(meta)
	var parts := meta_text.split(":")
	if parts.size() != 2:
		return

	var entity_type := parts[0]
	if entity_type != "station" and entity_type != "ship":
		return

	var entity_id := int(parts[1])
	EventBus.entity_selected.emit(entity_type, entity_id)

func _make_entity_link(entity_type: String, entity_id: int, name: String) -> String:
	return "[url=%s:%d][color=#9fc5ff][u]%s[/u][/color][/url]" % [entity_type, entity_id, name]
