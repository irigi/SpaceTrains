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

#include "data_loader/LoaderInternal.hpp"

// Core data (bodies, factions, goods, stations, recipes, events) and the loading order.

namespace spacetrains::data_loader::detail {

void load_bodies(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto bodies_path = root / "bodies" / "bodies.csv";
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

void load_factions(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto factions_path = root / "factions" / "factions.csv";
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

void load_commodities(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto commodities_path = root / "commodities" / "commodities.csv";
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

void load_stations(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto stations_path = root / "stations" / "stations.csv";
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

void load_recipes(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto recipes_path = root / "recipes" / "recipes.csv";
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

void load_station_recipes(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto station_recipes_path = root / "recipes" / "station_recipes.csv";
    const auto rows = read_csv_rows(station_recipes_path);
    require_header(rows.front(), {"station_id", "commodity_id", "units_per_day", "role"}, station_recipes_path);
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        require_field_count(row, 4, station_recipes_path, i + 1);
        require_station(universe, row[0], station_recipes_path);
        require_commodity(universe, row[1], station_recipes_path);
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
void check_recipe_inputs(const domain::UniverseDefinition& universe) {
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
}

void load_upkeep_penalties(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto upkeep_path = root / "economy" / "upkeep_penalties.csv";
    const auto rows = read_csv_rows(upkeep_path);
    require_header(rows.front(), {"commodity_id", "full_shortage_multiplier", "emergency"}, upkeep_path);
    std::unordered_set<std::string> seen_ids;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        require_field_count(row, 3, upkeep_path, i + 1);
        require_unique_id(row[0], seen_ids, upkeep_path, i + 1);
        require_commodity(universe, row[0], upkeep_path);
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

void load_export_markets(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    const auto export_markets_path = root / "economy" / "export_markets.csv";
    if (std::filesystem::exists(export_markets_path)) {
        const auto rows = read_csv_rows(export_markets_path);
        require_header(rows.front(), {"station_id", "commodity_id", "units_per_day"}, export_markets_path);
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 3, export_markets_path, i + 1);
            require_station(universe, row[0], export_markets_path);
            require_commodity(universe, row[1], export_markets_path);
            const double units_per_day = parse_double(row[2], export_markets_path, i + 1, "units_per_day");
            if (units_per_day <= 0.0) {
                throw std::runtime_error("units_per_day must be positive in " + export_markets_path.string());
            }
            universe.export_markets.push_back({.station_id = row[0], .commodity_id = row[1], .units_per_day = units_per_day});
        }
    }
}

void load_works_and_station_systems(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
    // The station systems view (display only): works per output, the services share and
    // typical staffing, reference needs. Optional: without works.csv there is no view.
    const auto works_path = root / "stations" / "works.csv";
    const auto systems_path = root / "stations" / "station_systems.csv";
    if (std::filesystem::exists(works_path)) {
        const auto rows = read_csv_rows(works_path);
        const std::vector<std::string> kinds {"labourer", "technician", "engineer", "scientist"};
        require_header(rows.front(), {"profile_id", "commodity_id", "works_id", "name", "kind", "labour_share",
            "labourer", "technician", "engineer", "scientist"}, works_path);
        std::unordered_set<std::string> seen;
        for (std::size_t i = 1; i < rows.size(); ++i) {
            const auto& row = rows[i];
            require_field_count(row, 10, works_path, i + 1);
            require_commodity(universe, row[1], works_path);
            require_unique_id(row[0] + "|" + row[1], seen, works_path, i + 1);
            domain::StationWorksDefinition works {
                .profile_id = row[0], .commodity_id = row[1], .works_id = row[2], .name = row[3], .kind = row[4],
                .labour_share = parse_double(row[5], works_path, i + 1, "labour_share"),
            };
            double total = 0.0;
            for (std::size_t k = 0; k < kinds.size(); ++k) {
                const double share = parse_double(row[6 + k], works_path, i + 1, kinds[k].c_str());
                works.staffing[kinds[k]] = share;
                total += share;
            }
            if (std::abs(total - 1.0) > 1e-6 || works.labour_share <= 0.0) {
                throw std::runtime_error("Staffing must sum to 1 and labour_share be positive, row "
                    + std::to_string(i + 1) + " of " + works_path.string());
            }
            universe.station_works.push_back(std::move(works));
        }
        const auto systems_rows = read_csv_rows(systems_path);
        require_header(systems_rows.front(), {"key", "value"}, systems_path);
        auto& systems = universe.station_systems;
        double services_total = 0.0;
        for (std::size_t i = 1; i < systems_rows.size(); ++i) {
            const auto& row = systems_rows[i];
            require_field_count(row, 2, systems_path, i + 1);
            if (row[0] == "life_needs") {
                for (std::size_t at = 0; at <= row[1].size();) {
                    const auto end = std::min(row[1].find('|', at), row[1].size());
                    require_commodity(universe, row[1].substr(at, end - at), systems_path);
                    systems.life_needs.push_back(row[1].substr(at, end - at));
                    at = end + 1;
                }
                continue;
            }
            const double value = parse_double(row[1], systems_path, i + 1, row[0].c_str());
            if (row[0] == "services_share") {
                systems.services_share = value;
            } else if (row[0] == "works_jobs_per_value") {
                systems.works_jobs_per_value = value;
            } else if (row[0].starts_with("services.")) {
                systems.services_staffing[row[0].substr(9)] = value;
                services_total += value;
            } else if (row[0].starts_with("need_kg_per_person_day.")) {
                require_commodity(universe, row[0].substr(23), systems_path);
                systems.need_kg_per_person_day[row[0].substr(23)] = value;
            } else {
                throw std::runtime_error("Unknown key '" + row[0] + "' in " + systems_path.string());
            }
        }
        if (std::abs(services_total - 1.0) > 1e-6 || systems.services_share < 0.0 || systems.services_share >= 1.0) {
            throw std::runtime_error("services.* must sum to 1 and services_share lie in [0, 1) in "
                + systems_path.string());
        }
    }
}

void load_events(const std::filesystem::path& root, domain::UniverseDefinition& universe) {
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
        require_station(universe, row[2], events_path);
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
            require_commodity(universe, event.commodity_id, events_path);
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
            // Or its colony mines it (a boom or outage then changes the colony's sales there).
            const bool colony_sells = wants_output && std::any_of(universe.surface_markets.begin(), universe.surface_markets.end(),
                [&](const auto& market) {
                    return market.station_id == station.id && market.commodity_id == event.commodity_id && market.ask_cr > 0.0
                        && market.sell_units_per_day > 0.0;
                });
            if (!has_recipe && !colony_sells) {
                throw std::runtime_error("Event '" + event.id + "': " + event.station_id + (wants_output ? " makes" : " uses")
                    + " no " + event.commodity_id + " in " + events_path.string());
            }
        }
        universe.events.push_back(std::move(event));
    }
}

// Cross-file references: every body, station, recipe and ship names things that exist.
void validate_references(const domain::UniverseDefinition& universe) {
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
}

}  // namespace spacetrains::data_loader::detail

namespace spacetrains::data_loader {

domain::UniverseDefinition DataLoader::load_universe(const std::filesystem::path& root) const {
    domain::UniverseDefinition universe;
    detail::load_bodies(root, universe);
    detail::load_factions(root, universe);
    detail::load_commodities(root, universe);
    detail::load_ship_classes(root, universe);
    detail::load_ship_components(root, universe);
    detail::load_modules_and_hull_slots(root, universe);
    detail::load_passengers(root, universe);
    detail::load_stations(root, universe);
    detail::load_recipes(root, universe);
    detail::load_ship_seeds(root, universe);
    detail::load_labour(root, universe);
    detail::load_spares(root, universe);
    detail::load_ship_operations(root, universe);
    detail::load_fuel_supply(root, universe);
    detail::load_station_recipes(root, universe);
    detail::check_recipe_inputs(universe);
    detail::load_upkeep_penalties(root, universe);
    detail::load_export_markets(root, universe);
    detail::load_households(root, universe);
    detail::load_labour_market(root, universe);
    detail::load_background_economies(root, universe);
    detail::load_surface_markets(root, universe);
    detail::load_station_incomes(root, universe);
    detail::load_fuel_factories(root, universe);
    detail::load_open_economy(root, universe);
    detail::load_fleet_investment(root, universe);
    detail::load_liners(root, universe);
    detail::load_growth(root, universe);
    detail::load_pricing(root, universe);
    detail::load_orders(root, universe);
    detail::load_market(root, universe);
    detail::load_ship_environment(root, universe);
    detail::load_failures(root, universe);
    detail::load_patrols(root, universe);
    detail::load_works_and_station_systems(root, universe);
    detail::load_reference_prices(root, universe);
    detail::load_reference_fares(root, universe);
    detail::load_events(root, universe);
    detail::validate_references(universe);
    return universe;
}

}  // namespace spacetrains::data_loader
