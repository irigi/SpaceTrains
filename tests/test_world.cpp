// Tests of the systems on World, called directly rather than through Simulation's tick
// (docs/plans/simulation_split.md, "Tests to add"). One block per system.

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

#include "simulation/Simulation.hpp"
#include "simulation/query/Accounts.hpp"
#include "simulation/query/Lookups.hpp"
#include "simulation/systems/Statistics.hpp"

namespace {

using spacetrains::simulation::Simulation;
using spacetrains::simulation::World;
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

// A freshly started game from the data. Its World is sim.world(). Until step 15 builds a World
// on its own, the Simulation owns it, so tests keep the Simulation alive.
Simulation make_sim(const std::function<void(spacetrains::domain::UniverseDefinition&)>& adjust = {}) {
    return adjust ? Simulation::from_data_root("data", adjust) : Simulation::from_data_root("data");
}

// Statistics (step 7): a hand-posted transfer lands in the right accounts, and the monthly close
// samples the balances and takes the period's flows with it.
void test_statistics() {
    using spacetrains::simulation::MoneyCategory;
    auto sim = make_sim();
    World& w = sim.world();
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

}  // namespace

int main() {
    try {
        test_statistics();
    } catch (const std::exception& ex) {
        std::cerr << "FAILED: " << ex.what() << '\n';
        return 1;
    }
    std::cout << "All world tests passed.\n";
    return 0;
}
