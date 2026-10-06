#include "simulation/Simulation.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace spacetrains::simulation {

namespace {
constexpr double FUEL_UNITS_TO_KG = 100.0;
constexpr std::size_t MAX_EVENT_HISTORY = 24;
constexpr std::size_t MAX_TRADE_HISTORY = 24;
// A cargo run must clear this profit rate or the ship repositions toward demand
// instead. Without a floor, ships accept months-long near-zero-profit hauls that
// tie up the fleet (observed: 1u water, 14 cr, 183 d transit). Long interplanetary
// runs legitimately score ~15-25 cr/day, so the floor must sit well below that.
constexpr double MIN_MISSION_SCORE_PER_DAY = 2.0;

math::Vec3d interpolate_sampled_path(const std::vector<math::Vec3d>& path, double progress) {
    if (path.empty()) {
        return {};
    }
    if (path.size() == 1) {
        return path.front();
    }

    const double clamped_progress = std::clamp(progress, 0.0, 1.0);
    const double scaled_index = clamped_progress * static_cast<double>(path.size() - 1);
    const auto lower_index = static_cast<std::size_t>(std::floor(scaled_index));
    const auto upper_index = std::min(lower_index + 1, path.size() - 1);
    const double segment_alpha = scaled_index - static_cast<double>(lower_index);
    return path[lower_index] * (1.0 - segment_alpha) + path[upper_index] * segment_alpha;
}

double interpolate_timed_scalar(
    const std::vector<double>& values,
    const std::vector<double>& sample_times_s,
    double time_s) {
    if (values.empty()) {
        return 0.0;
    }
    if (values.size() == 1 || sample_times_s.size() != values.size()) {
        return values.front();
    }
    if (time_s <= sample_times_s.front()) {
        return values.front();
    }
    if (time_s >= sample_times_s.back()) {
        return values.back();
    }
    const auto upper = std::upper_bound(sample_times_s.begin(), sample_times_s.end(), time_s);
    const auto upper_index = static_cast<std::size_t>(std::distance(sample_times_s.begin(), upper));
    const auto lower_index = upper_index - 1;
    const double span_s = std::max(1.0e-6, sample_times_s[upper_index] - sample_times_s[lower_index]);
    const double alpha = (time_s - sample_times_s[lower_index]) / span_s;
    return values[lower_index] * (1.0 - alpha) + values[upper_index] * alpha;
}

math::Vec3d interpolate_timed_sampled_path(
    const std::vector<math::Vec3d>& path,
    const std::vector<double>& sample_times_s,
    double time_s) {
    if (path.empty()) {
        return {};
    }
    if (path.size() == 1 || sample_times_s.size() != path.size()) {
        return path.front();
    }
    if (time_s <= sample_times_s.front()) {
        return path.front();
    }
    if (time_s >= sample_times_s.back()) {
        return path.back();
    }
    const auto upper = std::upper_bound(sample_times_s.begin(), sample_times_s.end(), time_s);
    const auto upper_index = static_cast<std::size_t>(std::distance(sample_times_s.begin(), upper));
    const auto lower_index = upper_index - 1;
    const double span_s = std::max(1.0e-6, sample_times_s[upper_index] - sample_times_s[lower_index]);
    const double alpha = (time_s - sample_times_s[lower_index]) / span_s;
    return path[lower_index] * (1.0 - alpha) + path[upper_index] * alpha;
}

std::string json_escape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += ch;
                break;
        }
    }
    return out;
}
}

Simulation::Simulation(domain::UniverseDefinition universe)
    : universe_(std::move(universe)),
      mechanics_(universe_),
      economy_(universe_) {
    kepler_planner_ = std::make_unique<trajectory::KeplerTrajectoryPlanner>(universe_, mechanics_);
    for (const auto& station : universe_.stations) {
        station_defs_by_id_[station.id] = &station;
        stations_.push_back({.station_id = station.id, .inventory = station.initial_inventory, .credits = station.initial_credits});
    }
    for (const auto& ship_class : universe_.ship_classes) {
        ship_classes_by_id_[ship_class.id] = &ship_class;
    }
    for (const auto& seed : universe_.ship_seeds) {
        ships_.push_back({
            .id = seed.id,
            .name = seed.name,
            .faction_id = seed.faction_id,
            .class_id = seed.class_id,
            .home_station_id = seed.home_station_id,
            .current_station_id = seed.start_station_id,
            .phase = domain::ShipMissionPhase::Idle,
            .propellant_kg = seed.initial_propellant_kg,
            .credits = seed.initial_credits,
            .active_mission = {},
        });
    }
}

Simulation Simulation::from_data_root(const std::string& data_root) {
    data_loader::DataLoader loader;
    auto universe = loader.load_universe(data_root);
    Simulation sim(std::move(universe));

    // Try to load the VariableISP atlas. It lives next to the data root.
    const std::filesystem::path data_path(data_root);
    const std::filesystem::path atlas_path =
        data_path.parent_path() / "tests" / "data" / "variable_isp" / "variable_isp_atlas.bin";
    if (std::filesystem::exists(atlas_path)) {
        try {
            sim.atlas_.load_binary(atlas_path.string());
            sim.atlas_loaded_ = true;
            sim.variable_isp_planner_ = std::make_unique<trajectory::VariableIspTrajectoryPlanner>(
                sim.universe_, sim.mechanics_, sim.atlas_);
        } catch (const std::exception& ex) {
            std::cerr << "[VariableISP] Atlas load failed: " << ex.what() << " — electric ships will be stranded.\n";
        }
    } else {
        std::cerr << "[VariableISP] Atlas not found at " << atlas_path.string()
                  << " — electric ships will be stranded.\n";
    }

    return sim;
}

void Simulation::set_timewarp(double timewarp_factor) {
    timewarp_factor_ = std::max(1.0, timewarp_factor);
}

double Simulation::timewarp_factor() const {
    return timewarp_factor_;
}

const domain::UniverseDefinition& Simulation::universe() const {
    return universe_;
}

const economy::EconomySystem& Simulation::economy_system() const {
    return economy_;
}

const domain::ShipClassDefinition& Simulation::get_ship_class(const std::string& class_id) const {
    const auto it = ship_classes_by_id_.find(class_id);
    if (it == ship_classes_by_id_.end()) {
        throw std::runtime_error("Unknown ship class: " + class_id);
    }
    return *it->second;
}

const domain::CelestialBodyDefinition& Simulation::get_body_definition(const std::string& body_id) const {
    const auto it = std::find_if(
        universe_.bodies.begin(),
        universe_.bodies.end(),
        [&](const domain::CelestialBodyDefinition& body) { return body.id == body_id; });
    if (it == universe_.bodies.end()) {
        throw std::runtime_error("Unknown body: " + body_id);
    }
    return *it;
}

const domain::StationDefinition& Simulation::get_station_definition(const std::string& station_id) const {
    const auto it = station_defs_by_id_.find(station_id);
    if (it == station_defs_by_id_.end()) {
        throw std::runtime_error("Unknown station: " + station_id);
    }
    return *it->second;
}

domain::StationState& Simulation::get_station_state(const std::string& station_id) {
    const auto it = std::find_if(
        stations_.begin(),
        stations_.end(),
        [&](const domain::StationState& station) { return station.station_id == station_id; });
    if (it == stations_.end()) {
        throw std::runtime_error("Unknown station state: " + station_id);
    }
    return *it;
}

const domain::StationState& Simulation::get_station_state(const std::string& station_id) const {
    const auto it = std::find_if(
        stations_.begin(),
        stations_.end(),
        [&](const domain::StationState& station) { return station.station_id == station_id; });
    if (it == stations_.end()) {
        throw std::runtime_error("Unknown station state: " + station_id);
    }
    return *it;
}

void Simulation::add_event(std::string text, std::string category) {
    recent_events_.push_back({.time_s = game_time_s_, .text = std::move(text), .category = std::move(category)});
    if (recent_events_.size() > MAX_EVENT_HISTORY) {
        recent_events_.erase(recent_events_.begin(), recent_events_.begin() + static_cast<long>(recent_events_.size() - MAX_EVENT_HISTORY));
    }
}

void Simulation::record_trade(domain::TradeEntry trade) {
    recent_trades_.push_back(std::move(trade));
    if (recent_trades_.size() > MAX_TRADE_HISTORY) {
        recent_trades_.erase(recent_trades_.begin(), recent_trades_.begin() + static_cast<long>(recent_trades_.size() - MAX_TRADE_HISTORY));
    }
}

void Simulation::record_trajectory_audit(
    const domain::ShipState& ship,
    const domain::StationDefinition& origin,
    const domain::StationDefinition& destination,
    const domain::TrajectoryPlan& plan,
    double planning_propellant_kg) {
    const double r_origin_m = mechanics_.get_heliocentric_radius(origin.parent_body_id, plan.departure_time_s);
    const double r_dest_m = mechanics_.get_heliocentric_radius(destination.parent_body_id, plan.arrival_time_s);
    auto metrics = trajectory::audit_trajectory(plan);
    const bool flagged = !metrics.flags.empty();
    trajectory_audit_records_.push_back({
        .planned_at_s = game_time_s_,
        .ship_name = ship.name,
        .class_id = ship.class_id,
        .origin_station_id = origin.id,
        .destination_station_id = destination.id,
        .trajectory_type = plan.trajectory_type,
        .planning_propellant_kg = planning_propellant_kg,
        .wait_time_s = plan.wait_time_s,
        .coast_time_s = plan.coast_time_s,
        .r_origin_m = r_origin_m,
        .r_dest_m = r_dest_m,
        .diagnostics = plan.diagnostics,
        .metrics = std::move(metrics),
        .sampled_path = flagged ? plan.sampled_path : std::vector<math::Vec3d>{},
        .sampled_times_s = flagged ? plan.sampled_times_s : std::vector<double>{},
    });
}

const domain::CommodityDefinition& Simulation::get_commodity(const std::string& commodity_id) const {
    const auto it = std::find_if(
        universe_.commodities.begin(),
        universe_.commodities.end(),
        [&](const domain::CommodityDefinition& commodity) { return commodity.id == commodity_id; });
    if (it == universe_.commodities.end()) {
        throw std::runtime_error("Unknown commodity: " + commodity_id);
    }
    return *it;
}

double Simulation::station_price(const domain::StationState& state, const std::string& commodity_id) const {
    const auto& definition = get_station_definition(state.station_id);
    const auto stock_it = state.inventory.find(commodity_id);
    const double stock = stock_it == state.inventory.end() ? 0.0 : stock_it->second;
    return economy_.get_price(definition.economy_profile_id, commodity_id, stock, get_commodity(commodity_id).base_price);
}

bool Simulation::try_refuel(domain::ShipState& ship) {
    auto& station = get_station_state(ship.current_station_id);
    const auto& ship_class = get_ship_class(ship.class_id);
    // Refuel discipline: top up to 80% and only when below half tank. A fleet
    // that chases 100% every idle tick drinks entire stations dry and locks the
    // system's fuel supply inside ship tanks instead of the market.
    if (ship.propellant_kg >= ship_class.propellant_capacity_kg * 0.5) {
        return ship.propellant_kg >= ship_class.propellant_capacity_kg * 0.4;
    }
    const double missing_kg = std::max(0.0, ship_class.propellant_capacity_kg * 0.8 - ship.propellant_kg);

    // Leave the station its own working reserve (it may consume fuel too) —
    // except for a nearly-dry ship, which may tap the reserve to get unstuck.
    const bool emergency = ship.propellant_kg <= ship_class.propellant_capacity_kg * 0.05;
    const auto station_rates = economy_.get_profile_net_rates(
        get_station_definition(ship.current_station_id).economy_profile_id);
    const double station_fuel_rate = station_rates.contains("fuel") ? station_rates.at("fuel") : 0.0;
    const double station_reserve = (station_fuel_rate < 0.0 && !emergency)
        ? std::abs(station_fuel_rate) * 14.0 : 0.0;
    const double available_fuel_units = std::max(0.0, station.inventory["fuel"] - station_reserve);
    const double transferable_kg = std::min(missing_kg, available_fuel_units * FUEL_UNITS_TO_KG);
    const double transferred_units = transferable_kg / FUEL_UNITS_TO_KG;
    // Price the fuel on the stock before the transfer. Ships may go into debt for fuel
    // (but not for cargo) so an empty wallet never permanently strands a ship.
    const double fuel_unit_price = station_price(station, "fuel");
    const double fuel_bill = transferred_units * fuel_unit_price;
    station.inventory["fuel"] -= transferred_units;
    ship.propellant_kg += transferable_kg;
    ship.credits -= fuel_bill;
    ship.lifetime_profit -= fuel_bill;
    station.credits += fuel_bill;

    if (transferable_kg > 0.0) {
        record_trade({
            .time_s = game_time_s_,
            .ship_id = ship.id,
            .station_id = station.station_id,
            .commodity_id = "fuel",
            .kind = "fuel",
            .units = transferred_units,
            .unit_price = fuel_unit_price,
            .total = fuel_bill,
        });
        add_event(std::format(
            "{} refueled {:.0f} kg at {} for {:.0f} cr",
            ship.name, transferable_kg, get_station_definition(ship.current_station_id).name, fuel_bill),
            "fuel");
    }
    return ship.propellant_kg >= ship_class.propellant_capacity_kg * 0.4;
}

void Simulation::step_idle_ship(domain::ShipState& ship) {
    const bool has_refuel_reserve = try_refuel(ship);

    auto& origin_state = get_station_state(ship.current_station_id);
    const auto& origin_def = get_station_definition(ship.current_station_id);
    const auto& ship_class = get_ship_class(ship.class_id);

    // Ion ships fly max-burn trajectories that consume the whole planning budget,
    // so plan with a 15% captain's reserve held back — they then always arrive
    // with enough margin to leave again instead of gambling on destination stock.
    domain::ShipState planning_ship = ship;
    if (ship_class.propulsion_type == "electric_ion") {
        planning_ship.propellant_kg = std::max(
            0.0, ship.propellant_kg - ship_class.propellant_capacity_kg * 0.15);
    }

    double best_score = MIN_MISSION_SCORE_PER_DAY;
    const domain::StationDefinition* best_destination = nullptr;
    std::string best_commodity;
    double best_cargo_units = 0.0;

    const double origin_fuel_price = station_price(origin_state, "fuel");
    const auto origin_rates = economy_.get_profile_net_rates(origin_def.economy_profile_id);
    for (const auto& [commodity_id, stock] : origin_state.inventory) {
        const double origin_rate = origin_rates.contains(commodity_id) ? origin_rates.at(commodity_id) : 0.0;
        if (origin_rate <= 0.0) {
            continue;
        }
        const double origin_reserve = 8.0 + origin_rate * 7.0;
        const double surplus = std::max(0.0, stock - origin_reserve);
        if (surplus <= 1.0) {
            continue;
        }

        const double origin_price = station_price(origin_state, commodity_id);
        const double decay_per_day = get_commodity(commodity_id).decay_fraction_per_day;

        for (const auto& destination : universe_.stations) {
            if (destination.id == origin_def.id) {
                continue;
            }
            const auto destination_rates = economy_.get_profile_net_rates(destination.economy_profile_id);
            const double destination_rate = destination_rates.contains(commodity_id) ? destination_rates.at(commodity_id) : 0.0;
            if (destination_rate >= 0.0) {
                continue;
            }
            const auto& destination_state = get_station_state(destination.id);

            // Cap cargo by hold size, available surplus, what the ship can afford,
            // and the destination's free storage.
            double free_capacity = ship_class.cargo_capacity_units;
            if (destination.storage_capacity_units > 0.0) {
                double stored = 0.0;
                for (const auto& [_, units] : destination_state.inventory) {
                    stored += std::max(0.0, units);
                }
                free_capacity = std::max(0.0, destination.storage_capacity_units - stored);
            }
            const double affordable = origin_price > 0.0 ? std::max(0.0, ship.credits) / origin_price : surplus;
            const double cargo_units = std::min({ship_class.cargo_capacity_units, surplus, affordable, free_capacity});
            if (cargo_units <= 1.0) {
                continue;
            }

            const auto& planner = (ship_class.propulsion_type == "electric_ion" && variable_isp_planner_)
                ? static_cast<trajectory::ITrajectoryPlanner&>(*variable_isp_planner_)
                : static_cast<trajectory::ITrajectoryPlanner&>(*kepler_planner_);
            const auto plan = planner.plan_transfer(origin_def, destination, planning_ship, ship_class, game_time_s_);
            if (!plan.feasible) {
                continue;
            }

            // Expected profit per day. A starving destination prices the commodity at the
            // 4x scarcity clamp, so urgency is embedded in the price spread.
            const double travel_days = plan.travel_time_s / 86400.0;
            const double surviving = cargo_units * std::pow(1.0 - decay_per_day, travel_days);
            const double revenue = surviving * station_price(destination_state, commodity_id);
            const double fuel_cost = (plan.propellant_required_kg / FUEL_UNITS_TO_KG) * origin_fuel_price;
            const double cost = cargo_units * origin_price + fuel_cost;
            // Ion ships burn nearly their whole budget per leg, so don't send one
            // where it cannot refuel afterwards (unless the cargo itself is fuel).
            // Chemical ships keep large margins and are not restricted.
            if (ship_class.propulsion_type == "electric_ion" && commodity_id != "fuel") {
                const double after_arrival_kg = ship.propellant_kg - plan.propellant_required_kg;
                const double dest_fuel_kg = (destination_state.inventory.count("fuel")
                    ? destination_state.inventory.at("fuel") : 0.0) * FUEL_UNITS_TO_KG;
                if (after_arrival_kg + dest_fuel_kg < ship_class.propellant_capacity_kg * 0.25) {
                    continue;
                }
            }

            // The 4x price clamp saturates, so a starving station cannot bid any
            // higher; weight the score by how few days of stock the destination
            // has left (this is what gets fuel hauled to fuel-dry stations).
            const double dest_stock = destination_state.inventory.count(commodity_id)
                ? destination_state.inventory.at(commodity_id) : 0.0;
            const double dest_days_left = dest_stock / std::abs(destination_rate);
            const double urgency = std::clamp(14.0 / std::max(dest_days_left, 0.5), 1.0, 5.0);
            const double score = urgency * (revenue - cost) / std::max(1.0, travel_days);
            if (score > best_score) {
                best_score = score;
                best_destination = &destination;
                best_commodity = commodity_id;
                best_cargo_units = cargo_units;
            }
        }
    }

    if (best_destination == nullptr) {
        best_score = 0.0;  // repositioning scores are in urgency units, not credits/day

        // Sourcing score: does this station have surplus goods urgently needed
        // elsewhere? An empty ship is only useful where there is something to
        // pick up, so repositioning targets producers; starving stations are
        // served by the cargo loop's urgency weighting once a ship is loaded.
        const auto sourcing_score_for = [&](const domain::StationDefinition& station_def,
                                            const domain::StationState& station_state) {
            const auto rates = economy_.get_profile_net_rates(station_def.economy_profile_id);
            double sourcing_score = 0.0;
            for (const auto& [commodity_id, rate] : rates) {
                if (rate <= 0.0) continue;
                const double stock = station_state.inventory.count(commodity_id)
                    ? station_state.inventory.at(commodity_id) : 0.0;
                const double reserve = 8.0 + rate * 7.0;
                const double surplus = std::max(0.0, stock - reserve);
                if (surplus <= 1.0) continue;
                // Sum up how urgently this commodity is needed across the whole system.
                double system_urgency = 0.0;
                for (const auto& other : universe_.stations) {
                    if (other.id == station_def.id) continue;
                    const auto other_rates = economy_.get_profile_net_rates(other.economy_profile_id);
                    const double other_rate = other_rates.count(commodity_id) ? other_rates.at(commodity_id) : 0.0;
                    if (other_rate >= 0.0) continue;
                    const auto& other_state = get_station_state(other.id);
                    const double other_stock = other_state.inventory.count(commodity_id)
                        ? other_state.inventory.at(commodity_id) : 0.0;
                    const double days_rem = other_stock > 0.0 ? other_stock / std::abs(other_rate) : 0.0;
                    system_urgency += std::max(1.0, 30.0 / std::max(1.0, days_rem)) * std::abs(other_rate);
                }
                sourcing_score += std::min(surplus, ship_class.cargo_capacity_units) * system_urgency / 100.0;
            }
            return sourcing_score;
        };
        const double origin_sourcing_score = sourcing_score_for(origin_def, origin_state);

        for (const auto& destination : universe_.stations) {
            if (destination.id == origin_def.id) {
                continue;
            }
            const auto& dest_state = get_station_state(destination.id);

            const double destination_score = sourcing_score_for(destination, dest_state);
            // Only burn propellant relocating if the destination is a clearly
            // better pickup spot than where the ship already sits — otherwise
            // the fleet thrashes between comparable producers, hauling nothing.
            if (destination_score <= 0.0 || destination_score < origin_sourcing_score * 1.5) {
                continue;
            }
            const auto& planner = (ship_class.propulsion_type == "electric_ion" && variable_isp_planner_)
                ? static_cast<trajectory::ITrajectoryPlanner&>(*variable_isp_planner_)
                : static_cast<trajectory::ITrajectoryPlanner&>(*kepler_planner_);
            const auto plan = planner.plan_transfer(origin_def, destination, planning_ship, ship_class, game_time_s_);
            if (!plan.feasible) {
                continue;
            }
            // Never reposition an ion ship into a fuel-dry trap it cannot leave from.
            if (ship_class.propulsion_type == "electric_ion") {
                const double after_arrival_kg = ship.propellant_kg - plan.propellant_required_kg;
                const double dest_fuel_kg = (dest_state.inventory.count("fuel")
                    ? dest_state.inventory.at("fuel") : 0.0) * FUEL_UNITS_TO_KG;
                if (after_arrival_kg + dest_fuel_kg < ship_class.propellant_capacity_kg * 0.25) {
                    continue;
                }
            }
            // Crowd penalty: don't send the whole fleet to the same "best" station.
            int congestion = 0;
            for (const auto& other : ships_) {
                if (other.id == ship.id) {
                    continue;
                }
                const bool docked_there = other.current_station_id == destination.id
                    && other.phase != domain::ShipMissionPhase::InTransit;
                const bool inbound = (other.phase == domain::ShipMissionPhase::InTransit
                    || other.phase == domain::ShipMissionPhase::AwaitingDeparture)
                    && other.active_mission.destination_station_id == destination.id;
                if (docked_there || inbound) {
                    ++congestion;
                }
            }
            const double score = destination_score
                / std::max(1.0, plan.travel_time_s / 86400.0)
                / (1.0 + static_cast<double>(congestion));
            if (score > best_score) {
                best_score = score;
                best_destination = &destination;
                best_commodity.clear();
                best_cargo_units = 0.0;
            }
        }
    }

    if (best_destination == nullptr) {
        if (!has_refuel_reserve && ship.propellant_kg <= 0.0) {
            ship.phase = domain::ShipMissionPhase::Stranded;
            add_event(std::format("{} is stranded at {} due to fuel shortage", ship.name, origin_def.name), "alert");
        }
        return;
    }

    const auto& final_planner = (ship_class.propulsion_type == "electric_ion" && variable_isp_planner_)
        ? static_cast<trajectory::ITrajectoryPlanner&>(*variable_isp_planner_)
        : static_cast<trajectory::ITrajectoryPlanner&>(*kepler_planner_);
    auto plan = final_planner.plan_transfer(origin_def, *best_destination, planning_ship, ship_class, game_time_s_);
    if (!plan.feasible) {
        return;
    }
    if (trajectory_audit_enabled_) {
        record_trajectory_audit(ship, origin_def, *best_destination, plan, planning_ship.propellant_kg);
    }

    double purchase_cost = 0.0;
    double expected_revenue = 0.0;
    if (best_cargo_units > 0.0 && !best_commodity.empty()) {
        // Buy at origin: price on the stock before the cargo is reserved.
        const double unit_price = station_price(origin_state, best_commodity);
        purchase_cost = best_cargo_units * unit_price;
        origin_state.inventory[best_commodity] -= best_cargo_units;
        ship.credits -= purchase_cost;
        ship.lifetime_profit -= purchase_cost;
        origin_state.credits += purchase_cost;
        record_trade({
            .time_s = game_time_s_,
            .ship_id = ship.id,
            .station_id = origin_state.station_id,
            .commodity_id = best_commodity,
            .kind = "buy",
            .units = best_cargo_units,
            .unit_price = unit_price,
            .total = purchase_cost,
        });
        const auto& destination_state = get_station_state(best_destination->id);
        expected_revenue = best_cargo_units * station_price(destination_state, best_commodity);
    }
    // Chemical ships: deduct propellant at mission start (instantaneous burns).
    // Electric ion ships: propellant is consumed continuously during transit and
    // tracked per-sample, so we don't deduct here — ship.propellant_kg is updated
    // in step_in_transit_ship from sampled_propellant_kg.
    if (ship_class.propulsion_type != "electric_ion") {
        ship.propellant_kg -= plan.propellant_required_kg;
    }
    // The plan was built from the reserve-reduced budget; shift the per-sample
    // propellant up by the held-back reserve so it stays in the tank instead of
    // being erased by the first in-transit interpolation.
    auto sampled_propellant = plan.sampled_propellant_kg;
    const double reserve_kg = ship.propellant_kg - planning_ship.propellant_kg;
    if (reserve_kg > 0.0) {
        for (double& sample_kg : sampled_propellant) {
            sample_kg += reserve_kg;
        }
    }
    ship.phase = plan.wait_time_s > 0.0 ? domain::ShipMissionPhase::AwaitingDeparture : domain::ShipMissionPhase::InTransit;
    ship.active_mission = {
        .origin_station_id = origin_def.id,
        .destination_station_id = best_destination->id,
        .commodity_id = best_commodity,
        .cargo_units = best_cargo_units,
        .departure_time_s = plan.departure_time_s,
        .arrival_time_s = plan.arrival_time_s,
        .wait_time_s = plan.wait_time_s,
        .coast_time_s = plan.coast_time_s,
        .total_travel_time_s = plan.travel_time_s,
        .remaining_travel_time_s = plan.travel_time_s,
        .propellant_cost_kg = plan.propellant_required_kg,
        .purchase_cost = purchase_cost,
        .fuel_cost = (plan.propellant_required_kg / FUEL_UNITS_TO_KG) * station_price(origin_state, "fuel"),
        .expected_revenue = expected_revenue,
        .sampled_path = plan.sampled_path,
        .sampled_times_s = plan.sampled_times_s,
        .sampled_propellant_kg = std::move(sampled_propellant),
        .trajectory_type = plan.trajectory_type,
    };
    if (plan.wait_time_s > 0.0) {
        if (best_cargo_units > 0.0) {
            add_event(std::format(
                "{} scheduled {}->{} ({}) in {:.1f}d  est. profit {:.0f} cr ({:.1f}u/{:.1f}d travel)",
                ship.name,
                origin_def.name,
                best_destination->name,
                best_commodity,
                plan.wait_time_s / 86400.0,
                expected_revenue - purchase_cost,
                best_cargo_units,
                plan.travel_time_s / 86400.0),
                "mission");
        } else {
            add_event(std::format(
                "{} repositioning {}->{} in {:.1f}d",
                ship.name,
                origin_def.name,
                best_destination->name,
                plan.wait_time_s / 86400.0),
                "mission");
        }
    } else if (best_cargo_units > 0.0) {
        add_event(std::format(
            "{} departed {}->{} ({}) {:.1f}u  est. profit {:.0f} cr ({:.1f}d transit, prop={:.0f}kg)",
            ship.name,
            origin_def.name,
            best_destination->name,
            best_commodity,
            best_cargo_units,
            expected_revenue - purchase_cost,
            plan.travel_time_s / 86400.0,
            plan.propellant_required_kg),
            "mission");
    } else {
        add_event(std::format("{} repositioned from {} to {}", ship.name, origin_def.name, best_destination->name), "mission");
    }
}

void Simulation::step_awaiting_departure_ship(domain::ShipState& ship) {
    ship.active_mission.remaining_travel_time_s = std::max(0.0, ship.active_mission.arrival_time_s - game_time_s_);
    if (game_time_s_ < ship.active_mission.departure_time_s) {
        return;
    }
    ship.phase = domain::ShipMissionPhase::InTransit;
    const auto& origin = get_station_definition(ship.active_mission.origin_station_id);
    const auto& destination = get_station_definition(ship.active_mission.destination_station_id);
    if (ship.active_mission.cargo_units > 0.0) {
        add_event(std::format(
            "{} departed {} for {} carrying {:.1f} units of {}",
            ship.name,
            origin.name,
            destination.name,
            ship.active_mission.cargo_units,
            ship.active_mission.commodity_id),
            "mission");
    } else {
        add_event(std::format("{} departed {} for {}", ship.name, origin.name, destination.name), "mission");
    }
}

void Simulation::step_in_transit_ship(domain::ShipState& ship, double dt_s) {
    (void)dt_s;
    ship.active_mission.remaining_travel_time_s = std::max(0.0, ship.active_mission.arrival_time_s - game_time_s_);

    // Update propellant continuously from the pre-computed mass samples (ion drives only).
    // The samples run from initial propellant at departure to final propellant at arrival,
    // giving a smooth display rather than a step-change at mission assignment.
    if (!ship.active_mission.sampled_propellant_kg.empty()
        && ship.active_mission.sampled_times_s.size() == ship.active_mission.sampled_propellant_kg.size()) {
        ship.propellant_kg = interpolate_timed_scalar(
            ship.active_mission.sampled_propellant_kg,
            ship.active_mission.sampled_times_s,
            game_time_s_);
    }

    if (ship.active_mission.remaining_travel_time_s > 0.0) {
        return;
    }

    ship.current_station_id = ship.active_mission.destination_station_id;
    ship.phase = domain::ShipMissionPhase::Idle;
    auto& destination = get_station_state(ship.current_station_id);
    if (ship.active_mission.cargo_units > 0.0 && !ship.active_mission.commodity_id.empty()) {
        // Apply cargo decay: some goods (food, medicine) spoil in transit.
        double decay_per_day = 0.0;
        for (const auto& c : universe_.commodities) {
            if (c.id == ship.active_mission.commodity_id) {
                decay_per_day = c.decay_fraction_per_day;
                break;
            }
        }
        const double transit_days = ship.active_mission.total_travel_time_s / 86400.0;
        const double surviving = (decay_per_day > 0.0)
            ? std::max(0.0, std::pow(1.0 - decay_per_day, transit_days))
            : 1.0;
        double arrived = ship.active_mission.cargo_units * surviving;
        const double spoiled = ship.active_mission.cargo_units - arrived;

        // Clamp delivery to the destination's free storage; the overflow is jettisoned.
        const auto& destination_def = get_station_definition(ship.current_station_id);
        if (destination_def.storage_capacity_units > 0.0) {
            double stored = 0.0;
            for (const auto& [_, units] : destination.inventory) {
                stored += std::max(0.0, units);
            }
            const double free_capacity = std::max(0.0, destination_def.storage_capacity_units - stored);
            if (arrived > free_capacity) {
                add_event(std::format(
                    "{} jettisoned {:.1f}u {} at {} — storage full",
                    ship.name, arrived - free_capacity, ship.active_mission.commodity_id, destination_def.name),
                    "alert");
                arrived = free_capacity;
            }
        }

        // Sell at arrival: price on the stock before the delivery lands.
        const double unit_price = station_price(destination, ship.active_mission.commodity_id);
        const double revenue = arrived * unit_price;
        destination.inventory[ship.active_mission.commodity_id] += arrived;
        destination.credits -= revenue;
        ship.credits += revenue;
        ship.lifetime_profit += revenue;
        if (arrived > 0.0) {
            record_trade({
                .time_s = game_time_s_,
                .ship_id = ship.id,
                .station_id = destination.station_id,
                .commodity_id = ship.active_mission.commodity_id,
                .kind = "sell",
                .units = arrived,
                .unit_price = unit_price,
                .total = revenue,
            });
        }

        if (spoiled > 0.1) {
            add_event(std::format(
                "{} arrived at {} with {:.1f}u {} for {:.0f} cr ({:.1f}u spoiled in {:.0f}d transit)",
                ship.name,
                get_station_definition(ship.current_station_id).name,
                arrived,
                ship.active_mission.commodity_id,
                revenue,
                spoiled,
                transit_days),
                "arrival");
        } else {
            add_event(std::format(
                "{} arrived at {} and sold {:.1f}u {} for {:.0f} cr",
                ship.name,
                get_station_definition(ship.current_station_id).name,
                arrived,
                ship.active_mission.commodity_id,
                revenue),
                "arrival");
        }
    } else {
        add_event(std::format("{} arrived at {}", ship.name, get_station_definition(ship.current_station_id).name), "arrival");
    }
    ship.active_mission = {};
}

void Simulation::step(double real_dt_s) {
    const double dt_s = real_dt_s * timewarp_factor_;
    game_time_s_ += dt_s;
    economy_.step(stations_, dt_s);

    for (auto& ship : ships_) {
        switch (ship.phase) {
            case domain::ShipMissionPhase::Idle:
                step_idle_ship(ship);
                break;
            case domain::ShipMissionPhase::AwaitingDeparture:
                step_awaiting_departure_ship(ship);
                break;
            case domain::ShipMissionPhase::InTransit:
                step_in_transit_ship(ship, dt_s);
                break;
            case domain::ShipMissionPhase::Refueling:
                ship.phase = domain::ShipMissionPhase::Idle;
                break;
            case domain::ShipMissionPhase::Stranded:
                // A partial fuel delivery is enough to get moving again; the 40%
                // bar inside try_refuel is the comfortable mission reserve, not
                // the recovery threshold.
                (void)try_refuel(ship);
                if (ship.propellant_kg >= get_ship_class(ship.class_id).propellant_capacity_kg * 0.15) {
                    ship.phase = domain::ShipMissionPhase::Idle;
                    add_event(std::format("{} recovered from stranded state at {}", ship.name, get_station_definition(ship.current_station_id).name), "alert");
                }
                break;
        }
    }
}

domain::SimulationSnapshot Simulation::snapshot() const {
    return {
        .game_time_s = game_time_s_,
        .stations = stations_,
        .ships = ships_,
        .recent_events = recent_events_,
    };
}

std::string Simulation::mission_phase_name(domain::ShipMissionPhase phase) const {
    switch (phase) {
        case domain::ShipMissionPhase::Idle:
            return "idle";
        case domain::ShipMissionPhase::AwaitingDeparture:
            return "awaiting_departure";
        case domain::ShipMissionPhase::InTransit:
            return "in_transit";
        case domain::ShipMissionPhase::Refueling:
            return "refueling";
        case domain::ShipMissionPhase::Stranded:
            return "stranded";
    }
    return "unknown";
}

math::Vec3d Simulation::get_ship_render_position(const domain::ShipState& ship) const {
    if ((ship.phase != domain::ShipMissionPhase::InTransit && ship.phase != domain::ShipMissionPhase::AwaitingDeparture)
        || ship.active_mission.total_travel_time_s <= 0.0) {
        const auto& station = get_station_definition(ship.current_station_id);
        return mechanics_.get_station_position(station, game_time_s_);
    }

    if (ship.phase == domain::ShipMissionPhase::AwaitingDeparture
        && game_time_s_ < ship.active_mission.departure_time_s) {
        const auto& station = get_station_definition(ship.current_station_id);
        return mechanics_.get_station_position(station, game_time_s_);
    }

    if (!ship.active_mission.sampled_path.empty() && !ship.active_mission.sampled_times_s.empty()) {
        return interpolate_timed_sampled_path(ship.active_mission.sampled_path, ship.active_mission.sampled_times_s, game_time_s_);
    }

    const double progress = std::clamp(
        (game_time_s_ - ship.active_mission.departure_time_s) / ship.active_mission.total_travel_time_s,
        0.0,
        1.0);
    if (!ship.active_mission.sampled_path.empty()) {
        return interpolate_sampled_path(ship.active_mission.sampled_path, progress);
    }

    const auto& origin = get_station_definition(ship.active_mission.origin_station_id);
    const auto& destination = get_station_definition(ship.active_mission.destination_station_id);
    const auto origin_position = mechanics_.get_station_position(origin, ship.active_mission.departure_time_s);
    const auto destination_position = mechanics_.get_station_position(destination, ship.active_mission.arrival_time_s);
    return origin_position * (1.0 - progress) + destination_position * progress;
}

std::string Simulation::build_bridge_snapshot_json(bool paused, std::uint64_t snapshot_seq, double snapshot_real_time_s) const {
    auto inventory_value = [](const domain::Inventory& inventory, const std::string& commodity_id) {
        const auto it = inventory.find(commodity_id);
        return it == inventory.end() ? 0.0 : it->second;
    };

    auto station_inventory = [&](const std::string& station_id) -> const domain::Inventory& {
        return get_station_state(station_id).inventory;
    };

    std::ostringstream output;
    output << std::fixed << std::setprecision(6);
    output << "{";
    output << "\"snapshot_seq\":" << snapshot_seq << ",";
    output << "\"snapshot_real_time_s\":" << snapshot_real_time_s << ",";
    output << "\"game_time_s\":" << game_time_s_ << ",";
    output << "\"game_time_days\":" << (game_time_s_ / 86400.0) << ",";
    output << "\"timewarp_factor\":" << timewarp_factor_ << ",";
    output << "\"paused\":" << (paused ? "true" : "false") << ",";

    output << "\"factions\":[";
    for (std::size_t i = 0; i < universe_.factions.size(); ++i) {
        const auto& faction = universe_.factions[i];
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"id\":\"" << json_escape(faction.id) << "\","
               << "\"name\":\"" << json_escape(faction.name) << "\","
               << "\"color\":\"" << json_escape(faction.color_hex) << "\""
               << "}";
    }
    output << "],";

    output << "\"commodities\":[";
    for (std::size_t i = 0; i < universe_.commodities.size(); ++i) {
        const auto& commodity = universe_.commodities[i];
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"id\":\"" << json_escape(commodity.id) << "\","
               << "\"name\":\"" << json_escape(commodity.name) << "\","
               << "\"base_price\":" << commodity.base_price
               << "}";
    }
    output << "],";

    double total_credits = 0.0;
    for (const auto& station : stations_) {
        total_credits += station.credits;
    }
    for (const auto& ship : ships_) {
        total_credits += ship.credits;
    }
    output << "\"total_credits\":" << total_credits << ",";

    output << "\"recent_trades\":[";
    for (std::size_t i = 0; i < recent_trades_.size(); ++i) {
        const auto& trade = recent_trades_[i];
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"time_s\":" << trade.time_s << ","
               << "\"ship_id\":\"" << json_escape(trade.ship_id) << "\","
               << "\"station_id\":\"" << json_escape(trade.station_id) << "\","
               << "\"commodity_id\":\"" << json_escape(trade.commodity_id) << "\","
               << "\"kind\":\"" << json_escape(trade.kind) << "\","
               << "\"units\":" << trade.units << ","
               << "\"unit_price\":" << trade.unit_price << ","
               << "\"total\":" << trade.total
               << "}";
    }
    output << "],";

    output << "\"bodies\":[";
    for (std::size_t i = 0; i < universe_.bodies.size(); ++i) {
        const auto& body = universe_.bodies[i];
        const auto position = mechanics_.get_body_position(body.id, game_time_s_);
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"id\":\"" << json_escape(body.id) << "\","
               << "\"name\":\"" << json_escape(body.name) << "\","
               << "\"parent_id\":\"" << json_escape(body.orbit.parent_id) << "\","
               << "\"radius_m\":" << body.radius_m << ","
               << "\"x\":" << position.x << ","
               << "\"y\":" << position.y << ","
               << "\"z\":" << position.z
               << "}";
    }
    output << "],";

    output << "\"stations\":[";
    for (std::size_t i = 0; i < universe_.stations.size(); ++i) {
        const auto& station = universe_.stations[i];
        const auto position = mechanics_.get_station_position(station, game_time_s_);
        if (i > 0) {
            output << ",";
        }
        const auto& inventory = station_inventory(station.id);
        output << "{"
               << "\"id\":\"" << json_escape(station.id) << "\","
               << "\"name\":\"" << json_escape(station.name) << "\","
               << "\"faction_id\":\"" << json_escape(station.faction_id) << "\","
               << "\"parent_body_id\":\"" << json_escape(station.parent_body_id) << "\","
               << "\"population\":" << station.population << ","
               << "\"x\":" << position.x << ","
               << "\"y\":" << position.y << ","
               << "\"z\":" << position.z << ","
               // Legacy fields kept for Godot frontend compatibility
               << "\"food\":" << inventory_value(inventory, "food") << ","
               << "\"fuel\":" << inventory_value(inventory, "fuel") << ","
               << "\"metals\":" << inventory_value(inventory, "metals") << ","
               // Full inventory dict for all commodities
               << "\"inventory\":{";
        for (std::size_t ci = 0; ci < universe_.commodities.size(); ++ci) {
            if (ci > 0) output << ",";
            output << "\"" << json_escape(universe_.commodities[ci].id) << "\":"
                   << inventory_value(inventory, universe_.commodities[ci].id);
        }
        output << "},";

        const auto& station_state = get_station_state(station.id);
        double storage_used = 0.0;
        for (const auto& [_, units] : inventory) {
            storage_used += std::max(0.0, units);
        }
        output << "\"credits\":" << station_state.credits << ","
               << "\"storage_capacity\":" << station.storage_capacity_units << ","
               << "\"storage_used\":" << storage_used << ","
               << "\"prices\":{";
        for (std::size_t ci = 0; ci < universe_.commodities.size(); ++ci) {
            if (ci > 0) output << ",";
            output << "\"" << json_escape(universe_.commodities[ci].id) << "\":"
                   << station_price(station_state, universe_.commodities[ci].id);
        }
        output << "},"
               << "\"net_rates\":{";
        const auto net_rates = economy_.get_profile_net_rates(station.economy_profile_id);
        bool first_rate = true;
        for (const auto& commodity : universe_.commodities) {
            const auto rate_it = net_rates.find(commodity.id);
            if (rate_it == net_rates.end()) {
                continue;
            }
            if (!first_rate) output << ",";
            first_rate = false;
            output << "\"" << json_escape(commodity.id) << "\":" << rate_it->second;
        }
        output << "}"
               << "}";
    }
    output << "],";

    output << "\"ships\":[";
    for (std::size_t i = 0; i < ships_.size(); ++i) {
        const auto& ship = ships_[i];
        const auto& ship_class = get_ship_class(ship.class_id);
        const auto position = get_ship_render_position(ship);
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"id\":\"" << json_escape(ship.id) << "\","
               << "\"name\":\"" << json_escape(ship.name) << "\","
               << "\"faction_id\":\"" << json_escape(ship.faction_id) << "\","
               << "\"class_id\":\"" << json_escape(ship.class_id) << "\","
               << "\"cargo_capacity_units\":" << ship_class.cargo_capacity_units << ","
               << "\"credits\":" << ship.credits << ","
               << "\"lifetime_profit\":" << ship.lifetime_profit << ","
               << "\"mission_value\":" << (ship.active_mission.expected_revenue - ship.active_mission.purchase_cost - ship.active_mission.fuel_cost) << ","
               << "\"propulsion_type\":\"" << json_escape(ship_class.propulsion_type) << "\","
               << "\"trajectory_type\":\"" << json_escape(ship.active_mission.trajectory_type) << "\","
               << "\"phase\":\"" << json_escape(mission_phase_name(ship.phase)) << "\","
               << "\"current_station_id\":\"" << json_escape(ship.current_station_id) << "\","
               << "\"propellant_kg\":" << ship.propellant_kg << ","
               << "\"dry_mass_kg\":" << ship_class.dry_mass_kg << ","
               << "\"propellant_capacity_kg\":" << ship_class.propellant_capacity_kg << ","
               << "\"initial_mass_kg\":" << (ship_class.dry_mass_kg + ship_class.propellant_capacity_kg) << ","
               << "\"current_mass_kg\":" << (ship_class.dry_mass_kg + std::max(0.0, ship.propellant_kg)) << ","
               << "\"origin_station_id\":\"" << json_escape(ship.active_mission.origin_station_id) << "\","
               << "\"destination_station_id\":\"" << json_escape(ship.active_mission.destination_station_id) << "\","
               << "\"commodity_id\":\"" << json_escape(ship.active_mission.commodity_id) << "\","
               << "\"cargo_units\":" << ship.active_mission.cargo_units << ","
               << "\"departure_time_s\":" << ship.active_mission.departure_time_s << ","
               << "\"arrival_time_s\":" << ship.active_mission.arrival_time_s << ","
               << "\"wait_time_s\":" << ship.active_mission.wait_time_s << ","
               << "\"coast_time_s\":" << ship.active_mission.coast_time_s << ","
               << "\"total_travel_time_s\":" << ship.active_mission.total_travel_time_s << ","
               << "\"remaining_travel_time_s\":" << ship.active_mission.remaining_travel_time_s << ","
               << "\"x\":" << position.x << ","
               << "\"y\":" << position.y << ","
               << "\"z\":" << position.z;
        if (ship.phase == domain::ShipMissionPhase::InTransit || ship.phase == domain::ShipMissionPhase::AwaitingDeparture) {
            output << ",\"trajectory_path\":[";
            for (std::size_t path_index = 0; path_index < ship.active_mission.sampled_path.size(); ++path_index) {
                const auto& point = ship.active_mission.sampled_path[path_index];
                if (path_index > 0) {
                    output << ",";
                }
                output << "{"
                       << "\"t_s\":" << (path_index < ship.active_mission.sampled_times_s.size() ? ship.active_mission.sampled_times_s[path_index] : 0.0) << ","
                       << "\"x\":" << point.x << ","
                       << "\"y\":" << point.y << ","
                       << "\"z\":" << point.z
                       << "}";
            }
            output << "]";
            if (!ship.active_mission.destination_station_id.empty()) {
                const auto& destination_station = get_station_definition(ship.active_mission.destination_station_id);
                const auto& destination_body = get_body_definition(destination_station.parent_body_id);
                const auto destination_body_position = mechanics_.get_body_position(destination_body.id, ship.active_mission.arrival_time_s);
                output << ",\"destination_body_at_arrival\":{"
                       << "\"id\":\"" << json_escape(destination_body.id) << "\","
                       << "\"name\":\"" << json_escape(destination_body.name) << "\","
                       << "\"radius_m\":" << destination_body.radius_m << ","
                       << "\"x\":" << destination_body_position.x << ","
                       << "\"y\":" << destination_body_position.y << ","
                       << "\"z\":" << destination_body_position.z
                       << "}";
            }
        }
        output << "}";
    }
    output << "],";

    output << "\"recent_events\":[";
    for (std::size_t i = 0; i < recent_events_.size(); ++i) {
        const auto& event = recent_events_[i];
        if (i > 0) {
            output << ",";
        }
        output << "{"
               << "\"time_s\":" << event.time_s << ","
               << "\"category\":\"" << json_escape(event.category) << "\","
               << "\"text\":\"" << json_escape(event.text) << "\""
               << "}";
    }
    output << "]";
    output << "}";
    return output.str();
}

std::string Simulation::build_report() const {
    auto inventory_value = [](const domain::Inventory& inventory, const std::string& commodity_id) {
        const auto it = inventory.find(commodity_id);
        return it == inventory.end() ? 0.0 : it->second;
    };

    std::ostringstream output;
    output << "SpaceTrains snapshot at day " << (game_time_s_ / 86400.0) << "\n";
    output << "Stations:\n";
    for (const auto& station : stations_) {
        output << "  - " << get_station_definition(station.station_id).name;
        for (const auto& commodity : universe_.commodities) {
            const double val = inventory_value(station.inventory, commodity.id);
            if (val > 0.1) {
                output << " " << commodity.id << "=" << static_cast<int>(val);
            }
        }
        output << "\n";
    }
    output << "Ships:\n";
    for (const auto& ship : ships_) {
        output << "  - " << ship.name << " at " << get_station_definition(ship.current_station_id).name
               << " propellant=" << ship.propellant_kg
               << " phase=" << static_cast<int>(ship.phase) << "\n";
    }
    output << "Recent events:\n";
    for (const auto& event : recent_events_) {
        output << "  - [day " << (event.time_s / 86400.0) << "] " << event.text << "\n";
    }
    return output.str();
}

}  // namespace spacetrains::simulation
