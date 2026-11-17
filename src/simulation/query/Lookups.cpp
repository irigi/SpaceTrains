#include "simulation/query/Lookups.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>
#include <unordered_map>

#include "domain/ShipDesign.hpp"

namespace spacetrains::simulation::query {

const domain::StationDefinition& get_station_definition(const World& w, const std::string& station_id) {
    const auto it = w.data.station_defs_by_id_.find(station_id);
    if (it == w.data.station_defs_by_id_.end()) {
        throw std::runtime_error("Unknown station: " + station_id);
    }
    return *it->second;
}

domain::StationState& get_station_state(World& w, const std::string& station_id) {
    const auto it = std::find_if(
        w.state.stations_.begin(),
        w.state.stations_.end(),
        [&](const domain::StationState& station) { return station.station_id == station_id; });
    if (it == w.state.stations_.end()) {
        throw std::runtime_error("Unknown station state: " + station_id);
    }
    return *it;
}

const domain::StationState& get_station_state(const World& w, const std::string& station_id) {
    const auto it = std::find_if(
        w.state.stations_.begin(),
        w.state.stations_.end(),
        [&](const domain::StationState& station) { return station.station_id == station_id; });
    if (it == w.state.stations_.end()) {
        throw std::runtime_error("Unknown station state: " + station_id);
    }
    return *it;
}

const domain::ShipClassDefinition& get_ship_class(const World& w, const std::string& class_id) {
    const auto it = w.data.ship_classes_by_id_.find(class_id);
    if (it == w.data.ship_classes_by_id_.end()) {
        throw std::runtime_error("Unknown ship class: " + class_id);
    }
    return *it->second;
}

const domain::CelestialBodyDefinition& get_body_definition(const World& w, const std::string& body_id) {
    const auto it = std::find_if(
        w.data.universe_.bodies.begin(),
        w.data.universe_.bodies.end(),
        [&](const domain::CelestialBodyDefinition& body) { return body.id == body_id; });
    if (it == w.data.universe_.bodies.end()) {
        throw std::runtime_error("Unknown body: " + body_id);
    }
    return *it;
}

const domain::CommodityDefinition& get_commodity(const World& w, const std::string& commodity_id) {
    const auto it = std::find_if(
        w.data.universe_.commodities.begin(),
        w.data.universe_.commodities.end(),
        [&](const domain::CommodityDefinition& commodity) { return commodity.id == commodity_id; });
    if (it == w.data.universe_.commodities.end()) {
        throw std::runtime_error("Unknown commodity: " + commodity_id);
    }
    return *it;
}

const domain::ShipModuleDefinition* find_module(const World& w, const std::string& hull_id, const std::string& module_id) {
    const auto it = w.data.modules_by_key_.find(hull_id + "|" + module_id);
    return it == w.data.modules_by_key_.end() ? nullptr : it->second;
}

bool module_has_active_effect(const World& w, const domain::ShipModuleDefinition& module) {
    for (const auto& [effect, value] : module.effects) {
        if (value != 0.0 && w.data.active_effects_.contains(effect)) {
            return true;
        }
    }
    return false;
}

double radiator_bucket(const World& w, const domain::ShipState& ship) {
    // Only a plasma drive's radiators limit its power (register_design makes damaged designs
    // for variable-Isp hulls only), so other hulls are always in the undamaged bucket.
    if (!w.data.universe_.ship_environment.wear || get_ship_class(w, ship.class_id).propulsion_type != "variable_isp") {
        return 1.0;
    }
    return std::clamp(std::round(ship.radiator_condition * 10.0) / 10.0, 0.1, 1.0);
}

namespace {
std::string damaged_design_id(const std::string& class_id, const std::vector<std::string>& fit, double radiator_bucket) {
    auto key = domain::design_id(class_id, fit);
    if (radiator_bucket < 1.0) {
        key += std::format("#r{}", static_cast<int>(std::lround(radiator_bucket * 10.0)));
    }
    return key;
}
}  // namespace

const domain::ShipClassDefinition& ship_design(const World& w, const domain::ShipState& ship) {
    const double bucket = radiator_bucket(w, ship);
    if (ship.fit.empty() && bucket >= 1.0) {
        return get_ship_class(w, ship.class_id);
    }
    const auto key = damaged_design_id(ship.class_id, ship.fit, bucket);
    const auto it = w.caches.designs_.find(key);
    if (it == w.caches.designs_.end()) {
        throw std::runtime_error("Unregistered ship design: " + key);
    }
    return it->second;
}

const domain::ShipClassDefinition& register_design(
    World& w, const std::string& class_id, const std::vector<std::string>& fit, double radiator_bucket) {
    const auto& base = get_ship_class(w, class_id);
    const bool damaged = radiator_bucket < 1.0 && base.propulsion_type == "variable_isp";
    if (fit.empty() && !damaged) {
        return base;
    }
    const auto key = damaged_design_id(class_id, fit, damaged ? radiator_bucket : 1.0);
    if (const auto it = w.caches.designs_.find(key); it != w.caches.designs_.end()) {
        return it->second;
    }
    std::vector<std::string> sorted = fit;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
        throw std::runtime_error("Ship design fits a module twice: " + key);
    }
    std::vector<domain::ShipComponentDefinition> parts;
    std::unordered_map<std::string, int> used;
    std::unordered_map<std::string, double> effects;
    std::string names;
    for (const auto& module_id : sorted) {
        const auto* module = find_module(w, base.hull_id, module_id);
        if (module == nullptr) {
            throw std::runtime_error("Hull " + base.hull_id + " has no module " + module_id);
        }
        const auto slots = w.data.universe_.hull_slots.find(base.hull_id);
        const int capacity = slots == w.data.universe_.hull_slots.end() || !slots->second.contains(module->slot)
            ? 0 : slots->second.at(module->slot);
        if (++used[module->slot] > capacity) {
            throw std::runtime_error("Ship design overfills the " + module->slot + " slots: " + key);
        }
        parts.push_back(module->part);
        for (const auto& [effect, value] : module->effects) {
            effects[effect] += value;
        }
        names += (names.empty() ? "" : ", ") + module->part.name;
    }
    auto design = domain::derive_design(base, parts, key);
    design.name = names.empty() ? base.name : base.name + " [" + names + "]";
    design.base_class_id = base.id;
    if (damaged) {
        // Holed radiators reject less heat, and the drive may only make as much waste heat as
        // they reject: jet power, and with it alpha, falls in proportion.
        design.specific_engine_power_w_per_kg *= radiator_bucket;
        for (auto& part : design.components) {
            if (part.kind == "engine") {
                part.power_w *= radiator_bucket;
                part.heat_w *= radiator_bucket;
            } else if (part.kind == "radiator") {
                part.heat_w *= radiator_bucket;
            }
        }
        design.name += std::format(" (radiators {:.0f}%)", 100.0 * radiator_bucket);
    }
    if (const auto it = effects.find("berths"); it != effects.end() && w.data.active_effects_.contains("berths")) {
        design.berths += it->second;
    }
    design.fit = std::move(sorted);
    design.effects = std::move(effects);
    return w.caches.designs_.emplace(key, std::move(design)).first->second;
}

std::string mission_phase_name(domain::ShipMissionPhase phase) {
    switch (phase) {
        case domain::ShipMissionPhase::Idle:
            return "idle";
        case domain::ShipMissionPhase::AwaitingDeparture:
            return "awaiting_departure";
        case domain::ShipMissionPhase::InTransit:
            return "in_transit";
        case domain::ShipMissionPhase::Refueling:
            return "refueling";
        case domain::ShipMissionPhase::Stranded:
            return "stranded";
        case domain::ShipMissionPhase::LaidUp:
            return "laid_up";
        case domain::ShipMissionPhase::Refitting:
            return "refitting";
    }
    return "unknown";
}

}  // namespace spacetrains::simulation::query
