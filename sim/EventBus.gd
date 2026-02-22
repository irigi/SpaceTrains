extends Node
## Global event bus (autoload). All simulation events pass through here.

# Ship events
signal ship_launched(ship_id: int, station_id: int)
signal ship_docked(ship_id: int, station_id: int)
signal cargo_delivered(ship_id: int, station_id: int, commodity: String, amount: int)

# Piracy / security events (v0.2)
signal piracy_attempt(attacker_id: int, target_id: int)
signal piracy_success(attacker_id: int, target_id: int)
signal piracy_fail(attacker_id: int, target_id: int)
signal inspection(patrol_id: int, target_id: int)
signal arrest(patrol_id: int, target_id: int)
signal escort_assigned(escort_id: int, target_id: int)

# Station events
signal station_production(station_id: int, commodity: String, amount: int)
signal station_shortage(station_id: int, commodity: String)

# Faction events
signal relation_changed(faction_a: String, faction_b: String, new_value: float)
signal bounty_issued(faction: String, target_id: int, amount: int)

# Selection
signal entity_selected(entity_type: String, entity_id: int)
signal entity_deselected()

# Simulation control
signal simulation_paused()
signal simulation_resumed()
signal simulation_speed_changed(new_speed: float)

# Generic log
signal log_event(category: String, message: String)

func emit_log(category: String, message: String) -> void:
	log_event.emit(category, message)
