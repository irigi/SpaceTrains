// Atlas fill: solve the VariableISP atlas cells the Python generator left empty, and add κ levels
// below its grid, with the general solver (GeneralTransfer: Cartesian primer form, analytic
// transition matrix).
//
//   spacetrains_atlas_fill --input atlas.bin --output new.bin [--extend N] [--threads T]
//                          [--max-passes P] [--checkpoint-s S] [--below-kappa K]
//
// A cell (ρ, κ, θ) is the canonical mission: a circular orbit at 1 AU to a circular orbit at ρ,
// arriving after sweeping θ (counted mod 2π, as the planner reads the atlas), with effort at
// most κ (normalised effort is κ). Each new cell holds the minimum-time transfer on its branch
// for that budget; the old cells are kept as they are.
//
// The fill is a wavefront: each pass tries every empty cell next to a cell solved before the
// pass, seeded from those neighbours (the next κ up first: the path does not depend on κ, so
// only the time changes; then κ down, θ, ρ). A seed is shot onto this cell's target at the
// neighbour's time, then its time is lengthened until the effort fits the budget, or shortened
// until it uses it (never closer to the Sun than kSpendMinRadius). New cells are checked by
// integrating the stored polar record with the planner's own integrator. Passes are
// synchronous (a pass reads only cells solved before it), so the result does not depend on the
// thread count. The output file is rewritten after every pass that takes longer than the
// checkpoint interval; rerun with --input <output> --extend 0 to resume.
//
// --below-kappa K fills only cells with κ < K. The game's atlas (2026-10-22) is
//   --extend 14 --below-kappa 0.1
// i.e. 14 new levels (κ down to 0.0032; 99% of the planner's below-grid requests, mostly
// departures from Mercury, whose κ scales by 0.387^2.5) with the old cells untouched. Filling
// the old grid's holes as well found 12% more cells but cost the planner ~12% more time per
// simulated day for little economy gain (docs/modules/trajectory.md).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "variable_isp/GeneralTransfer.hpp"
#include "variable_isp/VariableIsp.hpp"

namespace vi = spacetrains::variable_isp;
namespace g = spacetrains::variable_isp::general;

namespace {

constexpr std::uint64_t kMagic = 0x3154415053495654ULL;  // "TVISPAT1"
constexpr std::size_t kWidth = 6;
constexpr double kTau = 6.283185307179586;
constexpr double kBudgetFraction = 0.999;      // as the planner: m_end just above m_dry
constexpr double kMinRadius = 0.1;              // reject paths closer to the Sun (canonical, Earth departures)
constexpr double kSpendMinRadius = 0.26;        // shortening stops here: 0.1 AU from a Mercury departure

struct Atlas {
    std::vector<double> rho, kappa, theta;
    std::vector<std::uint8_t> solved;
    std::vector<double> data;
    [[nodiscard]] std::size_t index(std::size_t i, std::size_t j, std::size_t k) const {
        return (i * kappa.size() + j) * theta.size() + k;
    }
};

template <typename T>
void read_into(std::ifstream& in, T* out, std::size_t count) {
    in.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(count * sizeof(T)));
    if (!in) {
        std::fprintf(stderr, "atlas file truncated\n");
        std::exit(1);
    }
}

Atlas load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(1);
    }
    std::uint64_t header[4];
    read_into(in, header, 4);
    if (header[0] != kMagic) {
        std::fprintf(stderr, "%s is not a VariableISP atlas\n", path.c_str());
        std::exit(1);
    }
    Atlas a;
    a.rho.resize(header[1]);
    a.kappa.resize(header[2]);
    a.theta.resize(header[3]);
    read_into(in, a.rho.data(), a.rho.size());
    read_into(in, a.kappa.data(), a.kappa.size());
    read_into(in, a.theta.data(), a.theta.size());
    const std::size_t cells = a.rho.size() * a.kappa.size() * a.theta.size();
    a.solved.resize(cells);
    a.data.resize(cells * kWidth);
    read_into(in, a.solved.data(), cells);
    read_into(in, a.data.data(), cells * kWidth);
    return a;
}

void save(const Atlas& a, const std::string& path) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        const std::uint64_t header[4] = {kMagic, a.rho.size(), a.kappa.size(), a.theta.size()};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        out.write(reinterpret_cast<const char*>(a.rho.data()), static_cast<std::streamsize>(a.rho.size() * 8));
        out.write(reinterpret_cast<const char*>(a.kappa.data()), static_cast<std::streamsize>(a.kappa.size() * 8));
        out.write(reinterpret_cast<const char*>(a.theta.data()), static_cast<std::streamsize>(a.theta.size() * 8));
        out.write(reinterpret_cast<const char*>(a.solved.data()), static_cast<std::streamsize>(a.solved.size()));
        out.write(reinterpret_cast<const char*>(a.data.data()), static_cast<std::streamsize>(a.data.size() * 8));
    }
    std::rename(tmp.c_str(), path.c_str());
}

// Add `levels` κ levels below the grid at its own log step; the new cells are empty.
Atlas extend(const Atlas& a, std::size_t levels) {
    if (levels == 0) {
        return a;
    }
    const double step = a.kappa[1] / a.kappa[0];
    Atlas b;
    b.rho = a.rho;
    b.theta = a.theta;
    for (std::size_t m = levels; m > 0; --m) {
        b.kappa.push_back(a.kappa[0] * std::pow(step, -static_cast<double>(m)));
    }
    b.kappa.insert(b.kappa.end(), a.kappa.begin(), a.kappa.end());
    const std::size_t cells = b.rho.size() * b.kappa.size() * b.theta.size();
    b.solved.assign(cells, 0);
    b.data.assign(cells * kWidth, std::nan(""));
    for (std::size_t i = 0; i < a.rho.size(); ++i) {
        for (std::size_t j = 0; j < a.kappa.size(); ++j) {
            for (std::size_t k = 0; k < a.theta.size(); ++k) {
                const std::size_t from = a.index(i, j, k);
                const std::size_t to = b.index(i, j + levels, k);
                b.solved[to] = a.solved[from];
                std::copy_n(a.data.begin() + static_cast<std::ptrdiff_t>(from * kWidth), kWidth,
                    b.data.begin() + static_cast<std::ptrdiff_t>(to * kWidth));
            }
        }
    }
    return b;
}

vi::AtlasSeed record(const Atlas& a, std::size_t cell) {
    vi::AtlasSeed seed;
    for (std::size_t n = 0; n < 5; ++n) {
        seed.params[n] = a.data[cell * kWidth + n];
    }
    seed.transfer_time_days = a.data[cell * kWidth + 5];
    return seed;
}

g::State circular(double r, double theta) {
    const g::Vec2 er {std::cos(theta), std::sin(theta)};
    return {r * er, (1.0 / std::sqrt(r)) * g::Vec2 {-er.y, er.x}};
}

struct Outcome {
    bool solved {false};
    vi::AtlasSeed seed;
    int via {-1};  // which neighbour seeded it
};

// The polar record flown by the planner's integrator must land on the cell's target with the
// fuel it has (the planner's own tolerances and its m_end >= 0.998 m_dry rule).
bool verify(const vi::AtlasSeed& seed, const vi::CanonicalMissionConfig& config, double rho, double theta) {
    static const vi::VariableIspIntegrator integrator;
    const auto summary = integrator.integrate_fixed_time(seed, config, 2);
    if (summary.samples.empty()) {
        return false;
    }
    const auto& end = summary.samples.back();
    const double r_target = rho * config.r0_m;
    const double v_circ = std::sqrt(config.mu_m3_s2 / r_target);
    const vi::ShootingSettings tol;
    return std::abs(end.r_m / r_target - 1.0) < tol.r_tolerance_rel
        && std::abs(std::remainder(end.theta_rad - theta, kTau)) < tol.theta_tolerance_rad
        && std::abs(end.vr_mps) / v_circ < tol.velocity_tolerance_rel
        && std::abs(end.vtheta_mps - v_circ) / v_circ < tol.velocity_tolerance_rel
        && end.mass_kg >= 0.998 * config.m_dry_kg;
}

Outcome solve_cell(const Atlas& a, std::size_t i, std::size_t j, std::size_t k, const std::vector<std::uint8_t>& known) {
    static const g::Propagator propagator = [] {
        g::PropagationSettings settings;
        settings.min_radius = 0.05;  // fail fast on Sun dives
        return g::Propagator(settings);
    }();
    const double rho = a.rho[i];
    const double kappa = a.kappa[j];
    const double theta = a.theta[k];
    const auto config = vi::VariableIspIntegrator::canonical_config(rho, kappa);
    const double budget = kBudgetFraction * kappa;
    const g::State start {{1.0, 0.0}, {0.0, 1.0}};
    const g::Target target = g::fixed_target(circular(rho, theta));
    g::ShootingOptions fixed;
    fixed.free_time = false;
    g::ShootingOptions free_time = fixed;
    free_time.free_time = true;
    free_time.match_budget = false;

    // Neighbours, the most similar first: κ up (same path), κ down, θ, ρ.
    struct Offset {
        int di, dj, dk;
    };
    constexpr Offset kOffsets[] = {{0, 1, 0}, {0, -1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};
    Outcome outcome;
    for (int n = 0; n < 6; ++n) {
        const auto& o = kOffsets[n];
        const long ni = static_cast<long>(i) + o.di;
        const long nj = static_cast<long>(j) + o.dj;
        const long nk = static_cast<long>(k) + o.dk;
        if (ni < 0 || nj < 0 || nk < 0 || ni >= static_cast<long>(a.rho.size()) || nj >= static_cast<long>(a.kappa.size())
            || nk >= static_cast<long>(a.theta.size())) {
            continue;
        }
        const std::size_t cell = a.index(static_cast<std::size_t>(ni), static_cast<std::size_t>(nj), static_cast<std::size_t>(nk));
        if (!known[cell]) {
            continue;
        }
        const auto neighbour = record(a, cell);
        const auto neighbour_config = vi::VariableIspIntegrator::canonical_config(a.rho[ni], a.kappa[nj]);
        const g::Primer primer = vi::primer_from_costates(neighbour, neighbour_config);
        const double T0 = neighbour.transfer_time_days / vi::canonical_days_per_unit(neighbour_config);
        auto solution = g::shoot(propagator, start, target, g::EndCondition::Rendezvous, primer, T0, fixed);
        if (!solution.converged) {
            solution = g::shoot(propagator, start, target, g::EndCondition::Rendezvous, primer, T0, free_time);
        }
        if (!solution.converged) {
            continue;
        }
        if (solution.end.effort > budget) {
            solution = g::fit_effort_by_time(propagator, start, target, g::EndCondition::Rendezvous, solution, budget, 1e-3, fixed);
            if (!solution.converged) {
                continue;
            }
        } else if (solution.end.effort < budget * (1.0 - 1e-3)) {
            solution = g::spend_effort_by_time(propagator, start, target, g::EndCondition::Rendezvous, solution, budget,
                kSpendMinRadius, 1e-3, fixed);
        }
        if (!solution.converged || solution.end.effort > kappa || solution.end.min_radius < kMinRadius) {
            continue;
        }
        const auto seed = vi::costates_from_primer(solution.primer, solution.T, config);
        if (!verify(seed, config, rho, theta)) {
            continue;
        }
        outcome = {true, seed, n};
        return outcome;
    }
    return outcome;
}

}  // namespace

int main(int argc, char** argv) {
    std::string input = "tests/data/variable_isp/variable_isp_atlas.bin";
    std::string output;
    std::size_t levels = 0;
    std::size_t threads = std::max(1u, std::thread::hardware_concurrency());
    std::size_t max_passes = 1000;
    double checkpoint_s = 300.0;
    double below_kappa = std::numeric_limits<double>::infinity();
    for (int n = 1; n < argc; ++n) {
        const std::string arg = argv[n];
        const auto next = [&]() -> std::string { return n + 1 < argc ? argv[++n] : std::string(); };
        if (arg == "--input") input = next();
        else if (arg == "--output") output = next();
        else if (arg == "--extend") levels = std::stoul(next());
        else if (arg == "--threads") threads = std::stoul(next());
        else if (arg == "--max-passes") max_passes = std::stoul(next());
        else if (arg == "--checkpoint-s") checkpoint_s = std::stod(next());
        else if (arg == "--below-kappa") below_kappa = std::stod(next());
        else {
            std::fprintf(stderr, "unknown argument %s\n", arg.c_str());
            return 2;
        }
    }
    if (output.empty()) {
        std::fprintf(stderr, "--output is required\n");
        return 2;
    }
    Atlas atlas = extend(load(input), levels);
    const std::size_t cells = atlas.solved.size();
    std::fprintf(stderr, "atlas %zu x %zu x %zu, kappa %.4g..%.4g, solved %zu of %zu\n", atlas.rho.size(),
        atlas.kappa.size(), atlas.theta.size(), atlas.kappa.front(), atlas.kappa.back(),
        static_cast<std::size_t>(std::count(atlas.solved.begin(), atlas.solved.end(), 1)), cells);

    // Cells solved in the last pass: only their empty neighbours get a new try.
    std::vector<std::uint8_t> fresh = atlas.solved;
    const auto began = std::chrono::steady_clock::now();
    auto last_save = began;
    std::size_t total_new = 0;
    for (std::size_t pass = 0; pass < max_passes; ++pass) {
        std::vector<std::size_t> todo;
        for (std::size_t i = 0; i < atlas.rho.size(); ++i) {
            for (std::size_t j = 0; j < atlas.kappa.size(); ++j) {
                for (std::size_t k = 0; k < atlas.theta.size(); ++k) {
                    const std::size_t c = atlas.index(i, j, k);
                    if (atlas.solved[c] || atlas.kappa[j] >= below_kappa) continue;
                    const bool near_fresh = (i > 0 && fresh[atlas.index(i - 1, j, k)])
                        || (i + 1 < atlas.rho.size() && fresh[atlas.index(i + 1, j, k)])
                        || (j > 0 && fresh[atlas.index(i, j - 1, k)])
                        || (j + 1 < atlas.kappa.size() && fresh[atlas.index(i, j + 1, k)])
                        || (k > 0 && fresh[atlas.index(i, j, k - 1)])
                        || (k + 1 < atlas.theta.size() && fresh[atlas.index(i, j, k + 1)]);
                    if (near_fresh) todo.push_back(c);
                }
            }
        }
        if (todo.empty()) {
            break;
        }
        const auto pass_began = std::chrono::steady_clock::now();
        std::vector<Outcome> results(todo.size());
        std::atomic<std::size_t> next {0};
        const auto& known = atlas.solved;
        const auto work = [&] {
            for (std::size_t n = next++; n < todo.size(); n = next++) {
                const std::size_t c = todo[n];
                const std::size_t k = c % atlas.theta.size();
                const std::size_t j = (c / atlas.theta.size()) % atlas.kappa.size();
                const std::size_t i = c / (atlas.theta.size() * atlas.kappa.size());
                results[n] = solve_cell(atlas, i, j, k, known);
            }
        };
        std::vector<std::thread> pool;
        for (std::size_t t = 0; t < threads; ++t) pool.emplace_back(work);
        for (auto& t : pool) t.join();

        std::fill(fresh.begin(), fresh.end(), 0);
        std::size_t added = 0;
        std::size_t via[6] = {};
        for (std::size_t n = 0; n < todo.size(); ++n) {
            if (!results[n].solved) continue;
            const std::size_t c = todo[n];
            const auto& seed = results[n].seed;
            for (std::size_t p = 0; p < 5; ++p) atlas.data[c * kWidth + p] = seed.params[p];
            atlas.data[c * kWidth + 5] = seed.transfer_time_days;
            atlas.solved[c] = 1;
            fresh[c] = 1;
            ++added;
            ++via[results[n].via];
        }
        total_new += added;
        const auto now = std::chrono::steady_clock::now();
        const double pass_s = std::chrono::duration<double>(now - pass_began).count();
        const double total_s = std::chrono::duration<double>(now - began).count();
        std::fprintf(stderr,
            "pass %zu: tried %zu, solved %zu (via k+ %zu, k- %zu, t- %zu, t+ %zu, r- %zu, r+ %zu) in %.0f s; "
            "new total %zu, %.0f s elapsed\n",
            pass, todo.size(), added, via[0], via[1], via[2], via[3], via[4], via[5], pass_s, total_new, total_s);
        if (std::chrono::duration<double>(now - last_save).count() >= checkpoint_s) {
            save(atlas, output);
            last_save = now;
            std::fprintf(stderr, "checkpoint written to %s\n", output.c_str());
        }
        if (added == 0) {
            break;
        }
    }
    save(atlas, output);
    std::fprintf(stderr, "done: %zu new cells, solved %zu of %zu; written to %s\n", total_new,
        static_cast<std::size_t>(std::count(atlas.solved.begin(), atlas.solved.end(), 1)), cells, output.c_str());
    return 0;
}
