#pragma once

// CSV reading and checks shared by the data loader files (not part of the public interface).

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

#include "domain/Definitions.hpp"

namespace spacetrains::data_loader::detail {

std::string trim(std::string text);
std::vector<std::string> split_csv_line(const std::string& line);
std::vector<std::vector<std::string>> read_csv_rows(const std::filesystem::path& path);
void require_header(
    const std::vector<std::string>& actual,
    const std::vector<std::string>& expected,
    const std::filesystem::path& path);
void require_field_count(
    const std::vector<std::string>& row,
    std::size_t expected,
    const std::filesystem::path& path,
    std::size_t line_number);
double parse_double(
    const std::string& text,
    const std::filesystem::path& path,
    std::size_t line_number,
    const char* field_name);
std::int64_t parse_int64(
    const std::string& text,
    const std::filesystem::path& path,
    std::size_t line_number,
    const char* field_name);
void require_unique_id(
    const std::string& id,
    std::unordered_set<std::string>& seen_ids,
    const std::filesystem::path& path,
    std::size_t line_number);
domain::Inventory parse_inventory(
    const std::string& text,
    const std::filesystem::path& path,
    std::size_t line_number);
void set_role(domain::RecipeDefinition& recipe, const std::string& text, const std::filesystem::path& path, std::size_t line);
void require_station(const domain::UniverseDefinition& universe, const std::string& station_id, const std::filesystem::path& path);
void require_commodity(const domain::UniverseDefinition& universe, const std::string& commodity_id, const std::filesystem::path& path);

template <typename Collection>
bool contains_id(const Collection& values, const std::string& id) {
    return std::any_of(values.begin(), values.end(), [&](const auto& value) { return value.id == id; });
}

// One loader per data file, called in this order by DataLoader::load_universe.
void load_bodies(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_factions(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_commodities(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_stations(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_recipes(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_station_recipes(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void check_recipe_inputs(const domain::UniverseDefinition& universe);
void load_upkeep_penalties(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_export_markets(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_works_and_station_systems(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_events(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void validate_references(const domain::UniverseDefinition& universe);
void load_ship_classes(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_ship_components(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_modules_and_hull_slots(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_ship_seeds(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_spares(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_ship_operations(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_ship_environment(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_passengers(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_reference_fares(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_labour(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_fuel_supply(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_households(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_labour_market(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_background_economies(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_surface_markets(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_station_incomes(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_fuel_factories(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_open_economy(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_fleet_investment(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_liners(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_growth(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_pricing(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_orders(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_market(const std::filesystem::path& root, domain::UniverseDefinition& universe);
void load_reference_prices(const std::filesystem::path& root, domain::UniverseDefinition& universe);

}  // namespace spacetrains::data_loader::detail
