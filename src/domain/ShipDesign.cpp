#include "domain/ShipDesign.hpp"

#include <algorithm>
#include <cmath>

namespace spacetrains::domain {

namespace {
constexpr double G0 = 9.80665;
// The builder rates 200 kg of cargo per unit (RATED_KG_PER_CARGO_UNIT).
constexpr double RATED_KG_PER_CARGO_UNIT = 200.0;
}  // namespace

std::string design_id(const std::string& class_id, const std::vector<std::string>& fit) {
    if (fit.empty()) {
        return class_id;
    }
    std::vector<std::string> sorted = fit;
    std::sort(sorted.begin(), sorted.end());
    std::string id = class_id;
    for (const auto& module : sorted) {
        id += '+';
        id += module;
    }
    return id;
}

DesignTotals totals_from_parts(const ShipClassDefinition& ship_class) {
    DesignTotals totals;
    double tank_kg = 0.0;
    double isp_s = 0.0;
    double thrust_n = 0.0;
    double jet_power_w = 0.0;
    for (const auto& part : ship_class.components) {
        if (!part.parent_id.empty()) {
            continue;
        }
        totals.dry_mass_kg += part.mass_kg;
        totals.ship_value_cr += part.price_cr;
        if (part.kind == "tank") {
            tank_kg += part.capacity_kg;
        } else if (part.kind == "engine") {
            isp_s = part.isp_s;
            thrust_n += part.thrust_n;
            jet_power_w += part.power_w;
        } else if (part.kind == "habitat") {
            totals.crew_capacity += part.crew_capacity;
        } else if (part.kind == "hold") {
            totals.cargo_capacity_units += part.capacity_kg / RATED_KG_PER_CARGO_UNIT;
        }
    }
    if (totals.dry_mass_kg <= 0.0) {
        return totals;
    }
    if (ship_class.propulsion_type == "variable_isp") {
        totals.specific_engine_power_w_per_kg = jet_power_w / totals.dry_mass_kg;
    } else {
        const double wet_kg = totals.dry_mass_kg + tank_kg;
        totals.max_delta_v_mps = isp_s * G0 * std::log(wet_kg / totals.dry_mass_kg);
        totals.cruise_accel_mps2 = thrust_n / wet_kg;
    }
    return totals;
}

ShipClassDefinition derive_design(
    const ShipClassDefinition& base, const std::vector<ShipComponentDefinition>& added_parts, std::string id) {
    ShipClassDefinition design = base;
    design.id = std::move(id);
    double added_kg = 0.0;
    for (const auto& part : added_parts) {
        design.components.push_back(part);
        if (part.parent_id.empty()) {
            added_kg += part.mass_kg;
            design.ship_value_cr += part.price_cr;
        }
    }
    if (added_kg == 0.0) {
        return design;
    }
    const double old_dry = base.dry_mass_kg;
    const double new_dry = old_dry + added_kg;
    design.dry_mass_kg = new_dry;
    if (base.propulsion_type == "variable_isp") {
        // Same jet power over a heavier ship.
        design.specific_engine_power_w_per_kg = base.specific_engine_power_w_per_kg * old_dry / new_dry;
    } else if (old_dry > 0.0) {
        // The class's own exhaust speed and thrust, as the Kepler planner derives them.
        const double tank = base.propellant_capacity_kg;
        const double ve = base.max_delta_v_mps / std::log((old_dry + tank) / old_dry);
        design.max_delta_v_mps = ve * std::log((new_dry + tank) / new_dry);
        design.cruise_accel_mps2 = base.cruise_accel_mps2 * (old_dry + tank) / (new_dry + tank);
    }
    return design;
}

}  // namespace spacetrains::domain
