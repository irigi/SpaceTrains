// The station systems view (display only; docs/plans/station_structures.md, "Light view"):
// people, works and their jobs by kind, needs by tier, site and depot, derived from the
// numbers the economy already has. Nothing here feeds back into the simulation.

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <string>
#include <vector>

#include "economy/EconomySystem.hpp"
#include "simulation/Simulation.hpp"

namespace spacetrains::simulation {

namespace {

constexpr double STANDARD_GRAVITY = 9.80665;

std::string escape(const std::string& text) {
    std::string out;
    for (const char ch : text) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
        }
        out += ch;
    }
    return out;
}

std::string staffing_json(const std::map<std::string, double>& staffing) {
    std::string out = "{";
    for (const auto& [kind, people] : staffing) {
        out += std::format("{}\"{}\":{:.1f}", out.size() > 1 ? "," : "", kind, people);
    }
    return out + "}";
}

struct Works {
    const domain::StationWorksDefinition* definition {nullptr};
    std::string id;
    std::string name;
    std::string kind;
    std::map<std::string, double> capacity;   // output -> units/day with every job filled
    std::map<std::string, double> output;     // output -> units/day now
    std::map<std::string, double> inputs;     // input -> units/day at capacity
    double weight {0.0};                      // output value x labour share
    double jobs {0.0};
};

}  // namespace

std::string Simulation::station_systems_json(const domain::StationDefinition& station, const domain::StationState& state) const {
    if (universe_.station_works.empty()) {
        return "";
    }
    const auto& systems = universe_.station_systems;
    const double factor = economy_.population_factor(station);
    const double employed_share = economy_.employment(station.id);

    const auto works_definition = [&](const std::string& commodity_id) -> const domain::StationWorksDefinition* {
        const domain::StationWorksDefinition* fallback = nullptr;
        for (const auto& works : universe_.station_works) {
            if (works.commodity_id != commodity_id) {
                continue;
            }
            if (works.profile_id == station.economy_profile_id) {
                return &works;
            }
            if (works.profile_id == "*") {
                fallback = &works;
            }
        }
        return fallback;
    };
    const auto base_price = [&](const std::string& commodity_id) {
        for (const auto& commodity : universe_.commodities) {
            if (commodity.id == commodity_id) {
                return commodity.base_price;
            }
        }
        return 0.0;
    };
    const auto mass_per_unit = [&](const std::string& commodity_id) {
        for (const auto& commodity : universe_.commodities) {
            if (commodity.id == commodity_id) {
                return commodity.mass_per_unit_kg;
            }
        }
        return 1.0;
    };

    // Works, in the order of their first output in the recipes.
    std::vector<Works> works;
    std::map<std::string, std::size_t> works_of_output;
    const auto add_output = [&](const std::string& commodity_id, double capacity, double now) {
        const auto* definition = works_definition(commodity_id);
        const std::string id = definition ? definition->works_id : commodity_id;
        auto it = std::find_if(works.begin(), works.end(), [&](const Works& w) { return w.id == id; });
        if (it == works.end()) {
            works.push_back({.definition = definition, .id = id, .name = definition ? definition->name : commodity_id,
                .kind = definition ? definition->kind : "plant"});
            it = std::prev(works.end());
        }
        it->capacity[commodity_id] += capacity;
        it->output[commodity_id] += now;
        it->weight += capacity * base_price(commodity_id) * (definition ? definition->labour_share : 0.5);
        works_of_output[commodity_id] = static_cast<std::size_t>(it - works.begin());
    };
    const auto& recipes = economy_.recipes_of(station);
    for (const auto* recipe : recipes) {
        if (recipe->units_per_day <= 0.0) {
            continue;
        }
        const double capacity = recipe->units_per_day * factor;
        const auto run = state.output_factor.find(recipe->commodity_id);
        add_output(recipe->commodity_id, capacity,
            capacity * employed_share * (run == state.output_factor.end() ? 1.0 : run->second));
    }
    const double fuel_factory = economy_.fuel_factory_output(station);
    if (fuel_factory > 0.0) {
        // The factory fills the depot to its buffer and idles above it.
        const auto fuel = state.inventory.find(economy::FUEL_ID);
        const double stock = fuel == state.inventory.end() ? 0.0 : fuel->second;
        add_output(economy::FUEL_ID, fuel_factory, stock < economy_.fuel_buffer_units(station) ? fuel_factory : 0.0);
    }
    for (const auto* recipe : recipes) {
        if (recipe->role != domain::RecipeRole::Input || recipe->feeds.empty()) {
            continue;
        }
        if (const auto it = works_of_output.find(recipe->feeds.front()); it != works_of_output.end()) {
            works[it->second].inputs[recipe->commodity_id] += -recipe->units_per_day * factor;
        }
    }

    // People. Earth's gateways have no labour market: every worker has a job.
    const double residents = state.population;
    const double workers = station_workers(state);
    const double jobs = in_labour_market(state) ? state.jobs : workers;
    const double employed = std::min(workers, jobs);
    double spacers = 0.0;
    for (const auto& ship : ships_) {
        if (ship.home_station_id == station.id) {
            spacers += ship_design(ship).crew_size;
        }
    }
    double visitors = 0.0;
    for (const auto& group : state.visiting) {
        visitors += group.count;
    }
    double waiting = 0.0;
    for (const auto& group : state.waiting) {
        waiting += group.count;
    }

    // Jobs: each works needs works_jobs_per_value jobs per cr/day of its output value x labour
    // share; together they take at most (1 - services_share) of the station's jobs (scaled down
    // if they would need more). The rest work in services: life support, port, trade, administration.
    double total_weight = 0.0;
    for (const auto& w : works) {
        total_weight += w.weight;
    }
    const double works_jobs = std::min(total_weight * systems.works_jobs_per_value, jobs * (1.0 - systems.services_share));
    const double services_jobs = jobs - works_jobs;
    std::map<std::string, double> jobs_by_kind;
    for (const auto& [kind, share] : systems.services_staffing) {
        jobs_by_kind[kind] += services_jobs * share;
    }
    for (auto& w : works) {
        w.jobs = total_weight > 0.0 ? works_jobs * w.weight / total_weight : 0.0;
        if (w.definition != nullptr) {
            for (const auto& [kind, share] : w.definition->staffing) {
                jobs_by_kind[kind] += w.jobs * share;
            }
        } else {
            jobs_by_kind["technician"] += w.jobs;
        }
    }

    std::string out = "\"systems\":{";
    const auto site_it = universe_.station_radiation.find(station.id);
    const std::string site = site_it == universe_.station_radiation.end() ? "orbit" : site_it->second.site;
    double gravity_g = 1.0;   // spin rings in orbit
    if (site != "orbit") {
        for (const auto& body : universe_.bodies) {
            if (body.id == station.parent_body_id && body.radius_m > 0.0) {
                gravity_g = body.mu_m3_s2 / (body.radius_m * body.radius_m) / STANDARD_GRAVITY;
            }
        }
    }
    out += std::format("\"site\":\"{}\",\"gravity_g\":{:.3f},\"shield_g_cm2\":{:.0f},", escape(site), gravity_g,
        site_it == universe_.station_radiation.end() ? 0.0 : site_it->second.shield_g_cm2);
    out += std::format("\"residents\":{:.0f},\"housing\":{:.0f},\"workers\":{:.0f},\"jobs\":{:.0f},\"employed\":{:.0f},"
                       "\"dependants\":{:.0f},\"spacers\":{:.0f},\"visitors\":{:.0f},\"waiting\":{:.0f},",
        residents, universe_.growth.max_population_factor * station.population, workers, jobs, employed,
        std::max(0.0, residents - workers), spacers, visitors, waiting);
    out += std::format("\"services_jobs\":{:.1f},\"jobs_by_kind\":{},", services_jobs, staffing_json(jobs_by_kind));

    out += "\"works\":[";
    for (std::size_t i = 0; i < works.size(); ++i) {
        const auto& w = works[i];
        std::map<std::string, double> staffing;
        if (w.definition != nullptr) {
            for (const auto& [kind, share] : w.definition->staffing) {
                if (share > 0.0) {
                    staffing[kind] = w.jobs * share;
                }
            }
        }
        const auto goods = [](const std::map<std::string, double>& rates) {
            std::string list = "{";
            for (const auto& [good, rate] : rates) {
                list += std::format("{}\"{}\":{:.3f}", list.size() > 1 ? "," : "", good, rate);
            }
            return list + "}";
        };
        out += std::format("{}{{\"id\":\"{}\",\"name\":\"{}\",\"kind\":\"{}\",\"capacity\":{},\"output\":{},\"inputs\":{},"
                           "\"jobs\":{:.1f},\"staffing\":{}}}",
            i > 0 ? "," : "", escape(w.id), escape(w.name), escape(w.kind), goods(w.capacity), goods(w.output),
            goods(w.inputs), w.jobs, staffing_json(staffing));
    }
    out += "],";

    // Needs: life needs (with the share the station's own farms and recycling cover, against a
    // reference need per person), station upkeep, everyday goods.
    out += "\"needs\":[";
    bool first = true;
    const auto stock_of = [&](const std::string& commodity_id) {
        const auto it = state.inventory.find(commodity_id);
        return it == state.inventory.end() ? 0.0 : it->second;
    };
    const auto add_need = [&](const std::string& commodity_id, const std::string& tier, double use) {
        const bool made_here = works_of_output.contains(commodity_id);
        out += std::format("{}{{\"good\":\"{}\",\"tier\":\"{}\",\"use_per_day\":{:.3f},\"made_here\":{}",
            first ? "" : ",", escape(commodity_id), tier, use, made_here ? "true" : "false");
        first = false;
        if (const auto need = systems.need_kg_per_person_day.find(commodity_id); need != systems.need_kg_per_person_day.end()) {
            out += std::format(",\"need_per_day\":{:.3f}", residents * need->second / mass_per_unit(commodity_id));
        }
        if (use > 0.0) {
            out += std::format(",\"availability\":{:.3f}", std::clamp(stock_of(commodity_id) / (use * 7.0), 0.0, 1.0));
        }
        out += "}";
    };
    std::map<std::string, double> use_of;
    std::map<std::string, domain::RecipeRole> role_of;
    for (const auto* recipe : recipes) {
        if (recipe->units_per_day >= 0.0 || recipe->role == domain::RecipeRole::Input) {
            continue;
        }
        const auto demand = state.demand_factor.find(recipe->commodity_id);
        use_of[recipe->commodity_id] += -recipe->units_per_day * factor * (demand == state.demand_factor.end() ? 1.0 : demand->second);
        role_of[recipe->commodity_id] = recipe->role;
    }
    for (const auto& good : systems.life_needs) {
        const auto use = use_of.find(good);
        add_need(good, "life", use == use_of.end() ? 0.0 : use->second);
    }
    for (const auto& [good, use] : use_of) {
        if (std::find(systems.life_needs.begin(), systems.life_needs.end(), good) != systems.life_needs.end()) {
            continue;
        }
        add_need(good, role_of[good] == domain::RecipeRole::Upkeep ? "upkeep" : "everyday", use);
    }
    out += "],";

    out += std::format("\"fuel_buffer\":{:.0f}}},", economy_.fuel_buffer_units(station));
    return out;
}

}  // namespace spacetrains::simulation
