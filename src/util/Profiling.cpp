#include "util/Profiling.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <format>
#include <mutex>

namespace spacetrains::profiling {

namespace {

struct Totals {
    double seconds {0.0};
    double worst_s {0.0};
    std::uint64_t calls {0};
};

constexpr std::array<const char*, static_cast<std::size_t>(Phase::Count)> kNames {
    "step",
    "  economy step",
    "  treasuries",
    "  fleet investment",
    "  ship review",
    "    choose_mission",
    "    consider_refit",
    "      estimate_leg",
    "      kepler plan",
    "        kepler path",
    "      variable-isp plan",
    "        refine_seed",
    "        integrate",
    "        path",
    "snapshot json",
};

std::atomic<bool> g_enabled {std::getenv("SPACETRAINS_PROFILE") != nullptr};
std::mutex g_mutex;
std::array<Totals, static_cast<std::size_t>(Phase::Count)> g_totals {};

}  // namespace

bool enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

void set_enabled(bool on) {
    g_enabled.store(on, std::memory_order_relaxed);
}

void add(Phase phase, double seconds) {
    const std::lock_guard lock(g_mutex);
    auto& totals = g_totals[static_cast<std::size_t>(phase)];
    totals.seconds += seconds;
    totals.worst_s = std::max(totals.worst_s, seconds);
    ++totals.calls;
}

void reset() {
    const std::lock_guard lock(g_mutex);
    g_totals = {};
}

double total_seconds(Phase phase) {
    const std::lock_guard lock(g_mutex);
    return g_totals[static_cast<std::size_t>(phase)].seconds;
}

std::string report() {
    const std::lock_guard lock(g_mutex);
    std::string text = std::format("{:<26}{:>10}{:>11}{:>11}{:>11}\n", "phase (inclusive)", "calls", "total s", "mean ms", "worst ms");
    for (std::size_t i = 0; i < g_totals.size(); ++i) {
        const auto& totals = g_totals[i];
        if (totals.calls == 0) {
            continue;
        }
        text += std::format("{:<26}{:>10}{:>11.2f}{:>11.3f}{:>11.1f}\n", kNames[i], totals.calls, totals.seconds,
            1000.0 * totals.seconds / static_cast<double>(totals.calls), 1000.0 * totals.worst_s);
    }
    return text;
}

}  // namespace spacetrains::profiling
