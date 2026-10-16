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
    const auto stations_path = root / "stations" / "stations.csv";
    const auto recipes_path = root / "recipes" / "recipes.csv";
    const auto station_recipes_path = root / "recipes" / "station_recipes.csv";
    const auto export_markets_path = root / "economy" / "export_markets.csv";
    const auto ships_path = root / "ships" / "ships.csv";
    const auto ship_operations_path = root / "economy" / "ship_operations.csv";
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
             "max_delta_v_mps", "cruise_accel_mps2", "specific_engine_power_w_per_kg", "ship_value_cr", "crew_size", "hull_id"},
            ship_classes_path);
        std::unordered_set<std::string> seen_ids;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 12, ship_classes_path, i + 1);
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
        static const std::unordered_set<std::string> top_kinds {"engine", "radiator", "tank", "habitat", "hold", "structure"};
        static const std::unordered_set<std::string> habitat_kinds {"pressure_hull", "shielding", "life_support",
            "thermal_control", "power_avionics", "crew_quarters", "common_rooms", "airlock_eva"};
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
            if (top ? !top_kinds.contains(row[3]) : (row[2] != "habitat" || !habitat_kinds.contains(row[3]))) {
                throw std::runtime_error(where + ": component kind '" + row[3] + "' does not fit parent '" + row[2] + "'");
            }
            if (!top) {
                const auto parent = std::find_if(ship_class.components.begin(), ship_class.components.end(),
                    [&](const auto& c) { return c.id == row[2]; });
                if (parent == ship_class.components.end() || parent->kind != "habitat") {
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
            std::unordered_map<std::string, std::pair<double, double>> parts;  // parent -> mass, price of its parts
            for (const auto& c : ship_class.components) {
                if (c.parent_id.empty()) {
                    mass += c.mass_kg;
                    price += c.price_cr;
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
            if (row[0] == "wage_cr_per_crew_day") {
                operations.wage_cr_per_crew_day = value;
            } else if (row[0] == "interest_rate_per_year") {
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
        for (const char* required : {"wage_cr_per_crew_day", "interest_rate_per_year", "lifetime_years"}) {
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
            if (row[0] == "depot_buffer_units") {
                fuel_supply.depot_buffer_units = value;
            } else if (row[0] == "factory_buffer_days") {
                fuel_supply.factory_buffer_days = value;
            } else {
                throw std::runtime_error("Unknown key '" + row[0] + "' in " + fuel_supply_path.string());
            }
        }
        for (const char* required : {"depot_buffer_units", "factory_buffer_days"}) {
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

    {
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
            {"cover_round_trip_factor", &pricing.cover_round_trip_factor},
            {"max_cover_days", &pricing.max_cover_days},
            {"input_value_share", &pricing.input_value_share},
            {"input_price_cap", &pricing.input_price_cap},
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
        if (pricing.input_value_share < 0.0 || pricing.input_price_cap < pricing.price_cap) {
            throw std::runtime_error("input_value_share must be >= 0 and input_price_cap >= price_cap in " + pricing_path.string());
        }
        if (pricing.cover_round_trip_factor < 0.0 || pricing.max_cover_days < 21.0) {
            throw std::runtime_error("cover_round_trip_factor must be >= 0 and max_cover_days >= 21 in " + pricing_path.string());
        }
    }

    {
        // Station purchase orders (step 29).
        const auto orders_path = root / "economy" / "orders.csv";
        const auto rows = read_csv_rows(orders_path);
        require_header(rows.front(), {"key", "value"}, orders_path);
        std::unordered_set<std::string> seen_keys;
        auto& orders = universe.orders;
        double order_pricing = 0.0;
        const std::unordered_map<std::string, double*> fields {
            {"order_pricing", &order_pricing},
            {"safety_factor", &orders.safety_factor},
            {"ema_deliveries", &orders.ema_deliveries},
            {"extra_discount", &orders.extra_discount},
            {"max_extra_days", &orders.max_extra_days},
            {"resale_margin", &orders.resale_margin},
            {"keep_share", &orders.keep_share},
            {"life_value", &orders.life_value},
            {"consume_value", &orders.consume_value},
            {"depot_fuel_value", &orders.depot_fuel_value},
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
        orders.order_pricing = order_pricing != 0.0;
        if (orders.safety_factor < 0.0 || orders.ema_deliveries < 1.0 || orders.extra_discount < 0.0
            || orders.extra_discount >= 1.0 || orders.keep_share < 0.0 || orders.keep_share > 1.0
            || orders.max_extra_days < 0.0 || orders.resale_margin < 0.0) {
            throw std::runtime_error("safety_factor and extra_discount must be >= 0 (discount < 1), ema_deliveries >= 1 "
                "and keep_share in [0, 1], max_extra_days >= 0 and resale_margin >= 0 in " + orders_path.string());
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
