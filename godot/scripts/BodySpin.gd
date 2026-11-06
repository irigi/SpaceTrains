extends RefCounted

# Visual only: the planets' and moons' axial tilt and spin, and the orbit of each station
# round its planet. The simulation stays planar and keeps the stations' fixed angles
# (CelestialMechanics); Main.gd turns these on the wall clock at the timewarp rate.
#
# The rates are the true ones at low time speeds and capped where a turn would look like a
# blur: no turn takes less than MIN_TURN_WALL_S seconds on screen. Stations and bodies share
# the cap.
#
# Obliquity is the angle between the spin axis and the orbit normal (the ecliptic's north,
# as the sim's orbits are counter-clockwise seen from there). Above 90 degrees the body spins
# backwards with respect to its orbit (Venus, Uranus). A period of 0 means tidally locked:
# the body turns once per orbit, in the sense of its obliquity.
#
# Values rounded from the standard astronomical tables; the masses are the same as in
# data/bodies/bodies.csv (only the planets that have stations need one).

const MIN_TURN_WALL_S := 4.0

const BODIES := {
    "sun": {"spin_s": 2192832.0, "obliquity_deg": 7.25},
    "mercury": {"spin_s": 5067014.0, "obliquity_deg": 0.03, "mu": 2.2032e13},
    "venus": {"spin_s": 20997360.0, "obliquity_deg": 177.36, "mu": 3.24859e14},
    "earth": {"spin_s": 86164.0, "obliquity_deg": 23.44, "mu": 3.986004418e14},
    "mars": {"spin_s": 88642.0, "obliquity_deg": 25.19, "mu": 4.282837e13},
    "ceres": {"spin_s": 32667.0, "obliquity_deg": 4.0, "mu": 6.26325e10},
    "jupiter": {"spin_s": 35730.0, "obliquity_deg": 3.13},
    "saturn": {"spin_s": 38362.0, "obliquity_deg": 26.73},
    "uranus": {"spin_s": 62064.0, "obliquity_deg": 97.77},
    "neptune": {"spin_s": 57996.0, "obliquity_deg": 28.32},
    "luna": {"spin_s": 0.0, "obliquity_deg": 1.5, "mu": 4.9048e12},
    "europa": {"spin_s": 0.0, "obliquity_deg": 0.1},
    "ganymede": {"spin_s": 0.0, "obliquity_deg": 0.2, "mu": 9.8878e12},
    "titan": {"spin_s": 0.0, "obliquity_deg": 0.3, "mu": 8.978e12},
    "triton": {"spin_s": 0.0, "obliquity_deg": 157.0},
}

# Caps a signed angular rate (rad per wall second) so that no turn is faster than MIN_TURN_WALL_S.
static func capped(rate_rad_s: float) -> float:
    var limit := TAU / MIN_TURN_WALL_S
    return clampf(rate_rad_s, -limit, limit)

# A planet's or moon's spin rate on screen (rad per wall second). orbit_period_s is only
# used for tidally locked bodies.
static func spin_rate(body_id: String, orbit_period_s: float, timewarp: float) -> float:
    var info: Dictionary = BODIES.get(body_id, {})
    if info.is_empty():
        return 0.0
    var spin_s := float(info.get("spin_s", 0.0))
    var sim_rate := 0.0
    if spin_s > 0.0:
        sim_rate = TAU / spin_s
    elif orbit_period_s > 0.0:
        var sense := 1.0 if float(info.get("obliquity_deg", 0.0)) < 90.0 else -1.0
        sim_rate = sense * TAU / orbit_period_s
    return capped(sim_rate * timewarp)

# A station's orbit round its planet on screen (rad per wall second, a positive value: the
# sim's direction). The circular orbit's rate is sqrt(mu / r^3).
static func station_rate(parent_body_id: String, orbit_radius_m: float, timewarp: float) -> float:
    var mu := float((BODIES.get(parent_body_id, {}) as Dictionary).get("mu", 0.0))
    if mu <= 0.0 or orbit_radius_m <= 0.0:
        return 0.0
    return capped(sqrt(mu / (orbit_radius_m * orbit_radius_m * orbit_radius_m)) * timewarp)

# The body's spin axis is its local Y axis (the sphere mesh's poles). The sim's orbits go from
# +x towards +z, so the orbit normal is -Y in the render frame (_to_render keeps the axes).
# Local Y is turned to lie obliquity_deg from that normal.
static func axis_basis(obliquity_deg: float) -> Basis:
    return Basis(Vector3.RIGHT, PI - deg_to_rad(obliquity_deg))

# Sets the spin of a body's visible meshes ("hull" and, for Earth, "clouds") at angle rad.
static func orient(container: Node3D, body_id: String, angle_rad: float) -> void:
    var info: Dictionary = BODIES.get(body_id, {})
    if info.is_empty():
        return
    var basis := axis_basis(float(info.get("obliquity_deg", 0.0))) * Basis(Vector3.UP, angle_rad)
    for child_name in ["hull", "clouds"]:
        var child := container.get_node_or_null(child_name) as Node3D
        if child != null:
            child.basis = basis
