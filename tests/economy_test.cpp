#include <algorithm>
#include <format>
#include <random>
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
    // These tests check the station price and production model on its own: without the
    // background economies (the economy overhaul), with Earth's export markets as they were.
    auto universe = loader.load_universe("data");
    universe.surface_markets.clear();
    universe.background_economies.clear();
    universe.station_incomes.clear();
    universe.households = {};
    universe.labour_market = {};
    for (const auto* station : {"earth_orbit", "earth_l1"}) {
        universe.export_markets.push_back({.station_id = station, .commodity_id = "platinum", .units_per_day = 10.0});
        universe.export_markets.push_back({.station_id = station, .commodity_id = "deuterium", .units_per_day = 10.0});
        universe.export_markets.push_back({.station_id = station, .commodity_id = "science_samples", .units_per_day = 5.0});
    }
    const spacetrains::economy::EconomySystem economy(universe);

    {
        // --- Price formula properties ---
        // A consumer's curve (it buys under orders, step 29) is placed at three weeks of use.
        // earth_l1 runs the agri_hub profile; its rates scale with its population.
        const auto& agri = universe.stations.front();
        require(agri.id == "earth_l1" && agri.economy_profile_id == "agri_hub", "expected earth_l1 (agri_hub) first");
        const auto agri_rates = economy.get_station_net_rates(agri);
        const double water_rate = agri_rates.at("water");
        require(water_rate < 0.0, "agri_hub must consume water");
        const double target = economy.get_target_stock(agri, "water");
        require_near(target, std::abs(water_rate) * 21.0, 1.0e-9, "agri_hub water target must be three weeks of use");

        // A consumer's curve is centred on its landed cost (step 15). Earth L1's water comes from
        // Earth's surface since the economy overhaul (no landed cost in the data), so its curve is
        // centred on the base price; Mercury's carries the haul from Earth L1.
        const double base = economy.reference_price(agri, "water");
        const auto commodity_base = [&](const char* id) {
            return std::find_if(universe.commodities.begin(), universe.commodities.end(), [&](const auto& c) { return c.id == id; })
                ->base_price;
        };
        require_near(base, commodity_base("water"), 1.0e-9, "Earth L1's water is centred on the base price");
        require(economy.reference_price(*std::find_if(universe.stations.begin(), universe.stations.end(),
                    [](const auto& s) { return s.id == "mercury_yard"; }), "water") > commodity_base("water"),
            "Mercury's water reference must include carrying it there");
        require_near(economy.get_price(agri, "water", target, base), base, 1.0e-9,
            "price at target stock must equal the reference price");
        require_near(economy.get_price(agri, "water", 0.0, base), base * economy.price_cap(agri, "water"), 1.0e-9,
            "price at zero stock must clamp at the price cap");
        // Distant consumers pay more for carriage; producers keep the base price.
        const auto station = [&](const char* id) -> const auto& {
            return *std::find_if(universe.stations.begin(), universe.stations.end(), [&](const auto& s) { return s.id == id; });
        };
        require(economy.reference_price(station("titan_works"), "electronics")
                > economy.reference_price(station("luna_base"), "electronics"),
            "Titan's electronics must cost more to land than Lunar Gateway's");
        require_near(economy.reference_price(station("earth_orbit"), "electronics"), commodity_base("electronics"), 1.0e-9,
            "a producer's reference price is the base price");
        // Depot fuel far from any factory costs its carriage (step 20); a factory's does not.
        require(economy.reference_price(station("mars_transfer"), "fuel") > 2.0 * economy.reference_price(station("venus_cloud"), "fuel"),
            "Mars fuel must cost the haul from the nearest factory");
        require_near(economy.reference_price(station("venus_cloud"), "fuel"),
            universe.fuel_supply.factory_price_cr.contains("venus_cloud") ? universe.fuel_supply.factory_price_cr.at("venus_cloud")
                                                                          : commodity_base("fuel"),
            1.0e-9, "a fuel factory prices its fuel at its own cost (the base price unless the data give one)");
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
        const auto factor = economy.population_factor(mercury);
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
        // The belt miners mine platinum and sell it at Ceres (colonies, below); Ceres itself does not.
        require(!economy.get_station_net_rates(ceres).contains("platinum"), "Ceres is a port: its colony mines the platinum");
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
        require(fuel_supply.factory_buffer_days > 0.0 && !fuel_supply.factory_output_units_per_day.empty(),
            "fuel_supply.csv must set a factory buffer and fuel_factories.csv list factories");
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
        require_near(buffer, output * fuel_supply.factory_buffer_days, 1.0e-9, "a factory's depot holds its buffer days of output");
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
        require_near(economy.get_price(luna, "fuel", buffer, 8.0), economy.reference_price(luna, "fuel"), 1.0e-9,
            "a full depot must sell fuel at its curve's centre (the factory's own price)");

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
        // --- Starting stocks (step 18): every ordered good at least at its order-up-to level
        // (step 29), unless the station's storage (85%) cannot hold it. ---
        const auto start = spacetrains::simulation::Simulation::from_data_root("data").snapshot();
        for (const auto& state : start.stations) {
            const auto& station = *std::find_if(universe.stations.begin(), universe.stations.end(),
                [&](const auto& s) { return s.id == state.station_id; });
            const bool storage_bound = station.storage_capacity_units > 0.0
                && economy.storage_used_units(state.inventory) >= 0.85 * station.storage_capacity_units - 1.0e-6;
            for (const auto& [commodity_id, rate] : economy.get_station_net_rates(station)) {
                if (rate >= 0.0 || !state.orders.contains(commodity_id) || storage_bound) {
                    continue;
                }
                const double stock = state.inventory.contains(commodity_id) ? state.inventory.at(commodity_id) : 0.0;
                require(stock >= state.orders.at(commodity_id).order_up_to_units - 1.0e-6,
                    "a station must start with its order-up-to level of every good it consumes");
            }
        }
    }

    {
        // --- Money conservation and trade settlement over 180 simulated days ---
        auto sim = spacetrains::simulation::Simulation::from_data_root("data");
        sim.set_timewarp(86400.0);

        // Residents start with savings brought from Earth (economy overhaul): the seeded money
        // the controller holds counts them, and Earth's account starts that much below zero.
        double initial_supply = 0.0;
        double seeded_internal = 0.0;
        {
            const auto start = sim.snapshot();
            for (const auto& station : start.stations) {
                initial_supply += station.credits;
            }
            for (const auto& ship : start.ships) {
                initial_supply += ship.credits;
            }
            seeded_internal = initial_supply;   // the controller holds stations + ships
            for (const auto& station : start.stations) {
                initial_supply += station.household_credits;
            }
            initial_supply += start.outside_economy_credits;
            for (const auto& [economy_id, balance] : start.background_credits) {
                initial_supply += balance;
            }
            for (const auto& [faction_id, balance] : start.faction_treasuries) {
                initial_supply += balance;
            }
        }
        require(initial_supply > 0.0, "initial money supply must be seeded from data files");

        for (int day = 0; day < 180; ++day) {
            sim.step(1.0);
        }

        const auto snap = sim.snapshot();
        double final_supply = 0.0;
        spacetrains::domain::StationLedger stations_ledger;
        for (const auto& station : snap.stations) {
            final_supply += station.credits + station.household_credits;
            stations_ledger.household_sales += station.ledger.household_sales;
            stations_ledger.producer_purchases += station.ledger.producer_purchases;
            stations_ledger.dividends += station.ledger.dividends;
            stations_ledger.subsidies += station.ledger.subsidies;
            stations_ledger.taxes += station.ledger.taxes;
            stations_ledger.passages_paid += station.ledger.passages_paid;
            stations_ledger.passages_recovered += station.ledger.passages_recovered;
        }
        double ship_dividends = 0.0;
        double fares = 0.0;
        for (const auto& ship : snap.ships) {
            final_supply += ship.credits;
            ship_dividends += ship.ledger.dividends;
            fares += ship.ledger.fares;
        }
        for (const auto& ship : snap.sold_ships) {
            require(ship.credits == 0.0, "a sold ship's cash must go to its home station");
            ship_dividends += ship.ledger.dividends;
            fares += ship.ledger.fares;
        }
        const auto& investment = snap.fleet_investment;
        double internal_supply = final_supply;
        for (const auto& station : snap.stations) {
            internal_supply -= station.household_credits;
        }
        double liner_subsidies = 0.0;
        for (const auto& ship : snap.ships) {
            liner_subsidies += ship.ledger.subsidies;
        }
        double treasuries = 0.0;
        for (const auto& [faction_id, balance] : snap.faction_treasuries) {
            treasuries += balance;
        }
        final_supply += snap.outside_economy_credits + treasuries;
        for (const auto& [economy_id, balance] : snap.background_credits) {
            final_supply += balance;
        }
        require_near(final_supply, initial_supply, 1.0e-3,
            "money must be conserved: stations, ships and the external account only transfer credits");

        // The open economy: every external payment is booked on a station ledger.
        require(stations_ledger.household_sales > 0.0 && stations_ledger.producer_purchases == 0.0,
            "residents must pay for consumed goods; local production is free (step 17)");
        // With households and background economies on (economy overhaul), Earth's account takes
        // many more flows (surface trade, incomes, yards); conservation above covers them.
        if (sim.universe().households.enabled == 0.0) require_near(snap.outside_economy_credits,
            stations_ledger.producer_purchases - stations_ledger.household_sales
                + investment.hulls_bought - investment.salvage + snap.faction_interest_paid
                - (fares - stations_ledger.passages_paid) - stations_ledger.passages_recovered, 1.0e-3,
            "the outside economy's balance must equal producer payments minus resident payments,"
            " plus ships sold to the treasuries minus salvage bought back, plus interest, less the fares"
            " travellers paid themselves and the passages stations recovered");
        require_near(treasuries, stations_ledger.taxes - stations_ledger.subsidies
                - investment.hulls_bought - investment.working_capital + investment.salvage - snap.emergency_paid
                - snap.faction_interest_paid - liner_subsidies, 1.0e-3,
            "the faction treasuries' balance must equal taxes minus subsidies, their ship trade and emergency premiums");
        require_near(stations_ledger.dividends, ship_dividends, 1.0e-3,
            "dividends paid by ships must equal dividends received by stations");
        // Economy overhaul: no station spends long in the red (factions cover it within days).
        if (sim.universe().households.enabled != 0.0) {
            for (const auto& station : snap.stations) {
                require(station.ledger.days_in_red < 30.0, "no station should spend a month of 180 days in the red");
            }
        }
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
        require_near(snap.money_supply_target, seeded_internal + investment.working_capital, 1.0e-3,
            "the money-supply target must grow by the working capital of new ships");
        // Money enters and leaves through reasons (residents paying what their station paid,
        // Earth's purchases, hulls, interest) and a slow controller (money_supply_days) pulls it
        // back, and it must not run away. Under station orders (step 29) a delivery is paid one
        // flat price, so imports and what residents pay start higher: 180 days end at +32%
        // (+24% on the curve).
        // Without the controller (economy overhaul: money enters and leaves through named flows,
        // Earth's incomes among them), it must still not run away.
        require(open.money_supply_days > 0.0 ? std::abs(internal_supply / snap.money_supply_target - 1.0) < 0.35
                                             : internal_supply / snap.money_supply_target < 3.0 && internal_supply > 0.0,
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
            require(trade.kind == "buy" || trade.kind == "sell" || trade.kind == "fuel" || trade.kind == "provisions"
                    || trade.kind == "spares",
                "trade kind must be buy/sell/fuel/provisions/spares");
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
                                "\"lifetime_profit\"", "\"mission_value\"", "\"category\"", "\"household_credits\"",
                                "\"surface_trade\"", "\"wage_cr_per_day\""}) {
            require(json.find(key) != std::string::npos, "snapshot JSON missing a market field");
        }
    }

    {
        // --- Events (step 28): an outage stops the output it hits; a demand spike multiplies use. ---
        const auto& events = universe.events;
        const auto storm = std::find_if(events.begin(), events.end(), [](const auto& e) { return e.id == "mercury_storm"; });
        const auto epidemic = std::find_if(events.begin(), events.end(), [](const auto& e) { return e.id == "mars_epidemic"; });
        require(storm != events.end() && epidemic != events.end(), "the data should define mercury_storm and mars_epidemic");
        const auto run_day = [&](const std::string& station_id, const std::string& commodity_id, const char* event_id) {
            spacetrains::domain::StationState state {.station_id = station_id};
            for (const auto& commodity : universe.commodities) {
                state.inventory[commodity.id] = 200.0;
            }
            if (event_id != nullptr) {
                state.events.push_back({.event_id = event_id, .start_s = 0.0, .end_s = 1.0e9});
            }
            std::vector<spacetrains::domain::StationState> states {state};
            economy.step(states, 86400.0);
            return states.front().inventory[commodity_id] - 200.0;
        };
        const double normal = run_day("mercury_yard", "metals", nullptr);
        require(normal > 1.0, "Mercury should make metals on a normal day");
        require(std::abs(run_day("mercury_yard", "metals", "mercury_storm")) < 1.0e-9,
            "a solar storm should halt Mercury's metals output");
        const double use = run_day("mars_transfer", "medicine", nullptr);
        require(use < 0.0, "Mars should use medicine");
        require_near(run_day("mars_transfer", "medicine", "mars_epidemic"), use * epidemic->magnitude, 1.0e-9,
            "an epidemic should multiply Mars's medicine use");
    }

    {
        // --- Economy overhaul (docs/plans/economy_overhaul.md): the game's own data. ---
        const auto full = loader.load_universe("data");
        require(full.households.enabled != 0.0 && full.labour_market.enabled != 0.0,
            "the data should switch households and the labour market on");
        const spacetrains::economy::EconomySystem overhaul(full);
        const auto station = [&](const char* id) -> const auto& {
            return *std::find_if(full.stations.begin(), full.stations.end(), [&](const auto& s) { return s.id == id; });
        };
        const auto commodity = [&](const char* id) -> const auto& {
            return *std::find_if(full.commodities.begin(), full.commodities.end(), [&](const auto& c) { return c.id == id; });
        };
        const double cr = full.households.credits_per_usd;
        // Earth sells at its surface price plus launch, in any amount: a lot costs the ask per unit.
        const auto& leo = station("earth_orbit");
        const double food_ask = (8.0 + 100.0) * commodity("food").mass_per_unit_kg * cr;
        require(overhaul.surface_supplied(leo, "food"), "Earth keeps Low Earth Logistics stocked with food");
        require_near(overhaul.reference_price(leo, "food"), food_ask, 1e-9, "a gateway's curve is centred on Earth's ask");
        require_near(overhaul.get_trade_value(leo, "food", overhaul.surface_depth_units(), -1000.0, commodity("food").base_price),
            1000.0 * food_ask, 1e-6, "buying from Earth's stock costs the ask, never more");
        // Earth buys space goods at its surface price less landing, at a flat price.
        const double platinum_bid = (32000.0 - 50.0) * commodity("platinum").mass_per_unit_kg * cr;
        require(overhaul.is_export_market(leo, "platinum"), "Earth buys platinum at its gateways");
        require_near(overhaul.get_price(leo, "platinum", 0.0, commodity("platinum").base_price), platinum_bid, 1e-9,
            "the platinum bid is Earth's surface price less landing");
        require(!overhaul.is_export_market(leo, "food") && overhaul.surface_bid(leo, "food") == 0.0,
            "Earth does not buy ordinary goods (stations make them for free)");
        // Low-orbit propellant for Earth's own traffic: bought at 0.9 of the ask, which floors the curve.
        require_near(overhaul.surface_bid(leo, "fuel"), 0.9 * overhaul.reference_price(leo, "fuel"), 1e-9,
            "Earth's traffic buys propellant at 0.9 of the ask");
        require(overhaul.get_price(leo, "fuel", 1.0e9, commodity("fuel").base_price) >= overhaul.surface_bid(leo, "fuel") - 1e-9,
            "a glut never sells below what Earth's traffic pays");
        // Earth's stock at its gateway stands in its own warehouse.
        spacetrains::domain::Inventory stocked {{"food", overhaul.surface_depth_units()}};
        require(overhaul.storage_used_units(leo, stocked) == 0.0, "Earth's stock does not fill the gateway's storage");
        // A colony's trade at its gateway is a rate the planners see.
        const auto& mars = station("mars_transfer");
        require(overhaul.surface_market(mars, "fuel") != nullptr && overhaul.get_station_net_rates(mars).at("fuel") > 0.0,
            "the Mars surface sells propellant at Mars Transfer Port");
        // Colonies: the surface mines, the gateway trades (docs/modules/economy.md, "Colonies").
        const auto& ceres_full = *std::find_if(full.stations.begin(), full.stations.end(), [](const auto& s) { return s.id == "ceres_depot"; });
        const auto& luna_full = *std::find_if(full.stations.begin(), full.stations.end(), [](const auto& s) { return s.id == "luna_base"; });
        const auto* belt_platinum = overhaul.surface_market(ceres_full, "platinum");
        require(belt_platinum != nullptr && belt_platinum->ask_cr > 0.6 * commodity("platinum").base_price
                && belt_platinum->ask_cr < 0.8 * commodity("platinum").base_price,
            "the belt sells platinum at Ceres at about 70% of Earth's price");
        require(!overhaul.makes_itself(ceres_full, "platinum") && !overhaul.makes_itself(luna_full, "helium_3"),
            "the gateways no longer mine");
        require_near(overhaul.reference_price(ceres_full, "platinum"), belt_platinum->ask_cr * full.households.colony_centre_markup,
            1e-9, "a colony good is centred just above the colony's ask");
        // A colony slows its imports as it runs down, essentials last; it never stops dead above its line.
        using Sim = spacetrains::simulation::Simulation;
        require_near(Sim::colony_affordability(-100000.0, 100000.0, 50000.0), 0.0, 1e-12, "nothing at the credit line");
        require_near(Sim::colony_affordability(60000.0, 100000.0, 50000.0), 1.0, 1e-12, "in full above the buffer");
        require_near(Sim::colony_affordability(-25000.0, 100000.0, 50000.0), 0.5, 1e-12, "half way between line and buffer");
        require(Sim::colony_affordability(0.0, 100000.0, 0.0) == 1.0 && Sim::colony_affordability(-100001.0, 100000.0, 0.0) == 0.0,
            "without a buffer, the old hard stop");
        require(Sim::colony_import_share(0.5, true) == 1.0 && Sim::colony_import_share(0.5, false) == 0.0
                && Sim::colony_import_share(0.75, false) == 0.5 && Sim::colony_import_share(1.0, false) == 1.0,
            "food and medicine keep coming while machinery and electronics are cut");

        auto sim = spacetrains::simulation::Simulation::from_data_root("data");
        sim.set_timewarp(86400.0);
        const auto start = sim.snapshot();
        // Distributions: the share of Earth's job seekers gaining at least g falls with g.
        const auto& titan = *std::find_if(start.stations.begin(), start.stations.end(),
            [](const auto& s) { return s.station_id == "titan_works"; });
        double last = 1.0;
        for (double gain : {0.0, 500.0, 1000.0, 2000.0, 4000.0}) {
            const double share = sim.settler_gain_tail(titan, gain);
            require(share >= 0.0 && share <= last + 1e-12, "fewer seekers gain more");
            last = share;
        }
        // Monte Carlo check of the closed form: P(w_S (1 - h) - w_E) x stay >= g) for lognormal w_E.
        {
            const auto& labour = full.labour_market;
            const double wage = sim.station_wage_cr_per_day(titan);
            const double gain = 1500.0;
            std::mt19937_64 rng(7);
            std::lognormal_distribution<double> earth(std::log(labour.earth_wage_median_cr_per_day), labour.earth_wage_sigma);
            int hits = 0;
            constexpr int N = 200000;
            for (int i = 0; i < N; ++i) {
                hits += (wage * (1.0 - labour.hardship_share) - earth(rng)) * 365.25 * labour.stay_years >= gain;
            }
            require(std::abs(sim.settler_gain_tail(titan, gain) - static_cast<double>(hits) / N) < 0.005,
                "the settlers' gain tail should match a Monte Carlo draw");
        }
        require(sim.traveller_budget_tail("earth_orbit", 100.0) > sim.traveller_budget_tail("earth_orbit", 1000.0),
            "fewer can afford a dearer trip");
        // A day's flows: wages paid, goods paid for by residents, incomes received; nothing made.
        sim.step(1.0);
        const auto day = sim.snapshot();
        double wages = 0.0;
        double household_sales = 0.0;
        double incomes = 0.0;
        for (const auto& s : day.stations) {
            wages += s.ledger.wages_paid;
            household_sales += s.ledger.household_sales;
            incomes += s.ledger.incomes;
        }
        double expected_incomes = 0.0;
        for (const auto& income : full.station_incomes) {
            expected_incomes += income.cr_per_day;
        }
        require(wages > 0.0 && household_sales > 0.0, "stations pay wages and residents pay for goods");
        require_near(incomes, expected_incomes, 1e-6, "every station income is paid each day");
        for (const auto& s : day.stations) {
            require(s.station_id == "earth_orbit" || s.station_id == "earth_l1" || s.jobs > 0.0,
                "every station off Earth has jobs");
        }
    }

    {
        // --- Money journal (MoneyJournal.hpp): every move is booked on both sides. ---
        using spacetrains::simulation::MoneyCategory;
        auto sim = spacetrains::simulation::Simulation::from_data_root("data");
        sim.set_timewarp(86400.0);
        for (int day = 0; day < 400; ++day) {
            sim.step(1.0);
        }
        const auto& journal = sim.money_journal();
        const auto& accounts = journal.accounts();
        require(accounts.size() > 20, "the journal has an account for Earth, colonies, factions, stations, residents, fleets");
        for (int i = 0; i < static_cast<int>(accounts.size()); ++i) {
            const double balance = sim.account_balance(i);
            if (std::abs(journal.journal_balance(i) - balance) > 1e-6 * std::max(1.0e3, std::abs(balance))) {
                throw std::runtime_error("journal balance differs from the account: " + accounts[static_cast<std::size_t>(i)].key()
                    + std::format(" journal {:.3f} real {:.3f}", journal.journal_balance(i), balance));
            }
        }
        for (std::size_t c = 0; c < spacetrains::simulation::kMoneyCategories; ++c) {
            double in = 0.0;
            double out = 0.0;
            for (int i = 0; i < static_cast<int>(accounts.size()); ++i) {
                in += journal.lifetime(i)[c].in;
                out += journal.lifetime(i)[c].out;
            }
            require_near(in, out, 1e-6 * std::max(1.0, in), "every category is paid by someone and received by someone");
        }
        // Monthly samples: each period's flows explain the change in balances between samples.
        const auto& history = journal.history();
        require(history.size() >= 13, "a sample every 30 days");
        for (std::size_t k = 1; k < history.size(); ++k) {
            for (std::size_t i = 0; i < accounts.size(); ++i) {
                double net = 0.0;
                for (const auto& flow : history[k].flows[i]) {
                    net += flow.in - flow.out;
                }
                const double change = history[k].balances[i] - history[k - 1].balances[i];
                require_near(change, net, 1e-6 * std::max(1.0e3, std::abs(history[k].balances[i])),
                    "a period's flows add up to the change in balance");
            }
        }
        // Wages, cargo and taxes are all booked.
        const int earth_orbit = journal.find(spacetrains::simulation::AccountKind::Station, "earth_orbit");
        require(journal.lifetime(earth_orbit)[static_cast<std::size_t>(MoneyCategory::Wages)].out > 0.0, "stations pay wages");
        require(journal.lifetime(earth_orbit)[static_cast<std::size_t>(MoneyCategory::Cargo)].in > 0.0, "ships buy cargo");
    }

    std::cout << "All economy tests passed.\n";
    return 0;
}
