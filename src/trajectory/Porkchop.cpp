#include "trajectory/Porkchop.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <tuple>

#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "variable_isp/GeneralTransfer.hpp"

namespace spacetrains::trajectory {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kAu = 1.495978707e11;
constexpr double kDay = 86400.0;
constexpr double kMaxSpanS = 730.0 * kDay;
// Nuclear-thermal rows: the planner's Lambert search range (TrajectoryPlanner.cpp).
constexpr double kMinTransitFraction = 0.3;   // of the Hohmann time
constexpr double kMaxTransitFraction = 1.5;
// Variable-Isp rows, log spaced. Measured 2026-10-27 over two game years: plasma trips take
// 0.12-0.25 Hohmann times to and between the outer planets and up to 3 for Mercury-Venus.
constexpr double kPlasmaMinFraction = 0.08;
constexpr double kPlasmaMinS = 20.0 * kDay;   // and never under 20 days (Mercury-Venus: shorter arcs do not solve)
constexpr double kPlasmaMaxFraction = 4.0;
constexpr double kPlasmaMaxOverHohmannS = 730.0 * kDay;   // and never more than two years past Hohmann
constexpr double kLeftSeedMaxHohmannS = 500.0 * kDay;
// Columns more than this many steps behind the present drop their primers (only the next column
// needs them as seeds).
constexpr long long kKeepPrimersBehind = 4;
constexpr float kNone = std::numeric_limits<float>::quiet_NaN();
// Bump when the cells' meaning or the solver settings change: older cache files are ignored.
constexpr const char* kAlgorithm = "porkchop-3";
constexpr char kMagic[4] = {'P', 'K', 'C', '2'};

const domain::CelestialBodyDefinition& body_by_id(const domain::UniverseDefinition& universe, const std::string& id) {
    for (const auto& body : universe.bodies) {
        if (body.id == id) {
            return body;
        }
    }
    throw std::runtime_error("porkchop: unknown body " + id);
}

std::string json_number(double value, int decimals) {
    return std::isfinite(value) ? std::format("{:.{}f}", value, decimals) : "null";
}

std::string json_string(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

std::uint64_t fnv1a(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char c : text) {
        hash = (hash ^ c) * 1099511628211ULL;
    }
    return hash;
}

template <typename T>
void write_raw(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
bool read_raw(std::ifstream& in, T& value) {
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(T)));
}

}  // namespace

PorkchopService::PorkchopService(const domain::UniverseDefinition& universe, bool start_worker)
    : universe_(universe), mechanics_(universe), kepler_(universe, mechanics_) {
    mu_sun_ = mechanics_.get_body(mechanics_.get_root_body_id()).mu_m3_s2;
    if (start_worker) {
        worker_ = std::thread([this] { worker_loop(); });
    }
}

PorkchopService::~PorkchopService() {
    {
        const std::lock_guard lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

PorkchopAxes PorkchopService::axes(const std::string& origin, const std::string& destination) const {
    const auto& a = body_by_id(universe_, origin).orbit;
    const auto& b = body_by_id(universe_, destination).orbit;
    PorkchopAxes axes;
    const double transfer_axis = 0.5 * (a.semi_major_axis_m + b.semi_major_axis_m);
    axes.hohmann_s = kPi * std::sqrt(transfer_axis * transfer_axis * transfer_axis / mu_sun_);
    const double relative = std::abs(1.0 / a.orbital_period_s - 1.0 / b.orbital_period_s);
    axes.synodic_s = relative > 0.0 ? 1.0 / relative : 2.0 * axes.hohmann_s;
    axes.span_s = std::min(axes.synodic_s, kMaxSpanS);
    axes.step_s = axes.span_s / kColumns;
    const double plasma_lo = std::max(kPlasmaMinFraction * axes.hohmann_s, kPlasmaMinS);
    const double plasma_hi = std::min(kPlasmaMaxFraction * axes.hohmann_s, axes.hohmann_s + kPlasmaMaxOverHohmannS);
    for (int row = 0; row < kRows; ++row) {
        const double t = static_cast<double>(row) / (kRows - 1);
        axes.transit_s.push_back((kMinTransitFraction + (kMaxTransitFraction - kMinTransitFraction) * t) * axes.hohmann_s);
        axes.plasma_transit_s.push_back(plasma_lo * std::pow(plasma_hi / plasma_lo, t));
    }
    return axes;
}

PorkchopColumn PorkchopService::compute_column(const std::string& origin, const std::string& destination,
    const PorkchopAxes& axes, long long index, const PorkchopColumn* left) const {
    namespace general = variable_isp::general;
    PorkchopColumn column;
    column.index = index;
    const double departure_s = static_cast<double>(index) * axes.step_s;

    for (const double transit_s : axes.transit_s) {
        const auto cell = kepler_.lambert_cell(origin, destination, departure_s, transit_s, mu_sun_);
        column.dv_mps.push_back(cell ? static_cast<float>(cell->dv_departure + cell->dv_arrival) : kNone);
    }

    // Normalised units for the variable-Isp solver: μ = 1, length 1 AU (the game's orbits lie in x-z).
    const double time_unit_s = std::sqrt(kAu * kAu * kAu / mu_sun_);
    const auto state_at = [&](const std::string& body, double t_s) {
        constexpr double h = 600.0;
        const auto p = mechanics_.get_body_position(body, t_s);
        const auto p1 = mechanics_.get_body_position(body, t_s + h);
        const auto p0 = mechanics_.get_body_position(body, t_s - h);
        const double v_scale = time_unit_s / kAu / (2.0 * h);
        return general::State {{p.x / kAu, p.z / kAu}, {(p1.x - p0.x) * v_scale, (p1.z - p0.z) * v_scale}};
    };
    general::TransferRequest request;
    request.start = state_at(origin, departure_s);
    request.target = [&](double T) {
        const auto s = state_at(destination, departure_s + T * time_unit_s);
        const double r = general::norm(s.r);
        return general::TargetPoint {s, s.v, (-1.0 / (r * r * r)) * s.r};
    };
    request.end_condition = general::EndCondition::Rendezvous;
    request.min_radius = kMinPerihelionM / kAu;
    general::PropagationSettings settings;
    settings.relative_tolerance = 1e-7;
    settings.absolute_tolerance = 1e-8;
    const general::Propagator propagator(settings);
    general::SolveOptions options;
    options.fallbacks = false;   // fast first; a cell that fails is solved again with them below
    options.branches = 2;
    const general::SolveOptions thorough {};   // the full portfolio: fallbacks, three branches

    const auto valid = [&](const general::Solution& s) {
        return s.converged && s.end.ok && s.end.min_radius >= request.min_radius;
    };
    general::Primer warm {};
    bool have_warm = false;
    for (std::size_t row = 0; row < axes.plasma_transit_s.size(); ++row) {
        const double T = axes.plasma_transit_s[row] / time_unit_s;
        // Seed: the same flight time one departure earlier, else the shorter flight below.
        const general::Primer* seed = have_warm ? &warm : nullptr;
        general::Primer from_left {};
        // Measured 2026-10-27: from the left halves the inner pairs' cost; for the outer planets
        // (long arcs, many branches) it is slower and lands on other branches, so only the row seed.
        if (left != nullptr && axes.hohmann_s < kLeftSeedMaxHohmannS && row < left->primers.size()
            && std::isfinite(left->primers[row][0])) {
            const auto& p = left->primers[row];
            from_left = {{p[0], p[1]}, {p[2], p[3]}};
            seed = &from_left;
        }
        auto solution = general::solve_transfer(propagator, request, T, options, seed);
        bool ok = valid(solution);
        if (seed == &from_left) {
            // Each seed can land on its own local minimum (speckle in the plot): also solve from the
            // row below and keep the lower effort. Twice the work, for the inner pairs only.
            auto other = general::solve_transfer(propagator, request, T, options, have_warm ? &warm : nullptr);
            if (valid(other) && (!ok || other.end.effort < solution.end.effort)) {
                solution = std::move(other);
                ok = true;
            }
        }
        if (!ok) {
            // Measured 2026-10-27 (Earth-Saturn): the fast settings missed 6% of cells, all of which
            // the full portfolio solves (about 0.2 s each).
            auto again = general::solve_transfer(propagator, request, T, thorough, have_warm ? &warm : nullptr);
            if (valid(again)) {
                solution = std::move(again);
                ok = true;
            }
        }
        column.effort.push_back(ok ? static_cast<float>(solution.end.effort) : kNone);
        const double nan = std::numeric_limits<double>::quiet_NaN();
        column.primers.push_back(ok ? std::array<double, 4> {solution.primer.p.x, solution.primer.p.y, solution.primer.w.x,
            solution.primer.w.y} : std::array<double, 4> {nan, nan, nan, nan});
        if (ok) {
            warm = solution.primer;
            have_warm = true;
        }
    }
    // Down pass. Seeded from the row below, a column can stay on one family of solutions while a
    // cheaper one exists. Measured 2026-10-27 on Earth-Saturn: the cheaper family was up to 3x
    // lower, found only at longer flights and in odd cells, so the reach lines wrapped around
    // islands. Going down from the longer flights, a cell much dearer than the one above (or
    // dearer than the one below, or missing) is solved again from the one above. Only cells next
    // to a jump are solved again.
    constexpr double kJump = 1.25;   // neighbouring rows differ by about 1.1 on one family
    for (std::size_t row = axes.plasma_transit_s.size() - 1; row-- > 0;) {
        const float above = column.effort[row + 1];
        const float here = column.effort[row];
        if (!std::isfinite(above) || !std::isfinite(column.primers[row + 1][0])) {
            continue;
        }
        const bool suspect = !std::isfinite(here) || here > kJump * above
            || (row > 0 && std::isfinite(column.effort[row - 1]) && here > column.effort[row - 1]);
        if (!suspect) {
            continue;
        }
        const auto& p = column.primers[row + 1];
        const general::Primer seed {{p[0], p[1]}, {p[2], p[3]}};
        auto solution = general::solve_transfer(propagator, request, axes.plasma_transit_s[row] / time_unit_s, options, &seed);
        if (valid(solution) && (!std::isfinite(here) || solution.end.effort < here)) {
            column.effort[row] = static_cast<float>(solution.end.effort);
            column.primers[row] = {solution.primer.p.x, solution.primer.p.y, solution.primer.w.x, solution.primer.w.y};
        }
    }
    return column;
}

bool PorkchopService::is_planet(const std::string& body) const {
    try {
        return body_by_id(universe_, body).orbit.parent_id == mechanics_.get_root_body_id();
    } catch (const std::exception&) {
        return false;
    }
}

void PorkchopService::request(std::vector<std::pair<std::string, std::string>> pairs, double now_s) {
    {
        const std::lock_guard lock(mutex_);
        requested_ = std::move(pairs);
        now_s_ = now_s;
    }
    wake_.notify_all();
}

void PorkchopService::set_routes(std::vector<PorkchopRoute> routes) {
    {
        const std::lock_guard lock(mutex_);
        std::erase_if(routes, [&](const PorkchopRoute& r) {
            return r.origin == r.destination || !is_planet(r.origin) || !is_planet(r.destination);
        });
        std::sort(routes.begin(), routes.end(), [](const PorkchopRoute& a, const PorkchopRoute& b) {
            return a.trips != b.trips ? a.trips > b.trips : std::tie(a.origin, a.destination) < std::tie(b.origin, b.destination);
        });
        routes_ = std::move(routes);
    }
    wake_.notify_all();
}

void PorkchopService::set_cache_dir(std::string directory) {
    const std::lock_guard lock(mutex_);
    cache_dir_ = std::move(directory);
    if (!cache_dir_.empty()) {
        std::error_code error;
        std::filesystem::create_directories(cache_dir_, error);
    }
}

std::vector<PorkchopService::Pair> PorkchopService::expanded_requests() const {
    std::vector<Pair> out;
    const auto add = [&](const Pair& pair) {
        if (pair.first != pair.second && is_planet(pair.first) && is_planet(pair.second)
            && std::find(out.begin(), out.end(), pair) == out.end()) {
            out.push_back(pair);
        }
    };
    for (const auto& pair : requested_) {
        if (pair.second != "*") {
            add(pair);
            continue;
        }
        // Every route in use from the origin, nearest the Sun first (inner pairs take seconds).
        std::vector<Pair> from_here;
        for (const auto& route : routes_) {
            if (route.origin == pair.first) {
                from_here.emplace_back(route.origin, route.destination);
            }
        }
        std::sort(from_here.begin(), from_here.end(), [&](const Pair& a, const Pair& b) {
            return body_by_id(universe_, a.second).orbit.semi_major_axis_m < body_by_id(universe_, b.second).orbit.semi_major_axis_m;
        });
        for (const auto& p : from_here) {
            add(p);
        }
    }
    return out;
}

std::uint64_t PorkchopService::fingerprint(const Pair& pair, const PorkchopAxes& axes) const {
    std::string key = std::format("{}|{}|{}|{:.17g}|{}x{}", kAlgorithm, pair.first, pair.second, mu_sun_, kColumns, kRows);
    for (const auto& id : {pair.first, pair.second}) {
        const auto& o = body_by_id(universe_, id).orbit;
        key += std::format("|{:.17g},{:.17g},{:.17g},{:.17g}", o.semi_major_axis_m, o.eccentricity, o.orbital_period_s,
            o.phase_at_epoch_rad);
    }
    key += std::format("|{:.17g}|{:.17g}|{:.17g}", axes.step_s, axes.transit_s.back(), axes.plasma_transit_s.back());
    return fnv1a(key);
}

std::string PorkchopService::cache_path(const Pair& pair) const {
    return (std::filesystem::path(cache_dir_) / std::format("{}_{}.bin", pair.first, pair.second)).string();
}

PorkchopService::Grid& PorkchopService::grid_for(const Pair& pair) {
    if (const auto found = grids_.find(pair); found != grids_.end()) {
        return found->second;
    }
    auto& grid = grids_[pair];
    grid.axes = axes(pair.first, pair.second);
    if (cache_dir_.empty()) {
        return grid;
    }
    // Columns kept by earlier sessions: a header (magic, fingerprint, row counts) and records
    // (index, Δv rows, effort rows, primers). A foreign or damaged file is ignored and rewritten.
    std::ifstream in(cache_path(pair), std::ios::binary);
    char magic[4] {};
    std::uint64_t print = 0;
    std::uint32_t thermal_rows = 0;
    std::uint32_t plasma_rows = 0;
    if (!in || !in.read(magic, 4) || std::memcmp(magic, kMagic, 4) != 0 || !read_raw(in, print)
        || print != fingerprint(pair, grid.axes) || !read_raw(in, thermal_rows) || !read_raw(in, plasma_rows)
        || thermal_rows != grid.axes.transit_s.size() || plasma_rows != grid.axes.plasma_transit_s.size()) {
        return grid;
    }
    for (;;) {
        PorkchopColumn column;
        if (!read_raw(in, column.index)) {
            break;
        }
        column.dv_mps.resize(thermal_rows);
        column.effort.resize(plasma_rows);
        column.primers.resize(plasma_rows);
        if (!in.read(reinterpret_cast<char*>(column.dv_mps.data()), static_cast<std::streamsize>(thermal_rows * sizeof(float)))
            || !in.read(reinterpret_cast<char*>(column.effort.data()), static_cast<std::streamsize>(plasma_rows * sizeof(float)))
            || !in.read(reinterpret_cast<char*>(column.primers.data()),
                static_cast<std::streamsize>(plasma_rows * sizeof(std::array<double, 4>)))) {
            break;   // a record cut short (the game quit while writing): the rest is recomputed
        }
        grid.columns[column.index] = std::move(column);
    }
    grid.cache_header_written = true;
    if (!grid.columns.empty()) {
        ++version_;
    }
    return grid;
}

bool PorkchopService::next_job(Job& job) {
    const auto pick = [&](const Pair& pair, long long first) {
        auto& grid = grid_for(pair);
        for (long long k = first; k < first + kColumns; ++k) {
            if (!grid.columns.contains(k)) {
                job.pair = pair;
                job.axes = grid.axes;
                job.index = k;
                const auto l = grid.columns.find(k - 1);
                job.have_left = l != grid.columns.end() && !l->second.primers.empty();
                if (job.have_left) {
                    job.left = l->second;
                }
                return true;
            }
        }
        return false;
    };
    // The pairs on screen, the first one first (the open plot), nearest departures first.
    for (const auto& pair : expanded_requests()) {
        if (pick(pair, static_cast<long long>(std::floor(now_s_ / grid_for(pair).axes.step_s)))) {
            return true;
        }
    }
    // Then the routes in use, once each: the window from when its prefetch began (so the work
    // ends even while time runs on), inner planets first.
    std::vector<Pair> routes;
    for (const auto& route : routes_) {
        const Pair pair {route.origin, route.destination};
        if (!prefetched_.contains(pair)) {
            routes.push_back(pair);
        }
    }
    const auto outer = [&](const Pair& p) {
        return std::max(body_by_id(universe_, p.first).orbit.semi_major_axis_m, body_by_id(universe_, p.second).orbit.semi_major_axis_m);
    };
    std::stable_sort(routes.begin(), routes.end(), [&](const Pair& a, const Pair& b) { return outer(a) < outer(b); });
    for (const auto& pair : routes) {
        const auto start = prefetch_start_.try_emplace(pair,
            static_cast<long long>(std::floor(now_s_ / grid_for(pair).axes.step_s))).first->second;
        if (pick(pair, start)) {
            return true;
        }
        prefetched_.insert(pair);
    }
    return false;
}

void PorkchopService::store(const Job& job, PorkchopColumn column) {
    auto& grid = grid_for(job.pair);
    if (!cache_dir_.empty()) {
        std::ofstream out(cache_path(job.pair), std::ios::binary | (grid.cache_header_written ? std::ios::app : std::ios::trunc));
        if (out && !grid.cache_header_written) {
            out.write(kMagic, 4);
            write_raw(out, fingerprint(job.pair, grid.axes));
            write_raw(out, static_cast<std::uint32_t>(grid.axes.transit_s.size()));
            write_raw(out, static_cast<std::uint32_t>(grid.axes.plasma_transit_s.size()));
            grid.cache_header_written = true;
        }
        if (out) {
            write_raw(out, column.index);
            out.write(reinterpret_cast<const char*>(column.dv_mps.data()), static_cast<std::streamsize>(column.dv_mps.size() * sizeof(float)));
            out.write(reinterpret_cast<const char*>(column.effort.data()), static_cast<std::streamsize>(column.effort.size() * sizeof(float)));
            out.write(reinterpret_cast<const char*>(column.primers.data()),
                static_cast<std::streamsize>(column.primers.size() * sizeof(std::array<double, 4>)));
        }
    }
    grid.columns[job.index] = std::move(column);
    // Columns are kept (a few hundred bytes each without their primers): only the next column needs those.
    const auto first = static_cast<long long>(std::floor(now_s_ / grid.axes.step_s));
    for (auto& [index, kept] : grid.columns) {
        if (index >= first - kKeepPrimersBehind) {
            break;
        }
        if (!kept.primers.empty() && !grid.columns.contains(index + 1)) {
            continue;   // still the seed for a missing neighbour
        }
        kept.primers.clear();
        kept.primers.shrink_to_fit();
    }
    ++version_;
}

void PorkchopService::compute_all_now() {
    std::unique_lock lock(mutex_);
    for (;;) {
        Job job;
        if (!next_job(job)) {
            return;
        }
        lock.unlock();
        auto column = compute_column(job.pair.first, job.pair.second, job.axes, job.index, job.have_left ? &job.left : nullptr);
        lock.lock();
        store(job, std::move(column));
    }
}

void PorkchopService::worker_loop() {
#if defined(__linux__)
    // Low priority: the simulation's own threads come first.
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 15);
#endif
    std::unique_lock lock(mutex_);
    while (!stop_) {
        Job job;
        if (!next_job(job)) {
            wake_.wait(lock);
            continue;
        }
        lock.unlock();
        auto column = compute_column(job.pair.first, job.pair.second, job.axes, job.index, job.have_left ? &job.left : nullptr);
        lock.lock();
        store(job, std::move(column));
    }
}

std::string PorkchopService::json() const {
    const std::lock_guard lock(mutex_);
    std::string out = std::format("{{\"now_day\":{},\"au_m\":{},\"mu_sun\":{},\"pairs\":[", json_number(now_s_ / kDay, 3),
        json_number(kAu, 0), json_number(mu_sun_, 0));
    bool first_pair = true;
    for (const auto& pair : expanded_requests()) {
        const auto found = grids_.find(pair);
        const auto grid_axes = found == grids_.end() ? axes(pair.first, pair.second) : found->second.axes;
        out += std::format("{}{{\"origin\":\"{}\",\"destination\":\"{}\",\"step_days\":{},\"hohmann_days\":{},\"synodic_days\":{},"
            "\"transit_days\":[", first_pair ? "" : ",", pair.first, pair.second, json_number(grid_axes.step_s / kDay, 6),
            json_number(grid_axes.hohmann_s / kDay, 3), json_number(grid_axes.synodic_s / kDay, 3));
        first_pair = false;
        for (std::size_t r = 0; r < grid_axes.transit_s.size(); ++r) {
            out += (r > 0 ? "," : "") + json_number(grid_axes.transit_s[r] / kDay, 3);
        }
        out += "],\"plasma_transit_days\":[";
        for (std::size_t r = 0; r < grid_axes.plasma_transit_s.size(); ++r) {
            out += (r > 0 ? "," : "") + json_number(grid_axes.plasma_transit_s[r] / kDay, 3);
        }
        out += "],\"columns\":[";
        bool first_column = true;
        if (found != grids_.end()) {
            const auto first = static_cast<long long>(std::floor(now_s_ / grid_axes.step_s));
            for (auto it = found->second.columns.lower_bound(first); it != found->second.columns.end() && it->first < first + kColumns; ++it) {
                const auto& column = it->second;
                out += std::format("{}{{\"day\":{},\"dv\":[", first_column ? "" : ",",
                    json_number(static_cast<double>(it->first) * grid_axes.step_s / kDay, 3));
                first_column = false;
                for (std::size_t r = 0; r < column.dv_mps.size(); ++r) {
                    out += (r > 0 ? "," : "") + json_number(column.dv_mps[r], 0);
                }
                out += "],\"effort\":[";
                for (std::size_t r = 0; r < column.effort.size(); ++r) {
                    out += (r > 0 ? "," : "") + (std::isfinite(column.effort[r]) ? std::format("{:.5g}", column.effort[r]) : "null");
                }
                out += "]}";
            }
        }
        out += "]}";
    }
    // The routes ships fly (last year), busiest first, with their trips by class.
    out += "],\"routes\":[";
    for (std::size_t i = 0; i < routes_.size(); ++i) {
        const auto& route = routes_[i];
        out += std::format("{}{{\"origin\":\"{}\",\"destination\":\"{}\",\"trips\":{},\"classes\":{{", i > 0 ? "," : "", route.origin,
            route.destination, json_number(route.trips, 0));
        bool first_class = true;
        for (const auto& [class_id, trips] : route.classes) {
            out += std::format("{}{}:{}", first_class ? "" : ",", json_string(class_id), json_number(trips, 0));
            first_class = false;
        }
        out += "}}";
    }
    // What turns a cell into propellant for each class: thermal Δv with the exhaust speed, plasma
    // effort with the drive power (1/m_f - 1/m0 = J_SI / 2P, J_SI = J · μ^1.5 / AU^2.5).
    out += "],\"classes\":[";
    bool first_class = true;
    for (const auto& ship_class : universe_.ship_classes) {
        const bool plasma = ship_class.propulsion_type == "variable_isp";
        out += std::format("{}{{\"id\":\"{}\",\"name\":{},\"propulsion_type\":\"{}\",\"dry_mass_kg\":{},"
            "\"propellant_capacity_kg\":{},\"cargo_capacity_units\":{},\"exhaust_velocity_mps\":{},\"power_w\":{}}}",
            first_class ? "" : ",", ship_class.id, json_string(ship_class.name), ship_class.propulsion_type,
            json_number(ship_class.dry_mass_kg, 1), json_number(ship_class.propellant_capacity_kg, 1),
            json_number(ship_class.cargo_capacity_units, 1),
            json_number(plasma ? 0.0 : thermal_exhaust_velocity_mps(ship_class), 1),
            json_number(plasma ? ship_class.specific_engine_power_w_per_kg * ship_class.dry_mass_kg : 0.0, 1));
        first_class = false;
    }
    // Each station's planet (the pair its routes use).
    out += "],\"stations\":{";
    bool first_station = true;
    for (const auto& station : universe_.stations) {
        out += std::format("{}\"{}\":\"{}\"", first_station ? "" : ",", station.id,
            planet_system_primary(mechanics_, station.parent_body_id));
        first_station = false;
    }
    out += "}}";
    return out;
}

}  // namespace spacetrains::trajectory
