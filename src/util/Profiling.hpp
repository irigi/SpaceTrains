#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace spacetrains::profiling {

// Wall-time accounting for the expensive parts of a simulation step. Off by default;
// `spacetrains_headless --profile` or SPACETRAINS_PROFILE=1 turns it on. Phases nest
// (a plan inside a mission choice), so their times are inclusive, not additive.
enum class Phase : std::uint8_t {
    Step,
    EconomyStep,
    Treasuries,
    FleetInvestment,
    ShipReview,
    ChooseMission,
    ConsiderRefit,
    EstimateLeg,
    KeplerPlan,
    VariableIspPlan,
    VariableIspRefine,
    VariableIspIntegrate,
    VariableIspPath,
    Snapshot,
    Count,
};

bool enabled();
void set_enabled(bool on);
void add(Phase phase, double seconds);
void reset();
// One line per phase: calls, total seconds, mean milliseconds, worst call.
std::string report();
double total_seconds(Phase phase);

class Scope {
public:
    explicit Scope(Phase phase) : phase_(phase), active_(enabled()) {
        if (active_) {
            start_ = std::chrono::steady_clock::now();
        }
    }
    ~Scope() {
        if (active_) {
            add(phase_, std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count());
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Phase phase_;
    bool active_;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace spacetrains::profiling
