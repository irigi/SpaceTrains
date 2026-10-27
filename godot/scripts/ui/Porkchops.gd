extends RefCounted

# Reading the porkchop file (trajectory/Porkchop.hpp) for one class: how far its tank reaches on
# a pair's grid and the fastest flight it can make at each departure. Shared by the Statistics
# Routes tab and the station inspector's launch windows.
#
# A class's reach, with the planner's 10% reserve kept back (PROPELLANT_RESERVE_FRACTION):
#   - nuclear-thermal: Δv = v_e ln(m0 / m_f), m_f = dry + payload, m0 = m_f + tank / 1.1;
#   - plasma: effort J = 2P (1/m_f - 1/m0) in SI, J_norm = J_SI AU^2.5 / μ^1.5 (the grid's units).
# "Full hold" is the class's cargo units at 100 kg each (food, water, fuel; metals are 200 kg,
# electronics 20): a typical load, not the heaviest.

const RESERVE := 0.1
const UNIT_KG := 100.0

static func is_plasma(ship_class: Dictionary) -> bool:
    return String(ship_class.get("propulsion_type", "")) == "variable_isp"

static func class_by_id(data: Dictionary, class_id: String) -> Dictionary:
    for c in data.get("classes", []):
        if String(c.get("id", "")) == class_id:
            return c
    return {}

# The most the class can spend on one flight: Δv in m/s (thermal) or the normalised effort (plasma).
static func reach(data: Dictionary, ship_class: Dictionary, full_hold: bool) -> float:
    var dry := float(ship_class.get("dry_mass_kg", 0.0))
    var payload := float(ship_class.get("cargo_capacity_units", 0.0)) * UNIT_KG if full_hold else 0.0
    var burn := float(ship_class.get("propellant_capacity_kg", 0.0)) / (1.0 + RESERVE)
    var m_final := dry + payload
    var m_start := m_final + burn
    if m_final <= 0.0:
        return 0.0
    if is_plasma(ship_class):
        var j_si := 2.0 * float(ship_class.get("power_w", 0.0)) * (1.0 / m_final - 1.0 / m_start)
        var au := float(data.get("au_m", 1.495978707e11))
        var mu := float(data.get("mu_sun", 1.32712440018e20))
        return j_si * pow(au, 2.5) / pow(mu, 1.5)
    return float(ship_class.get("exhaust_velocity_mps", 0.0)) * log(m_start / m_final)

# What a cell costs the class as a share of its tank (the burn plus the 10% reserve; > 1: it cannot).
# `value`: Δv in m/s (thermal) or the normalised effort (plasma).
static func propellant_share(data: Dictionary, ship_class: Dictionary, value: float, full_hold: bool) -> float:
    var dry := float(ship_class.get("dry_mass_kg", 0.0))
    var m_final := dry + (float(ship_class.get("cargo_capacity_units", 0.0)) * UNIT_KG if full_hold else 0.0)
    var tank := maxf(1.0, float(ship_class.get("propellant_capacity_kg", 0.0)))
    var burn := INF
    if is_plasma(ship_class):
        var au := float(data.get("au_m", 1.495978707e11))
        var mu := float(data.get("mu_sun", 1.32712440018e20))
        var j_si := value * pow(mu, 1.5) / pow(au, 2.5)
        var inv_start := 1.0 / m_final - j_si / (2.0 * maxf(1.0, float(ship_class.get("power_w", 0.0))))
        if inv_start > 0.0:
            burn = 1.0 / inv_start - m_final
    else:
        burn = m_final * (exp(value / maxf(1.0, float(ship_class.get("exhaust_velocity_mps", 0.0)))) - 1.0)
    return burn * (1.0 + RESERVE) / tank

# The shortest flight (days) in one column whose cost fits `budget`, between rows by interpolation
# (Δv linearly, effort on log scales); NAN if none fits.
static func fastest(values: Array, rows: Array, budget: float, plasma: bool) -> float:
    var previous := NAN
    for r in range(mini(values.size(), rows.size())):
        if values[r] == null:
            previous = NAN
            continue
        var v := float(values[r])
        if v <= budget:
            if r == 0 or is_nan(previous):
                return float(rows[r])
            var t0 := float(rows[r - 1])
            var t1 := float(rows[r])
            if plasma:
                var f := (log(previous) - log(budget)) / maxf(log(previous) - log(v), 1e-9)
                return exp(log(t0) + (log(t1) - log(t0)) * clampf(f, 0.0, 1.0))
            var g := (previous - budget) / maxf(previous - v, 1e-9)
            return t0 + (t1 - t0) * clampf(g, 0.0, 1.0)
        previous = v
    return NAN

# Per column of the pair: the class's fastest flight (days, NAN where it cannot go).
static func frontier(data: Dictionary, pair: Dictionary, ship_class: Dictionary, full_hold: bool) -> Array:
    var plasma := is_plasma(ship_class)
    var budget := reach(data, ship_class, full_hold)
    var rows: Array = pair.get("plasma_transit_days" if plasma else "transit_days", [])
    var out: Array = []
    for column in pair.get("columns", []):
        out.append(fastest(column.get("effort" if plasma else "dv", []), rows, budget, plasma))
    return out

# The classes to show for a route: those that flew it in the last year (busiest first), else the
# classes in the fleet now; at most `limit`.
static func route_classes(data: Dictionary, origin: String, destination: String, ships: Array, limit := 6) -> Array:
    var counts := {}
    for route in data.get("routes", []):
        if String(route.get("origin", "")) == origin and String(route.get("destination", "")) == destination:
            var classes: Dictionary = route.get("classes", {})
            for id in classes.keys():
                counts[String(id)] = float(classes[id])
    var flown := not counts.is_empty()
    if not flown:
        for ship in ships:
            var id := String(ship.get("class_id", ""))
            counts[id] = float(counts.get(id, 0.0)) + 1.0
    var ids: Array = counts.keys().filter(func(id): return not class_by_id(data, id).is_empty())
    ids.sort_custom(func(a, b): return counts[a] > counts[b] if counts[a] != counts[b] else String(a) < String(b))
    var out: Array = []
    for id in ids.slice(0, limit):
        out.append({"class": class_by_id(data, id), "count": counts[id], "flown": flown})
    return out

# The routes in use, busiest first: [{origin, destination, trips, classes}].
static func routes(data: Dictionary) -> Array:
    return data.get("routes", [])
