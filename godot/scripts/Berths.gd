extends RefCounted

# Berths (docs/plans/main_gd_split.md, step 4): which berth slot each docked ship holds at its
# station, where a ship sits there, and the ease between a path and a berth near a station.

# Main's entity dictionaries, shared by reference (never reassigned).
var entity_nodes: Dictionary
var entity_details: Dictionary
var ship_metres_to_world := 0.0
var _docked_slots: Dictionary = {}    # ship id -> berth slot, while docked
var _docked_station: Dictionary = {}  # ship id -> station id, while docked
var _last_berth: Dictionary = {}      # ship id -> [station id, slot] of its last stay

func setup(nodes: Dictionary, details: Dictionary, metres_to_world: float) -> void:
    entity_nodes = nodes
    entity_details = details
    ship_metres_to_world = metres_to_world

# A docked ship sits at one of its station's berths (StationModels: the ends of the
# berthing arms), bow to the hub; ships beyond the berth count queue further out. A ship
# keeps its slot while docked; an arriving ship takes the lowest free one (_docked_slots).
func place_docked_ship(node: Node3D, ship_id: String) -> void:
    var detail: Dictionary = entity_details.get(ship_id, {})
    var station: Node3D = entity_nodes.get(String(detail.get("current_station_id", "")))
    var pose := _berth_pose(station, int(_docked_slots.get(ship_id, 0)), float(node.get_meta("length_m", 50.0)))
    if pose.is_empty():
        return
    node.position = pose["position"]
    node.basis = (pose["basis"] as Basis).scaled(Vector3.ONE * ship_metres_to_world)

# Where a ship of this length sits at a berth slot: {position, basis (unscaled)}, or {}.
func _berth_pose(station: Node3D, slot: int, length_m: float) -> Dictionary:
    if station == null or not station.has_meta("berths"):
        return {}
    var berths: Array = station.get_meta("berths")
    if berths.is_empty():
        return {}
    var berth: Dictionary = berths[slot % berths.size()]
    var layer := slot / berths.size()
    var dir := (station.basis * (berth["dir"] as Vector3)).normalized()
    var axis := (station.basis * Vector3(0.0, 0.0, 1.0)).normalized()
    # Queued ships stand off a ship length apart (the longest is ~140 m).
    var out_m := length_m * 0.5 + 3.0 + float(layer) * 160.0
    return {"position": station.position + station.basis * (berth["pos"] as Vector3) + dir * out_m * ship_metres_to_world,
        "basis": Basis.looking_at(-dir, axis)}

# Near a station a ship in flight eases between its path (which ends at the station's
# centre) and its berth: the one it left, or the lowest free one where it is going. The
# blend follows distance (full within 1.3 station radii, none beyond 4), so the ship
# docks without a jump and passes over the habitat ring rather than through it.
func ease_into_berth(node: Node3D, ship_id: String) -> void:
    var detail: Dictionary = entity_details.get(ship_id, {})
    var best: Node3D = null
    var best_id := ""
    var best_d := INF
    for key in ["origin_station_id", "destination_station_id"]:
        var station_id := String(detail.get(key, ""))
        var station: Node3D = entity_nodes.get(station_id)
        if station == null:
            continue
        var d := node.position.distance_to(station.position)
        if d < best_d:
            best = station
            best_id = station_id
            best_d = d
    if best == null:
        return
    var radius := float(best.get_meta("radius_m", 1000.0)) * best.scale.x
    var w := 1.0 - smoothstep(1.3 * radius, 4.0 * radius, best_d)
    if w <= 0.0:
        return
    var slot := 0
    var last: Array = _last_berth.get(ship_id, [])
    if not last.is_empty() and String(last[0]) == best_id and best_id == String(detail.get("origin_station_id", "")):
        slot = int(last[1])
    else:
        slot = _free_slot(best_id)
    var pose := _berth_pose(best, slot, float(node.get_meta("length_m", 50.0)))
    if pose.is_empty():
        return
    node.position += ((pose["position"] as Vector3) - best.position) * w
    var turn := node.basis.get_rotation_quaternion().slerp((pose["basis"] as Basis).get_rotation_quaternion(), w)
    node.basis = Basis(turn).scaled(Vector3.ONE * ship_metres_to_world)

func rebuild(bridge_state: Dictionary) -> void:
    var by_station := {}
    for ship in bridge_state.get("ships", []):
        if String(ship.get("phase", "idle")) in ["in_transit", "derelict", "patrol"]:
            continue
        var station_id := String(ship.get("current_station_id", ""))
        if not by_station.has(station_id):
            by_station[station_id] = []
        by_station[station_id].append(String(ship["id"]))
    var kept := {}
    for station_id in by_station.keys():
        var ids: Array = by_station[station_id]
        ids.sort()
        for ship_id in ids:
            if String(_docked_station.get(ship_id, "")) == station_id:
                kept[ship_id] = int(_docked_slots[ship_id])
    var slots := {}
    var stations := {}
    for station_id in by_station.keys():
        var used := {}
        for ship_id in by_station[station_id]:
            if kept.has(ship_id):
                used[kept[ship_id]] = true
        var next := 0
        for ship_id in by_station[station_id]:
            var slot: int
            if kept.has(ship_id):
                slot = kept[ship_id]
            else:
                while used.has(next):
                    next += 1
                slot = next
                used[slot] = true
            slots[ship_id] = slot
            stations[ship_id] = station_id
            _last_berth[ship_id] = [station_id, slot]
    _docked_slots = slots
    _docked_station = stations

func _free_slot(station_id: String) -> int:
    var used := {}
    for ship_id in _docked_station.keys():
        if String(_docked_station[ship_id]) == station_id:
            used[int(_docked_slots[ship_id])] = true
    var slot := 0
    while used.has(slot):
        slot += 1
    return slot
