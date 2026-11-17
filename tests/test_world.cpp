// Tests of the systems on World, called directly rather than through Simulation's tick
// (docs/plans/simulation_split.md, "Tests to add"). One block per system.

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

#include "simulation/Simulation.hpp"
#include "simulation/WorldSetup.hpp"
#include "simulation/query/Accounts.hpp"
#include "simulation/query/Lookups.hpp"
#include "simulation/query/Positions.hpp"
#include "simulation/systems/Patrols.hpp"
#include "simulation/systems/Statistics.hpp"

namespace {

using spacetrains::simulation::Simulation;
using spacetrains::simulation::World;
using spacetrains::simulation::make_world;
namespace domain = spacetrains::domain;
namespace query = spacetrains::simulation::query;
namespace systems = spacetrains::simulation::systems;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_near(double actual, double expected, double tolerance, const std::string& message) {
    if (std::abs(actual - expected) > tolerance) {
        throw std::runtime_error(message + " (got " + std::to_string(actual) + ", expected " + std::to_string(expected) + ")");
    }
}

// A freshly started game from the data (make_world, the code from_data_root runs). The patrols
// test still advances time through a Simulation (until step 15b gives World its own step).
Simulation make_sim(const std::function<void(spacetrains::domain::UniverseDefinition&)>& adjust = {}) {
    return adjust ? Simulation::from_data_root("data", adjust) : Simulation::from_data_root("data");
}

// Statistics (step 7): a hand-posted transfer lands in the right accounts, and the monthly close
// samples the balances and takes the period's flows with it.
void test_statistics() {
    using spacetrains::simulation::MoneyCategory;
    const auto world = make_world("data");
    World& w = *world;
    auto& station = query::get_station_state(w, "earth_orbit");
    const int payer = query::account_station(w, station);
    const int payee = query::account_households(w, station);
    const auto wages = static_cast<std::size_t>(MoneyCategory::Wages);
    const double payer_before = systems::account_balance(w, payer);
    const double payee_before = systems::account_balance(w, payee);
    require(w.journal_.period(payer)[wages].out == 0.0 && w.journal_.period(payee)[wages].in == 0.0,
        "a new game has paid no wages yet");

    // The station pays its residents 1,234 cr, as Households does: move the money, then book it.
    constexpr double amount = 1234.0;
    station.credits -= amount;
    station.household_credits += amount;
    w.journal_.record(payer, payee, MoneyCategory::Wages, amount);
    require_near(systems::account_balance(w, payer), payer_before - amount, 1e-9, "the station's balance falls");
    require_near(systems::account_balance(w, payee), payee_before + amount, 1e-9, "the residents' balance rises");
    require_near(w.journal_.period(payer)[wages].out, amount, 1e-9, "the period shows the station paying wages");
    require_near(w.journal_.period(payee)[wages].in, amount, 1e-9, "the period shows the residents receiving wages");
    for (const int account : {payer, payee}) {
        require_near(w.journal_.journal_balance(account), systems::account_balance(w, account), 1e-6,
            "the journal accounts for the whole balance");
    }

    // The monthly close: one sample, at the scheduled day, with today's balances and the flows.
    const double close_day = w.journal_.next_close_day();
    systems::close_journal_period(w);
    const auto& history = w.journal_.history();
    require(history.size() == 1, "the close appends one sample");
    const auto& sample = history.back();
    require_near(sample.day, close_day, 0.0, "the sample is dated at the scheduled close");
    require(sample.balances.size() == w.journal_.accounts().size(), "the sample has a balance per account");
    require_near(sample.balances[static_cast<std::size_t>(payer)], payer_before - amount, 1e-9, "the sample has the station's balance");
    require_near(sample.balances[static_cast<std::size_t>(payee)], payee_before + amount, 1e-9, "the sample has the residents' balance");
    require_near(sample.flows[static_cast<std::size_t>(payer)][wages].out, amount, 1e-9, "the sample keeps the period's wages");
    require(sample.stations.size() == w.stations_.size(), "the sample has a row per station");
    require(w.journal_.period(payer)[wages].out == 0.0, "the next period starts empty");
    require_near(w.journal_.next_close_day(), close_day + spacetrains::simulation::MoneyJournal::kPeriodDays, 0.0,
        "the next close is a period later");
    require(w.flows_.history().size() == 1, "the flow record closes its month with the journal's");

    // The statistics JSON shows the sample.
    const std::string json = systems::build_stats_json(w);
    require(json.starts_with("{\"day\":") && json.ends_with("}"), "the statistics JSON is one object");
    require(json.find("\"" + w.journal_.accounts()[static_cast<std::size_t>(payer)].key() + "\"") != std::string::npos,
        "the statistics JSON lists the station's account");
}

// Patrols (step 8): one cutter and a ship in flight to inspect; the sortie is planned (a path
// to a meeting before the ship arrives) and its day's costs are paid by Earth to the home port.
void test_patrols() {
    using spacetrains::simulation::MoneyCategory;
    using spacetrains::domain::PatrolPhase;
    using spacetrains::domain::ShipMissionPhase;
    auto sim = make_sim([](spacetrains::domain::UniverseDefinition& universe) {
        universe.patrols.patrols = true;
        universe.patrols.bases = {{.faction_id = "earth", .station_id = "earth_orbit", .ships = 1}};
    });
    World& w = sim.world();
    require(w.patrols_.size() == 1, "one base with one cutter gives one cutter");
    require(w.patrols_.front().phase == PatrolPhase::Docked && w.patrols_.front().propellant_kg > 0.0,
        "the cutter starts docked and fuelled");

    // Let the game run with the cutter kept in port, until a ship is on a leg it may inspect.
    constexpr double day_s = 86400.0;
    const auto& p = w.universe_.patrols;
    const auto inspectable = [&] {
        for (const auto& ship : w.ships_) {
            const auto& mission = ship.active_mission;
            if (ship.phase == ShipMissionPhase::InTransit && mission.sampled_path.size() >= 2
                && mission.arrival_time_s - w.game_time_s_ > (p.inspection_days + 10.0) * day_s
                && !query::local_leg(w, mission)) {
                return true;
            }
        }
        return false;
    };
    sim.set_timewarp(day_s);
    for (int day = 0; day < 120 && !inspectable(); ++day) {
        w.patrols_.front().next_review_s = w.game_time_s_ + 1000.0 * day_s;
        sim.step(1.0);
    }
    auto& cutter = w.patrols_.front();  // taken after the run, so no reference is held across ticks
    require(inspectable(), "some ship flies an interplanetary leg within 120 days");
    require(cutter.phase == PatrolPhase::Docked && w.patrol_audit_.sorties == 0, "the cutter stayed in port");

    // The review falls due: one step of the system plans and starts a sortie.
    const auto& home = query::get_station_state(w, cutter.home_station_id);
    const int earth = query::account_earth();
    const int port = query::account_station(w, home);
    const auto patrols = static_cast<std::size_t>(MoneyCategory::Patrols);
    const double paid_before = w.journal_.period(earth)[patrols].out;
    const double port_before = w.journal_.period(port)[patrols].in;
    const double costs_before = w.patrol_audit_.costs_cr;
    const double port_credits_before = home.credits;
    cutter.next_review_s = w.game_time_s_;
    systems::step_patrols(w, 0.25 * day_s);

    require(w.patrol_audit_.sorties == 1, "the review starts one sortie");
    require(cutter.phase == PatrolPhase::Outbound, "the cutter sets out");
    require(!cutter.target_ship_id.empty() && !cutter.target_name.empty(), "the cutter has a target");
    const domain::ShipState* target = nullptr;
    for (const auto& ship : w.ships_) {
        if (ship.id == cutter.target_ship_id) {
            target = &ship;
        }
    }
    require(target != nullptr && target->phase == ShipMissionPhase::InTransit, "the target is a ship in flight");
    require(cutter.meet_s > w.game_time_s_ && cutter.meet_s < target->active_mission.arrival_time_s,
        "the meeting is before the target arrives");
    require(cutter.path.size() >= 2 && cutter.path.size() == cutter.times.size()
            && cutter.times.size() == cutter.propellant.size(),
        "the sortie has a path with times and propellant");
    require(cutter.propellant.back() < cutter.propellant.front(), "the way out burns propellant");

    const double paid = w.patrol_audit_.costs_cr - costs_before;
    require(paid > 0.0, "the cutter's hull and crew are paid for the step");
    require_near(w.journal_.period(earth)[patrols].out - paid_before, paid, 1e-6, "Earth pays its police");
    require_near(w.journal_.period(port)[patrols].in - port_before, paid, 1e-6, "the home port is paid");
    require_near(home.credits - port_credits_before, paid, 1e-6, "the home port's credits rise by the payment");

    // Where the cutter is shown: at its departure point now, on its path later.
    const auto here = systems::patrol_position(w, cutter);
    require((here - cutter.path.front()).length() < 1.0e3, "the cutter is shown where its path starts");
}

}  // namespace

int main() {
    try {
        test_statistics();
        test_patrols();
    } catch (const std::exception& ex) {
        std::cerr << "FAILED: " << ex.what() << '\n';
        return 1;
    }
    std::cout << "All world tests passed.\n";
    return 0;
}
