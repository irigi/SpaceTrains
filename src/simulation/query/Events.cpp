#include "simulation/query/Events.hpp"

#include <cstdlib>
#include <format>
#include <iostream>

#include "simulation/SimulationInternal.hpp"

namespace spacetrains::simulation::query {

using namespace detail;

void add_event(World& w, std::string text, std::string category) {
    // Debug aid: SPACETRAINS_TRACE_CONTRACTS=1 logs rotation-contract events (stderr).
    if (static const bool trace_contracts = std::getenv("SPACETRAINS_TRACE_CONTRACTS") != nullptr;
        trace_contracts && text.find("rotation contract") != std::string::npos) {
        std::cerr << std::format("[CONTRACT] day {:.1f} {}\n", w.state.game_time_s / 86400.0, text);
    }
    w.state.recent_events.push_back({.time_s = w.state.game_time_s, .text = std::move(text), .category = std::move(category)});
    if (w.state.recent_events.size() > MAX_EVENT_HISTORY) {
        w.state.recent_events.erase(w.state.recent_events.begin(), w.state.recent_events.begin() + static_cast<long>(w.state.recent_events.size() - MAX_EVENT_HISTORY));
    }
}

void record_trade(World& w, domain::TradeEntry trade) {
    w.state.recent_trades.push_back(std::move(trade));
    if (w.state.recent_trades.size() > MAX_TRADE_HISTORY) {
        w.state.recent_trades.erase(w.state.recent_trades.begin(), w.state.recent_trades.begin() + static_cast<long>(w.state.recent_trades.size() - MAX_TRADE_HISTORY));
    }
}

}  // namespace spacetrains::simulation::query
