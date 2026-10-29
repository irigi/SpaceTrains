// Ship data: classes, components, modules and hull slots, the starting fleet, spares, operations and the ship environment.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "data_loader/LoaderInternal.hpp"

namespace spacetrains::data_loader::detail {

void load_ship_classes(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto ship_classes_path = root / "ship_classes" / "ship_classes.csv";
    const auto rows = read_csv_rows(ship_classes_path);
    require_header(rows.front(),
        {"id", "name", "propulsion_type", "dry_mass_kg", "propellant_capacity_kg", "cargo_capacity_units",
         "max_delta_v_mps", "cruise_accel_mps2", "specific_engine_power_w_per_kg", "ship_value_cr", "crew_size", "hull_id",
         "berths"},
        ship_classes_path);
    std::unordered_set<std::string> seen_ids;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        require_field_count(row, 13, ship_classes_path, i + 1);
        require_unique_id(row[0], seen_ids, ship_classes_path, i + 1);
        if (row[2] != "nuclear_thermal" && row[2] != "variable_isp") {
            throw std::runtime_error(std::format("{}:{}: propulsion_type must be nuclear_thermal or variable_isp, got '{}'",
                ship_classes_path.string(), i + 1, row[2]));
        }
        universe.ship_classes.push_back({
            .id = row[0],
            .name = row[1],
            .propulsion_type = row[2],
            .dry_mass_kg = parse_double(row[3], ship_classes_path, i + 1, "dry_mass_kg"),
            .propellant_capacity_kg = parse_double(row[4], ship_classes_path, i + 1, "propellant_capacity_kg"),
            .cargo_capacity_units = parse_double(row[5], ship_classes_path, i + 1, "cargo_capacity_units"),
            .max_delta_v_mps = parse_double(row[6], ship_classes_path, i + 1, "max_delta_v_mps"),
            .cruise_accel_mps2 = parse_double(row[7], ship_classes_path, i + 1, "cruise_accel_mps2"),
            .specific_engine_power_w_per_kg = parse_double(row[8], ship_classes_path, i + 1, "specific_engine_power_w_per_kg"),
            .ship_value_cr = parse_double(row[9], ship_classes_path, i + 1, "ship_value_cr"),
            .crew_size = parse_double(row[10], ship_classes_path, i + 1, "crew_size"),
            .berths = parse_double(row[12], ship_classes_path, i + 1, "berths"),
            .hull_id = row[11].empty() ? row[0] : row[11],
            .components = {},
        });
    }
}

// Ship components (docs/modules/ships_and_fleet.md): the parts of every class. They
// must decompose the class rows exactly, so the simulation's totals and the parts agree.
void load_ship_components(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto ship_components_path = root / "ship_classes" / "ship_components.csv";
    const auto rows = read_csv_rows(ship_components_path);
    require_header(rows.front(),
        {"class_id", "component_id", "parent_id", "kind", "name", "mass_kg", "price_cr", "power_w", "heat_w",
         "temperature_k", "volume_m3", "area_m2", "capacity_kg", "crew_capacity", "isp_s", "thrust_n"},
        ship_components_path);
    static const std::unordered_set<std::string> top_kinds {"engine", "radiator", "tank", "habitat", "hold", "structure",
        "cabin"};
    static const std::unordered_set<std::string> habitat_kinds {"pressure_hull", "shielding", "life_support",
        "thermal_control", "power_avionics", "crew_quarters", "common_rooms", "airlock_eva"};
    // Wearing and lasting parts (tier 2: spares.csv lists the wearing ones).
    static const std::unordered_map<std::string, std::unordered_set<std::string>> child_kinds {
        {"habitat", habitat_kinds},
        {"engine", {"fuel_elements", "reactor_vessel", "reactor_fuel_load", "power_electronics", "magnets_structure"}},
        {"radiators", {"heat_pipes", "panel_frame"}},
    };
    std::unordered_map<std::string, domain::ShipClassDefinition*> classes;
    for (auto& ship_class : universe.ship_classes) {
        classes[ship_class.id] = &ship_class;
    }
    std::unordered_set<std::string> seen;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        const auto line = i + 1;
        require_field_count(row, 16, ship_components_path, line);
        const auto where = std::format("{}:{}", ship_components_path.string(), line);
        const auto found = classes.find(row[0]);
        if (found == classes.end()) {
            throw std::runtime_error(where + ": unknown ship class '" + row[0] + "'");
        }
        if (!seen.insert(row[0] + "|" + row[1]).second) {
            throw std::runtime_error(where + ": duplicate component '" + row[1] + "' of " + row[0]);
        }
        auto& ship_class = *found->second;
        const bool top = row[2].empty();
        const auto allowed = child_kinds.find(row[2]);
        if (top ? !top_kinds.contains(row[3]) : (allowed == child_kinds.end() || !allowed->second.contains(row[3]))) {
            throw std::runtime_error(where + ": component kind '" + row[3] + "' does not fit parent '" + row[2] + "'");
        }
        if (!top) {
            const auto parent = std::find_if(ship_class.components.begin(), ship_class.components.end(),
                [&](const auto& c) { return c.id == row[2]; });
            if (parent == ship_class.components.end() || !parent->parent_id.empty()) {
                throw std::runtime_error(where + ": parent '" + row[2] + "' must be listed before its parts");
            }
        }
        const auto number = [&](std::size_t column, const char* field) {
            const double value = row[column].empty() ? 0.0 : parse_double(row[column], ship_components_path, line, field);
            if (value < 0.0) {
                throw std::runtime_error(where + ": " + field + " must not be negative");
            }
            return value;
        };
        ship_class.components.push_back({
            .id = row[1],
            .parent_id = row[2],
            .kind = row[3],
            .name = row[4],
            .mass_kg = number(5, "mass_kg"),
            .price_cr = number(6, "price_cr"),
            .power_w = number(7, "power_w"),
            .heat_w = number(8, "heat_w"),
            .temperature_k = number(9, "temperature_k"),
            .volume_m3 = number(10, "volume_m3"),
            .area_m2 = number(11, "area_m2"),
            .capacity_kg = number(12, "capacity_kg"),
            .crew_capacity = number(13, "crew_capacity"),
            .isp_s = number(14, "isp_s"),
            .thrust_n = number(15, "thrust_n"),
        });
    }
    for (const auto& ship_class : universe.ship_classes) {
        double mass = 0.0;
        double price = 0.0;
        double berths = 0.0;
        std::unordered_map<std::string, std::pair<double, double>> parts;  // parent -> mass, price of its parts
        for (const auto& c : ship_class.components) {
            if (c.parent_id.empty()) {
                mass += c.mass_kg;
                price += c.price_cr;
                if (c.kind == "cabin") {
                    berths += c.crew_capacity;
                }
            } else {
                parts[c.parent_id].first += c.mass_kg;
                parts[c.parent_id].second += c.price_cr;
            }
        }
        if (ship_class.components.empty() || std::abs(mass - ship_class.dry_mass_kg) > 1.0
            || std::abs(price - ship_class.ship_value_cr) > 1.0) {
            throw std::runtime_error(std::format(
                "{}: components of '{}' add up to {:.0f} kg and {:.0f} cr, the class has {:.0f} kg and {:.0f} cr",
                ship_components_path.string(), ship_class.id, mass, price, ship_class.dry_mass_kg, ship_class.ship_value_cr));
        }
        if (berths != ship_class.berths) {
            throw std::runtime_error(std::format("{}: the cabins of '{}' have {:.0f} berths, the class {:.0f}",
                ship_components_path.string(), ship_class.id, berths, ship_class.berths));
        }
        for (const auto& [parent_id, sums] : parts) {
            const auto& parent = *std::find_if(ship_class.components.begin(), ship_class.components.end(),
                [&](const auto& c) { return c.id == parent_id; });
            if (std::abs(sums.first - parent.mass_kg) > 1.0 || std::abs(sums.second - parent.price_cr) > 1.0) {
                throw std::runtime_error(std::format("{}: the parts of '{}' in '{}' do not add up to it",
                    ship_components_path.string(), parent_id, ship_class.id));
            }
        }
    }
}

// Modules and hull slots (tier 2): what each hull can fit at its home yard.
void load_modules_and_hull_slots(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto ship_modules_path = root / "ship_classes" / "modules.csv";
    const auto hull_slots_path = root / "ship_classes" / "hull_slots.csv";
    static const std::unordered_set<std::string> slots {"habitat", "hold", "drive", "tank", "cabin"};
    std::unordered_set<std::string> hulls;
    for (const auto& ship_class : universe.ship_classes) {
        hulls.insert(ship_class.hull_id);
    }
    const auto slot_rows = read_csv_rows(hull_slots_path);
    require_header(slot_rows.front(), {"hull_id", "slot", "count"}, hull_slots_path);
    for (std::size_t i = 1; i < slot_rows.size(); ++i) {
        const auto& row = slot_rows[i];
        require_field_count(row, 3, hull_slots_path, i + 1);
        const auto where = std::format("{}:{}", hull_slots_path.string(), i + 1);
        if (!hulls.contains(row[0]) || !slots.contains(row[1])) {
            throw std::runtime_error(where + ": unknown hull or slot");
        }
        const double count = parse_double(row[2], hull_slots_path, i + 1, "count");
        if (count < 0.0 || count != std::floor(count)) {
            throw std::runtime_error(where + ": count must be a whole number");
        }
        universe.hull_slots[row[0]][row[1]] = static_cast<int>(count);
    }
    const auto rows = read_csv_rows(ship_modules_path);
    require_header(rows.front(),
        {"hull_id", "module_id", "slot", "name", "mass_kg", "price_cr", "power_w", "heat_w", "temperature_k",
         "volume_m3", "area_m2", "capacity_kg", "crew_capacity", "effects"},
        ship_modules_path);
    std::unordered_set<std::string> seen;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        const auto line = i + 1;
        require_field_count(row, 14, ship_modules_path, line);
        const auto where = std::format("{}:{}", ship_modules_path.string(), line);
        if (!hulls.contains(row[0])) {
            throw std::runtime_error(where + ": unknown hull '" + row[0] + "'");
        }
        if (!slots.contains(row[2])) {
            throw std::runtime_error(where + ": unknown slot '" + row[2] + "'");
        }
        if (!seen.insert(row[0] + "|" + row[1]).second) {
            throw std::runtime_error(where + ": duplicate module '" + row[1] + "' for " + row[0]);
        }
        const auto number = [&](std::size_t column, const char* field) {
            const double value = row[column].empty() ? 0.0 : parse_double(row[column], ship_modules_path, line, field);
            if (value < 0.0) {
                throw std::runtime_error(where + ": " + field + " must not be negative");
            }
            return value;
        };
        domain::ShipModuleDefinition module {
            .hull_id = row[0],
            .id = row[1],
            .slot = row[2],
            .part = {
                .id = "module." + row[1],
                .parent_id = "",
                .kind = "module",
                .name = row[3],
                .mass_kg = number(4, "mass_kg"),
                .price_cr = number(5, "price_cr"),
                .power_w = number(6, "power_w"),
                .heat_w = number(7, "heat_w"),
                .temperature_k = number(8, "temperature_k"),
                .volume_m3 = number(9, "volume_m3"),
                .area_m2 = number(10, "area_m2"),
                .capacity_kg = number(11, "capacity_kg"),
                .crew_capacity = number(12, "crew_capacity"),
            },
            .effects = {},
        };
        // effects: key=value;key=value
        std::size_t at = 0;
        const auto& text = row[13];
        while (at < text.size()) {
            const auto end = std::min(text.find(';', at), text.size());
            const auto item = text.substr(at, end - at);
            const auto eq = item.find('=');
            if (eq == std::string::npos || eq == 0) {
                throw std::runtime_error(where + ": effect '" + item + "' is not key=value");
            }
            module.effects[item.substr(0, eq)] = parse_double(item.substr(eq + 1), ship_modules_path, line, "effect");
            at = end + 1;
        }
        universe.ship_modules.push_back(std::move(module));
    }
}

void load_ship_seeds(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto ships_path = root / "ships" / "ships.csv";
    const auto rows = read_csv_rows(ships_path);
    require_header(rows.front(),
        {"id", "name", "faction_id", "class_id", "home_station_id", "start_station_id", "initial_propellant_kg", "initial_credits"},
        ships_path);
    std::unordered_set<std::string> seen_ids;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        require_field_count(row, 8, ships_path, i + 1);
        require_unique_id(row[0], seen_ids, ships_path, i + 1);
        universe.ship_seeds.push_back({
            .id = row[0],
            .name = row[1],
            .faction_id = row[2],
            .class_id = row[3],
            .home_station_id = row[4],
            .start_station_id = row[5],
            .initial_propellant_kg = parse_double(row[6], ships_path, i + 1, "initial_propellant_kg"),
            .initial_credits = parse_double(row[7], ships_path, i + 1, "initial_credits"),
        });
    }
}

void load_spares(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto spares_path = root / "ship_classes" / "spares.csv";
    if (std::filesystem::exists(spares_path)) {
        const auto rows = read_csv_rows(spares_path);
        require_header(rows.front(),
            {"kind", "wears_with", "grade", "fabrication_h_per_kg", "install_h_per_kg", "materials"}, spares_path);
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 6, spares_path, i + 1);
            const auto where = std::format("{}:{}", spares_path.string(), i + 1);
            if (row[1] != "drive" && row[1] != "radiators") {
                throw std::runtime_error(where + ": wears_with must be drive or radiators");
            }
            if (!universe.labour.grades.contains(row[2])) {
                throw std::runtime_error(where + ": unknown labour grade '" + row[2] + "'");
            }
            domain::SparePartDefinition part {
                .kind = row[0],
                .wears_with = row[1],
                .grade = row[2],
                .fabrication_h_per_kg = parse_double(row[3], spares_path, i + 1, "fabrication_h_per_kg"),
                .install_h_per_kg = parse_double(row[4], spares_path, i + 1, "install_h_per_kg"),
                .materials = {},
            };
            double shares = 0.0;
            std::size_t at = 0;
            const auto& text = row[5];
            while (at < text.size()) {
                const auto end = std::min(text.find(';', at), text.size());
                const auto item = text.substr(at, end - at);
                const auto equals = item.find('=');
                if (equals == std::string::npos) {
                    throw std::runtime_error(where + ": materials must be commodity=share;...");
                }
                const auto good = item.substr(0, equals);
                if (!contains_id(universe.commodities, good)) {
                    throw std::runtime_error(where + ": unknown commodity '" + good + "'");
                }
                const double share = parse_double(item.substr(equals + 1), spares_path, i + 1, "materials");
                part.materials.emplace_back(good, share);
                shares += share;
                at = end + 1;
            }
            if (part.fabrication_h_per_kg < 0.0 || part.install_h_per_kg < 0.0 || std::abs(shares - 1.0) > 1e-6) {
                throw std::runtime_error(where + ": hours must not be negative and material shares must add up to 1");
            }
            if (!universe.spare_parts.emplace(part.kind, part).second) {
                throw std::runtime_error(where + ": duplicate spare part '" + part.kind + "'");
            }
        }
    }
}

void load_ship_operations(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto ship_operations_path = root / "economy" / "ship_operations.csv";
    // key,value rows; life support entries are "life_support.<commodity_id>".
    const auto rows = read_csv_rows(ship_operations_path);
    require_header(rows.front(), {"key", "value"}, ship_operations_path);
    std::unordered_set<std::string> seen_keys;
    auto& operations = universe.ship_operations;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        require_field_count(row, 2, ship_operations_path, i + 1);
        require_unique_id(row[0], seen_keys, ship_operations_path, i + 1);
        const double value = parse_double(row[1], ship_operations_path, i + 1, row[0].c_str());
        constexpr std::string_view life_support_prefix = "life_support.";
        if (row[0] == "interest_rate_per_year") {
            operations.interest_rate_per_year = value;
        } else if (row[0] == "lifetime_years") {
            operations.lifetime_years = value;
        } else if (row[0] == "refit_days") {
            operations.refit_days = value;
        } else if (row[0] == "refit_cost_fraction") {
            operations.refit_cost_fraction = value;
        } else if (row[0].starts_with(life_support_prefix)) {
            operations.life_support_units_per_crew_day[row[0].substr(life_support_prefix.size())] = value;
        } else {
            throw std::runtime_error("Unknown key '" + row[0] + "' in " + ship_operations_path.string());
        }
    }
    for (const char* required : {"interest_rate_per_year", "lifetime_years"}) {
        if (!seen_keys.contains(required)) {
            throw std::runtime_error(std::string("Missing key '") + required + "' in " + ship_operations_path.string());
        }
    }
    if (operations.lifetime_years <= 0.0) {
        throw std::runtime_error("lifetime_years must be positive in " + ship_operations_path.string());
    }
    if (operations.refit_days < 0.0 || operations.refit_cost_fraction < 0.0) {
        throw std::runtime_error("refit_days and refit_cost_fraction must not be negative in " + ship_operations_path.string());
    }
    for (const auto& [commodity_id, _] : operations.life_support_units_per_crew_day) {
        if (!contains_id(universe.commodities, commodity_id)) {
            throw std::runtime_error("Ship operations reference unknown life-support commodity '" + commodity_id + "'");
        }
    }
}

// Ship failures and salvage (optional file; every key optional, defaults in FailureDefinition).
void load_failures(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto path = root / "economy" / "failures.csv";
    if (!std::filesystem::exists(path)) {
        return;
    }
    const auto rows = read_csv_rows(path);
    require_header(rows.front(), {"key", "value"}, path);
    std::unordered_set<std::string> seen_keys;
    auto& f = universe.failures;
    double failures = f.failures ? 1.0 : 0.0;
    double salvage = f.salvage ? 1.0 : 0.0;
    const std::unordered_map<std::string, double*> fields {
        {"failures", &failures},
        {"salvage", &salvage},
        {"weibull_shape", &f.weibull_shape},
        {"mean_life_factor", &f.mean_life_factor},
        {"scrap_fraction", &f.scrap_fraction},
        {"claim_days", &f.claim_days},
        {"max_racers", &f.max_racers},
        {"racer_margin", &f.racer_margin},
        {"salvage_review_days", &f.salvage_review_days},
        {"replacement_days", &f.replacement_days},
    };
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        require_field_count(row, 2, path, i + 1);
        require_unique_id(row[0], seen_keys, path, i + 1);
        const auto field = fields.find(row[0]);
        if (field == fields.end()) {
            throw std::runtime_error("Unknown key '" + row[0] + "' in " + path.string());
        }
        *field->second = parse_double(row[1], path, i + 1, row[0].c_str());
    }
    f.failures = failures != 0.0;
    f.salvage = salvage != 0.0;
    if (f.weibull_shape <= 0.0 || f.mean_life_factor <= 0.0 || f.scrap_fraction < 0.0 || f.claim_days <= 0.0
        || f.max_racers < 0.0 || f.racer_margin < 0.0 || f.salvage_review_days <= 0.0 || f.replacement_days < 0.0) {
        throw std::runtime_error(path.string() + ": a failure setting is out of range");
    }
}

// Police inspections (optional files).
void load_patrols(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto path = root / "economy" / "patrols.csv";
    if (!std::filesystem::exists(path)) {
        return;
    }
    auto& p = universe.patrols;
    const auto rows = read_csv_rows(path);
    require_header(rows.front(), {"key", "value"}, path);
    std::unordered_set<std::string> seen_keys;
    double patrols = p.patrols ? 1.0 : 0.0;
    const std::unordered_map<std::string, double*> fields {
        {"patrols", &patrols},
        {"inspection_days", &p.inspection_days},
        {"reinspect_days", &p.reinspect_days},
        {"review_days", &p.review_days},
        {"outbound_share", &p.outbound_share},
        {"return_reserve", &p.return_reserve},
    };
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        require_field_count(row, 2, path, i + 1);
        require_unique_id(row[0], seen_keys, path, i + 1);
        if (row[0] == "class_id") {
            p.class_id = row[1];
            continue;
        }
        const auto field = fields.find(row[0]);
        if (field == fields.end()) {
            throw std::runtime_error("Unknown key '" + row[0] + "' in " + path.string());
        }
        *field->second = parse_double(row[1], path, i + 1, row[0].c_str());
    }
    p.patrols = patrols != 0.0;
    if (p.inspection_days <= 0.0 || p.reinspect_days < 0.0 || p.review_days <= 0.0 || p.outbound_share <= 0.0
        || p.outbound_share >= 1.0 || p.return_reserve < 0.0 || p.return_reserve >= 1.0) {
        throw std::runtime_error(path.string() + ": a patrol setting is out of range");
    }
    if (std::none_of(universe.ship_classes.begin(), universe.ship_classes.end(), [&](const auto& c) { return c.id == p.class_id; })) {
        throw std::runtime_error(path.string() + ": unknown cutter class " + p.class_id);
    }
    const auto bases_path = root / "economy" / "patrol_bases.csv";
    if (!std::filesystem::exists(bases_path)) {
        return;
    }
    const auto base_rows = read_csv_rows(bases_path);
    require_header(base_rows.front(), {"faction_id", "station_id", "ships"}, bases_path);
    for (std::size_t i = 1; i < base_rows.size(); ++i) {
        const auto& row = base_rows[i];
        require_field_count(row, 3, bases_path, i + 1);
        require_station(universe, row[1], bases_path);
        // "earth": the police paid by Earth (every faction's ships); else a faction's own cutters.
        if (row[0] != "earth"
            && std::none_of(universe.factions.begin(), universe.factions.end(), [&](const auto& f) { return f.id == row[0]; })) {
            throw std::runtime_error(bases_path.string() + ": unknown faction " + row[0]);
        }
        p.bases.push_back({.faction_id = row[0], .station_id = row[1],
            .ships = static_cast<int>(parse_double(row[2], bases_path, i + 1, "ships"))});
    }
}

// Ship environment (tier 2): crew radiation and the other effects modules answer. Optional
// file and keys; the defaults in domain::ShipEnvironmentDefinition are the sourced values,
// with every mechanism switched off.
void load_ship_environment(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto env_path = root / "economy" / "ship_environment.csv";
    if (std::filesystem::exists(env_path)) {
        const auto rows = read_csv_rows(env_path);
        require_header(rows.front(), {"key", "value"}, env_path);
        std::unordered_set<std::string> seen_keys;
        auto& env = universe.ship_environment;
        double radiation = env.radiation ? 1.0 : 0.0;
        double boiloff = env.boiloff ? 1.0 : 0.0;
        double cold_storage = env.cold_storage ? 1.0 : 0.0;
        double wear = env.wear ? 1.0 : 0.0;
        const std::unordered_map<std::string, double*> fields {
            {"radiation", &radiation},
            {"gcr_msv_per_day", &env.gcr_msv_per_day},
            {"gcr_cycle_amplitude", &env.gcr_cycle_amplitude},
            {"solar_cycle_days", &env.solar_cycle_days},
            {"solar_minimum_day", &env.solar_minimum_day},
            {"gcr_gradient_per_au", &env.gcr_gradient_per_au},
            {"gcr_attenuation_g_cm2", &env.gcr_attenuation_g_cm2},
            {"shelter_occupancy", &env.shelter_occupancy},
            {"spe_rate_per_year", &env.spe_rate_per_year},
            {"spe_min_magnitude", &env.spe_min_magnitude},
            {"spe_max_magnitude", &env.spe_max_magnitude},
            {"spe_index", &env.spe_index},
            {"spe_hull_msv", &env.spe_hull_msv},
            {"spe_shelter_msv", &env.spe_shelter_msv},
            {"spe_shelter_g_cm2", &env.spe_shelter_g_cm2},
            {"career_limit_msv", &env.career_limit_msv},
            {"hazard_cr_per_msv", &env.hazard_cr_per_msv},
            {"boiloff", &boiloff},
            {"mli_flux_w_m2", &env.mli_flux_w_m2},
            {"mli_flux_exponent", &env.mli_flux_exponent},
            {"lh2_latent_j_kg", &env.lh2_latent_j_kg},
            {"tank_area_factor", &env.tank_area_factor},
            {"cold_storage", &cold_storage},
            {"wear", &wear},
            {"ntr_core_life_h", &env.ntr_core_life_h},
            {"plasma_drive_life_h", &env.plasma_drive_life_h},
            {"puncture_flux_m2_yr", &env.puncture_flux_m2_yr},
            {"puncture_flux_exponent", &env.puncture_flux_exponent},
            {"radiator_vulnerable_fraction", &env.radiator_vulnerable_fraction},
            {"radiator_segment_m2", &env.radiator_segment_m2},
            {"overhaul_wear", &env.overhaul_wear},
            {"radiator_repair_condition", &env.radiator_repair_condition},
            {"repair_team_size", &env.repair_team_size},
            {"repair_team_hours_per_day", &env.repair_team_hours_per_day},
            {"min_repair_days", &env.min_repair_days},
            {"initial_wear_max", &env.initial_wear_max},
        };
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 2, env_path, i + 1);
            require_unique_id(row[0], seen_keys, env_path, i + 1);
            const auto field = fields.find(row[0]);
            if (field == fields.end()) {
                throw std::runtime_error("Unknown key '" + row[0] + "' in " + env_path.string());
            }
            *field->second = parse_double(row[1], env_path, i + 1, row[0].c_str());
        }
        env.radiation = radiation != 0.0;
        env.boiloff = boiloff != 0.0;
        env.cold_storage = cold_storage != 0.0;
        env.wear = wear != 0.0;
        if (env.ntr_core_life_h <= 0.0 || env.plasma_drive_life_h <= 0.0 || env.overhaul_wear <= 0.0
            || env.overhaul_wear > 1.0 || env.min_repair_days < 0.0 || env.repair_team_size <= 0.0
            || env.repair_team_hours_per_day <= 0.0) {
            throw std::runtime_error(env_path.string() + ": drive lives, overhaul share or repair days out of range");
        }
        if (env.lh2_latent_j_kg <= 0.0 || env.tank_area_factor <= 0.0 || env.mli_flux_w_m2 < 0.0) {
            throw std::runtime_error(env_path.string() + ": boil-off latent heat, tank area factor or flux out of range");
        }
        if (env.solar_cycle_days <= 0.0 || env.gcr_attenuation_g_cm2 <= 0.0 || env.spe_index <= 0.0
            || env.spe_min_magnitude <= 0.0 || env.spe_max_magnitude <= env.spe_min_magnitude
            || env.spe_hull_msv <= 0.0 || env.spe_shelter_msv <= 0.0 || env.spe_shelter_g_cm2 <= 0.0
            || env.career_limit_msv <= 0.0) {
            throw std::runtime_error(env_path.string() + ": a cycle, attenuation, magnitude range, dose or limit is not positive");
        }
    }
    const auto stations_radiation_path = root / "economy" / "station_radiation.csv";
    if (std::filesystem::exists(stations_radiation_path)) {
        const auto rows = read_csv_rows(stations_radiation_path);
        require_header(rows.front(), {"station_id", "location_factor", "shield_g_cm2", "site", "note"}, stations_radiation_path);
        std::unordered_set<std::string> seen_stations;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 5, stations_radiation_path, i + 1);
            require_station(universe, row[0], stations_radiation_path);
            require_unique_id(row[0], seen_stations, stations_radiation_path, i + 1);
            if (row[3] != "orbit" && row[3] != "surface" && row[3] != "underground" && row[3] != "aerostat") {
                throw std::runtime_error("Unknown site '" + row[3] + "' in " + stations_radiation_path.string());
            }
            universe.station_radiation[row[0]] = {
                .location_factor = parse_double(row[1], stations_radiation_path, i + 1, "location_factor"),
                .shield_g_cm2 = parse_double(row[2], stations_radiation_path, i + 1, "shield_g_cm2"),
                .site = row[3],
            };
        }
    }
}

}  // namespace spacetrains::data_loader::detail
