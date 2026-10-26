extends RefCounted

# What the UI remembers of the economy between snapshots: prices per station (for the
# inspector's trend arrows) and cumulative demand and unmet demand (for the recent unmet share).

const PRICE_HISTORY_MAX_SAMPLES := 600
const PRICE_HISTORY_MIN_DAY_STEP := 0.5
const PRICE_TREND_LOOKBACK_DAYS := 10.0
const PRICE_TREND_THRESHOLD := 0.03

var price_history: Array[Dictionary] = []
# Cumulative demand and unmet demand (base value), sampled once per game day, for the
# share that went short over a recent window.
var economy_history: Array = []

func record(bridge_state: Dictionary) -> void:
    _record_prices(bridge_state)
    _record_economy(bridge_state)

func has_unmet_window() -> bool:
    return economy_history.size() >= 2

func clear() -> void:
    price_history.clear()
    economy_history.clear()

func _record_prices(bridge_state: Dictionary) -> void:
    var day := float(bridge_state.get("game_time_days", 0.0))
    if not price_history.is_empty() and day - float(price_history.back()["day"]) < PRICE_HISTORY_MIN_DAY_STEP:
        return
    var sample := {}
    for station in bridge_state.get("stations", []):
        var prices: Dictionary = station.get("prices", {})
        if not prices.is_empty():
            sample[String(station["id"])] = prices.duplicate()
    if sample.is_empty():
        return
    price_history.append({"day": day, "prices": sample})
    if price_history.size() > PRICE_HISTORY_MAX_SAMPLES:
        price_history.pop_front()

func _record_economy(bridge_state: Dictionary) -> void:
    var economy: Dictionary = bridge_state.get("economy", {})
    if economy.is_empty():
        return
    var day := float(bridge_state.get("game_time_days", 0.0))
    if not economy_history.is_empty() and day - float(economy_history.back()["day"]) < 1.0:
        return
    economy_history.append({"day": day, "demand": float(economy.get("demand_value", 0.0)), "unmet": float(economy.get("unmet_value", 0.0))})
    if economy_history.size() > 400:
        economy_history.pop_front()

# The share of demand that went unmet over about the last `days` (0 before two samples).
func recent_unmet_share(days: float) -> float:
    var latest: Dictionary = economy_history.back()
    var reference: Dictionary = economy_history.front()
    for i in range(economy_history.size() - 1, -1, -1):
        reference = economy_history[i]
        if float(latest["day"]) - float(reference["day"]) >= days:
            break
    var demand := float(latest["demand"]) - float(reference["demand"])
    return (float(latest["unmet"]) - float(reference["unmet"])) / demand if demand > 0.0 else 0.0

# Per good at a station: 1 rising, -1 falling, 0 flat over the lookback.
func price_trends_for(station_id: String) -> Dictionary:
    var trends := {}
    if price_history.size() < 2:
        return trends
    var current: Dictionary = price_history.back()
    var current_day := float(current["day"])
    var reference: Dictionary = price_history.front()
    for i in range(price_history.size() - 2, -1, -1):
        if current_day - float(price_history[i]["day"]) >= PRICE_TREND_LOOKBACK_DAYS:
            reference = price_history[i]
            break
    var now_prices: Dictionary = (current["prices"] as Dictionary).get(station_id, {})
    var then_prices: Dictionary = (reference["prices"] as Dictionary).get(station_id, {})
    for commodity_id in now_prices.keys():
        var then := float(then_prices.get(commodity_id, 0.0))
        if then <= 0.0:
            continue
        var ratio := float(now_prices[commodity_id]) / then
        if ratio > 1.0 + PRICE_TREND_THRESHOLD:
            trends[commodity_id] = 1
        elif ratio < 1.0 - PRICE_TREND_THRESHOLD:
            trends[commodity_id] = -1
        else:
            trends[commodity_id] = 0
    return trends
