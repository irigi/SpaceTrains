// Porkchop plots (docs/modules/trajectory.md, "Porkchop plots"): for a pair of planets (stations
// around one planet have no launch windows), departure date against flight time, each cell
//   - the nuclear-thermal Δv, the same Lambert cell the Kepler planner flies, and
//   - the variable-Isp effort J = ∫a²dt of the minimum-effort rendezvous (GeneralTransfer), which
//     is the same for every ship: a class's propellant follows from 1/m_f − 1/m0 = J/(2P).
// The two drives have their own flight-time axes: thermal ships fly near the Hohmann time (the
// planner's 0.3-1.5 range), plasma ships from a tenth of it (the outer planets) to a few times it
// (Mercury and Venus), so their rows are spaced on a log scale over a wider range.
//
// Cheap by construction:
//   - cells are geometry only: one grid per planet pair serves every class, payload and fuel load;
//   - departures sit on a fixed grid (multiples of a step per pair) and flight times on fixed
//     axes, so a column is a pure function of (pair, departure index): computed once, kept, and
//     appended to a disk cache (every game starts on the same date, so later sessions reuse it);
//   - pairs on screen first, nearest departures first; when idle, the routes ships fly (from the
//     flow record) are filled once each, so switching to them is instant; pairs off screen are not
//     kept up to date (opening one again computes only the days that have passed);
//   - one low-priority background thread that never touches the simulation (its own copy of the
//     celestial mechanics); variable-Isp cells are warm-started from the one below, then cells
//     much dearer than the one above are solved again from it (a down pass).
// Display only: nothing in the simulation reads it.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "celestial/CelestialMechanics.hpp"
#include "domain/Definitions.hpp"
#include "trajectory/TrajectoryPlanner.hpp"

namespace spacetrains::trajectory {

// The fixed axes of a pair's grid: departures at multiples of step_s; flight times per drive.
struct PorkchopAxes {
    double step_s {0.0};
    double hohmann_s {0.0};
    double synodic_s {0.0};
    double span_s {0.0};                   // the window shown: min(synodic period, 730 days)
    std::vector<double> transit_s;         // nuclear-thermal rows, shortest first
    std::vector<double> plasma_transit_s;  // variable-Isp rows, shortest first (log spaced)
};

// One departure: per row, Δv in m/s (thermal rows) and the effort in normalised units (plasma rows;
// μ = 1, length 1 AU: the atlas's κ scale). NaN where there is no solution (no arc, or one passing
// under the perihelion limit).
struct PorkchopColumn {
    long long index {0};            // departure = index × step_s
    std::vector<float> dv_mps;
    std::vector<float> effort;
    // Each variable-Isp cell's solution (primer p0, w0; NaN when none): the warm start for the
    // same flight time one departure later. Dropped for columns well behind the present.
    std::vector<std::array<double, 4>> primers;
};

// A route ships fly (planet to planet), from the flow record: trips in the last year by class.
struct PorkchopRoute {
    std::string origin;
    std::string destination;
    double trips {0.0};
    std::map<std::string, double> classes;
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
    // worker fills their windows [now, now + span), nearest departures first. A destination "*"
    // stands for every route in use from the origin (the station inspector's windows).
    void request(std::vector<std::pair<std::string, std::string>> pairs, double now_s);
    // The routes in use (busiest first); the worker fills them once each when it has nothing else.
    void set_routes(std::vector<PorkchopRoute> routes);
    // Where columns are kept across sessions (empty: no disk cache).
    void set_cache_dir(std::string directory);
    // The requested pairs' axes and the columns computed so far, the routes in use, the classes
    // needed to turn cells into propellant, and each station's planet. `version` changes whenever
    // a column is added.
    [[nodiscard]] std::string json() const;
    [[nodiscard]] std::uint64_t version() const { return version_.load(); }

    // Pure pieces (tests, tools): a pair's axes and one column.
    [[nodiscard]] PorkchopAxes axes(const std::string& origin, const std::string& destination) const;
    // `left`: the column one departure earlier, if known (its solutions seed this one's).
    [[nodiscard]] PorkchopColumn compute_column(const std::string& origin, const std::string& destination,
        const PorkchopAxes& axes, long long index, const PorkchopColumn* left = nullptr) const;
    // Work the requested pairs (and the routes' prefetch) until nothing is missing (tests; the
    // worker does this in the background).
    void compute_all_now();

private:
    using Pair = std::pair<std::string, std::string>;
    struct Grid {
        PorkchopAxes axes;
        std::map<long long, PorkchopColumn> columns;
        bool cache_header_written {false};
    };
    struct Job {
        Pair pair;
        PorkchopAxes axes;
        long long index {0};
        PorkchopColumn left;
        bool have_left {false};
    };
    // Under mutex_: the requested pairs with "*" expanded, planets of the Sun only.
    [[nodiscard]] std::vector<Pair> expanded_requests() const;
    // Under mutex_: a pair's grid, created (and read from the disk cache) on first use.
    Grid& grid_for(const Pair& pair);
    // Under mutex_: the next missing column, on-screen pairs first, then the prefetch.
    bool next_job(Job& job);
    // Under mutex_: keep a computed column (and append it to the disk cache).
    void store(const Job& job, PorkchopColumn column);
    void worker_loop();
    [[nodiscard]] bool is_planet(const std::string& body) const;
    [[nodiscard]] std::uint64_t fingerprint(const Pair& pair, const PorkchopAxes& axes) const;
    [[nodiscard]] std::string cache_path(const Pair& pair) const;

    const domain::UniverseDefinition& universe_;
    celestial::CelestialMechanics mechanics_;
    KeplerTrajectoryPlanner kepler_;
    double mu_sun_ {0.0};

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<Pair> requested_;
    std::vector<PorkchopRoute> routes_;
    std::map<Pair, long long> prefetch_start_;   // the first column of a route's prefetched window
    std::set<Pair> prefetched_;
    std::string cache_dir_;
    double now_s_ {0.0};
    std::map<Pair, Grid> grids_;
    std::atomic<std::uint64_t> version_ {0};
    bool stop_ {false};
    std::thread worker_;
};

}  // namespace spacetrains::trajectory
