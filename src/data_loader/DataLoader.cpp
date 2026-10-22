#include "data_loader/DataLoader.hpp"

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

namespace spacetrains::data_loader {

namespace {

std::string trim(std::string text) {
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    bool in_quotes = false;

    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (ch == '"') {
            if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                current += '"';
                ++i;
            } else {
                in_quotes = !in_quotes;
            }
            continue;
        }
        if (ch == ',' && !in_quotes) {
            fields.push_back(trim(current));
            current.clear();
            continue;
        }
        current += ch;
    }

    if (in_quotes) {
        throw std::runtime_error("Unterminated quoted CSV field");
    }

    fields.push_back(trim(current));
    return fields;
}

std::vector<std::vector<std::string>> read_csv_rows(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Failed to open file: " + path.string());
    }

    std::vector<std::vector<std::string>> rows;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (trim(line).empty()) {
            continue;
        }
        rows.push_back(split_csv_line(line));
    }

    if (rows.empty()) {
        throw std::runtime_error("CSV file is empty: " + path.string());
    }
    return rows;
}

void require_header(
    const std::vector<std::string>& actual,
    const std::vector<std::string>& expected,
    const std::filesystem::path& path) {
    if (actual != expected) {
        std::ostringstream message;
        message << "Unexpected CSV header in " << path.string();
        throw std::runtime_error(message.str());
    }
}

void require_field_count(
    const std::vector<std::string>& row,
    std::size_t expected,
    const std::filesystem::path& path,
    std::size_t line_number) {
    if (row.size() != expected) {
        std::ostringstream message;
        message << "Unexpected field count in " << path.string() << ":" << line_number
                << " expected " << expected << " got " << row.size();
        throw std::runtime_error(message.str());
    }
}

double parse_double(
    const std::string& text,
    const std::filesystem::path& path,
    std::size_t line_number,
    const char* field_name) {
    try {
        std::size_t parsed = 0;
        const double value = std::stod(text, &parsed);
        if (parsed != text.size()) {
            throw std::runtime_error("trailing characters");
        }
        return value;
    } catch (const std::exception&) {
        std::ostringstream message;
        message << "Invalid number for " << field_name << " in " << path.string() << ":" << line_number;
        throw std::runtime_error(message.str());
    }
}

std::int64_t parse_int64(
    const std::string& text,
    const std::filesystem::path& path,
    std::size_t line_number,
    const char* field_name) {
    try {
        std::size_t parsed = 0;
        const std::int64_t value = std::stoll(text, &parsed);
        if (parsed != text.size()) {
            throw std::runtime_error("trailing characters");
        }
        return value;
    } catch (const std::exception&) {
        std::ostringstream message;
        message << "Invalid integer for " << field_name << " in " << path.string() << ":" << line_number;
        throw std::runtime_error(message.str());
    }
}

void require_unique_id(
    const std::string& id,
    std::unordered_set<std::string>& seen_ids,
    const std::filesystem::path& path,
    std::size_t line_number) {
    if (!seen_ids.insert(id).second) {
        std::ostringstream message;
        message << "Duplicate id '" << id << "' in " << path.string() << ":" << line_number;
        throw std::runtime_error(message.str());
    }
}

domain::Inventory parse_inventory(
    const std::string& text,
    const std::filesystem::path& path,
    std::size_t line_number) {
    domain::Inventory inventory;
    if (trim(text).empty()) {
        return inventory;
    }

    std::stringstream entries(text);
    std::string entry;
    while (std::getline(entries, entry, ';')) {
        entry = trim(entry);
        if (entry.empty()) {
            continue;
        }
        const auto separator = entry.find(':');
        if (separator == std::string::npos) {
            std::ostringstream message;
            message << "Invalid inventory entry in " << path.string() << ":" << line_number;
            throw std::runtime_error(message.str());
        }

        const std::string commodity_id = trim(entry.substr(0, separator));
        const std::string amount_text = trim(entry.substr(separator + 1));
        inventory[commodity_id] = parse_double(amount_text, path, line_number, commodity_id.c_str());
    }
    return inventory;
}

template <typename Collection>
bool contains_id(const Collection& values, const std::string& id) {
    return std::any_of(values.begin(), values.end(), [&](const auto& value) { return value.id == id; });
}

}  // namespace

domain::UniverseDefinition DataLoader::load_universe(const std::filesystem::path& root) const {
    domain::UniverseDefinition universe;

    const auto bodies_path = root / "bodies" / "bodies.csv";
    const auto factions_path = root / "factions" / "factions.csv";
    const auto commodities_path = root / "commodities" / "commodities.csv";
    const auto ship_classes_path = root / "ship_classes" / "ship_classes.csv";
    const auto ship_components_path = root / "ship_classes" / "ship_components.csv";
    const auto ship_modules_path = root / "ship_classes" / "modules.csv";
    const auto hull_slots_path = root / "ship_classes" / "hull_slots.csv";
    const auto stations_path = root / "stations" / "stations.csv";
    const auto recipes_path = root / "recipes" / "recipes.csv";
    const auto station_recipes_path = root / "recipes" / "station_recipes.csv";
    const auto export_markets_path = root / "economy" / "export_markets.csv";
    const auto ships_path = root / "ships" / "ships.csv";
    const auto ship_operations_path = root / "economy" / "ship_operations.csv";
    const auto labour_path = root / "economy" / "labour.csv";
    const auto spares_path = root / "ship_classes" / "spares.csv";
    const auto fuel_supply_path = root / "economy" / "fuel_supply.csv";
    const auto fuel_factories_path = root / "economy" / "fuel_factories.csv";
    const auto open_economy_path = root / "economy" / "open_economy.csv";
    const auto fleet_investment_path = root / "economy" / "fleet_investment.csv";

    {
        const auto rows = read_csv_rows(bodies_path);
        require_header(rows.front(),
            {"id", "name", "radius_m", "mu_m3_s2", "parent_id", "semi_major_axis_m", "eccentricity", "orbital_period_s", "phase_at_epoch_rad"},
            bodies_path);
        std::unordered_set<std::string> seen_ids;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 9, bodies_path, i + 1);
            require_unique_id(row[0], seen_ids, bodies_path, i + 1);
            universe.bodies.push_back({
                .id = row[0],
                .name = row[1],
                .radius_m = parse_double(row[2], bodies_path, i + 1, "radius_m"),
                .mu_m3_s2 = parse_double(row[3], bodies_path, i + 1, "mu_m3_s2"),
                .orbit = {
                    .parent_id = row[4],
                    .semi_major_axis_m = parse_double(row[5], bodies_path, i + 1, "semi_major_axis_m"),
                    .eccentricity = parse_double(row[6], bodies_path, i + 1, "eccentricity"),
                    .orbital_period_s = parse_double(row[7], bodies_path, i + 1, "orbital_period_s"),
                    .phase_at_epoch_rad = parse_double(row[8], bodies_path, i + 1, "phase_at_epoch_rad"),
                },
            });
        }
    }

    {
        const auto rows = read_csv_rows(factions_path);
        require_header(rows.front(), {"id", "name", "color"}, factions_path);
        std::unordered_set<std::string> seen_ids;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 3, factions_path, i + 1);
            require_unique_id(row[0], seen_ids, factions_path, i + 1);
            universe.factions.push_back({.id = row[0], .name = row[1], .color_hex = row[2]});
        }
    }

    {
        const auto rows = read_csv_rows(commodities_path);
        require_header(rows.front(), {"id", "name", "mass_per_unit_kg", "decay_fraction_per_day", "base_price"}, commodities_path);
        std::unordered_set<std::string> seen_ids;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 5, commodities_path, i + 1);
            require_unique_id(row[0], seen_ids, commodities_path, i + 1);
            universe.commodities.push_back({
                .id = row[0],
                .name = row[1],
                .mass_per_unit_kg = parse_double(row[2], commodities_path, i + 1, "mass_per_unit_kg"),
                .decay_fraction_per_day = parse_double(row[3], commodities_path, i + 1, "decay_fraction_per_day"),
                .base_price = parse_double(row[4], commodities_path, i + 1, "base_price"),
            });
        }
    }

    {
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

    // Ship components (tier 1 of docs/plans/ship_internals.md): the parts of every class. They
    // must decompose the class rows exactly, so the simulation's totals and the parts agree.
    {
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
    {
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

    {
        // Passengers (docs/plans/passengers.md): every key optional, defaults in PassengerDefinition.
        const auto passengers_path = root / "economy" / "passengers.csv";
        auto& passengers = universe.passengers;
        if (std::filesystem::exists(passengers_path)) {
            const auto rows = read_csv_rows(passengers_path);
            require_header(rows.front(), {"key", "value"}, passengers_path);
            std::unordered_set<std::string> seen_keys;
            const std::unordered_map<std::string, double*> fields {
                {"passengers", &passengers.enabled},
                {"rotation", &passengers.rotation},
                {"migration", &passengers.migration},
                {"discretionary", &passengers.discretionary},
                {"body_luggage_kg", &passengers.body_luggage_kg},
                {"consumables_kg_per_day", &passengers.consumables_kg_per_day},
                {"recovery_days_per_flight_day", &passengers.recovery_days_per_flight_day},
                {"rotation_share", &passengers.rotation_share},
                {"hardship_per_year", &passengers.hardship_per_year},
                {"resident_wage_multiple", &passengers.resident_wage_multiple},
                {"visit_trips_per_person_year", &passengers.visit_trips_per_person_year},
                {"earth_population_weight", &passengers.earth_population_weight},
                {"fare_elasticity", &passengers.fare_elasticity},
                {"demand_bands", &passengers.demand_bands},
                {"band_ratio", &passengers.band_ratio},
                {"visit_days", &passengers.visit_days},
                {"tank_wrap_mean_fill", &passengers.tank_wrap_mean_fill},
                {"settlers_sponsored", &passengers.settlers_sponsored},
                {"settler_payback_half_years", &passengers.settler_payback_half_years},
                {"settler_payback_exponent", &passengers.settler_payback_exponent},
                {"passage_recovery_days", &passengers.passage_recovery_days},
                {"passage_max_markup", &passengers.passage_max_markup},
                {"visitor_batch_days", &passengers.visitor_batch_days},
                {"min_leg_passengers", &passengers.min_leg_passengers},
                {"rider_time_min_share", &passengers.rider_time_min_share},
                {"rider_time_relative_step", &passengers.rider_time_relative_step},
            };
            for (std::size_t i = 1; i < rows.size(); ++i) {
                const auto& row = rows[i];
                require_field_count(row, 2, passengers_path, i + 1);
                require_unique_id(row[0], seen_keys, passengers_path, i + 1);
                if (row[0] == "rotation_grade") {
                    passengers.rotation_grade = row[1];
                    continue;
                }
                if (row[0] == "earth_gateways") {
                    passengers.earth_gateways.clear();
                    std::size_t at = 0;
                    while (at <= row[1].size()) {
                        const auto end = std::min(row[1].find('|', at), row[1].size());
                        if (end > at) {
                            passengers.earth_gateways.push_back(row[1].substr(at, end - at));
                        }
                        at = end + 1;
                    }
                    continue;
                }
                const auto field = fields.find(row[0]);
                if (field == fields.end()) {
                    throw std::runtime_error("Unknown key '" + row[0] + "' in " + passengers_path.string());
                }
                *field->second = parse_double(row[1], passengers_path, i + 1, row[0].c_str());
            }
            if (passengers.body_luggage_kg < 0.0 || passengers.consumables_kg_per_day < 0.0
                || passengers.recovery_days_per_flight_day < 0.0 || passengers.rotation_share < 0.0
                || passengers.rotation_share > 1.0 || passengers.hardship_per_year <= 0.0
                || passengers.demand_bands < 1.0 || passengers.band_ratio <= 1.0 || passengers.visit_days <= 0.0
                || passengers.fare_elasticity >= 0.0) {
                throw std::runtime_error("Passenger settings out of range in " + passengers_path.string());
            }
        }
        if (passengers.enabled == 0.0) {
            // Without passengers the liner classes (built-in berths) do not exist: the economy is the
            // one before passengers, class for class.
            std::unordered_set<std::string> liner_hulls;
            std::erase_if(universe.ship_classes, [&](const auto& ship_class) {
                if (ship_class.berths > 0.0) {
                    liner_hulls.insert(ship_class.hull_id);
                    return true;
                }
                return false;
            });
            for (const auto& ship_class : universe.ship_classes) {
                liner_hulls.erase(ship_class.hull_id);
            }
            std::erase_if(universe.ship_modules, [&](const auto& module) { return liner_hulls.contains(module.hull_id); });
            for (const auto& hull : liner_hulls) {
                universe.hull_slots.erase(hull);
            }
        }
    }

    {
        const auto rows = read_csv_rows(stations_path);
        require_header(rows.front(),
            {"id", "name", "faction_id", "parent_body_id", "altitude_m", "theta_rad", "population", "economy_profile_id",
             "storage_capacity_units", "initial_credits", "initial_inventory"},
            stations_path);
        std::unordered_set<std::string> seen_ids;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 11, stations_path, i + 1);
            require_unique_id(row[0], seen_ids, stations_path, i + 1);
            universe.stations.push_back({
                .id = row[0],
                .name = row[1],
                .faction_id = row[2],
                .parent_body_id = row[3],
                .altitude_m = parse_double(row[4], stations_path, i + 1, "altitude_m"),
                .theta_rad = parse_double(row[5], stations_path, i + 1, "theta_rad"),
                .population = parse_int64(row[6], stations_path, i + 1, "population"),
                .economy_profile_id = row[7],
                .storage_capacity_units = parse_double(row[8], stations_path, i + 1, "storage_capacity_units"),
                .initial_credits = parse_double(row[9], stations_path, i + 1, "initial_credits"),
                .initial_inventory = parse_inventory(row[10], stations_path, i + 1),
            });
        }
    }

    // A recipe row's role: empty for an output, "upkeep", "consume" or "input:<output>[|<output>]".
    const auto set_role = [](domain::RecipeDefinition& recipe, const std::string& text,
                              const std::filesystem::path& path, std::size_t line) {
        const auto where = path.string() + ":" + std::to_string(line);
        if (recipe.units_per_day > 0.0) {
            if (!text.empty()) {
                throw std::runtime_error("An output must have no role in " + where);
            }
            recipe.role = domain::RecipeRole::Output;
            return;
        }
        if (text == "upkeep") {
            recipe.role = domain::RecipeRole::Upkeep;
        } else if (text == "consume") {
            recipe.role = domain::RecipeRole::Consume;
        } else if (text.starts_with("input:") && text.size() > 6) {
            recipe.role = domain::RecipeRole::Input;
            std::size_t start = 6;
            while (start <= text.size()) {
                const auto end = std::min(text.find('|', start), text.size());
                if (end == start) {
                    throw std::runtime_error("Empty output name in role '" + text + "' in " + where);
                }
                recipe.feeds.push_back(text.substr(start, end - start));
                start = end + 1;
            }
        } else {
            throw std::runtime_error("A consumed good needs a role (upkeep, consume or input:<output>), got '"
                + text + "' in " + where);
        }
    };

    {
        const auto rows = read_csv_rows(recipes_path);
        require_header(rows.front(), {"profile_id", "commodity_id", "units_per_day", "role"}, recipes_path);
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 4, recipes_path, i + 1);
            domain::RecipeDefinition recipe {
                .profile_id = row[0],
                .commodity_id = row[1],
                .units_per_day = parse_double(row[2], recipes_path, i + 1, "units_per_day"),
            };
            set_role(recipe, row[3], recipes_path, i + 1);
            universe.recipes.push_back(std::move(recipe));
        }
    }

    {
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

    {
        // The wage scale: key,value rows, grades as "grade.<name>" multiples of the reference.
        const auto rows = read_csv_rows(labour_path);
        require_header(rows.front(), {"key", "value"}, labour_path);
        std::unordered_set<std::string> seen_keys;
        auto& labour = universe.labour;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 2, labour_path, i + 1);
            require_unique_id(row[0], seen_keys, labour_path, i + 1);
            const double value = parse_double(row[1], labour_path, i + 1, row[0].c_str());
            if (value <= 0.0) {
                throw std::runtime_error("'" + row[0] + "' must be positive in " + labour_path.string());
            }
            if (row[0] == "reference_wage_cr_per_day") {
                labour.reference_wage_cr_per_day = value;
            } else if (row[0] == "shift_hours") {
                labour.shift_hours = value;
            } else if (row[0].starts_with("grade.")) {
                labour.grades[row[0].substr(6)] = value;
            } else {
                throw std::runtime_error("Unknown key '" + row[0] + "' in " + labour_path.string());
            }
        }
        if (!labour.grades.contains("crew")) {
            throw std::runtime_error("Missing key 'grade.crew' in " + labour_path.string());
        }
        // Crews are paid at the crew grade.
        universe.ship_operations.wage_cr_per_crew_day = labour.wage_cr_per_day("crew");
    }
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
    {
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

    {
        const auto rows = read_csv_rows(fuel_supply_path);
        require_header(rows.front(), {"key", "value"}, fuel_supply_path);
        std::unordered_set<std::string> seen_keys;
        auto& fuel_supply = universe.fuel_supply;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 2, fuel_supply_path, i + 1);
            require_unique_id(row[0], seen_keys, fuel_supply_path, i + 1);
            const double value = parse_double(row[1], fuel_supply_path, i + 1, row[0].c_str());
            if (value < 0.0) {
                throw std::runtime_error("'" + row[0] + "' must not be negative in " + fuel_supply_path.string());
            }
            if (row[0] == "factory_buffer_days") {
                fuel_supply.factory_buffer_days = value;
            } else {
                throw std::runtime_error("Unknown key '" + row[0] + "' in " + fuel_supply_path.string());
            }
        }
        for (const char* required : {"factory_buffer_days"}) {
            if (!seen_keys.contains(required)) {
                throw std::runtime_error(std::string("Missing key '") + required + "' in " + fuel_supply_path.string());
            }
        }
    }

    const auto require_station = [&](const std::string& station_id, const std::filesystem::path& path) {
        const bool known = std::any_of(universe.stations.begin(), universe.stations.end(),
            [&](const domain::StationDefinition& station) { return station.id == station_id; });
        if (!known) {
            throw std::runtime_error("Unknown station '" + station_id + "' in " + path.string());
        }
    };
    const auto require_commodity = [&](const std::string& commodity_id, const std::filesystem::path& path) {
        const bool known = std::any_of(universe.commodities.begin(), universe.commodities.end(),
            [&](const domain::CommodityDefinition& commodity) { return commodity.id == commodity_id; });
        if (!known) {
            throw std::runtime_error("Unknown commodity '" + commodity_id + "' in " + path.string());
        }
    };

    {
        const auto rows = read_csv_rows(station_recipes_path);
        require_header(rows.front(), {"station_id", "commodity_id", "units_per_day", "role"}, station_recipes_path);
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 4, station_recipes_path, i + 1);
            require_station(row[0], station_recipes_path);
            require_commodity(row[1], station_recipes_path);
            domain::RecipeDefinition recipe {
                .profile_id = {},
                .commodity_id = row[1],
                .units_per_day = parse_double(row[2], station_recipes_path, i + 1, "units_per_day"),
                .station_id = row[0],
            };
            set_role(recipe, row[3], station_recipes_path, i + 1);
            universe.recipes.push_back(std::move(recipe));
        }
    }

    // Every input feeds an output of the same station (its profile's rows or its own).
    for (const auto& station : universe.stations) {
        const auto applies = [&](const domain::RecipeDefinition& recipe) {
            return recipe.station_id.empty() ? recipe.profile_id == station.economy_profile_id : recipe.station_id == station.id;
        };
        for (const auto& recipe : universe.recipes) {
            if (!applies(recipe)) {
                continue;
            }
            for (const auto& output : recipe.feeds) {
                const bool produced = std::any_of(universe.recipes.begin(), universe.recipes.end(),
                    [&](const domain::RecipeDefinition& other) {
                        return applies(other) && other.commodity_id == output && other.units_per_day > 0.0;
                    });
                if (!produced) {
                    throw std::runtime_error("Recipe input " + recipe.commodity_id + " at " + station.id
                        + " feeds '" + output + "', which the station does not produce");
                }
            }
        }
    }

    {
        const auto upkeep_path = root / "economy" / "upkeep_penalties.csv";
        const auto rows = read_csv_rows(upkeep_path);
        require_header(rows.front(), {"commodity_id", "full_shortage_multiplier", "emergency"}, upkeep_path);
        std::unordered_set<std::string> seen_ids;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 3, upkeep_path, i + 1);
            require_unique_id(row[0], seen_ids, upkeep_path, i + 1);
            require_commodity(row[0], upkeep_path);
            const double multiplier = parse_double(row[1], upkeep_path, i + 1, "full_shortage_multiplier");
            // No penalty is total: a station that stops completely could never recover.
            if (multiplier <= 0.0 || multiplier > 1.0) {
                throw std::runtime_error("full_shortage_multiplier must be in (0, 1] in " + upkeep_path.string());
            }
            universe.upkeep_penalties[row[0]] = multiplier;
            if (row[2] == "1") {
                universe.emergency_goods.insert(row[0]);
            } else if (row[2] != "0") {
                throw std::runtime_error("emergency must be 0 or 1 in " + upkeep_path.string());
            }
        }
    }

    if (std::filesystem::exists(export_markets_path)) {
        const auto rows = read_csv_rows(export_markets_path);
        require_header(rows.front(), {"station_id", "commodity_id", "units_per_day"}, export_markets_path);
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 3, export_markets_path, i + 1);
            require_station(row[0], export_markets_path);
            require_commodity(row[1], export_markets_path);
            const double units_per_day = parse_double(row[2], export_markets_path, i + 1, "units_per_day");
            if (units_per_day <= 0.0) {
                throw std::runtime_error("units_per_day must be positive in " + export_markets_path.string());
            }
            universe.export_markets.push_back({.station_id = row[0], .commodity_id = row[1], .units_per_day = units_per_day});
        }
    }

    // Economy overhaul: households, background economies, surface markets, station incomes
    // (docs/plans/economy_overhaul.md). All optional; without households.csv the economy runs as before.
    if (const auto households_path = root / "economy" / "households.csv"; std::filesystem::exists(households_path)) {
        const auto rows = read_csv_rows(households_path);
        require_header(rows.front(), {"key", "value"}, households_path);
        auto& households = universe.households;
        const std::unordered_map<std::string, double*> fields {
            {"enabled", &households.enabled},
            {"participation", &households.participation},
            {"savings_days", &households.savings_days},
            {"spend_days", &households.spend_days},
            {"migrant_savings_cr", &households.migrant_savings_cr},
            {"visitor_spend_cr_per_day", &households.visitor_spend_cr_per_day},
            {"credits_per_usd", &households.credits_per_usd},
            {"yard_share", &households.yard_share},
            {"surface_depth_units", &households.surface_depth_units},
            {"supply_price_elasticity", &households.supply_price_elasticity},
            {"min_utilization", &households.min_utilization},
            {"consume_income_elasticity", &households.consume_income_elasticity},
            {"consume_price_elasticity", &households.consume_price_elasticity},
            {"upkeep_price_elasticity", &households.upkeep_price_elasticity},
        };
        std::unordered_set<std::string> seen_keys;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 2, households_path, i + 1);
            require_unique_id(row[0], seen_keys, households_path, i + 1);
            if (row[0] == "yards") {
                std::stringstream stream(row[1]);
                std::string station_id;
                while (std::getline(stream, station_id, '|')) {
                    require_station(station_id, households_path);
                    households.yards.push_back(station_id);
                }
                continue;
            }
            const double value = parse_double(row[1], households_path, i + 1, row[0].c_str());
            if (value < 0.0 && !row[0].ends_with("price_elasticity")) {
                throw std::runtime_error("'" + row[0] + "' must not be negative in " + households_path.string());
            }
            if (row[0].starts_with("wage_multiple.")) {
                const auto station_id = row[0].substr(std::string("wage_multiple.").size());
                require_station(station_id, households_path);
                households.wage_multiple[station_id] = value;
                continue;
            }
            const auto field = fields.find(row[0]);
            if (field == fields.end()) {
                throw std::runtime_error("Unknown key '" + row[0] + "' in " + households_path.string());
            }
            *field->second = value;
        }
        if (households.spend_days <= 0.0 || households.yard_share > 1.0) {
            throw std::runtime_error("spend_days must be positive and yard_share at most 1 in " + households_path.string());
        }
    }
    if (const auto labour_path = root / "economy" / "labour_market.csv"; std::filesystem::exists(labour_path)) {
        const auto rows = read_csv_rows(labour_path);
        require_header(rows.front(), {"key", "value"}, labour_path);
        auto& labour = universe.labour_market;
        const std::unordered_map<std::string, double*> fields {
            {"enabled", &labour.enabled},
            {"earth_wage_median_cr_per_day", &labour.earth_wage_median_cr_per_day},
            {"earth_wage_sigma", &labour.earth_wage_sigma},
            {"station_wage_sigma", &labour.station_wage_sigma},
            {"stay_years", &labour.stay_years},
            {"hardship_share", &labour.hardship_share},
            {"wage_adjust_per_year", &labour.wage_adjust_per_year},
            {"min_wage_multiple", &labour.min_wage_multiple},
            {"vacancy_fill_days", &labour.vacancy_fill_days},
            {"unemployed_leave_days", &labour.unemployed_leave_days},
            {"unemployment_benefit", &labour.unemployment_benefit},
            {"visitor_budget_years", &labour.visitor_budget_years},
            {"visitor_income_sigma", &labour.visitor_income_sigma},
        };
        std::unordered_set<std::string> seen_keys;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 2, labour_path, i + 1);
            require_unique_id(row[0], seen_keys, labour_path, i + 1);
            const auto field = fields.find(row[0]);
            if (field == fields.end()) {
                throw std::runtime_error("Unknown key '" + row[0] + "' in " + labour_path.string());
            }
            *field->second = parse_double(row[1], labour_path, i + 1, row[0].c_str());
            if (*field->second < 0.0) {
                throw std::runtime_error("'" + row[0] + "' must not be negative in " + labour_path.string());
            }
        }
        if (labour.vacancy_fill_days <= 0.0 || labour.unemployed_leave_days <= 0.0 || labour.stay_years <= 0.0
            || labour.earth_wage_sigma <= 0.0 || labour.visitor_income_sigma <= 0.0) {
            throw std::runtime_error("labour_market.csv: days, years and spreads must be positive");
        }
    }
    if (const auto economies_path = root / "economy" / "background_economies.csv"; std::filesystem::exists(economies_path)) {
        const auto rows = read_csv_rows(economies_path);
        require_header(rows.front(),
            {"id", "name", "gateways", "launch_usd_per_kg", "landing_usd_per_kg", "initial_credits", "income_cr_per_year"},
            economies_path);
        std::unordered_set<std::string> seen_ids;
        const double scale = universe.households.credits_per_usd;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 7, economies_path, i + 1);
            require_unique_id(row[0], seen_ids, economies_path, i + 1);
            domain::BackgroundEconomyDefinition economy {
                .id = row[0],
                .name = row[1],
                .launch_cr_per_kg = parse_double(row[3], economies_path, i + 1, "launch_usd_per_kg") * scale,
                .landing_cr_per_kg = parse_double(row[4], economies_path, i + 1, "landing_usd_per_kg") * scale,
                .initial_credits = parse_double(row[5], economies_path, i + 1, "initial_credits"),
                .income_cr_per_day = parse_double(row[6], economies_path, i + 1, "income_cr_per_year") / 365.25,
            };
            std::stringstream stream(row[2]);
            std::string station_id;
            while (std::getline(stream, station_id, '|')) {
                require_station(station_id, economies_path);
                economy.gateways.push_back(station_id);
            }
            if (economy.gateways.empty()) {
                throw std::runtime_error("Background economy '" + row[0] + "' has no gateway in " + economies_path.string());
            }
            universe.background_economies.push_back(std::move(economy));
        }
    }
    if (const auto markets_path = root / "economy" / "surface_markets.csv"; std::filesystem::exists(markets_path)) {
        // Each row: what an economy trades at one gateway (station_id), or at all of them (empty).
        // Ask = (surface + launch) x unit mass, bid = (surface - landing) x unit mass, in credits;
        // a bid_share instead sets the bid from the station's reference price at run time.
        const auto rows = read_csv_rows(markets_path);
        require_header(rows.front(),
            {"economy_id", "station_id", "commodity_id", "surface_usd_per_kg", "sells", "buys", "sell_units_per_day",
                "buy_units_per_day", "bid_share"},
            markets_path);
        const double scale = universe.households.credits_per_usd;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 9, markets_path, i + 1);
            const auto economy = std::find_if(universe.background_economies.begin(), universe.background_economies.end(),
                [&](const domain::BackgroundEconomyDefinition& e) { return e.id == row[0]; });
            if (economy == universe.background_economies.end()) {
                throw std::runtime_error("Unknown background economy '" + row[0] + "' in " + markets_path.string());
            }
            require_commodity(row[2], markets_path);
            const auto commodity = std::find_if(universe.commodities.begin(), universe.commodities.end(),
                [&](const domain::CommodityDefinition& c) { return c.id == row[2]; });
            const double surface = parse_double(row[3], markets_path, i + 1, "surface_usd_per_kg") * scale;
            const bool sells = row[4] == "1";
            const bool buys = row[5] == "1";
            const double sell_units = parse_double(row[6], markets_path, i + 1, "sell_units_per_day");
            const double buy_units = parse_double(row[7], markets_path, i + 1, "buy_units_per_day");
            const double bid_share = parse_double(row[8], markets_path, i + 1, "bid_share");
            std::vector<std::string> stations;
            if (row[1].empty()) {
                stations = economy->gateways;
            } else {
                require_station(row[1], markets_path);
                stations.push_back(row[1]);
            }
            for (const auto& station_id : stations) {
                domain::SurfaceMarketDefinition market {
                    .economy_id = row[0],
                    .station_id = station_id,
                    .commodity_id = row[2],
                    .ask_cr = sells ? (surface + economy->launch_cr_per_kg) * commodity->mass_per_unit_kg : 0.0,
                    .bid_cr = buys && bid_share <= 0.0
                        ? std::max(0.0, surface - economy->landing_cr_per_kg) * commodity->mass_per_unit_kg : 0.0,
                    .sell_units_per_day = sell_units,
                    .buy_units_per_day = buy_units,
                    .bid_share = buys ? bid_share : 0.0,
                };
                if (buys && market.bid_cr <= 0.0 && market.bid_share <= 0.0) {
                    throw std::runtime_error("A surface market that buys needs a positive bid in " + markets_path.string());
                }
                universe.surface_markets.push_back(std::move(market));
            }
        }
    }
    if (const auto incomes_path = root / "economy" / "station_incomes.csv"; std::filesystem::exists(incomes_path)) {
        const auto rows = read_csv_rows(incomes_path);
        require_header(rows.front(), {"station_id", "kind", "payer_id", "cr_per_year"}, incomes_path);
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 4, incomes_path, i + 1);
            require_station(row[0], incomes_path);
            const bool payer_known = std::any_of(universe.background_economies.begin(), universe.background_economies.end(),
                [&](const domain::BackgroundEconomyDefinition& e) { return e.id == row[2]; });
            if (!payer_known) {
                throw std::runtime_error("Unknown payer '" + row[2] + "' in " + incomes_path.string());
            }
            universe.station_incomes.push_back({
                .station_id = row[0],
                .kind = row[1],
                .payer_id = row[2],
                .cr_per_day = parse_double(row[3], incomes_path, i + 1, "cr_per_year") / 365.25,
            });
        }
    }

    {
        const auto rows = read_csv_rows(fuel_factories_path);
        require_header(rows.front(), {"station_id", "output_units_per_day"}, fuel_factories_path);
        std::unordered_set<std::string> seen_ids;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 2, fuel_factories_path, i + 1);
            require_unique_id(row[0], seen_ids, fuel_factories_path, i + 1);
            const bool known = std::any_of(universe.stations.begin(), universe.stations.end(),
                [&](const domain::StationDefinition& station) { return station.id == row[0]; });
            if (!known) {
                throw std::runtime_error("Fuel factory at unknown station '" + row[0] + "' in " + fuel_factories_path.string());
            }
            const double output = parse_double(row[1], fuel_factories_path, i + 1, "output_units_per_day");
            if (output <= 0.0) {
                throw std::runtime_error("output_units_per_day must be positive in " + fuel_factories_path.string());
            }
            universe.fuel_supply.factory_output_units_per_day[row[0]] = output;
        }
    }

    {
        const auto rows = read_csv_rows(open_economy_path);
        require_header(rows.front(), {"key", "value"}, open_economy_path);
        std::unordered_set<std::string> seen_keys;
        auto& open_economy = universe.open_economy;
        const std::unordered_map<std::string, double*> fields {
            {"station_credit_floor", &open_economy.station_credit_floor},
            {"station_credit_ceiling", &open_economy.station_credit_ceiling},
            {"station_balance_days", &open_economy.station_balance_days},
            {"ship_cash_reserve", &open_economy.ship_cash_reserve},
            {"dividend_days", &open_economy.dividend_days},
            {"money_supply_days", &open_economy.money_supply_days},
            {"core_crew_fraction", &open_economy.core_crew_fraction},
            {"faction_loan_to_value", &open_economy.faction_loan_to_value},
            {"faction_tax_years", &open_economy.faction_tax_years},
        };
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 2, open_economy_path, i + 1);
            require_unique_id(row[0], seen_keys, open_economy_path, i + 1);
            const double value = parse_double(row[1], open_economy_path, i + 1, row[0].c_str());
            if (value < 0.0) {
                throw std::runtime_error("'" + row[0] + "' must not be negative in " + open_economy_path.string());
            }
            const auto field = fields.find(row[0]);
            if (field == fields.end()) {
                throw std::runtime_error("Unknown key '" + row[0] + "' in " + open_economy_path.string());
            }
            *field->second = value;
        }
        for (const auto& [key, field] : fields) {
            if (!seen_keys.contains(key)) {
                throw std::runtime_error("Missing key '" + key + "' in " + open_economy_path.string());
            }
        }
        if (open_economy.station_balance_days <= 0.0 || open_economy.dividend_days <= 0.0) {
            throw std::runtime_error("station_balance_days and dividend_days must be positive in " + open_economy_path.string());
        }
        if (open_economy.station_credit_ceiling > 0.0
            && open_economy.station_credit_ceiling < open_economy.station_credit_floor) {
            throw std::runtime_error("station_credit_ceiling must not be below the floor in " + open_economy_path.string());
        }
    }

    {
        const auto rows = read_csv_rows(fleet_investment_path);
        require_header(rows.front(), {"key", "value"}, fleet_investment_path);
        std::unordered_set<std::string> seen_keys;
        auto& investment = universe.fleet_investment;
        const std::unordered_map<std::string, double*> fields {
            {"review_days", &investment.review_days},
            {"hurdle_return_per_year", &investment.hurdle_return_per_year},
            {"working_capital", &investment.working_capital},
            {"build_days", &investment.build_days},
            {"layup_sale_days", &investment.layup_sale_days},
            {"salvage_fraction", &investment.salvage_fraction},
            {"route_commitment_days", &investment.route_commitment_days},
            {"max_ships_per_review", &investment.max_ships_per_review},
            {"purchase_spacing_days", &investment.purchase_spacing_days},
            {"max_fleet_size", &investment.max_fleet_size},
            {"max_fleet_hold_units", &investment.max_fleet_hold_units},
        };
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 2, fleet_investment_path, i + 1);
            require_unique_id(row[0], seen_keys, fleet_investment_path, i + 1);
            const double value = parse_double(row[1], fleet_investment_path, i + 1, row[0].c_str());
            if (value < 0.0) {
                throw std::runtime_error("'" + row[0] + "' must not be negative in " + fleet_investment_path.string());
            }
            const auto field = fields.find(row[0]);
            if (field == fields.end()) {
                throw std::runtime_error("Unknown key '" + row[0] + "' in " + fleet_investment_path.string());
            }
            *field->second = value;
        }
        for (const auto& [key, field] : fields) {
            if (!seen_keys.contains(key)) {
                throw std::runtime_error("Missing key '" + key + "' in " + fleet_investment_path.string());
            }
        }
        if (investment.salvage_fraction > 1.0) {
            throw std::runtime_error("salvage_fraction must not exceed 1 in " + fleet_investment_path.string());
        }
    }

    {
        const auto liners_path = root / "economy" / "liners.csv";
        const auto rows = read_csv_rows(liners_path);
        require_header(rows.front(), {"ship_id", "stops"}, liners_path);
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 2, liners_path, i + 1);
            const auto seed = std::find_if(universe.ship_seeds.begin(), universe.ship_seeds.end(),
                [&](const domain::ShipSeedDefinition& s) { return s.id == row[0]; });
            if (seed == universe.ship_seeds.end()) {
                throw std::runtime_error("Liner '" + row[0] + "' is not a ship in ships.csv (" + liners_path.string() + ")");
            }
            std::vector<std::string> stops;
            std::size_t start = 0;
            while (start <= row[1].size()) {
                const auto end = std::min(row[1].find('|', start), row[1].size());
                stops.push_back(row[1].substr(start, end - start));
                require_station(stops.back(), liners_path);
                start = end + 1;
            }
            if (stops.size() < 2 || std::unordered_set<std::string>(stops.begin(), stops.end()).size() != stops.size()) {
                throw std::runtime_error("A liner needs two or more different stops in " + liners_path.string());
            }
            if (std::find(stops.begin(), stops.end(), seed->start_station_id) == stops.end()) {
                throw std::runtime_error("Liner '" + row[0] + "' must start at one of its stops in " + liners_path.string());
            }
            universe.liners[row[0]] = std::move(stops);
        }
    }

    {
        const auto growth_path = root / "economy" / "growth.csv";
        const auto rows = read_csv_rows(growth_path);
        require_header(rows.front(), {"key", "value"}, growth_path);
        std::unordered_set<std::string> seen_keys;
        auto& growth = universe.growth;
        const std::unordered_map<std::string, double*> fields {
            {"growth_per_year", &growth.growth_per_year},
            {"decline_per_year", &growth.decline_per_year},
            {"max_population_factor", &growth.max_population_factor},
            {"window_days", &growth.window_days},
            {"grow_above", &growth.grow_above},
            {"decline_below", &growth.decline_below},
        };
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 2, growth_path, i + 1);
            require_unique_id(row[0], seen_keys, growth_path, i + 1);
            const auto field = fields.find(row[0]);
            if (field == fields.end()) {
                throw std::runtime_error("Unknown key '" + row[0] + "' in " + growth_path.string());
            }
            *field->second = parse_double(row[1], growth_path, i + 1, row[0].c_str());
        }
        for (const auto& [key, field] : fields) {
            if (!seen_keys.contains(key)) {
                throw std::runtime_error("Missing key '" + key + "' in " + growth_path.string());
            }
        }
        if (growth.growth_per_year < 0.0 || growth.decline_per_year < 0.0 || growth.max_population_factor < 1.0
            || growth.window_days <= 0.0 || growth.decline_below > growth.grow_above) {
            throw std::runtime_error("growth rates must be >= 0, max_population_factor >= 1, window_days > 0 and "
                "decline_below <= grow_above in " + growth_path.string());
        }
    }

    {
        const auto pricing_path = root / "economy" / "pricing.csv";
        const auto rows = read_csv_rows(pricing_path);
        require_header(rows.front(), {"key", "value"}, pricing_path);
        std::unordered_set<std::string> seen_keys;
        auto& pricing = universe.pricing;
        const std::unordered_map<std::string, double*> fields {
            {"price_cap", &pricing.price_cap},
            {"carrier_margin", &pricing.carrier_margin},
            {"reference_departures", &pricing.reference_departures},
            {"max_cover_days", &pricing.max_cover_days},
        };
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 2, pricing_path, i + 1);
            require_unique_id(row[0], seen_keys, pricing_path, i + 1);
            const auto field = fields.find(row[0]);
            if (field == fields.end()) {
                throw std::runtime_error("Unknown key '" + row[0] + "' in " + pricing_path.string());
            }
            *field->second = parse_double(row[1], pricing_path, i + 1, row[0].c_str());
        }
        for (const auto& [key, field] : fields) {
            if (!seen_keys.contains(key)) {
                throw std::runtime_error("Missing key '" + key + "' in " + pricing_path.string());
            }
        }
        if (pricing.price_cap < 1.0 || pricing.carrier_margin < 0.0 || pricing.reference_departures < 1.0) {
            throw std::runtime_error("price_cap and reference_departures must be at least 1 in " + pricing_path.string());
        }
        if (pricing.max_cover_days < 21.0) {
            throw std::runtime_error("max_cover_days must be >= 21 in " + pricing_path.string());
        }
    }

    {
        // Station purchase orders (step 29).
        const auto orders_path = root / "economy" / "orders.csv";
        const auto rows = read_csv_rows(orders_path);
        require_header(rows.front(), {"key", "value"}, orders_path);
        std::unordered_set<std::string> seen_keys;
        auto& orders = universe.orders;
        const std::unordered_map<std::string, double*> fields {
            {"safety_factor", &orders.safety_factor},
            {"ema_deliveries", &orders.ema_deliveries},
            {"extra_discount", &orders.extra_discount},
            {"max_extra_days", &orders.max_extra_days},
            {"resale_margin", &orders.resale_margin},
            {"keep_share", &orders.keep_share},
            {"life_value", &orders.life_value},
            {"consume_value", &orders.consume_value},
            {"depot_fuel_value", &orders.depot_fuel_value},
            {"input_cap", &orders.input_cap},
        };
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 2, orders_path, i + 1);
            require_unique_id(row[0], seen_keys, orders_path, i + 1);
            const auto field = fields.find(row[0]);
            if (field == fields.end()) {
                throw std::runtime_error("Unknown key '" + row[0] + "' in " + orders_path.string());
            }
            *field->second = parse_double(row[1], orders_path, i + 1, row[0].c_str());
        }
        for (const auto& [key, field] : fields) {
            if (!seen_keys.contains(key)) {
                throw std::runtime_error("Missing key '" + key + "' in " + orders_path.string());
            }
        }
        if (orders.safety_factor < 0.0 || orders.ema_deliveries < 1.0 || orders.extra_discount < 0.0
            || orders.extra_discount >= 1.0 || orders.keep_share < 0.0 || orders.keep_share > 1.0
            || orders.max_extra_days < 0.0 || orders.resale_margin < 0.0) {
            throw std::runtime_error("safety_factor and extra_discount must be >= 0 (discount < 1), ema_deliveries >= 1 "
                "and keep_share in [0, 1], max_extra_days >= 0 and resale_margin >= 0 in " + orders_path.string());
        }
    }

    {
        // Market redesign switches: every key optional, defaults in MarketDefinition.
        const auto market_path = root / "economy" / "market.csv";
        if (std::filesystem::exists(market_path)) {
            const auto rows = read_csv_rows(market_path);
            require_header(rows.front(), {"key", "value"}, market_path);
            std::unordered_set<std::string> seen_keys;
            auto& market = universe.market;
            const std::unordered_map<std::string, double*> fields {
                {"competitive", &market.competitive},
                {"ask_markup", &market.ask_markup},
                {"return_share", &market.return_share},
                {"fill_ref", &market.fill_ref},
                {"ask_open_units", &market.ask_open_units},
                {"solo_premium", &market.solo_premium},
                {"available_days", &market.available_days},
                {"min_source_surplus", &market.min_source_surplus},
                {"urgency_elasticity", &market.urgency_elasticity},
                {"urgency_max", &market.urgency_max},
                {"contracts", &market.contracts},
                {"contract_max_stops", &market.contract_max_stops},
                {"contract_term_days", &market.contract_term_days},
                {"contract_review_days", &market.contract_review_days},
                {"contract_cover", &market.contract_cover},
                {"contract_accept", &market.contract_accept},
                {"contract_min_units", &market.contract_min_units},
                {"contract_refine", &market.contract_refine},
                {"contract_cost_plus", &market.contract_cost_plus},
                {"follow_up_horizon_days", &market.follow_up_horizon_days},
            };
            for (std::size_t i = 1; i < rows.size(); ++i) {
                const auto& row = rows[i];
                require_field_count(row, 2, market_path, i + 1);
                require_unique_id(row[0], seen_keys, market_path, i + 1);
                const auto field = fields.find(row[0]);
                if (field == fields.end()) {
                    throw std::runtime_error("Unknown key '" + row[0] + "' in " + market_path.string());
                }
                *field->second = parse_double(row[1], market_path, i + 1, row[0].c_str());
            }
        }
    }

    // Ship environment (tier 2): crew radiation and the other effects modules answer. Optional
    // file and keys; the defaults in domain::ShipEnvironmentDefinition are the sourced values,
    // with every mechanism switched off.
    {
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
            require_header(rows.front(), {"station_id", "location_factor", "shield_g_cm2", "note"}, stations_radiation_path);
            for (std::size_t i = 1; i < rows.size(); ++i) {
                const auto& row = rows[i];
                require_field_count(row, 4, stations_radiation_path, i + 1);
                universe.station_radiation[row[0]] = {
                    .location_factor = parse_double(row[1], stations_radiation_path, i + 1, "location_factor"),
                    .shield_g_cm2 = parse_double(row[2], stations_radiation_path, i + 1, "shield_g_cm2"),
                };
            }
        }
    }

    {
        // Written by `spacetrains_headless --write-reference-prices` (step 15); the price and the
        // round-trip days are read, the rest documents where each price comes from.
        const auto reference_path = root / "economy" / "reference_prices.csv";
        const auto rows = read_csv_rows(reference_path);
        require_header(rows.front(), {"station_id", "commodity_id", "reference_price", "base_price", "transport_per_unit",
            "producer_id", "class_id", "round_trip_days"}, reference_path);
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 8, reference_path, i + 1);
            require_station(row[0], reference_path);
            require_commodity(row[1], reference_path);
            const double price = parse_double(row[2], reference_path, i + 1, "reference_price");
            if (price <= 0.0) {
                throw std::runtime_error("reference_price must be positive in " + reference_path.string());
            }
            universe.pricing.reference_prices[row[0]][row[1]] = price;
            universe.pricing.round_trip_days[row[0]][row[1]] = parse_double(row[7], reference_path, i + 1, "round_trip_days");
        }
    }

    {
        // Written by `spacetrains_headless --write-reference-fares` (passengers): the cheapest cabin
        // carrier's fare per berth and its days. Optional: without it nobody can price a trip.
        const auto fares_path = root / "economy" / "reference_fares.csv";
        if (std::filesystem::exists(fares_path)) {
            const auto rows = read_csv_rows(fares_path);
            // The fuel columns (propellant per seat, 2026-10-22) are optional.
            const bool fuel_columns = rows.front().size() == 7;
            require_header(rows.front(), fuel_columns
                ? std::vector<std::string> {"origin_id", "destination_id", "fare_cr", "days", "class_id", "fuel_out_kg", "fuel_back_kg"}
                : std::vector<std::string> {"origin_id", "destination_id", "fare_cr", "days", "class_id"}, fares_path);
            for (std::size_t i = 1; i < rows.size(); ++i) {
                const auto& row = rows[i];
                require_field_count(row, fuel_columns ? 7 : 5, fares_path, i + 1);
                require_station(row[0], fares_path);
                require_station(row[1], fares_path);
                const double fare = parse_double(row[2], fares_path, i + 1, "fare_cr");
                const double days = parse_double(row[3], fares_path, i + 1, "days");
                if (fare <= 0.0 || days <= 0.0) {
                    throw std::runtime_error("fare_cr and days must be positive in " + fares_path.string());
                }
                universe.passengers.reference_fares[row[0]][row[1]] = {.fare_cr = fare, .days = days, .class_id = row[4],
                    .fuel_out_kg = fuel_columns ? parse_double(row[5], fares_path, i + 1, "fuel_out_kg") : 0.0,
                    .fuel_back_kg = fuel_columns ? parse_double(row[6], fares_path, i + 1, "fuel_back_kg") : 0.0};
            }
        }
        for (const auto& gateway : universe.passengers.earth_gateways) {
            require_station(gateway, root / "economy" / "passengers.csv");
        }
        if (universe.passengers.earth_gateways.empty()) {
            throw std::runtime_error("passengers.csv: earth_gateways must name at least one station");
        }
        if (universe.passengers.enabled != 0.0 && !universe.labour.grades.contains(universe.passengers.rotation_grade)) {
            throw std::runtime_error("passengers.csv: unknown rotation_grade '" + universe.passengers.rotation_grade + "'");
        }
    }

    {
        // Random events (step 28).
        const auto settings_path = root / "events" / "settings.csv";
        const auto settings = read_csv_rows(settings_path);
        require_header(settings.front(), {"key", "value"}, settings_path);
        for (std::size_t i = 1; i < settings.size(); ++i) {
            require_field_count(settings[i], 2, settings_path, i + 1);
            if (settings[i][0] != "seed") {
                throw std::runtime_error("Unknown key '" + settings[i][0] + "' in " + settings_path.string());
            }
            universe.event_seed = std::stoull(settings[i][1]);
        }
        const auto events_path = root / "events" / "events.csv";
        const auto rows = read_csv_rows(events_path);
        require_header(rows.front(), {"id", "kind", "station_id", "commodity_id", "rate_per_year", "min_days", "max_days",
            "magnitude", "headline", "end_headline"}, events_path);
        std::unordered_set<std::string> seen_ids;
        const std::unordered_map<std::string, domain::EventKind> kinds {
            {"outage", domain::EventKind::Outage},
            {"boom", domain::EventKind::Boom},
            {"demand_spike", domain::EventKind::DemandSpike},
            {"migration", domain::EventKind::Migration},
        };
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 10, events_path, i + 1);
            require_unique_id(row[0], seen_ids, events_path, i + 1);
            const auto kind = kinds.find(row[1]);
            if (kind == kinds.end()) {
                throw std::runtime_error("Unknown event kind '" + row[1] + "' in " + events_path.string());
            }
            require_station(row[2], events_path);
            domain::EventDefinition event {
                .id = row[0],
                .kind = kind->second,
                .station_id = row[2],
                .commodity_id = row[3],
                .rate_per_year = parse_double(row[4], events_path, i + 1, "rate_per_year"),
                .min_days = parse_double(row[5], events_path, i + 1, "min_days"),
                .max_days = parse_double(row[6], events_path, i + 1, "max_days"),
                .magnitude = parse_double(row[7], events_path, i + 1, "magnitude"),
                .headline = row[8],
                .end_headline = row[9],
            };
            if (event.rate_per_year < 0.0 || event.min_days < 0.0 || event.max_days < event.min_days || event.magnitude < 0.0) {
                throw std::runtime_error("Event '" + event.id + "' needs rate >= 0, 0 <= min_days <= max_days and magnitude >= 0 in "
                    + events_path.string());
            }
            if (event.kind != domain::EventKind::Migration) {
                require_commodity(event.commodity_id, events_path);
                // The station must make (outage, boom) or use (demand spike) the good.
                const auto& station = *std::find_if(universe.stations.begin(), universe.stations.end(),
                    [&](const auto& s) { return s.id == event.station_id; });
                const bool wants_output = event.kind != domain::EventKind::DemandSpike;
                const bool has_recipe = std::any_of(universe.recipes.begin(), universe.recipes.end(), [&](const auto& recipe) {
                    const bool here = recipe.station_id.empty() ? recipe.profile_id == station.economy_profile_id
                                                                : recipe.station_id == station.id;
                    return here && recipe.commodity_id == event.commodity_id
                        && (wants_output ? recipe.units_per_day > 0.0 : recipe.units_per_day < 0.0);
                });
                if (!has_recipe) {
                    throw std::runtime_error("Event '" + event.id + "': " + event.station_id + (wants_output ? " makes" : " uses")
                        + " no " + event.commodity_id + " in " + events_path.string());
                }
            }
            universe.events.push_back(std::move(event));
        }
    }

    for (const auto& body : universe.bodies) {
        if (!body.orbit.parent_id.empty() && !contains_id(universe.bodies, body.orbit.parent_id)) {
            throw std::runtime_error("Body '" + body.id + "' references unknown parent body '" + body.orbit.parent_id + "'");
        }
    }

    for (const auto& station : universe.stations) {
        if (!contains_id(universe.factions, station.faction_id)) {
            throw std::runtime_error("Station '" + station.id + "' references unknown faction '" + station.faction_id + "'");
        }
        if (!contains_id(universe.bodies, station.parent_body_id)) {
            throw std::runtime_error("Station '" + station.id + "' references unknown body '" + station.parent_body_id + "'");
        }
        for (const auto& [commodity_id, _] : station.initial_inventory) {
            if (!contains_id(universe.commodities, commodity_id)) {
                throw std::runtime_error("Station '" + station.id + "' inventory references unknown commodity '" + commodity_id + "'");
            }
        }
    }

    for (const auto& recipe : universe.recipes) {
        if (!contains_id(universe.commodities, recipe.commodity_id)) {
            throw std::runtime_error("Recipe profile '" + recipe.profile_id + "' references unknown commodity '" + recipe.commodity_id + "'");
        }
    }

    for (const auto& ship : universe.ship_seeds) {
        if (!contains_id(universe.factions, ship.faction_id)) {
            throw std::runtime_error("Ship '" + ship.id + "' references unknown faction '" + ship.faction_id + "'");
        }
        if (!contains_id(universe.ship_classes, ship.class_id)) {
            throw std::runtime_error("Ship '" + ship.id + "' references unknown class '" + ship.class_id + "'");
        }
        if (!contains_id(universe.stations, ship.home_station_id)) {
            throw std::runtime_error("Ship '" + ship.id + "' references unknown home station '" + ship.home_station_id + "'");
        }
        if (!contains_id(universe.stations, ship.start_station_id)) {
            throw std::runtime_error("Ship '" + ship.id + "' references unknown start station '" + ship.start_station_id + "'");
        }
    }

    return universe;
}

}  // namespace spacetrains::data_loader
