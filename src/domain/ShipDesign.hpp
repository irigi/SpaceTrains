#pragma once

// Ship designs (tier 2): a class plus the modules fitted to one ship. A design is an
// ordinary ShipClassDefinition whose totals are recomputed from its parts, so every
// planner and score keeps working on it unchanged.

#include "domain/Types.hpp"

#include <string>
#include <vector>

namespace spacetrains::domain {

// "class_id" for an unfitted ship, "class_id+module_a+module_b" (sorted) otherwise.
[[nodiscard]] std::string design_id(const std::string& class_id, const std::vector<std::string>& fit);

// The class totals as the builder computes them from the parts: dry mass, value, Δv,
// acceleration, α, berths and hold. Used to check the port against ship_classes.csv.
struct DesignTotals {
    double dry_mass_kg {0.0};
    double ship_value_cr {0.0};
    double max_delta_v_mps {0.0};
    double cruise_accel_mps2 {0.0};
    double specific_engine_power_w_per_kg {0.0};
    double crew_capacity {0.0};
    double cargo_capacity_units {0.0};
};
[[nodiscard]] DesignTotals totals_from_parts(const ShipClassDefinition& ship_class);

// The base class with extra top-level parts added. Dry mass and value grow by the parts;
// nuclear-thermal Δv and acceleration follow from the same exhaust speed and thrust;
// plasma α from the same jet power. Crew and hold stay the class's (modules that change
// them say so through their effects, applied by the caller).
[[nodiscard]] ShipClassDefinition derive_design(
    const ShipClassDefinition& base, const std::vector<ShipComponentDefinition>& added_parts, std::string id);

}  // namespace spacetrains::domain
