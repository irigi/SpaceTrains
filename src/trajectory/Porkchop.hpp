// Porkchop plots (docs/modules/trajectory.md, "Porkchop plots"): for a pair of planets (stations
// around one planet have no launch windows), departure date against flight time, each cell
//   - the nuclear-thermal Δv, the same Lambert cell the Kepler planner flies, and
//   - the variable-Isp effort J = ∫a²dt of the minimum-effort rendezvous (GeneralTransfer), which
//     is the same for every ship: a class's propellant follows from 1/m_f − 1/m0 = J/(2P).
//
// Cheap by construction:
//   - cells are geometry only: one grid per planet pair serves every class, payload and fuel load;
//   - departures sit on a fixed grid (multiples of a step per pair) and flight times on a fixed
//     axis, so a column is a pure function of (pair, departure index): computed once, kept while it
//     is in the window, and as game time moves on only the new columns at the far edge are solved;
//   - only requested pairs are computed, on one low-priority background thread that never touches
//     the simulation (its own copy of the celestial mechanics);
//   - variable-Isp cells along a column are warm-started from the one below.
// Display only: nothing in the simulation reads it.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "celestial/CelestialMechanics.hpp"
#include "domain/Definitions.hpp"
#include "trajectory/TrajectoryPlanner.hpp"

namespace spacetrains::trajectory {

// The fixed axes of a pair's grid: departures at multiples of step_s, flight times transit_s.
struct PorkchopAxes {
    double step_s {0.0};
    double hohmann_s {0.0};
    double synodic_s {0.0};
    double span_s {0.0};            // the window shown: min(synodic period, 730 days)
    std::vector<double> transit_s;  // rows, shortest first
};

// One departure: per row, Δv in m/s and the effort in normalised units (μ = 1, length 1 AU; the
// atlas's κ scale). NaN where there is no solution (no arc, or one passing under the perihelion limit).
struct PorkchopColumn {
    long long index {0};            // departure = index × step_s
    std::vector<float> dv_mps;
    std::vector<float> effort;
    // Each variable-Isp cell's solution (primer p0, w0; NaN when none): the warm start for the
    // same flight time one departure later.
    std::vector<std::array<double, 4>> primers;
};

class PorkchopService {
public:
    static constexpr int kColumns = 96;
    static constexpr int kRows = 48;

    explicit PorkchopService(const domain::UniverseDefinition& universe, bool start_worker = true);
    ~PorkchopService();
    PorkchopService(const PorkchopService&) = delete;
    PorkchopService& operator=(const PorkchopService&) = delete;

    // The pairs (origin primary, destination primary) the UI shows and the game time now. The
    // worker fills their windows [now, now + span), nearest departures first.
    void request(std::vector<std::pair<std::string, std::string>> pairs, double now_s);
    // The requested pairs' axes and the columns computed so far, the classes needed to turn cells
    // into propellant, and each station's planet. `version` changes whenever a column is added.
    [[nodiscard]] std::string json() const;
    [[nodiscard]] std::uint64_t version() const { return version_.load(); }

    // Pure pieces (tests, tools): a pair's axes and one column.
    [[nodiscard]] PorkchopAxes axes(const std::string& origin, const std::string& destination) const;
    // `left`: the column one departure earlier, if known (its solutions seed this one's).
    [[nodiscard]] PorkchopColumn compute_column(const std::string& origin, const std::string& destination,
        const PorkchopAxes& axes, long long index, const PorkchopColumn* left = nullptr) const;
    // Work the requested pairs until nothing is missing (tests; the worker does this in the background).
    void compute_all_now();

private:
    struct Grid {
        PorkchopAxes axes;
        std::map<long long, PorkchopColumn> columns;
    };
    // The next missing column of the requested pairs, nearest to now first; false when none.
    bool next_job(std::pair<std::string, std::string>& pair, PorkchopAxes& axes, long long& index) const;
    void worker_loop();

    const domain::UniverseDefinition& universe_;
    celestial::CelestialMechanics mechanics_;
    KeplerTrajectoryPlanner kepler_;
    double mu_sun_ {0.0};

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<std::pair<std::string, std::string>> requested_;
    double now_s_ {0.0};
    std::map<std::pair<std::string, std::string>, Grid> grids_;
    std::atomic<std::uint64_t> version_ {0};
    bool stop_ {false};
    std::thread worker_;
};

}  // namespace spacetrains::trajectory
