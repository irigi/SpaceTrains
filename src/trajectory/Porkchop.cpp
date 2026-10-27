#include "trajectory/Porkchop.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

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
constexpr double kMinTransitFraction = 0.25;   // of the Hohmann time
constexpr double kMaxTransitFraction = 2.0;
constexpr double kLeftSeedMaxHohmannS = 500.0 * kDay;
constexpr float kNone = std::numeric_limits<float>::quiet_NaN();

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
    for (int row = 0; row < kRows; ++row) {
        const double fraction = kMinTransitFraction
            + (kMaxTransitFraction - kMinTransitFraction) * static_cast<double>(row) / (kRows - 1);
        axes.transit_s.push_back(fraction * axes.hohmann_s);
    }
    return axes;
}

PorkchopColumn PorkchopService::compute_column(const std::string& origin, const std::string& destination,
    const PorkchopAxes& axes, long long index, const PorkchopColumn* left) const {
    namespace general = variable_isp::general;
    PorkchopColumn column;
    column.index = index;
    const double departure_s = static_cast<double>(index) * axes.step_s;

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
    options.fallbacks = false;   // measured: the slow fallbacks rarely add a cell worth showing
    options.branches = 2;

    general::Primer warm {};
    bool have_warm = false;
    for (std::size_t row = 0; row < axes.transit_s.size(); ++row) {
        const double transit_s = axes.transit_s[row];
        const auto cell = kepler_.lambert_cell(origin, destination, departure_s, transit_s, mu_sun_);
        column.dv_mps.push_back(cell ? static_cast<float>(cell->dv_departure + cell->dv_arrival) : kNone);

        const double T = transit_s / time_unit_s;
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
        bool ok = solution.converged && solution.end.ok && solution.end.min_radius >= request.min_radius;
        if (!ok && seed == &from_left) {
            // The neighbour's branch dives under the limit here: start again from the row below.
            solution = general::solve_transfer(propagator, request, T, options, have_warm ? &warm : nullptr);
            ok = solution.converged && solution.end.ok && solution.end.min_radius >= request.min_radius;
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
    return column;
}

void PorkchopService::request(std::vector<std::pair<std::string, std::string>> pairs, double now_s) {
    {
        const std::lock_guard lock(mutex_);
        requested_.clear();
        for (auto& pair : pairs) {
            if (pair.first == pair.second) {
                continue;
            }
            // Only planets (and dwarf planets) of the Sun: a moon's station flies from its planet.
            try {
                if (body_by_id(universe_, pair.first).orbit.parent_id != mechanics_.get_root_body_id()
                    || body_by_id(universe_, pair.second).orbit.parent_id != mechanics_.get_root_body_id()) {
                    continue;
                }
            } catch (const std::exception&) {
                continue;
            }
            requested_.push_back(std::move(pair));
        }
        now_s_ = now_s;
    }
    wake_.notify_all();
}

bool PorkchopService::next_job(std::pair<std::string, std::string>& pair, PorkchopAxes& axes, long long& index) const {
    // Under mutex_. The first requested pair first (the one on screen), nearest departures first.
    for (const auto& requested : requested_) {
        const auto found = grids_.find(requested);
        const PorkchopAxes grid_axes = found == grids_.end() ? this->axes(requested.first, requested.second) : found->second.axes;
        const auto first = static_cast<long long>(std::floor(now_s_ / grid_axes.step_s));
        for (long long k = first; k < first + kColumns; ++k) {
            if (found == grids_.end() || !found->second.columns.contains(k)) {
                pair = requested;
                axes = grid_axes;
                index = k;
                return true;
            }
        }
    }
    return false;
}

void PorkchopService::compute_all_now() {
    for (;;) {
        std::pair<std::string, std::string> pair;
        PorkchopAxes grid_axes;
        long long index = 0;
        {
            const std::lock_guard lock(mutex_);
            if (!next_job(pair, grid_axes, index)) {
                return;
            }
        }
        PorkchopColumn left_copy;
        bool have_left = false;
        {
            const std::lock_guard lock(mutex_);
            if (const auto g = grids_.find(pair); g != grids_.end()) {
                if (const auto l = g->second.columns.find(index - 1); l != g->second.columns.end()) {
                    left_copy = l->second;
                    have_left = true;
                }
            }
        }
        auto column = compute_column(pair.first, pair.second, grid_axes, index, have_left ? &left_copy : nullptr);
        const std::lock_guard lock(mutex_);
        auto& grid = grids_[pair];
        grid.axes = grid_axes;
        grid.columns[index] = std::move(column);
        ++version_;
    }
}

void PorkchopService::worker_loop() {
#if defined(__linux__)
    // Low priority: the simulation's own threads come first.
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 15);
#endif
    std::unique_lock lock(mutex_);
    while (!stop_) {
        std::pair<std::string, std::string> pair;
        PorkchopAxes grid_axes;
        long long index = 0;
        if (!next_job(pair, grid_axes, index)) {
            wake_.wait(lock);
            continue;
        }
        PorkchopColumn left_copy;
        bool have_left = false;
        if (const auto g = grids_.find(pair); g != grids_.end()) {
            if (const auto l = g->second.columns.find(index - 1); l != g->second.columns.end()) {
                left_copy = l->second;
                have_left = true;
            }
        }
        lock.unlock();
        auto column = compute_column(pair.first, pair.second, grid_axes, index, have_left ? &left_copy : nullptr);
        lock.lock();
        auto& grid = grids_[pair];
        grid.axes = grid_axes;
        grid.columns[index] = std::move(column);
        // Keep the window (and a little behind it); drop pairs no longer asked for beyond a few.
        const auto first = static_cast<long long>(std::floor(now_s_ / grid_axes.step_s));
        std::erase_if(grid.columns, [&](const auto& entry) { return entry.first < first - 4 || entry.first >= first + kColumns + 4; });
        if (grids_.size() > 24) {
            std::erase_if(grids_, [&](const auto& entry) {
                return std::find(requested_.begin(), requested_.end(), entry.first) == requested_.end();
            });
        }
        ++version_;
    }
}

std::string PorkchopService::json() const {
    const std::lock_guard lock(mutex_);
    std::string out = std::format("{{\"now_day\":{},\"au_m\":{},\"mu_sun\":{},\"pairs\":[", json_number(now_s_ / kDay, 3),
        json_number(kAu, 0), json_number(mu_sun_, 0));
    bool first_pair = true;
    for (const auto& pair : requested_) {
        const auto found = grids_.find(pair);
        const auto grid_axes = found == grids_.end() ? axes(pair.first, pair.second) : found->second.axes;
        out += std::format("{}{{\"origin\":\"{}\",\"destination\":\"{}\",\"step_days\":{},\"hohmann_days\":{},\"synodic_days\":{},"
            "\"transit_days\":[", first_pair ? "" : ",", pair.first, pair.second, json_number(grid_axes.step_s / kDay, 6),
            json_number(grid_axes.hohmann_s / kDay, 3), json_number(grid_axes.synodic_s / kDay, 3));
        first_pair = false;
        for (std::size_t r = 0; r < grid_axes.transit_s.size(); ++r) {
            out += (r > 0 ? "," : "") + json_number(grid_axes.transit_s[r] / kDay, 3);
        }
        out += "],\"columns\":[";
        bool first_column = true;
        if (found != grids_.end()) {
            const auto first = static_cast<long long>(std::floor(now_s_ / grid_axes.step_s));
            for (const auto& [index, column] : found->second.columns) {
                if (index < first || index >= first + kColumns) {
                    continue;
                }
                out += std::format("{}{{\"day\":{},\"dv\":[", first_column ? "" : ",",
                    json_number(static_cast<double>(index) * grid_axes.step_s / kDay, 3));
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
    // What turns a cell into propellant for each class: thermal Δv with the exhaust speed, plasma
    // effort with the drive power (1/m_f - 1/m0 = J_SI / 2P, J_SI = J · μ^1.5 / AU^2.5).
    out += "],\"classes\":[";
    bool first_class = true;
    for (const auto& ship_class : universe_.ship_classes) {
        const bool plasma = ship_class.propulsion_type == "variable_isp";
        out += std::format("{}{{\"id\":\"{}\",\"name\":\"{}\",\"propulsion_type\":\"{}\",\"dry_mass_kg\":{},"
            "\"propellant_capacity_kg\":{},\"exhaust_velocity_mps\":{},\"power_w\":{}}}",
            first_class ? "" : ",", ship_class.id, ship_class.name, ship_class.propulsion_type, json_number(ship_class.dry_mass_kg, 1),
            json_number(ship_class.propellant_capacity_kg, 1), json_number(plasma ? 0.0 : thermal_exhaust_velocity_mps(ship_class), 1),
            json_number(plasma ? ship_class.specific_engine_power_w_per_kg * ship_class.dry_mass_kg : 0.0, 1));
        first_class = false;
    }
    // Each station's planet (the pair its routes use) and the planets that can be asked for.
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
