// Faction treasuries, credit, fleet investment, ship sales and commissioning.

#include "simulation/Simulation.hpp"
#include "domain/ShipDesign.hpp"
#include "simulation/CrewEnvironment.hpp"

#include "util/Profiling.hpp"

#include <algorithm>
#include <chrono>
#include <array>
#include <tuple>
#include <cmath>
#include <numbers>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "simulation/SimulationInternal.hpp"

namespace spacetrains::simulation {

using namespace detail;

double Simulation::internal_money_supply() const {
    // Station and ship money (what the controller holds); residents' savings are their own.
    double total = 0.0;
    for (const auto& station : stations_) {
        total += station.credits;
    }
    for (const auto& ship : ships_) {
        total += ship.credits;
    }
    return total;
}

void Simulation::step_treasuries(double dt_s) {
    const profiling::Scope profile_scope(profiling::Phase::Treasuries);
    const auto& open = universe_.open_economy;
    const double dt_days = dt_s / 86400.0;

    // Ships keep a working reserve; the rest goes to the owner, the home station.
    if (open.ship_cash_reserve > 0.0) {
        const double share = std::min(1.0, dt_days / open.dividend_days);
        for (auto& ship : ships_) {
            if (ship.credits <= open.ship_cash_reserve) {
                continue;
            }
            const double dividend = (ship.credits - open.ship_cash_reserve) * share;
            auto& home = get_station_state(ship.home_station_id);
            ship.credits -= dividend;
            ship.ledger.dividends += dividend;
            home.credits += dividend;
            home.ledger.dividends += dividend;
            journal_.record(account_fleet(ship.faction_id), account_station(home), MoneyCategory::Dividends, dividend);
        }
    }

    // A liner keeps its schedule whatever it earns: its faction tops its cash up to the working
    // reserve when it runs out (step 22).
    for (auto& ship : ships_) {
        if (ship.credits < 0.0 && liner_stops(ship) != nullptr) {
            const double top_up = open.ship_cash_reserve - ship.credits;
            ship.credits += top_up;
            ship.ledger.subsidies += top_up;
            faction_treasuries_[ship.faction_id] -= top_up;
            journal_.record(account_faction(ship.faction_id), account_fleet(ship.faction_id), MoneyCategory::LinerTopup, top_up);
        }
    }

    // The slow controller hands the money-supply gap to the stations per head of population,
    // as subsidies (or taxes when there is too much money).
    double per_capita = 0.0;
    if (open.money_supply_days > 0.0) {
        double population = 0.0;
        for (const auto& station : stations_) {
            population += station.population;
        }
        if (population > 0.0) {
            per_capita = (seeded_money_supply_ - internal_money_supply()) * std::min(1.0, dt_days / open.money_supply_days)
                / population;
        }
    }

    // Faction treasuries close the gap of stations outside the credit band: they tax the excess
    // above the ceiling, and top up a station below the floor: while the faction borrows, by at
    // most the upkeep of its core crew (step 17), so a station that cannot pay its way gets less
    // than it spends; out of its savings, by the whole gap (step 26).
    const double band_share = std::min(1.0, dt_days / open.station_balance_days);
    const double tax_decay = std::exp(-dt_days / 365.0);
    for (auto& [faction_id, per_day] : faction_tax_per_day_) {
        per_day *= tax_decay;
    }
    for (auto& station : stations_) {
        const auto& definition = get_station_definition(station.station_id);
        double transfer = 0.0;
        MoneyCategory band_category = MoneyCategory::FactionTopup;
        if (station.credits < open.station_credit_floor) {
            transfer = (open.station_credit_floor - station.credits) * band_share;
            if (open.core_crew_fraction > 0.0) {
                // A faction in surplus covers the whole gap out of its savings (step 26): the
                // core-crew cap left outposts deep in debt for the stock step 23 has them hold,
                // while their faction's taxes piled up unused.
                const double savings = std::max(0.0, faction_treasuries_[definition.faction_id]);
                // Rotation fares (passengers) are upkeep too: the faction keeps its staff relieved.
                const auto* tour = rotation_tour(definition.id);
                const double rotation = tour == nullptr ? 0.0 : tour->fares_per_day * rotation_positions(station);
                // With households (economy overhaul), the core crew's wages are upkeep too.
                const double core_wages = households_on() ? resident_wages_per_day(station) : 0.0;
                // A station in the red (economy overhaul: wages make the gaps large) gets the whole gap
                // while its faction can still borrow it; it never lets its own station go insolvent.
                const bool insolvent = households_on() && station.credits < 0.0
                    && faction_spendable(definition.faction_id) > open.station_credit_floor - station.credits;
                if (insolvent) {
                    band_category = MoneyCategory::InsolvencyCover;
                    transfer = (open.station_credit_floor - station.credits) * std::min(1.0, dt_days / INSOLVENCY_COVER_DAYS);
                } else {
                    transfer = std::min(transfer,
                        std::max((open.core_crew_fraction * (consumption_value_per_day(definition, true) + core_wages) + rotation) * dt_days,
                            savings));
                }
            }
        } else if (const double ceiling = station_tax_ceiling(open, faction_treasuries_[definition.faction_id]);
                   ceiling > 0.0 && station.credits > ceiling) {
            transfer = -(station.credits - ceiling) * band_share;
            band_category = MoneyCategory::FactionTax;
        }
        const double band_transfer = transfer;
        // The controller never pushes a station out of the band, or it would fight the band.
        const double after_band = station.credits + transfer;
        const double controller = per_capita * station.population;
        if (controller < 0.0) {
            transfer -= std::min(-controller, std::max(0.0, after_band - open.station_credit_floor));
        } else if (const double ceiling = station_tax_ceiling(open, faction_treasuries_[definition.faction_id]); ceiling > 0.0) {
            transfer += std::min(controller, std::max(0.0, ceiling - after_band));
        } else {
            transfer += controller;
        }
        station.credits += transfer;
        faction_treasuries_[definition.faction_id] -= transfer;
        journal_.record(account_faction(definition.faction_id), account_station(station), band_category, band_transfer);
        journal_.record(account_faction(definition.faction_id), account_station(station), MoneyCategory::MoneyController,
            transfer - band_transfer);
        if (transfer > 0.0) {
            station.ledger.subsidies += transfer;
        } else {
            station.ledger.taxes -= transfer;
            faction_tax_per_day_[definition.faction_id] += -transfer / 365.0;
        }
    }

    // A faction in debt pays interest to the outside economy (Earth's lenders).
    const double rate = universe_.ship_operations.interest_rate_per_year;
    for (auto& [faction_id, balance] : faction_treasuries_) {
        if (balance < 0.0 && rate > 0.0) {
            const double interest = -balance * rate * dt_days / 365.0;
            balance -= interest;
            outside_economy_credits_ += interest;
            journal_.record(account_faction(faction_id), account_earth(), MoneyCategory::Interest, interest);
            faction_interest_paid_ += interest;
        }
    }
}

double Simulation::consumption_value_per_day(const domain::StationDefinition& station, bool upkeep_only) const {
    double value = 0.0;
    for (const auto& [commodity_id, rate] : economy_.get_station_net_rates(station)) {
        if (rate < 0.0 && !economy_.is_export_market(station, commodity_id)
            && (!upkeep_only || economy_.is_upkeep(station, commodity_id))) {
            value += -rate * economy_.reference_price(station, commodity_id);
        }
    }
    return value;
}

double Simulation::faction_credit_limit(const std::string& faction_id) const {
    const auto& open = universe_.open_economy;
    double fleet_value = 0.0;
    for (const auto& ship : ships_) {
        if (ship.faction_id == faction_id) {
            fleet_value += ship_design(ship).ship_value_cr;
        }
    }
    // Its stations' stock is collateral too, at reference prices (step 26): after step 23 a
    // remote colony stocks a year or more of its imports, which its faction paid for; Mars
    // Corporation, with one station and few ships, went over its limit building that stock.
    double stock_value = 0.0;
    for (const auto& station : stations_) {
        const auto& definition = get_station_definition(station.station_id);
        if (definition.faction_id != faction_id) {
            continue;
        }
        for (const auto& [commodity_id, units] : station.inventory) {
            stock_value += std::max(0.0, units) * economy_.reference_price(definition, commodity_id);
        }
    }
    const auto tax = faction_tax_per_day_.find(faction_id);
    return open.faction_loan_to_value * (fleet_value + stock_value)
        + open.faction_tax_years * 365.0 * (tax == faction_tax_per_day_.end() ? 0.0 : tax->second);
}

double Simulation::station_tax_ceiling(const domain::OpenEconomyDefinition& open, double treasury) {
    // A faction in debt draws its stations' cash earlier: it covered their red days, and with taxes
    // only above the full ceiling a station's cash pile stayed borrowed money (Mars Corporation).
    return treasury < 0.0 && open.station_credit_ceiling_in_debt > 0.0 ? open.station_credit_ceiling_in_debt
                                                                        : open.station_credit_ceiling;
}

double Simulation::faction_spendable(const std::string& faction_id) const {
    const auto it = faction_treasuries_.find(faction_id);
    return (it == faction_treasuries_.end() ? 0.0 : it->second) + faction_credit_limit(faction_id);
}

void Simulation::step_fleet_investment() {
    const profiling::Scope profile_scope(profiling::Phase::FleetInvestment);
    const auto& investment = universe_.fleet_investment;
    if (investment.layup_sale_days > 0.0) {
        for (std::size_t i = ships_.size(); i-- > 0;) {
            const auto& ship = ships_[i];
            if (ship.phase == domain::ShipMissionPhase::LaidUp && liner_stops(ship) == nullptr
                && game_time_s_ - ship.laid_up_since_s >= investment.layup_sale_days * 86400.0) {
                sell_ship(i);
            }
        }
    }
    if (investment.review_days > 0.0 && game_time_s_ >= next_investment_review_s_) {
        next_investment_review_s_ += investment.review_days * 86400.0;
        investment_purchases_left_ = static_cast<int>(investment.max_ships_per_review);
        review_probes_.clear();
    }
    // One purchase per tick (each takes a second or two of probing), so a review never
    // stalls the simulation for long. Each purchase is a committed flow and keeps its yard
    // busy, so the next one is valued against the demand still open.
    // The fleet is limited by its total hold (step 21: a cap on ship numbers made a cheap local
    // shuttle cost a place as much as a 2,000 u freighter, so none was bought), with a ship
    // count as a backstop for run time. Laid-up ships sold for salvage free their places.
    if ((investment.max_fleet_size > 0.0 && static_cast<double>(fleet_ship_count()) >= investment.max_fleet_size)
        || (investment.max_fleet_hold_units > 0.0 && fleet_hold_units() >= investment.max_fleet_hold_units)) {
        investment_purchases_left_ = 0;
    }
    // One batch of probes or one purchase per tick: a review never stalls the simulation for
    // long (probing every candidate at once took up to six seconds with ninety ships).
    // Purchases at least purchase_spacing_days apart: each plans every variant of the winning
    // hull, and a review's purchases falling within a few days made the slowest days (0.6-1 s).
    if (investment_purchases_left_ > 0 && game_time_s_ >= next_purchase_s_) {
        switch (commission_step()) {
            case CommissionStep::Probed:
                break;
            case CommissionStep::Bought:
                --investment_purchases_left_;
                if (investment.purchase_spacing_days > 0.0) {
                    next_purchase_s_ = game_time_s_ + investment.purchase_spacing_days * 86400.0;
                }
                break;
            case CommissionStep::NothingToBuy:
                investment_purchases_left_ = 0;
                break;
        }
    }
    if (investment_purchases_left_ <= 0) {
        review_probes_.clear();
    }
}

void Simulation::sell_ship(std::size_t index) {
    auto& ship = ships_[index];
    const auto& ship_class = ship_design(ship);
    // The outside economy buys the hull (with what is left in its tanks and stores) for its
    // salvage value, paid to the owner's treasury. The ship's cash, or its debt, goes to the
    // home station, like its dividends.
    const double salvage = universe_.fleet_investment.salvage_fraction * ship_class.ship_value_cr;
    outside_economy_credits_ -= salvage;
    faction_treasuries_[ship.faction_id] += salvage;
    journal_.record(account_earth(), account_faction(ship.faction_id), MoneyCategory::Salvage, salvage);
    investment_ledger_.salvage += salvage;
    ++investment_ledger_.ships_sold;
    auto& home = get_station_state(ship.home_station_id);
    home.credits += ship.credits;
    home.ledger.dividends += ship.credits;
    journal_.record(account_fleet(ship.faction_id), account_station(home), MoneyCategory::ShipSaleCash, ship.credits);
    ship.ledger.dividends += ship.credits;
    ship.credits = 0.0;
    add_event(std::format("{} sold for salvage at {} ({:.0f} cr) after {:.0f} days laid up",
        ship.name, get_station_definition(ship.current_station_id).name, salvage,
        (game_time_s_ - ship.laid_up_since_s) / 86400.0), "alert");
    sold_ships_.push_back(std::move(ship));
    ships_.erase(ships_.begin() + static_cast<std::ptrdiff_t>(index));
}

// One fleet investment review (see commission_step in Simulation.hpp): candidates (hull and yard)
// with a cheap bound on their returns, valued in order of the bound from dispatch probes run
// in batches, then the winner built with its best tanks.
class Simulation::InvestmentReview {
public:
    explicit InvestmentReview(Simulation& sim) : sim_(sim) {}

    CommissionStep run() {
        list_candidates();
        if (probe_or_value_candidates()) {
            return CommissionStep::Probed;
        }
        choose_tanks();
        if (trace) {
            std::cerr << std::format("[invest day {:.0f}] review: {} candidates, {} probed in {:.1f} s\n",
                sim_.game_time_s_ / 86400.0, candidates.size(), probes,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - review_start).count());
        }
        if (best_class == nullptr) {
            return CommissionStep::NothingToBuy;
        }
        buy();
        return CommissionStep::Bought;
    }

private:
    struct Candidate {
        const domain::ShipClassDefinition* ship_class {nullptr};
        const domain::StationDefinition* yard {nullptr};
        double return_bound {0.0};
        double rank_bound {0.0};  // in the review's ranking: return per year, or profit per day
    };
    struct Valuation {
        double annual_return {-std::numeric_limits<double>::infinity()};
        std::string label;
        std::string destination_id;
        std::string commodity_id;
        double units_per_day {0.0};
    };
    // Ships docked idle or laid up, by hull (looked up once per review, not per candidate).
    struct IdleHull {
        const std::string* station_id;
        const std::string* hull_id;
        double berths;
    };

    Simulation& sim_;
    const domain::FleetInvestmentDefinition& investment = sim_.universe_.fleet_investment;
    // Debug aid: SPACETRAINS_TRACE_INVESTMENT=1 logs every candidate and the review's time (stderr).
    const bool trace = trace_on();
    const std::chrono::steady_clock::time_point review_start = std::chrono::steady_clock::now();
    // Treasuries pay for hulls with their cash and what they may borrow (step 17).
    const double richest_treasury = richest_spendable();
    // Far from the fleet limit the treasuries rank candidates by return on their price; from
    // half the limit on, hold space in the fleet is the scarce thing, so by profit per day per
    // unit of hold (above the hurdle): what a ship earns for the share of the cap it takes.
    const double fleet_hold = sim_.fleet_hold_units();
    // Under a hold cap a candidate competes per unit of hold it takes; under a ship cap, per ship.
    const bool per_hold_unit = investment.max_fleet_hold_units > 0.0;
    const bool rank_by_profit = investment.max_fleet_hold_units > 0.0
        ? fleet_hold >= 0.5 * investment.max_fleet_hold_units
        : investment.max_fleet_size > 0.0 && static_cast<double>(sim_.fleet_ship_count()) >= 0.5 * investment.max_fleet_size;
    std::vector<Candidate> candidates;
    // Passenger flows and top fares per station pair: the same for every candidate in a review.
    std::map<std::pair<std::string, std::string>, double> flow_memo;
    std::map<std::tuple<std::string, std::string, double, double>, double> berth_revenue_memo;
    const domain::ShipClassDefinition* best_class = nullptr;
    const domain::StationDefinition* best_yard = nullptr;
    double best_return = investment.hurdle_return_per_year;
    double best_rank = rank_by_profit ? 0.0 : investment.hurdle_return_per_year;
    Valuation best_valuation;
    int probes = 0;
    // Candidates are valued in order of their bound, so the early stop picks the same ship
    // as probing one by one. A candidate not probed yet in this review stops the scan: the
    // next batch is probed (this tick) and the purchase waits for a later tick.
    static constexpr std::size_t kProbeBatch = 2;
    // A cabin design is valued from its bare hull's probe (its runs are tagged with the cabin
    // module); every other design is probed on its own.
    const bool shared_cabin_probe = sim_.passengers_on();

    static bool trace_on() {
        static const bool trace = std::getenv("SPACETRAINS_TRACE_INVESTMENT") != nullptr;
        return trace;
    }
    double richest_spendable() const {
        double richest_treasury = 0.0;
        for (const auto& [faction_id, balance] : sim_.faction_treasuries_) {
            richest_treasury = std::max(richest_treasury, sim_.faction_spendable(faction_id));
        }
        return richest_treasury;
    }
    domain::ShipState new_ship(const domain::ShipClassDefinition& ship_class, const domain::StationDefinition& yard) const {
        domain::ShipState ship {
            .id = "probe",
            .name = "probe",
            .faction_id = yard.faction_id,
            .class_id = ship_class.base_class_id.empty() ? ship_class.id : ship_class.base_class_id,
            .fit = ship_class.fit,
            .home_station_id = yard.id,
            .current_station_id = yard.id,
            .phase = domain::ShipMissionPhase::Idle,
            .propellant_kg = 0.0,
            .credits = investment.working_capital,
            .lifetime_profit = 0.0,
            .active_mission = {},
            .provisions = {},
            .ledger = {},
            .next_review_s = sim_.game_time_s_,
            .idle_since_s = sim_.game_time_s_,
            .refit_class_id = {},
            .refit_done_s = 0.0,
            .next_refit_review_s = 0.0,
            .laid_up_since_s = 0.0,
            .commissioned_s = sim_.game_time_s_,
            .route_destination_id = {},
            .route_commodity_id = {},
            .route_units_per_day = 0.0,
            .route_until_s = 0.0,
        };
        // New ships leave the yard with their full life-support endurance, like the starting fleet.
        for (const auto& [commodity_id, units_per_crew_day] : sim_.universe_.ship_operations.life_support_units_per_crew_day) {
            ship.provisions[commodity_id] = ship_class.crew_size * units_per_crew_day * PROVISION_ENDURANCE_DAYS;
        }
        return ship;
    }
    // A depot's fuel is also bought by ships.
    double consumption_rate(const domain::StationDefinition& station, const std::string& commodity_id) const {
        const auto& rates = sim_.economy_.get_station_net_rates(station);
        const auto it = rates.find(commodity_id);
        const double ship_demand = commodity_id == economy::FUEL_ID
            ? sim_.get_station_state(station.id).ship_fuel_units_per_day : 0.0;
        return std::max(0.0, (it == rates.end() ? 0.0 : -it->second) + ship_demand);
    }
    double flow_per_day(const std::string& from, const std::string& to) {
        const auto key = std::make_pair(from, to);
        if (const auto it = flow_memo.find(key); it != flow_memo.end()) {
            return it->second;
        }
        return flow_memo.emplace(key, sim_.passenger_flow_per_day(from, to)).first->second;
    }
    double berth_revenue(const std::string& from, const std::string& to, double days, double window_days,
        double berths) {
        const auto key = std::make_tuple(from, to, berths, days);
        if (const auto it = berth_revenue_memo.find(key); it != berth_revenue_memo.end()) {
            return it->second;
        }
        return berth_revenue_memo.emplace(key, sim_.expected_passenger_revenue(from, to, days, window_days, berths, 1.0).fares_cr)
            .first->second;
    }
    static bool cabin_fit(const domain::ShipClassDefinition& ship_class) {
        return !ship_class.fit.empty();
    }
    // Ships still committed to a route deliver to it; only the rest of the destination's
    // demand is open to a new ship.
    // Ships still in the yard have delivered nothing yet, so the observed imports miss them.
    double committed_flow(const std::string& destination_id, const std::string& commodity_id, bool in_yard) const {
        double flow = 0.0;
        for (const auto& ship : sim_.ships_) {
            if (ship.route_destination_id == destination_id && ship.route_commodity_id == commodity_id
                && sim_.game_time_s_ < ship.route_until_s
                && in_yard == (sim_.game_time_s_ < ship.commissioned_s + investment.build_days * 86400.0)) {
                flow += ship.route_units_per_day;
            }
        }
        return flow;
    }
    static double flow_of(const domain::Inventory& flows, const std::string& commodity_id) {
        const auto it = flows.find(commodity_id);
        return it == flows.end() ? 0.0 : it->second;
    }
    // A cargo run is valued over the route commitment as the ship would fly it: a hold out
    // every round trip, bought and sold along the price curves of the stocks both stations
    // would hold by then. The yard keeps making the good and other ships keep taking their
    // share of it; the destination keeps consuming it and other ships (at least those committed
    // to the route) keep delivering it. A route already served thus sells near the base
    // price, not at today's scarcity price.
    double sustained_profit_per_day(const domain::ShipClassDefinition& ship_class, const domain::StationDefinition& yard,
        const MissionChoice::CargoOption& option, double& units_per_day) const {
        const auto& commodity_id = option.commodity_id;
        const auto& commodity = sim_.get_commodity(commodity_id);
        const auto& destination = *option.destination;
        const auto& yard_state = sim_.get_station_state(yard.id);
        const auto& destination_state = sim_.get_station_state(destination.id);
        const auto& yard_rates = sim_.economy_.get_station_net_rates(yard);
        const auto& destination_rates = sim_.economy_.get_station_net_rates(destination);
        const bool fuel = commodity_id == economy::FUEL_ID;

        const double yard_rate = yard_rates.contains(commodity_id) ? yard_rates.at(commodity_id) : 0.0;
        const double yard_drift = yard_rate - flow_of(yard_state.export_units_per_day, commodity_id)
            - (fuel ? yard_state.ship_fuel_units_per_day : 0.0);
        const double yard_reserve = 8.0 + std::max(0.0, yard_rate) * 7.0;
        double yard_stock = flow_of(yard_state.inventory, commodity_id);
        const double yard_cap = std::max(yard_stock, sim_.economy_.production_cap_units(yard, commodity_id, yard_rate));
        const double destination_drift
            = (destination_rates.contains(commodity_id) ? destination_rates.at(commodity_id) : 0.0)
            - (fuel ? destination_state.ship_fuel_units_per_day : 0.0)
            + std::max(flow_of(destination_state.import_units_per_day, commodity_id),
                committed_flow(destination.id, commodity_id, false))
            + committed_flow(destination.id, commodity_id, true);
        double destination_stock = flow_of(destination_state.inventory, commodity_id);
        // What a delivery onto that stock earns: under the destination's order (at its price up to
        // the order-up-to level, the extra at a discount, the floor beyond), else along its curve.
        // A route sustained over the commitment sells at most at the destination's landed cost
        // (its reference price): scarcity bids do not last once ships serve it (economy overhaul:
        // with Earth selling at its ask, margins are the carriage and estimates at scarcity
        // prices ran 15x the earned returns).
        // A station that keeps paying more than that (a science station far out, short for years)
        // is valued at what it has been paying: its going rate.
        double sustained_cap = sim_.economy_.reference_price(destination, commodity_id);
        if (const auto stats = destination_state.order_stats.find(commodity_id); stats != destination_state.order_stats.end()) {
            sustained_cap = std::max(sustained_cap, stats->second.going_rate);
        }
        const auto sale_value = [&](double units) {
            return std::min(units * sustained_cap, [&] {
            const auto order = destination_state.order_stats.find(commodity_id);
            if (!sim_.priced_order(destination_state, commodity_id)) {
                return sim_.economy_.get_trade_value(destination, commodity_id, destination_stock, units, commodity.base_price);
            }
            const auto levels = sim_.order_levels(destination_state, commodity_id, order->second);
            OrderQuote quote;
            quote.open_units = std::max(0.0, levels.order_up_to_units - destination_stock);
            quote.extra_units = std::max(0.0, levels.max_units - std::max(destination_stock, levels.order_up_to_units));
            // The bid on the projected stock, as a quote would make it.
            quote.price = sim_.order_bid(destination_state, commodity_id, levels, destination_stock).price;
            quote.extra_price = std::min(quote.price, order->second.going_rate) * (1.0 - sim_.universe_.orders.extra_discount);
            quote.floor_price = std::min(quote.extra_price,
                economy::PRICE_FLOOR_MULTIPLIER * sim_.economy_.reference_price(destination, commodity_id));
            return sim_.quote_split(quote, 0.0, units).total;
            }());
        };
        const auto advance = [&](double days) {
            yard_stock = std::clamp(yard_stock + yard_drift * days, 0.0, yard_cap);
            destination_stock = std::max(0.0, destination_stock + destination_drift * days);
        };

        // Whole round trips over the commitment, at least one: a run longer than the
        // commitment earns its margin over its own cycle.
        // travel_days includes the wait for the launch window: a cycle is the wait plus the
        // transfer out and back (until 2026-10-08 the wait was counted three times).
        const double transfer_days = std::max(0.0, option.travel_days - option.wait_days);
        const double cycle_days = std::max(1.0, 2.0 * transfer_days + option.wait_days);
        const double cycles = std::max(1.0, std::floor(investment.route_commitment_days / cycle_days));
        const double window_days = cycles * cycle_days;
        const double leg_days = std::min(cycle_days, option.travel_days);
        advance(investment.build_days);
        double margin = 0.0;
        double delivered = 0.0;
        for (int cycle = 0; cycle < static_cast<int>(cycles); ++cycle) {
            const double units = std::min(option.cargo_units, yard_stock - yard_reserve);
            if (units <= 1.0) {
                advance(cycle_days);
                continue;
            }
            // The empty way back is lighter but is often fuelled at the dearer port: count it
            // as a second loaded leg.
            const double cost = sim_.economy_.get_trade_value(yard, commodity_id, yard_stock, -units, commodity.base_price)
                + 2.0 * option.fuel_cost;
            yard_stock -= units;
            advance(leg_days);
            const double surviving = units * std::pow(1.0 - sim_.cargo_decay_per_day(ship_class, commodity_id), option.travel_days);
            margin += sale_value(surviving) - cost;
            destination_stock += surviving;
            delivered += surviving;
            advance(cycle_days - leg_days);
        }
        units_per_day = delivered / window_days;
        return margin / window_days;
    }
    // Passengers (docs/plans/passengers.md): a shuttle between the yard and a station, full berths
    // at most each way, carrying the open part of the travellers' flow (what ships committed to
    // the route already carry is taken), at the reference fare adjusted for this ship's days.
    double sustained_passenger_profit_per_day(const domain::ShipClassDefinition& ship_class,
        const domain::StationDefinition& yard, const MissionChoice::CargoOption& option, double& units_per_day) {
        const auto& destination = *option.destination;
        const auto* back_fare = sim_.reference_fare(destination.id, yard.id);
        const double transfer_days = std::max(0.0, option.travel_days - option.wait_days);
        const double cycle_days = std::max(1.0, 2.0 * transfer_days + option.wait_days);
        double provisions_per_day = 0.0;
        for (const auto& [commodity_id, units] : sim_.universe_.ship_operations.life_support_units_per_crew_day) {
            provisions_per_day += units * sim_.station_price(sim_.get_station_state(yard.id), commodity_id);
        }
        // Taken: what ships flying the route carry now (cabin hulls and liners alike, committed or
        // not) plus ships still in the yard, and at least what the committed ships carry.
        const double committed_in_yard = committed_flow(destination.id, PASSENGER_GOOD, true);
        const double committed = committed_flow(destination.id, PASSENGER_GOOD, false) + committed_in_yard;
        const auto open_share = [&](const std::string& from, const std::string& to) {
            const double flow = flow_per_day(from, to);
            const double taken = std::max(committed, sim_.carried_passengers_per_day(from, to) + committed_in_yard);
            return flow > 0.0 ? std::max(0.0, flow - taken) / flow : 0.0;
        };
        const auto outbound = sim_.expected_passenger_revenue(yard.id, destination.id, option.travel_days, cycle_days,
            option.cargo_units, open_share(yard.id, destination.id));
        const auto inbound = back_fare == nullptr ? PassengerRevenue {}
            : sim_.expected_passenger_revenue(destination.id, yard.id, transfer_days, cycle_days, option.cargo_units,
                open_share(destination.id, yard.id));
        // The way back is fuelled at the destination at its price (Luna makes fuel, Earth buys it
        // dear), unless the ship carries that fuel from the yard.
        const double return_fuel_cost = option.return_fuel_kg < 0.0 ? option.fuel_cost
            : option.return_fuel_kg / FUEL_UNITS_TO_KG
                * (sim_.port_fuel_price(destination.id) + sim_.wear_cr_per_propellant_kg(ship_class) * FUEL_UNITS_TO_KG);
        const double margin = outbound.fares_cr - outbound.people * provisions_per_day * option.travel_days
            + inbound.fares_cr - inbound.people * provisions_per_day * transfer_days
            - option.fuel_cost - return_fuel_cost;
        const double out = outbound.people;
        units_per_day = out / cycle_days;
        return margin / cycle_days;
    }
    Valuation value_candidate(const domain::ShipClassDefinition& ship_class, const domain::StationDefinition& yard,
        double return_bound, const MissionChoice& choice) {
        const double running_cost = sim_.daily_capital_cost(ship_class) + sim_.daily_crew_cost(ship_class, sim_.get_station_state(yard.id))
            + sim_.hazard_cr_per_day(ship_class, yard, yard, sim_.game_time_s_) + sim_.wear_cr_per_flight_day(ship_class);
        Valuation valuation;
        valuation.label = "no cargo run";
        // A single-good run is valued alone; the lots of a mixed hold together (each along its
        // own good's stocks), less the running costs once.
        struct RunValue {
            double profit_per_day {0.0};
            std::string label;
            const MissionChoice::CargoOption* main_lot {nullptr};
            double main_units_per_day {0.0};
        };
        std::map<int, RunValue> mixed_runs;
        // A single lot's label is written only when it leads.
        const auto lot_label = [](const MissionChoice::CargoOption& option, double units_per_day) {
            return option.commodity_id == PASSENGER_GOOD
                ? std::format("{:.0f} berths -> {} in {:.1f} d, {:.2f} passengers/d", option.cargo_units,
                    option.destination->name, option.travel_days, units_per_day)
                : std::format("{:.0f}u {} -> {} in {:.1f} d, {:.2f} u/d", option.cargo_units,
                    option.commodity_id, option.destination->name, option.travel_days, units_per_day);
        };
        const auto consider = [&](const RunValue& run) {
            const double annual_return = (run.profit_per_day - running_cost) * 365.0 / ship_class.ship_value_cr;
            if (annual_return > valuation.annual_return) {
                valuation.annual_return = annual_return;
                valuation.label = run.label.empty() ? lot_label(*run.main_lot, run.main_units_per_day) : run.label;
                valuation.destination_id = run.main_lot->destination->id;
                valuation.commodity_id = run.main_lot->commodity_id;
                valuation.units_per_day = run.main_units_per_day;
            }
        };
        for (const auto& option : choice.cargo_options) {
            double units_per_day = 0.0;
            if (option.commodity_id == PASSENGER_GOOD) {
                const double profit_per_day = sustained_passenger_profit_per_day(ship_class, yard, option, units_per_day);
                consider({.profit_per_day = profit_per_day, .label = {}, .main_lot = &option, .main_units_per_day = units_per_day});
                continue;
            }
            if (cabin_fit(ship_class)) {
                continue;  // a cabin hull is valued for its berths (see the bound above)
            }
            const double profit_per_day = sustained_profit_per_day(ship_class, yard, option, units_per_day);
            if (option.run < 0) {
                consider({.profit_per_day = profit_per_day, .label = {}, .main_lot = &option, .main_units_per_day = units_per_day});
                continue;
            }
            auto& run = mixed_runs[option.run];
            run.profit_per_day += profit_per_day;
            run.label += (run.label.empty() ? "mixed: " : " + ") + lot_label(option, units_per_day);
            if (run.main_lot == nullptr || units_per_day > run.main_units_per_day) {
                run.main_lot = &option;
                run.main_units_per_day = units_per_day;
            }
        }
        for (const auto& [run_number, run] : mixed_runs) {
            consider(run);
        }
        if (trace) {
            std::cerr << std::format("[invest day {:.0f}] {} at {}: {} of {} runs: {:.0f}%/yr (bound {:.0f}%)\n",
                sim_.game_time_s_ / 86400.0, ship_class.id, yard.id, valuation.label, choice.cargo_options.size(),
                100.0 * valuation.annual_return, 100.0 * return_bound);
        }
        return valuation;
    }
    const domain::ShipClassDefinition& probe_class(const Candidate& candidate) const {
        return shared_cabin_probe && cabin_fit(*candidate.ship_class) ? sim_.get_ship_class(candidate.ship_class->base_class_id)
                                                                      : *candidate.ship_class;
    }
    std::string probe_key(const Candidate& candidate) const {
        return probe_class(candidate).id + "|" + candidate.yard->id;
    }
    std::string run_module(const Candidate& candidate) const {
        return shared_cabin_probe && cabin_fit(*candidate.ship_class) ? candidate.ship_class->fit.front() : std::string();
    }

    // Candidates: every hull in its standard tanks, built at every station; the winner's tank
    // variants are compared at the end. Dispatch's score is a one-shot rate (a 0.1-day hop with a one-off
    // price gap scores thousands of credits per day), so a candidate is valued by what its best
    // cargo run from the yard earns per day over the route commitment (sustained_profit_per_day),
    // minus its daily running costs.
    void list_candidates() {
        // Hulls in their standard tanks; with passengers also each hull with its cabin module (the
        // liners carry their berths built in).
        std::vector<const domain::ShipClassDefinition*> candidate_classes;
        for (const auto& ship_class : sim_.universe_.ship_classes) {
            if (!ship_class.hull_id.empty() && ship_class.hull_id != ship_class.id) {
                continue;
            }
            candidate_classes.push_back(&ship_class);
            if (sim_.passengers_on() && ship_class.berths <= 0.0) {
                for (const auto& module : sim_.universe_.ship_modules) {
                    if (module.hull_id == ship_class.id && module.slot == "cabin" && sim_.module_has_active_effect(module)) {
                        candidate_classes.push_back(&sim_.register_design(ship_class.id, {module.id}));
                    }
                }
            }
        }
        std::vector<IdleHull> idle_hulls;
        for (const auto& ship : sim_.ships_) {
            if (ship.phase == domain::ShipMissionPhase::Idle || ship.phase == domain::ShipMissionPhase::LaidUp) {
                const auto& design = sim_.ship_design(ship);
                idle_hulls.push_back({.station_id = &ship.current_station_id,
                    .hull_id = design.hull_id.empty() ? &design.id : &design.hull_id, .berths = design.berths});
            }
        }
        // A yard builds one ship at a time. (Routes already served are discounted by the
        // committed flow, so a yard may build again as soon as it is free.)
        for (const auto& yard : sim_.universe_.stations) {
            bool yard_busy = false;
            for (const auto& ship : sim_.ships_) {
                yard_busy = yard_busy
                    || (ship.home_station_id == yard.id && ship.commissioned_s > 0.0 && sim_.game_time_s_ < ship.refit_done_s
                        && ship.phase == domain::ShipMissionPhase::Refitting);
            }
            if (yard_busy) {
                continue;
            }
            // Margin per day each surplus good of the yard could make at its consumers' current
            // prices and consumption rates, a cheap upper bound on any ship's earnings.
            const auto& yard_state = sim_.get_station_state(yard.id);
            const auto& yard_rates = sim_.economy_.get_station_net_rates(yard);
            struct Outlet {
                double unit_margin;
                double rate;
            };
            std::vector<Outlet> outlets;
            for (const auto& [commodity_id, stock] : yard_state.inventory) {
                const auto rate_it = yard_rates.find(commodity_id);
                if (rate_it == yard_rates.end() || rate_it->second <= 0.0 || stock - (8.0 + rate_it->second * 7.0) <= 1.0) {
                    continue;
                }
                const double buy_price = sim_.station_price(yard_state, commodity_id);
                for (const auto& destination : sim_.universe_.stations) {
                    const double unit_margin = sim_.consumer_bid(sim_.get_station_state(destination.id), commodity_id) - buy_price;
                    const double rate = consumption_rate(destination, commodity_id);
                    if (unit_margin > 0.0 && rate > 0.0) {
                        outlets.push_back({unit_margin, rate});
                    }
                }
            }
            for (const auto* candidate_class : candidate_classes) {
                const auto& ship_class = *candidate_class;
                if (ship_class.ship_value_cr <= 0.0
                    || ship_class.ship_value_cr + investment.working_capital > richest_treasury
                    || (investment.max_fleet_hold_units > 0.0
                        && fleet_hold + ship_class.cargo_capacity_units > investment.max_fleet_hold_units)) {
                    continue;
                }
                // A ship of this hull docked idle or laid up here could already take the work.
                const auto& hull_id = ship_class.hull_id.empty() ? ship_class.id : ship_class.hull_id;
                const bool idle_here = std::any_of(idle_hulls.begin(), idle_hulls.end(), [&](const IdleHull& idle) {
                    return *idle.station_id == yard.id && *idle.hull_id == hull_id && idle.berths >= ship_class.berths;
                });
                if (idle_here) {
                    continue;
                }
                // A trip takes at least a day, so a ship moves at most a hold per day.
                double margin_bound = 0.0;
                for (const auto& outlet : outlets) {
                    margin_bound = std::max(margin_bound, outlet.unit_margin * std::min(outlet.rate, ship_class.cargo_capacity_units));
                }
                // Berths: a shuttle twice as fast as the reference carrier, its berths filled each way
                // as boarding fills them (the travellers expected over its round trip, by band, at
                // their fares or the clearing fare), before provisions and fuel. A passenger run is
                // valued on its own, so the bound is the larger of the cargo and passenger bounds.
                if (sim_.passengers_on() && ship_class.berths >= 1.0) {
                    double passenger_bound = 0.0;
                    for (const auto& destination : sim_.universe_.stations) {
                        const auto* out = sim_.reference_fare(yard.id, destination.id);
                        if (destination.id == yard.id || out == nullptr) {
                            continue;
                        }
                        const double days = 0.5 * out->days;
                        const double cycle_days = std::max(1.0, 2.0 * days);
                        double bound = berth_revenue(yard.id, destination.id, days, cycle_days, ship_class.berths);
                        if (sim_.reference_fare(destination.id, yard.id) != nullptr) {
                            bound += berth_revenue(destination.id, yard.id, days, cycle_days, ship_class.berths);
                        }
                        passenger_bound = std::max(passenger_bound, bound / cycle_days);
                    }
                    // A hull fitted with a cabin module is a candidate for its berths only: for cargo
                    // the bare hull is cheaper and lighter (and is a candidate itself).
                    margin_bound = cabin_fit(ship_class) ? passenger_bound : std::max(margin_bound, passenger_bound);
                }
                const double running_cost = sim_.daily_capital_cost(ship_class) + sim_.daily_crew_cost(ship_class, yard_state)
                    + sim_.hazard_cr_per_day(ship_class, yard, yard, sim_.game_time_s_) + sim_.wear_cr_per_flight_day(ship_class);
                const double return_bound = (margin_bound - running_cost) * 365.0 / ship_class.ship_value_cr;
                if (return_bound >= investment.hurdle_return_per_year) {
                    candidates.push_back({.ship_class = &ship_class, .yard = &yard, .return_bound = return_bound,
                        .rank_bound = rank_by_profit
                            ? (margin_bound - running_cost) / (per_hold_unit ? std::max(1.0, ship_class.cargo_capacity_units) : 1.0)
                            : return_bound});
                }
            }
        }
        // Probe the most promising first (plasma probes take seconds); stop once the best return
        // found beats every remaining bound. Sale prices are forecast at arrival, where a starving
        // consumer's price can be a little above today's, so the bound is a heuristic.
        std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.rank_bound > b.rank_bound; });
    }

    // Values the candidates already probed, best bound first. Returns true when it had to probe
    // the next batch instead (the purchase then waits for a later tick).
    bool probe_or_value_candidates() {
        std::vector<std::size_t> to_probe;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const auto& candidate = candidates[i];
            if (candidate.rank_bound <= best_rank) {
                break;
            }
            const auto probed = sim_.review_probes_.find(probe_key(candidate));
            if (probed == sim_.review_probes_.end()) {
                std::vector<std::string> batch_keys;
                for (std::size_t j = i; j < candidates.size() && to_probe.size() < kProbeBatch; ++j) {
                    const auto key = probe_key(candidates[j]);
                    if (!sim_.review_probes_.contains(key) && std::find(batch_keys.begin(), batch_keys.end(), key) == batch_keys.end()) {
                        to_probe.push_back(j);
                        batch_keys.push_back(key);
                    }
                }
                break;
            }
            const auto module_id = run_module(candidate);
            MissionChoice choice;
            for (const auto& run : probed->second) {
                if (run.module_id != module_id) {
                    continue;
                }
                choice.cargo_options.push_back({.destination = &sim_.get_station_definition(run.destination_id),
                    .commodity_id = run.commodity_id, .cargo_units = run.cargo_units, .travel_days = run.travel_days,
                    .wait_days = run.wait_days, .fuel_cost = run.fuel_cost, .run = run.run,
                    .return_fuel_kg = run.return_fuel_kg});
            }
            const auto valuation = value_candidate(*candidate.ship_class, *candidate.yard, candidate.return_bound, choice);
            ++probes;
            const double rank = rank_by_profit
                ? valuation.annual_return * candidate.ship_class->ship_value_cr / 365.0
                    / (per_hold_unit ? std::max(1.0, candidate.ship_class->cargo_capacity_units) : 1.0)
                : valuation.annual_return;
            if (valuation.annual_return > investment.hurdle_return_per_year && rank > best_rank) {
                best_rank = rank;
                best_return = valuation.annual_return;
                best_class = candidate.ship_class;
                best_yard = candidate.yard;
                best_valuation = valuation;
            }
        }
        if (!to_probe.empty()) {
            std::vector<domain::ShipState> probe_ships;
            probe_ships.reserve(to_probe.size());
            std::vector<MissionRequest> requests;
            for (const auto index : to_probe) {
                const auto& probed_class = probe_class(candidates[index]);
                probe_ships.push_back(new_ship(probed_class, *candidates[index].yard));
                requests.push_back({.ship = &probe_ships.back(), .ship_class = &probed_class,
                    .earliest_departure_s = sim_.game_time_s_ + investment.build_days * 86400.0, .cargo_only = true});
            }
            const auto choices = sim_.choose_missions(requests);
            for (std::size_t k = 0; k < to_probe.size(); ++k) {
                auto& runs = sim_.review_probes_[probe_key(candidates[to_probe[k]])];
                for (const auto& option : choices[k].cargo_options) {
                    runs.push_back({.destination_id = option.destination->id, .commodity_id = option.commodity_id,
                        .cargo_units = option.cargo_units, .travel_days = option.travel_days,
                        .wait_days = option.wait_days, .fuel_cost = option.fuel_cost, .run = option.run,
                        .module_id = option.module_id, .return_fuel_kg = option.return_fuel_kg});
                }
            }
            if (trace) {
                std::string names;
                for (const auto index : to_probe) {
                    names += std::format(" {}@{}", probe_class(candidates[index]).id, candidates[index].yard->id);
                }
                std::cerr << std::format("[invest day {:.1f}] probed {} candidates in {:.2f} s:{}\n", sim_.game_time_s_ / 86400.0,
                    to_probe.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - review_start).count(), names);
            }
            return true;
        }
        return false;
    }

    // The winning hull is built with the tanks that suit it best, judged like a refit (by
    // dispatch's full score, which carries each variant's capital charge); otherwise it would
    // go straight back into the yard.
    // (A hull bought for its cabin module keeps it.)
    void choose_tanks() {
        if (best_class != nullptr && best_class->fit.empty()) {
            const auto* hull = best_class;
            std::vector<const domain::ShipClassDefinition*> variants;
            std::vector<domain::ShipState> variant_ships;
            for (const auto& variant : sim_.universe_.ship_classes) {
                if ((variant.hull_id.empty() ? variant.id : variant.hull_id) == hull->id
                    && variant.ship_value_cr + investment.working_capital <= richest_treasury) {
                    variants.push_back(&variant);
                }
            }
            // And the standard tanks with each module that does something (tier 2).
            for (const auto& module : sim_.universe_.ship_modules) {
                if (module.hull_id == hull->id && sim_.module_has_active_effect(module)) {
                    const auto& fitted = sim_.register_design(hull->id, {module.id});
                    if (fitted.ship_value_cr + investment.working_capital <= richest_treasury) {
                        variants.push_back(&fitted);
                    }
                }
            }
            variant_ships.reserve(variants.size());
            std::vector<MissionRequest> requests;
            for (const auto* variant : variants) {
                variant_ships.push_back(new_ship(*variant, *best_yard));
                requests.push_back({.ship = &variant_ships.back(), .ship_class = variant,
                    .earliest_departure_s = sim_.game_time_s_ + investment.build_days * 86400.0, .cargo_only = false});
            }
            const auto choices = sim_.choose_missions(requests);
            double best_score = -std::numeric_limits<double>::infinity();
            for (std::size_t i = 0; i < variants.size(); ++i) {
                ++probes;
                if (choices[i].kind == MissionChoice::Kind::Mission && choices[i].score > best_score) {
                    best_score = choices[i].score;
                    best_class = variants[i];
                }
            }
        }
    }

    void buy() {
        // The yard's own faction invests if it can pay for the ship and its working capital (with
        // what it may borrow), otherwise the faction that can spend most (which can: candidates
        // are priced against it).
        const double price = best_class->ship_value_cr + investment.working_capital;
        std::string investor = best_yard->faction_id;
        if (sim_.faction_spendable(investor) < price) {
            for (const auto& [faction_id, balance] : sim_.faction_treasuries_) {
                if (sim_.faction_spendable(faction_id) >= sim_.faction_spendable(investor)) {
                    investor = faction_id;
                }
            }
        }
        // The treasury buys the hull from the outside economy and gives the ship its working
        // capital, which raises the money supply the controller holds.
        sim_.faction_treasuries_[investor] -= price;
        // A yard in space builds it (economy overhaul): its share of the price goes there, the
        // parts made on Earth to Earth's economy.
        double earth_part = best_class->ship_value_cr;
        if (const auto* yard = sim_.households_on() ? sim_.yard_station_for(*best_yard) : nullptr; yard != nullptr) {
            double yard_part = sim_.universe_.households.yard_share * best_class->ship_value_cr;
            auto& yard_state = sim_.get_station_state(yard->id);
            // The yard builds the hull from goods worth yard_materials_share of the price (metals half,
            // machinery 30%, electronics 20%, at its reference prices), out of its stock; what it lacks
            // comes from Earth, and that part of the price goes there.
            const double materials = sim_.universe_.households.yard_materials_share * best_class->ship_value_cr;
            for (const auto& [good, share] : std::initializer_list<std::pair<const char*, double>> {
                     {"metals", 0.5}, {"machinery", 0.3}, {"electronics", 0.2}}) {
                const double price = sim_.economy_.reference_price(*yard, good);
                if (price <= 0.0) {
                    continue;
                }
                const double wanted = share * materials / price;
                double& stock = yard_state.inventory[good];
                const double used = std::clamp(stock, 0.0, wanted);
                stock -= used;
                yard_state.demand_units[good] += used;
                yard_part -= (wanted - used) * price;
            }
            yard_part = std::max(0.0, yard_part);
            yard_state.credits += yard_part;
            yard_state.ledger.yard_sales += yard_part;
            sim_.journal_.record(sim_.account_faction(investor), sim_.account_station(yard_state), MoneyCategory::HullYard, yard_part);
            earth_part -= yard_part;
        }
        sim_.outside_economy_credits_ += earth_part;
        sim_.journal_.record(sim_.account_faction(investor), sim_.account_earth(), MoneyCategory::HullEarth, earth_part);
        sim_.journal_.record(sim_.account_faction(investor), sim_.account_fleet(investor), MoneyCategory::WorkingCapital, investment.working_capital);
        sim_.seeded_money_supply_ += investment.working_capital;
        sim_.investment_ledger_.hulls_bought += best_class->ship_value_cr;
        sim_.investment_ledger_.working_capital += investment.working_capital;
        const int number = ++sim_.investment_ledger_.ships_commissioned;

        std::string initials;
        for (const auto& faction : sim_.universe_.factions) {
            if (faction.id != investor) {
                continue;
            }
            bool word_start = true;
            for (const char ch : faction.name) {
                if (word_start && ch != ' ') {
                    initials += ch;
                }
                word_start = ch == ' ';
            }
        }
        auto ship = new_ship(*best_class, *best_yard);
        ship.id = std::format("commissioned_{:03d}", number);
        ship.name = std::format("{} {} {}", initials, sim_.get_ship_class(best_class->hull_id.empty() ? best_class->id : best_class->hull_id).name, number);
        ship.faction_id = investor;
        // Built in the home yard: the refit machinery launches it when the build is done.
        ship.phase = domain::ShipMissionPhase::Refitting;
        ship.refit_class_id = ship.class_id;
        ship.refit_fit = ship.fit;
        ship.refit_done_s = sim_.game_time_s_ + investment.build_days * 86400.0;
        // Like a refit, the tank choice holds for the payback period before it is reconsidered.
        ship.next_refit_review_s = ship.refit_done_s + REFIT_PAYBACK_DAYS * 86400.0;
        // It works the route it was bought for until the commitment ends.
        ship.route_destination_id = best_valuation.destination_id;
        ship.route_commodity_id = best_valuation.commodity_id;
        ship.route_units_per_day = best_valuation.units_per_day;
        ship.route_until_s = ship.refit_done_s + investment.route_commitment_days * 86400.0;
        sim_.add_event(std::format("{} ordered at {} for {:.0f} cr: {:.0f}%/yr expected ({})",
            ship.name, best_yard->name, best_class->ship_value_cr, 100.0 * best_return, best_valuation.label), "mission");
        sim_.ships_.push_back(std::move(ship));
    }
};

Simulation::CommissionStep Simulation::commission_step() {
    return InvestmentReview(*this).run();
}

}  // namespace spacetrains::simulation
