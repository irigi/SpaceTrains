// Passenger data: travel settings and reference fares.

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

void load_passengers(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
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

void load_reference_fares(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
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
            require_station(universe, row[0], fares_path);
            require_station(universe, row[1], fares_path);
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
        require_station(universe, gateway, root / "economy" / "passengers.csv");
    }
    if (universe.passengers.earth_gateways.empty()) {
        throw std::runtime_error("passengers.csv: earth_gateways must name at least one station");
    }
    if (universe.passengers.enabled != 0.0 && !universe.labour.grades.contains(universe.passengers.rotation_grade)) {
        throw std::runtime_error("passengers.csv: unknown rotation_grade '" + universe.passengers.rotation_grade + "'");
    }
}

}  // namespace spacetrains::data_loader::detail
