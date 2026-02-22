extends Node
## Global event bus (autoload). All simulation events pass through here.
## Signals are emitted by simulation systems and connected by UI/other consumers.

# Ship events
@warning_ignore("unused_signal")
signal ship_launched(ship_id: int, station_id: int)
@warning_ignore("unused_signal")
signal ship_docked(ship_id: int, station_id: int)
@warning_ignore("unused_signal")
signal cargo_delivered(ship_id: int, station_id: int, commodity: String, amount: int)

# Piracy / security events (v0.2)
@warning_ignore("unused_signal")
signal piracy_attempt(attacker_id: int, target_id: int)
@warning_ignore("unused_signal")
signal piracy_success(attacker_id: int, target_id: int)
@warning_ignore("unused_signal")
signal piracy_fail(attacker_id: int, target_id: int)
@warning_ignore("unused_signal")
signal inspection(patrol_id: int, target_id: int)
@warning_ignore("unused_signal")
signal arrest(patrol_id: int, target_id: int)
@warning_ignore("unused_signal")
signal escort_assigned(escort_id: int, target_id: int)

# Station events
@warning_ignore("unused_signal")
signal station_production(station_id: int, commodity: String, amount: int)
@warning_ignore("unused_signal")
signal station_shortage(station_id: int, commodity: String)

# Faction events
@warning_ignore("unused_signal")
signal relation_changed(faction_a: String, faction_b: String, new_value: float)
@warning_ignore("unused_signal")
signal bounty_issued(faction: String, target_id: int, amount: int)

# Selection
@warning_ignore("unused_signal")
signal entity_selected(entity_type: String, entity_id: int)
@warning_ignore("unused_signal")
signal entity_deselected()

# Simulation control
@warning_ignore("unused_signal")
signal simulation_paused()
@warning_ignore("unused_signal")
signal simulation_resumed()
@warning_ignore("unused_signal")
signal simulation_speed_changed(new_speed: float)

# Generic log
@warning_ignore("unused_signal")
signal log_event(category: String, message: String)

func emit_log(category: String, message: String) -> void:
	log_event.emit(category, message)
