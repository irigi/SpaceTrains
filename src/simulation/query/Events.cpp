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
        std::cerr << std::format("[CONTRACT] day {:.1f} {}\n", w.state.game_time_s_ / 86400.0, text);
    }
    w.state.recent_events_.push_back({.time_s = w.state.game_time_s_, .text = std::move(text), .category = std::move(category)});
    if (w.state.recent_events_.size() > MAX_EVENT_HISTORY) {
        w.state.recent_events_.erase(w.state.recent_events_.begin(), w.state.recent_events_.begin() + static_cast<long>(w.state.recent_events_.size() - MAX_EVENT_HISTORY));
    }
}

void record_trade(World& w, domain::TradeEntry trade) {
    w.state.recent_trades_.push_back(std::move(trade));
    if (w.state.recent_trades_.size() > MAX_TRADE_HISTORY) {
        w.state.recent_trades_.erase(w.state.recent_trades_.begin(), w.state.recent_trades_.begin() + static_cast<long>(w.state.recent_trades_.size() - MAX_TRADE_HISTORY));
    }
}

}  // namespace spacetrains::simulation::query
