class_name OrbitSystem
extends RefCounted
## Updates celestial body positions based on analytic Keplerian orbits.
## Bodies follow circular/elliptical paths - no n-body physics.

func update(_world: WorldState, _dt: float) -> void:
	# Positions are computed analytically from sim_time in CelestialBodyData.get_position_at_time()
	# This system exists for any per-tick orbital side effects (none for now).
	pass
