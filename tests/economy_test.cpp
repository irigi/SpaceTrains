#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

#include "data_loader/DataLoader.hpp"
#include "economy/EconomySystem.hpp"
#include "simulation/Simulation.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_near(double actual, double expected, double tolerance, const char* message) {
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    spacetrains::data_loader::DataLoader loader;
    const auto universe = loader.load_universe("data");
    const spacetrains::economy::EconomySystem economy(universe);

    {
        // --- Price formula properties ---
        // Consumed commodities target a 21-day buffer of the data-defined rate.
        const auto agri_rates = economy.get_profile_net_rates("agri_hub");
        const double water_rate = agri_rates.at("water");
        require(water_rate < 0.0, "agri_hub must consume water");
        const double target = economy.get_target_stock("agri_hub", "water");
        require_near(target, std::abs(water_rate) * 21.0, 1.0e-9,
            "agri_hub water target must be 21-day consumption buffer");

        const double base = 4.0;
        require_near(economy.get_price("agri_hub", "water", target, base), base, 1.0e-9,
            "price at target stock must equal base price");
        require_near(economy.get_price("agri_hub", "water", 0.0, base), base * 16.0, 1.0e-9,
            "price at zero stock must clamp at 16x base");
        require_near(economy.get_price("agri_hub", "water", 1.0e9, base), base * 0.25, 1.0e-9,
            "price at huge stock must clamp at 0.25x base");

        double previous = 1.0e18;
        for (double stock = 0.0; stock <= 200.0; stock += 1.0) {
            const double price = economy.get_price("agri_hub", "water", stock, base);
            require(price <= previous + 1.0e-12, "price must be non-increasing in stock");
            previous = price;
        }

        // Producer target: agri_hub produces food at 8/day → target = 112 units.
        require_near(economy.get_target_stock("agri_hub", "food"), 8.0 * 14.0, 1.0e-9,
            "producer target must be 14-day production buffer");
        // Untraded commodity falls back to the flat target.
        require_near(economy.get_target_stock("agri_hub", "reactor_fuel"), 20.0, 1.0e-9,
            "untraded commodity target must be flat 20 units");
    }

    {
        // --- Storage cap halts production ---
        const auto* earth_l1 = &universe.stations.front();
        require(earth_l1->id == "earth_l1", "expected earth_l1 as first station");
        require(earth_l1->storage_capacity_units > 0.0, "earth_l1 must have a storage capacity");

        std::vector<spacetrains::domain::StationState> stations;
        stations.push_back({.station_id = "earth_l1", .inventory = earth_l1->initial_inventory});
        // Fill the station to capacity with water so production must halt. Depot fuel
        // is stored outside the cargo storage and keeps refilling.
        stations[0].inventory["water"] += earth_l1->storage_capacity_units
            - economy.storage_used_units(stations[0].inventory);

        const double before = economy.storage_used_units(stations[0].inventory);
        economy.step(stations, 86400.0);
        const double after = economy.storage_used_units(stations[0].inventory);
        require(after <= before + 1.0e-6, "full station must not gain inventory (production halted)");
    }

    {
        // --- Propellant depots refill toward their buffer at a bounded rate ---
        const auto& fuel_supply = universe.fuel_supply;
        require(fuel_supply.depot_buffer_units > 0.0 && fuel_supply.depot_output_units_per_day > 0.0,
            "fuel_supply.csv must enable depots");
        const auto& earth_l1 = universe.stations.front();
        std::vector<spacetrains::domain::StationState> stations;
        stations.push_back({.station_id = earth_l1.id, .inventory = earth_l1.initial_inventory});
        stations[0].inventory["fuel"] = 0.0;
        economy.step(stations, 86400.0);
        // earth_l1 also produces fuel by recipe; the depot adds at most its output per day.
        const double one_day = stations[0].inventory["fuel"];
        require(one_day >= fuel_supply.depot_output_units_per_day - 1.0e-6
                && one_day <= fuel_supply.depot_output_units_per_day + 50.0,
            "an empty depot must refill at its output rate");
        for (int day = 0; day < 365; ++day) {
            economy.step(stations, 86400.0);
        }
        require(stations[0].inventory["fuel"] <= fuel_supply.depot_buffer_units + 1.0e-6,
            "a depot must stop at its buffer");
        require_near(economy.fuel_stock_after_days(0.0, 2.0), 2.0 * fuel_supply.depot_output_units_per_day, 1.0e-9,
            "forecast must count refills");
        require_near(economy.fuel_stock_after_days(0.0, 1.0e6), fuel_supply.depot_buffer_units, 1.0e-9,
            "forecast must stop at the buffer");
        require_near(economy.get_price("agri_hub", "fuel", fuel_supply.depot_buffer_units, 8.0), 8.0, 1.0e-9,
            "a full depot must sell fuel at base price");
    }

    {
        // --- Money conservation and trade settlement over 180 simulated days ---
        auto sim = spacetrains::simulation::Simulation::from_data_root("data");
        sim.set_timewarp(86400.0);

        double initial_supply = 0.0;
        for (const auto& station : sim.snapshot().stations) {
            initial_supply += station.credits;
        }
        for (const auto& ship : sim.snapshot().ships) {
            initial_supply += ship.credits;
        }
        require(initial_supply > 0.0, "initial money supply must be seeded from data files");

        for (int day = 0; day < 180; ++day) {
            sim.step(1.0);
        }

        const auto snap = sim.snapshot();
        double final_supply = 0.0;
        for (const auto& station : snap.stations) {
            final_supply += station.credits;
        }
        for (const auto& ship : snap.ships) {
            final_supply += ship.credits;
        }
        require_near(final_supply, initial_supply, 1.0e-3,
            "money must be conserved: every trade is a transfer, never a source or sink");

        // Trades must have happened and be internally consistent.
        require(!sim.recent_trades().empty(), "180 days of simulation must produce trades");
        for (const auto& trade : sim.recent_trades()) {
            require(trade.kind == "buy" || trade.kind == "sell" || trade.kind == "fuel" || trade.kind == "provisions",
                "trade kind must be buy/sell/fuel/provisions");
            require(trade.units > 0.0, "trade units must be positive");
            require_near(trade.total, trade.units * trade.unit_price, 1.0e-6,
                "trade total must equal units * unit_price");
        }

        // At least one ship should have completed a sale (positive revenue recorded).
        bool any_sell = false;
        for (const auto& trade : sim.recent_trades()) {
            if (trade.kind == "sell") {
                any_sell = true;
                break;
            }
        }
        bool any_profit_movement = false;
        for (const auto& ship : snap.ships) {
            if (std::abs(ship.lifetime_profit) > 1.0e-9) {
                any_profit_movement = true;
                break;
            }
        }
        require(any_sell || any_profit_movement, "fleet must be trading after 180 days");

        // --- Snapshot JSON exposes the market layer ---
        const auto json = sim.build_bridge_snapshot_json(false, 1, 0.0);
        for (const char* key : {"\"factions\"", "\"commodities\"", "\"total_credits\"", "\"recent_trades\"",
                                "\"prices\"", "\"net_rates\"", "\"credits\"", "\"storage_capacity\"",
                                "\"lifetime_profit\"", "\"mission_value\"", "\"category\""}) {
            require(json.find(key) != std::string::npos, "snapshot JSON missing a market field");
        }
    }

    std::cout << "All economy tests passed.\n";
    return 0;
}
