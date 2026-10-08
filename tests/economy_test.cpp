#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

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
        // earth_l1 runs the agri_hub profile; its rates scale with its population.
        const auto& agri = universe.stations.front();
        require(agri.id == "earth_l1" && agri.economy_profile_id == "agri_hub", "expected earth_l1 (agri_hub) first");
        const auto agri_rates = economy.get_station_net_rates(agri);
        const double water_rate = agri_rates.at("water");
        require(water_rate < 0.0, "agri_hub must consume water");
        const double target = economy.get_target_stock(agri, "water");
        require_near(target, std::abs(water_rate) * economy.cover_days(agri, "water"), 1.0e-9,
            "agri_hub water target must be its cover of consumption");
        require(economy.cover_days(agri, "water") >= 21.0, "a cover is at least three weeks");

        // A consumer's curve is centred on its landed cost (step 15): Earth L1 buys Lunar water.
        const double base = economy.reference_price(agri, "water");
        require(base > 20.0, "Earth L1's water reference must include carrying it from Lunar Gateway");
        require_near(economy.get_price(agri, "water", target, base), base, 1.0e-9,
            "price at target stock must equal the reference price");
        require_near(economy.get_price(agri, "water", 0.0, base), base * economy.price_cap(agri, "water"), 1.0e-9,
            "price at zero stock must clamp at the price cap");
        // An input's cap rises with what it makes (step 24): Earth L1's water feeds its farms.
        require(economy.price_cap(agri, "water") > universe.pricing.price_cap,
            "Earth L1's water must be worth more than 4x its landed cost to a dry farm");
        require(economy.price_cap(agri, "water") <= universe.pricing.input_price_cap + 1.0e-9,
            "an input's cap must stay within input_price_cap");
        require_near(economy.price_cap(agri, "medicine"), universe.pricing.price_cap, 1.0e-9,
            "a good that is not an input keeps the price cap");
        // Distant consumers pay more for carriage; producers keep the base price.
        const auto station = [&](const char* id) -> const auto& {
            return *std::find_if(universe.stations.begin(), universe.stations.end(), [&](const auto& s) { return s.id == id; });
        };
        require(economy.reference_price(station("titan_works"), "machinery")
                > economy.reference_price(station("luna_base"), "machinery"),
            "Titan's machinery must cost more to land than Lunar Gateway's");
        require_near(economy.reference_price(station("earth_orbit"), "electronics"), 200.0, 1.0e-9,
            "a producer's reference price is the base price");
        // An outpost's cover lasts the reference round trip (step 23), within max_cover_days.
        const double titan_trip = universe.pricing.round_trip_days.at("titan_works").at("electronics");
        const double titan_cover = economy.cover_days(station("titan_works"), "electronics");
        require(titan_cover >= std::min(titan_trip, universe.pricing.max_cover_days) - 1.0e-9,
            "Titan's electronics cover must last the reference round trip");
        require(titan_cover <= universe.pricing.max_cover_days + 1.0e-9, "a cover is at most max_cover_days");
        // Depot fuel far from any factory costs its carriage (step 20); a factory's does not.
        require(economy.reference_price(station("mars_transfer"), "fuel") > 2.0 * 8.0,
            "Mars fuel must cost the haul from the nearest factory");
        require_near(economy.reference_price(station("venus_cloud"), "fuel"), 8.0, 1.0e-9,
            "a fuel factory's fuel keeps the base price");
        require_near(economy.get_price(agri, "water", 1.0e9, base), base * 0.25, 1.0e-9,
            "price at huge stock must clamp at 0.25x base");

        double previous = 1.0e18;
        for (double stock = 0.0; stock <= 200.0; stock += 1.0) {
            const double price = economy.get_price(agri, "water", stock, base);
            require(price <= previous + 1.0e-12, "price must be non-increasing in stock");
            previous = price;
        }

        // Trades are valued along the price curve: a big delivery into a starving station
        // sells for less than its scarcity price, and buying back the same units costs the same.
        const double delivered = economy.get_trade_value(agri, "water", 0.0, 100.0, base);
        require(delivered < 100.0 * economy.get_price(agri, "water", 0.0, base),
            "a large delivery must not all sell at the scarcity price");
        require(delivered > 100.0 * economy.get_price(agri, "water", 100.0, base),
            "a delivery must sell above the price after it");
        require_near(economy.get_trade_value(agri, "water", 100.0, -100.0, base), delivered, 1.0e-9,
            "buying units back must cost what delivering them earned");
        require_near(economy.get_trade_value(agri, "water", 1.0e6, 10.0, base), 10.0 * base * 0.25, 1.0e-6,
            "trades on a flat (clamped) price must be units x price");
        for (const auto& [from, units] : {std::pair {0.0, 3.0}, std::pair {2.0, 40.0}, std::pair {60.0, -55.0}, std::pair {0.2, 500.0}}) {
            double riemann = 0.0;
            constexpr int STEPS = 200000;
            const double step = units / STEPS;
            for (int i = 0; i < STEPS; ++i) {
                riemann += economy.get_price(agri, "water", from + (i + 0.5) * step, base) * std::abs(step);
            }
            require_near(economy.get_trade_value(agri, "water", from, units, base), riemann, 1.0e-4 * riemann,
                "exact trade value must match the integrated price curve");
        }

        // Producer target: a 14-day production buffer.
        require(agri_rates.at("food") > 0.0, "agri_hub must produce food");
        require_near(economy.get_target_stock(agri, "food"), agri_rates.at("food") * 14.0, 1.0e-9,
            "producer target must be 14-day production buffer");
        require_near(agri_rates.at("food"),
            economy.population_factor(agri) * universe.recipes.front().units_per_day, 1.0e-9,
            "station rates must be recipe rates per 10,000 inhabitants");
        // Untraded commodity falls back to the flat target.
        require_near(economy.get_target_stock(agri, "reactor_fuel"), 20.0, 1.0e-9,
            "untraded commodity target must be flat 20 units");
    }

    {
        // --- Production dependencies (step 14) ---
        // Mercury runs smelter_hub: metals need nothing, machinery needs electronics; food,
        // water, oxygen, fuel and medicine are upkeep.
        const auto& mercury = *std::find_if(universe.stations.begin(), universe.stations.end(),
            [](const auto& station) { return station.id == "mercury_yard"; });
        const auto factor = spacetrains::economy::EconomySystem::population_factor(mercury);
        const auto stocked = [&] {
            spacetrains::domain::StationState state {.station_id = mercury.id};
            for (const auto* good : {"food", "water", "oxygen", "fuel", "medicine", "electronics"}) {
                state.inventory[good] = 1000.0 * factor;  // far above a 7-day buffer
            }
            return state;
        };
        const auto run_day = [&](spacetrains::domain::StationState state) {
            std::vector<spacetrains::domain::StationState> states {std::move(state)};
            economy.step(states, 86400.0);
            return states.front();
        };

        const auto full = run_day(stocked());
        require_near(full.upkeep_multiplier, 1.0, 1.0e-12, "a stocked station pays no upkeep penalty");
        require_near(full.output_factor.at("metals"), 1.0, 1.0e-12, "a stocked station's outputs run at full rate");
        require_near(full.output_factor.at("machinery"), 1.0, 1.0e-12, "a stocked station's outputs run at full rate");

        auto no_electronics = stocked();
        no_electronics.inventory["electronics"] = 0.0;
        const auto starved = run_day(no_electronics);
        require_near(starved.output_factor.at("machinery"), 0.0, 1.0e-12, "an output stops without its material input");
        require_near(starved.output_factor.at("metals"), 1.0, 1.0e-12, "another output's input does not affect an output");
        require_near(starved.inventory.at("electronics"), 0.0, 1.0e-12, "an idle plant uses none of its input");
        require(starved.unmet_units.at("electronics") > 0.0, "an input's stock-out counts as unmet demand");

        auto no_oxygen = stocked();
        no_oxygen.inventory["oxygen"] = 0.0;
        const auto breathless = run_day(no_oxygen);
        require_near(breathless.upkeep_multiplier, 0.1, 1.0e-12, "no oxygen leaves 10% of output");
        require_near(breathless.output_factor.at("metals"), 0.1, 1.0e-12, "upkeep penalties apply to every output");
        auto no_oxygen_or_food = no_oxygen;
        no_oxygen_or_food.inventory["food"] = 0.0;
        require_near(run_day(no_oxygen_or_food).upkeep_multiplier, 0.05, 1.0e-12, "upkeep penalties multiply");
        auto half_medicine = stocked();
        half_medicine.inventory["medicine"] = 0.75 * factor * 7.0 * 0.5;  // half the 7-day buffer
        require_near(run_day(half_medicine).upkeep_multiplier, 1.0 - 0.15 * 0.5, 1.0e-12,
            "a partial shortage scales its penalty linearly");
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
        // --- Outer-system exports: local resources, and Earth markets that buy at base price ---
        const auto station_by_id = [&](const std::string& id) -> const spacetrains::domain::StationDefinition& {
            for (const auto& station : universe.stations) {
                if (station.id == id) {
                    return station;
                }
            }
            throw std::runtime_error("missing station " + id);
        };
        const auto& ceres = station_by_id("ceres_depot");
        const auto& ganymede = station_by_id("ganymede_depot");
        require(economy.get_station_net_rates(ceres).contains("platinum")
                && economy.get_station_net_rates(ceres).at("platinum") > 0.0,
            "Ceres must mine platinum");
        require(!economy.get_station_net_rates(ganymede).contains("platinum"),
            "a station recipe must not reach other stations of the same profile");
        const auto& leo = station_by_id("earth_orbit");
        require(economy.is_export_market(leo, "platinum"), "Low Earth Logistics must buy platinum for Earth");
        const double base = 3000.0;
        require_near(economy.get_price(leo, "platinum", 0.0, base), base, 1.0e-9, "an export market pays the base price");
        require_near(economy.get_trade_value(leo, "platinum", 0.0, 500.0, base), 500.0 * base, 1.0e-6,
            "an export market's price does not move with the delivery");
        require_near(economy.get_price(station_by_id("mars_transfer"), "platinum", 0.0, base), 0.25 * base, 1.0e-9,
            "a station that neither makes nor exports platinum must not bid for it");
        std::vector<spacetrains::domain::StationState> stations;
        stations.push_back({.station_id = leo.id, .inventory = leo.initial_inventory});
        stations[0].inventory["platinum"] = 120.0;
        economy.step(stations, 3600.0);
        require(stations[0].inventory["platinum"] == 0.0 && stations[0].market_sold_units["platinum"] == 120.0,
            "an export market sells everything delivered on to the outside economy");
        require(!stations[0].demand_units.contains("platinum"), "export markets are not consumer demand");
    }

    {
        // --- Fuel factories fill their depots; other depots hold what ships deliver ---
        const auto& fuel_supply = universe.fuel_supply;
        require(fuel_supply.depot_buffer_units > 0.0 && !fuel_supply.factory_output_units_per_day.empty(),
            "fuel_supply.csv must enable depots and fuel_factories.csv list factories");
        const auto station_by_id = [&](const std::string& id) -> const spacetrains::domain::StationDefinition& {
            for (const auto& station : universe.stations) {
                if (station.id == id) {
                    return station;
                }
            }
            throw std::runtime_error("missing station " + id);
        };
        const auto& luna = station_by_id("luna_base");
        const double output = economy.fuel_factory_output(luna);
        require(output > 0.0, "Lunar Gateway must make fuel");
        const double buffer = economy.fuel_buffer_units(luna);
        require(buffer >= fuel_supply.depot_buffer_units && buffer >= output * fuel_supply.factory_buffer_days - 1.0e-9,
            "a factory's depot holds its buffer days of output");
        std::vector<spacetrains::domain::StationState> stations;
        stations.push_back({.station_id = luna.id, .inventory = luna.initial_inventory});
        stations[0].inventory["fuel"] = 0.0;
        economy.step(stations, 86400.0);
        const double one_day = stations[0].inventory["fuel"];
        require(one_day <= output + 1.0e-6 && one_day >= output + economy.get_station_net_rates(luna).at("fuel") - output - 1.0e-6,
            "an empty factory depot must fill at its output rate");
        for (int day = 0; day < 365; ++day) {
            economy.step(stations, 86400.0);
        }
        require(stations[0].inventory["fuel"] <= buffer + 1.0e-6, "a factory depot must stop at its buffer");
        require_near(economy.fuel_stock_after_days(luna, 0.0, 2.0), 2.0 * output, 1.0e-9, "forecast must count output");
        require_near(economy.fuel_stock_after_days(luna, 0.0, 1.0e6), buffer, 1.0e-9, "forecast must stop at the buffer");
        require_near(economy.get_price(luna, "fuel", buffer, 8.0), 8.0, 1.0e-9, "a full depot must sell fuel at base price");

        // Low Earth Logistics has no factory: its depot only drains.
        const auto& leo = station_by_id("earth_orbit");
        require(economy.fuel_factory_output(leo) == 0.0, "no fuel factory in Earth orbit");
        std::vector<spacetrains::domain::StationState> leo_state;
        leo_state.push_back({.station_id = leo.id, .inventory = leo.initial_inventory});
        const double before = leo_state[0].inventory["fuel"];
        economy.step(leo_state, 86400.0);
        require(leo_state[0].inventory["fuel"] < before, "a depot without a factory must not refill");
        require_near(economy.fuel_stock_after_days(leo, 100.0, 30.0), 100.0, 1.0e-9, "no output to forecast without a factory");
    }

    {
        // --- Starting stocks (step 18): every consumed good at least at its target stock,
        // unless the station's storage (85%) cannot hold it. ---
        const auto start = spacetrains::simulation::Simulation::from_data_root("data").snapshot();
        for (const auto& state : start.stations) {
            const auto& station = *std::find_if(universe.stations.begin(), universe.stations.end(),
                [&](const auto& s) { return s.id == state.station_id; });
            const bool storage_bound = station.storage_capacity_units > 0.0
                && economy.storage_used_units(state.inventory) >= 0.85 * station.storage_capacity_units - 1.0e-6;
            for (const auto& [commodity_id, rate] : economy.get_station_net_rates(station)) {
                if (rate >= 0.0 || economy.is_export_market(station, commodity_id) || storage_bound) {
                    continue;
                }
                const double stock = state.inventory.contains(commodity_id) ? state.inventory.at(commodity_id) : 0.0;
                require(stock >= economy.get_target_stock(station, commodity_id) - 1.0e-6,
                    "a station must start with its target stock of every good it consumes");
            }
        }
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
        spacetrains::domain::StationLedger stations_ledger;
        for (const auto& station : snap.stations) {
            final_supply += station.credits;
            stations_ledger.household_sales += station.ledger.household_sales;
            stations_ledger.producer_purchases += station.ledger.producer_purchases;
            stations_ledger.dividends += station.ledger.dividends;
            stations_ledger.subsidies += station.ledger.subsidies;
            stations_ledger.taxes += station.ledger.taxes;
        }
        double ship_dividends = 0.0;
        for (const auto& ship : snap.ships) {
            final_supply += ship.credits;
            ship_dividends += ship.ledger.dividends;
        }
        for (const auto& ship : snap.sold_ships) {
            require(ship.credits == 0.0, "a sold ship's cash must go to its home station");
            ship_dividends += ship.ledger.dividends;
        }
        const auto& investment = snap.fleet_investment;
        const double internal_supply = final_supply;
        double liner_subsidies = 0.0;
        for (const auto& ship : snap.ships) {
            liner_subsidies += ship.ledger.subsidies;
        }
        double treasuries = 0.0;
        for (const auto& [faction_id, balance] : snap.faction_treasuries) {
            treasuries += balance;
        }
        final_supply += snap.outside_economy_credits + treasuries;
        require_near(final_supply, initial_supply, 1.0e-3,
            "money must be conserved: stations, ships and the external account only transfer credits");

        // The open economy: every external payment is booked on a station ledger.
        require(stations_ledger.household_sales > 0.0 && stations_ledger.producer_purchases == 0.0,
            "residents must pay for consumed goods; local production is free (step 17)");
        require_near(snap.outside_economy_credits,
            stations_ledger.producer_purchases - stations_ledger.household_sales
                + investment.hulls_bought - investment.salvage + snap.faction_interest_paid, 1.0e-3,
            "the outside economy's balance must equal producer payments minus resident payments,"
            " plus ships sold to the treasuries minus salvage bought back, plus interest");
        require_near(treasuries, stations_ledger.taxes - stations_ledger.subsidies
                - investment.hulls_bought - investment.working_capital + investment.salvage - snap.emergency_paid
                - snap.faction_interest_paid - liner_subsidies, 1.0e-3,
            "the faction treasuries' balance must equal taxes minus subsidies, their ship trade and emergency premiums");
        require_near(stations_ledger.dividends, ship_dividends, 1.0e-3,
            "dividends paid by ships must equal dividends received by stations");
        // The fleet stays within its hold cap (step 21).
        double fleet_hold = 0.0;
        for (const auto& ship : snap.ships) {
            for (const auto& ship_class : sim.universe().ship_classes) {
                if (ship_class.id == ship.class_id) {
                    fleet_hold += ship_class.cargo_capacity_units;
                }
            }
        }
        const double hold_cap = sim.universe().fleet_investment.max_fleet_hold_units;
        require(hold_cap <= 0.0 || fleet_hold <= hold_cap, "fleet investment must not take the fleet's holds past the cap");
        const auto& open = sim.universe().open_economy;
        require(open.station_credit_ceiling > open.station_credit_floor, "open_economy.csv must enable a credit band");
        require_near(snap.money_supply_target, initial_supply + investment.working_capital, 1.0e-3,
            "the money-supply target must grow by the working capital of new ships");
        // Since step 17 the controller only measures (money_supply_days 0): money enters and
        // leaves through reasons (residents' wages at reference prices, Earth's purchases, hulls,
        // interest), and must still not run away.
        require(std::abs(internal_supply / snap.money_supply_target - 1.0) < (open.money_supply_days > 0.0 ? 0.25 : 0.5),
            "stations + ships must stay near the seeded money");
        // Dividends drain cash above the reserve over a month: a ship just paid for a big hold may
        // hold more for a while, but not the fleet on average.
        double fleet_cash = 0.0;
        for (const auto& ship : snap.ships) {
            fleet_cash += ship.credits;
        }
        require(fleet_cash / static_cast<double>(std::max<std::size_t>(1, snap.ships.size())) < 4.0 * open.ship_cash_reserve,
            "ships must pay out cash far above their working reserve as dividends");

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
