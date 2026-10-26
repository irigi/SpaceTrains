// CSV reading and the checks every loader shares.

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

// A recipe row's role: empty for an output, "upkeep", "consume" or "input:<output>[|<output>]".
void set_role(domain::RecipeDefinition& recipe, const std::string& text, const std::filesystem::path& path, std::size_t line) {
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
}

void require_station(const domain::UniverseDefinition& universe, const std::string& station_id, const std::filesystem::path& path) {
    const bool known = std::any_of(universe.stations.begin(), universe.stations.end(),
        [&](const domain::StationDefinition& station) { return station.id == station_id; });
    if (!known) {
        throw std::runtime_error("Unknown station '" + station_id + "' in " + path.string());
    }
}
void require_commodity(const domain::UniverseDefinition& universe, const std::string& commodity_id, const std::filesystem::path& path) {
    const bool known = std::any_of(universe.commodities.begin(), universe.commodities.end(),
        [&](const domain::CommodityDefinition& commodity) { return commodity.id == commodity_id; });
    if (!known) {
        throw std::runtime_error("Unknown commodity '" + commodity_id + "' in " + path.string());
    }
}

}  // namespace spacetrains::data_loader::detail
