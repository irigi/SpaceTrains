// Banded-exhaust study driver (docs/studies/banded_transfer): plan-ideal-fly-banded (A) against
// plan-banded (B) for the game's plasma ships on heliocentric legs between the planets.
//
//   spacetrains_banded_study tune  <out_dir>   solver variants on a tuning set
//   spacetrains_banded_study study <out_dir>   the comparison (CSV files for make_figures.py)
//
// Run from the repository root (reads data/).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <functional>
#include <map>
#include <mutex>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "celestial/CelestialMechanics.hpp"
#include "data_loader/DataLoader.hpp"
#include "trajectory/ChaseSolver.hpp"
#include "variable_isp/BandedTransfer.hpp"
#include "variable_isp/GeneralTransfer.hpp"

using namespace spacetrains;
namespace b = variable_isp::banded;
namespace g = variable_isp::general;

namespace {

constexpr double kAu = 1.495978707e11;
constexpr double kMuSun = 1.32712440018e20;
constexpr double kDay = 86400.0;
constexpr double kPi = std::numbers::pi;
const double kTimeUnit = std::sqrt(kAu * kAu * kAu / kMuSun);   // s
const double kSpeedUnit = std::sqrt(kMuSun / kAu);              // m/s
const double kAccelUnit = kMuSun / (kAu * kAu);                 // m/s²
const double kSUnit = kAccelUnit * kSpeedUnit;                  // W/kg (2P/m)
constexpr double kMinRadius = 0.1;                               // AU, closest solar pass allowed

double now_ms() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- Game data -----------------------------------------------------------------------------------

struct ShipSpec {
    std::string id;
    std::string hull;
    std::string name;
    double dry_kg {0.0};
    double capacity_kg {0.0};
    double power_w {0.0};
    double payload_kg {0.0};
    double daily_cost_cr {0.0};
    double tank_factor {1.0};
};

struct World {
    domain::UniverseDefinition universe;
    std::unique_ptr<celestial::CelestialMechanics> mechanics;
    trajectory::ChaseUnits units;
    std::vector<ShipSpec> ships;   // every tank variant of the studied hulls
    double fuel_cr_per_kg {0.0};
    double v_min_mps {50000.0};
    double v_max_mps {250000.0};
};

const std::vector<std::string> kHulls {"plasma_freighter", "plasma_courier", "plasma_bulk_tanker", "plasma_liner", "large_plasma_liner"};

World load_world() {
    World w;
    w.universe = data_loader::DataLoader().load_universe("data");
    w.mechanics = std::make_unique<celestial::CelestialMechanics>(w.universe);
    w.units.mu_m3_s2 = kMuSun;
    w.units.length_m = kAu;
    const auto price = [&](const std::string& id) {
        for (const auto& c : w.universe.commodities) {
            if (c.id == id) return c.base_price;
        }
        return 0.0;
    };
    w.fuel_cr_per_kg = price("fuel") / 100.0;   // FUEL_UNITS_TO_KG
    const auto& ops = w.universe.ship_operations;
    const auto& pax = w.universe.passengers;
    for (const auto& c : w.universe.ship_classes) {
        const std::string hull = c.hull_id.empty() ? c.id : c.hull_id;
        if (c.propulsion_type != "variable_isp" || std::find(kHulls.begin(), kHulls.end(), hull) == kHulls.end()) continue;
        ShipSpec s;
        s.id = c.id;
        s.hull = hull;
        s.name = c.name;
        s.dry_kg = c.dry_mass_kg;
        s.capacity_kg = c.propellant_capacity_kg;
        s.power_w = c.specific_engine_power_w_per_kg * c.dry_mass_kg;
        // Full hold at 100 kg a unit (food, water, fuel), or full berths (100-day trip provisions).
        s.payload_kg = c.cargo_capacity_units * 100.0 + c.berths * (pax.body_luggage_kg + pax.consumables_kg_per_day * 100.0);
        // Market.cpp daily_cost: capital charge + crew wages + life support at base prices.
        double cost = c.ship_value_cr * (ops.interest_rate_per_year + 1.0 / ops.lifetime_years) / 365.0
            + c.crew_size * ops.wage_cr_per_crew_day;
        for (const auto& [commodity, units] : ops.life_support_units_per_crew_day) cost += c.crew_size * units * price(commodity);
        s.daily_cost_cr = cost;
        const auto pos = c.id.rfind("_tank");
        const bool variant = pos != std::string::npos && pos + 5 < c.id.size() && std::isdigit(static_cast<unsigned char>(c.id[pos + 5]));
        s.tank_factor = variant ? std::stod(c.id.substr(pos + 5)) / 100.0 : 1.0;
        w.ships.push_back(s);
    }
    return w;
}

const ShipSpec& base_ship(const World& w, const std::string& hull) {
    for (const auto& s : w.ships) {
        if (s.id == hull) return s;
    }
    throw std::runtime_error("no ship " + hull);
}

// ---- Legs ----------------------------------------------------------------------------------------

struct Route {
    std::string from;
    std::string to;
};
const std::vector<Route> kRoutes {{"earth", "mars"}, {"mars", "earth"}, {"earth", "venus"}, {"earth", "mercury"},
    {"earth", "ceres"}, {"mars", "ceres"}, {"earth", "jupiter"}, {"jupiter", "saturn"}};

double orbital_period_s(const World& w, const std::string& body) {
    for (const auto& b : w.universe.bodies) {
        if (b.id == body) return b.orbit.orbital_period_s;
    }
    throw std::runtime_error("no body " + body);
}
double semi_major_au(const World& w, const std::string& body) {
    for (const auto& b : w.universe.bodies) {
        if (b.id == body) return b.orbit.semi_major_axis_m / kAu;
    }
    throw std::runtime_error("no body " + body);
}

g::State body_state(const World& w, const std::string& body, double t) {
    const double h = 600.0;
    const auto r = w.mechanics->get_body_position(body, t);
    const auto rp = w.mechanics->get_body_position(body, t + h);
    const auto rm = w.mechanics->get_body_position(body, t - h);
    const math::Vec3d v {(rp.x - rm.x) / (2 * h), (rp.y - rm.y) / (2 * h), (rp.z - rm.z) / (2 * h)};
    return w.units.to_normalised(r, v);
}

struct Leg {
    Route route;
    int index {0};          // departure index within the synodic period
    double depart_s {0.0};
    double phase_deg {0.0}; // destination's lead over the origin at departure
    g::State start;
    g::State dest;          // destination at departure
    g::Target target;
    double hohmann {0.0};   // normalised time
};

Leg make_leg(const World& w, const Route& route, int index, int count) {
    Leg leg;
    leg.route = route;
    leg.index = index;
    const double p1 = orbital_period_s(w, route.from), p2 = orbital_period_s(w, route.to);
    const double synodic = 1.0 / std::abs(1.0 / p1 - 1.0 / p2);
    leg.depart_s = synodic * index / count;
    leg.start = body_state(w, route.from, leg.depart_s);
    leg.dest = body_state(w, route.to, leg.depart_s);
    leg.target = g::coasting_target(leg.dest);
    double phase = std::atan2(leg.dest.r.y, leg.dest.r.x) - std::atan2(leg.start.r.y, leg.start.r.x);
    phase = std::fmod(phase + 4 * kPi, 2 * kPi);
    leg.phase_deg = phase * 180.0 / kPi;
    const double a = 0.5 * (semi_major_au(w, route.from) + semi_major_au(w, route.to));
    leg.hohmann = kPi * std::sqrt(a * a * a);
    return leg;
}

// ---- A ship on a leg -----------------------------------------------------------------------------

struct Loaded {
    double m0 {0.0};
    double s0 {0.0};          // normalised 2P/m0
    double budget_kg {0.0};   // burnable propellant (10% reserve)
    b::Band band;
};
Loaded loaded(const World& w, const ShipSpec& ship, double load_kg) {
    Loaded l;
    l.m0 = ship.dry_kg + ship.payload_kg + load_kg;
    l.s0 = 2.0 * ship.power_w / l.m0 / kSUnit;
    l.budget_kg = load_kg / 1.1;
    l.band = {w.v_min_mps / kSpeedUnit, w.v_max_mps / kSpeedUnit};
    return l;
}
double burn_kg(const Loaded& l, double s_end) { return l.m0 * (1.0 - l.s0 / s_end); }

const g::Propagator& ideal_propagator() {
    static const g::Propagator p;
    return p;
}

// One T: the ideal solution, approach A on it and the banded optimum B.
struct Point {
    double T {0.0};
    bool ideal_ok {false};
    g::Primer ideal;
    double ideal_kg {0.0};
    bool a_ok {false};        // throttled ideal path flies (floor respected)
    double a_kg {0.0};
    double a_floor {0.0};
    bool b_ok {false};
    b::Solution b;
    double b_kg {0.0};
    double ms_ideal {0.0}, ms_a {0.0}, ms_b {0.0};
};

struct Solver {
    const World& w;
    const Leg& leg;
    Loaded load;
    b::Propagator prop;
    b::Options options;
    g::SolveOptions ideal_options;

    Point at(double T, const g::Primer* ideal_warm, const g::Primer* banded_warm, bool want_b = true) const {
        Point p;
        p.T = T;
        double t0 = now_ms();
        const g::TransferRequest req {leg.start, leg.target, g::EndCondition::Rendezvous, kMinRadius};
        const auto ideal = g::solve_transfer(ideal_propagator(), req, T, ideal_options, ideal_warm);
        p.ms_ideal = now_ms() - t0;
        if (!ideal.converged || ideal.end.min_radius < kMinRadius) return p;
        p.ideal_ok = true;
        p.ideal = ideal.primer;
        p.ideal_kg = burn_kg(load, load.s0 + ideal.end.effort);
        t0 = now_ms();
        const auto followed = prop.follow_ideal(leg.start, ideal.primer, load.s0, load.band, T);
        p.ms_a = now_ms() - t0;
        p.a_floor = followed.floor_load;
        p.a_ok = followed.ok && followed.floor_ok();
        p.a_kg = burn_kg(load, followed.s_end);
        if (!want_b) return p;
        t0 = now_ms();
        const b::Request breq {leg.start, leg.target, load.s0, load.band, kMinRadius};
        p.b = b::solve_fixed_time(prop, breq, T, ideal.primer, options, banded_warm);
        p.ms_b = now_ms() - t0;
        p.b_ok = p.b.converged;
        if (p.b_ok) p.b_kg = burn_kg(load, p.b.end.s_flown);
        return p;
    }
};

// ---- Tuning --------------------------------------------------------------------------------------

struct TuneCase {
    const ShipSpec* ship;
    Leg leg;
    double T;
};

void run_tune(const World& w, const std::filesystem::path& out_dir, int only) {
    // Tuning set: every hull, every route, 6 departures, T at 0.25, 0.5 and 1 Hohmann.
    std::vector<TuneCase> cases;
    for (const auto& hull : kHulls) {
        for (const auto& route : kRoutes) {
            for (int k = 0; k < 6; ++k) {
                const Leg leg = make_leg(w, route, k * 6, 36);
                for (double f : {0.25, 0.5, 1.0}) cases.push_back({&base_ship(w, hull), leg, f * leg.hohmann});
            }
        }
    }
    struct Variant {
        std::string name;
        b::Settings settings;
        b::Options options;
    };
    std::vector<Variant> variants;
    {
        const Variant v {"default", {}, {}};
        variants.push_back(v);
        Variant exact_only = v;
        exact_only.name = "no smoothing fallback";
        exact_only.options.smoothing = 0.0;
        variants.push_back(exact_only);
        Variant fd = v;
        fd.name = "finite-difference Jacobian";
        fd.options.finite_difference = true;
        variants.push_back(fd);
        Variant d5 = v;
        d5.name = "DOPRI5 (1e-10)";
        d5.settings.high_order = false;
        variants.push_back(d5);
        Variant loose = v;
        loose.name = "DOP853 at 1e-9";
        loose.settings.relative_tolerance = 1e-9;
        loose.settings.absolute_tolerance = 1e-10;
        variants.push_back(loose);
        Variant emin = v;
        emin.name = "fallback smoothing down to 1e-3";
        emin.options.smoothing_min = 1e-3;
        variants.push_back(emin);
        Variant direct = v;
        direct.name = "band in one step";
        direct.options.first_step = 1.0;
        variants.push_back(direct);
        Variant nopred = v;
        nopred.name = "no secant predictor";
        nopred.options.predictor = false;
        variants.push_back(nopred);
    }
    std::ofstream csv(out_dir / std::format("tune_{}.csv", only));
    csv << "variant,ship,route,index,T_hohmann,ideal_ok,converged,exact,rejected,ms,propagations,iterations,steps,b_kg,a_kg,a_floor\n";
    std::printf("%-48s %6s %6s %5s %8s %8s %8s %7s\n", "variant", "solved", "exact", "of", "mean ms", "p90 ms", "props", "steps");
    for (std::size_t vi = 0; vi < variants.size(); ++vi) {
        if (only >= 0 && static_cast<int>(vi) != only) continue;
        const auto& v = variants[vi];
        int solved = 0, tried = 0, exact = 0;
        std::vector<double> ms;
        double props = 0, steps = 0;
        for (const auto& c : cases) {
            Solver solver {w, c.leg, loaded(w, *c.ship, c.ship->capacity_kg), b::Propagator(v.settings), v.options, {}};
            const auto p = solver.at(c.T, nullptr, nullptr);
            if (!p.ideal_ok) continue;
            ++tried;
            ms.push_back(p.ms_b);
            props += p.b.propagations;
            steps += p.b.continuation_steps;
            if (p.b_ok) ++solved;
            if (p.b_ok && p.b.exact) ++exact;
            csv << '"' << v.name << "\"," << c.ship->id << ',' << c.leg.route.from << '-' << c.leg.route.to << ',' << c.leg.index << ','
                << c.T / c.leg.hohmann << ',' << p.ideal_ok << ',' << p.b_ok << ',' << p.b.exact << ',' << p.b.rejected_worse << ',' << p.ms_b << ','
                << p.b.propagations << ',' << p.b.iterations << ',' << p.b.continuation_steps << ',' << p.b_kg << ',' << p.a_kg << ','
                << p.a_floor << '\n';
        }
        std::sort(ms.begin(), ms.end());
        double mean = 0;
        for (double x : ms) mean += x;
        mean /= std::max<std::size_t>(1, ms.size());
        std::printf("%-48s %6d %6d %5d %8.2f %8.2f %8.1f %7.1f\n", v.name.c_str(), solved, exact, tried, mean,
            ms.empty() ? 0.0 : ms[ms.size() * 9 / 10], props / std::max(1, tried), steps / std::max(1, tried));
        std::fflush(stdout);
    }
}

// ---- Study ---------------------------------------------------------------------------------------

// The transfer-time grid of a leg: log spaced over [max(0.05 T_H, 20 d), min(4 T_H, T_H + 2 y)].
std::vector<double> time_grid(const Leg& leg, std::size_t count = 44) {
    const double lo = std::max(0.05 * leg.hohmann, 20.0 * kDay / kTimeUnit);
    const double hi = std::min(4.0 * leg.hohmann, leg.hohmann + 730.0 * kDay / kTimeUnit);
    std::vector<double> out;
    for (std::size_t i = 0; i < count; ++i) out.push_back(lo * std::pow(hi / lo, static_cast<double>(i) / (count - 1)));
    return out;
}

// Ideal solutions over the grid (shared by every ship: the ideal path does not depend on the
// ship), walked outwards from the Hohmann time with warm starts.
struct IdealCurve {
    std::vector<double> T;
    std::vector<std::optional<g::Primer>> primer;
    std::vector<double> effort;
    std::vector<double> ms;
};

std::optional<g::Solution> ideal_at(const Leg& leg, double T, const g::Primer* warm) {
    const g::TransferRequest req {leg.start, leg.target, g::EndCondition::Rendezvous, kMinRadius};
    auto sol = g::solve_transfer(ideal_propagator(), req, T, {}, warm);
    if (!sol.converged || sol.end.min_radius < kMinRadius) {
        if (warm == nullptr) return std::nullopt;
        sol = g::solve_transfer(ideal_propagator(), req, T, {}, nullptr);
        if (!sol.converged || sol.end.min_radius < kMinRadius) return std::nullopt;
    }
    return sol;
}

IdealCurve ideal_curve(const Leg& leg) {
    IdealCurve c;
    c.T = time_grid(leg);
    const std::size_t n = c.T.size();
    c.primer.assign(n, std::nullopt);
    c.effort.assign(n, 0.0);
    c.ms.assign(n, 0.0);
    std::size_t mid = 0;
    while (mid + 1 < n && c.T[mid + 1] <= leg.hohmann) ++mid;
    for (int dir : {-1, 1}) {
        std::optional<g::Primer> warm;
        for (long i = static_cast<long>(mid) + (dir > 0 ? 1 : 0); i >= 0 && i < static_cast<long>(n); i += dir) {
            const double t0 = now_ms();
            const auto sol = ideal_at(leg, c.T[i], warm ? &*warm : nullptr);
            c.ms[i] = now_ms() - t0;
            if (sol) {
                c.primer[i] = sol->primer;
                c.effort[i] = sol->end.effort;
                warm = sol->primer;
            }
        }
    }
    return c;
}

// Hermite-interpolated position on a sampled path.
g::Vec2 position_at(const std::vector<b::Sample>& path, double t) {
    if (t <= path.front().t) return path.front().state.r;
    if (t >= path.back().t) return path.back().state.r;
    const auto it = std::upper_bound(path.begin(), path.end(), t, [](double v, const b::Sample& s) { return v < s.t; });
    const auto& p1 = *it;
    const auto& p0 = *(it - 1);
    const double h = p1.t - p0.t;
    if (h <= 0.0) return p1.state.r;
    const double s = (t - p0.t) / h, s2 = s * s, s3 = s2 * s;
    const double h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s, h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
    return {h00 * p0.state.r.x + h10 * h * p0.state.v.x + h01 * p1.state.r.x + h11 * h * p1.state.v.x,
        h00 * p0.state.r.y + h10 * h * p0.state.v.y + h01 * p1.state.r.y + h11 * h * p1.state.v.y};
}
const b::Sample& sample_before(const std::vector<b::Sample>& path, double t) {
    auto it = std::upper_bound(path.begin(), path.end(), t, [](double v, const b::Sample& s) { return v < s.t; });
    if (it == path.begin()) return path.front();
    return *(it - 1);
}

struct Outcome {
    bool ok {false};
    double kg {0.0};
    bool floor {false};   // A: the floor, not the fuel, rules this T out
};

struct ShipOnLeg {
    const World& w;
    const Leg& leg;
    const IdealCurve& ideal;
    const ShipSpec& ship;
    b::Propagator prop;
    b::Options options;
    // statistics of the banded solves
    std::size_t b_solves {0}, b_fail {0}, b_cold {0};
    double b_ms {0.0}, a_ms {0.0};
    std::size_t a_runs {0};

    Outcome a_at(const Loaded& load, double T, const g::Primer& primer, std::vector<b::Sample>* samples = nullptr) {
        const double t0 = now_ms();
        const auto f = prop.follow_ideal(leg.start, primer, load.s0, load.band, T, samples);
        a_ms += now_ms() - t0;
        ++a_runs;
        Outcome o;
        o.kg = burn_kg(load, f.s_end);
        o.floor = !f.floor_ok();
        o.ok = f.ok && !o.floor;
        return o;
    }
    std::optional<b::Solution> b_at(const Loaded& load, double T, const g::Primer& primer, const g::Primer* warm) {
        const double t0 = now_ms();
        const b::Request req {leg.start, leg.target, load.s0, load.band, kMinRadius};
        auto sol = b::solve_fixed_time(prop, req, T, primer, options, warm);
        b_ms += now_ms() - t0;
        ++b_solves;
        if (!sol.from_warm) ++b_cold;
        if (!sol.converged) {
            ++b_fail;
            return std::nullopt;
        }
        return sol;
    }
};

// Full-tank curves of one ship on one leg.
struct Curves {
    Loaded load;
    std::vector<Outcome> a;
    std::vector<Outcome> b;
    std::vector<std::optional<b::Solution>> bsol;
};

Curves full_tank_curves(ShipOnLeg& sl, const Loaded& load) {
    const auto& c = sl.ideal;
    const std::size_t n = c.T.size();
    Curves cv;
    cv.load = load;
    cv.a.assign(n, {});
    cv.b.assign(n, {});
    cv.bsol.assign(n, std::nullopt);
    std::size_t mid = 0;
    while (mid + 1 < n && c.T[mid + 1] <= sl.leg.hohmann) ++mid;
    for (int dir : {-1, 1}) {
        std::optional<g::Primer> warm;
        int misses = 0;
        for (long i = static_cast<long>(mid) + (dir > 0 ? 1 : 0); i >= 0 && i < static_cast<long>(n); i += dir) {
            if (!c.primer[i]) continue;
            const double ideal_kg = burn_kg(load, load.s0 + c.effort[i]);
            // Both approaches burn at least the ideal: past the budget neither can fly.
            if (ideal_kg > load.budget_kg) {
                if (dir < 0) break;
                continue;
            }
            cv.a[i] = sl.a_at(load, c.T[i], *c.primer[i]);
            const auto sol = sl.b_at(load, c.T[i], *c.primer[i], warm ? &*warm : nullptr);
            if (sol) {
                cv.b[i] = {true, burn_kg(load, sol->end.s_flown), false};
                cv.bsol[i] = sol;
                warm = sol->primer;
                misses = 0;
            } else if (dir < 0 && ++misses >= 3) {
                break;
            }
        }
    }
    return cv;
}

// Shortest feasible time: bracket on the grid, then bisection (Illinois on the fuel where both
// ends are flyable) with warm starts from the feasible side.
struct MinTime {
    bool ok {false};
    bool at_grid_floor {false};
    double T {0.0};
    double kg {0.0};
    bool limited_by_floor {false};   // A: what rules out the next shorter time
    std::optional<b::Solution> bsol;
    g::Primer ideal;
};

MinTime min_time(ShipOnLeg& sl, const Curves& cv, bool banded) {
    const auto& c = sl.ideal;
    const auto& outs = banded ? cv.b : cv.a;
    const double budget = cv.load.budget_kg;
    const auto feasible = [&](const Outcome& o) { return o.ok && o.kg <= budget; };
    MinTime m;
    long lowest = -1;
    for (long i = 0; i < static_cast<long>(outs.size()); ++i) {
        if (feasible(outs[i])) {
            lowest = i;
            break;
        }
    }
    if (lowest < 0) return m;
    m.ok = true;
    m.T = c.T[lowest];
    m.kg = outs[lowest].kg;
    m.ideal = *c.primer[lowest];
    if (banded) m.bsol = cv.bsol[lowest];
    if (lowest == 0) {
        m.at_grid_floor = true;
        return m;
    }
    m.limited_by_floor = !banded && outs[lowest - 1].floor;
    double t_bad = c.T[lowest - 1], t_good = c.T[lowest];
    g::Primer ideal_warm = *c.primer[lowest];
    std::optional<g::Primer> b_warm;
    if (banded && cv.bsol[lowest]) b_warm = cv.bsol[lowest]->primer;
    for (int it = 0; it < 10 && (t_good - t_bad) > 2e-4 * t_good; ++it) {
        const double T = std::sqrt(t_bad * t_good);
        const auto ideal = ideal_at(sl.leg, T, &ideal_warm);
        bool ok = false;
        double kg = 0.0;
        bool floor = false;
        std::optional<b::Solution> sol;
        if (ideal) {
            if (banded) {
                sol = sl.b_at(cv.load, T, ideal->primer, b_warm ? &*b_warm : nullptr);
                ok = sol.has_value();
                if (ok) kg = burn_kg(cv.load, sol->end.s_flown);
            } else {
                const auto o = sl.a_at(cv.load, T, ideal->primer);
                ok = o.ok;
                kg = o.kg;
                floor = o.floor;
            }
        }
        if (ok && kg <= budget) {
            t_good = T;
            m.T = T;
            m.kg = kg;
            m.ideal = ideal->primer;
            ideal_warm = ideal->primer;
            if (banded) {
                m.bsol = sol;
                b_warm = sol->primer;
            }
        } else {
            t_bad = T;
            if (!banded) m.limited_by_floor = floor;
        }
    }
    return m;
}

// Economic optimum with the fuel load solved for (the ship tanks 1.1 × the burn, up to its tank):
// minimum fuel cost + time cost over the grid, refined by a parabola through the best three.
struct Economic {
    bool ok {false};
    double T {0.0};
    double kg {0.0};
    double load {0.0};
    double cost {0.0};
};

Economic economic(ShipOnLeg& sl, const Curves& cv, bool banded) {
    const auto& c = sl.ideal;
    const std::size_t n = c.T.size();
    std::vector<double> cost(n, std::numeric_limits<double>::infinity()), kgs(n, 0.0), loads(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& o = banded ? cv.b[i] : cv.a[i];
        if (!o.ok || o.kg > cv.load.budget_kg) continue;
        double kg = o.kg;
        double load = std::min(sl.ship.capacity_kg, 1.1 * kg);
        std::optional<g::Primer> warm;
        if (banded) warm = cv.bsol[i]->primer;
        bool ok = true;
        for (int it = 0; it < 3; ++it) {
            const auto l = loaded(sl.w, sl.ship, load);
            if (banded) {
                const auto sol = sl.b_at(l, c.T[i], *c.primer[i], &*warm);
                if (!sol) {
                    ok = false;
                    break;
                }
                warm = sol->primer;
                kg = burn_kg(l, sol->end.s_flown);
            } else {
                const auto a = sl.a_at(l, c.T[i], *c.primer[i]);
                if (!a.ok) {
                    ok = false;
                    break;
                }
                kg = a.kg;
            }
            const double next = std::min(sl.ship.capacity_kg, 1.1 * kg);
            if (std::abs(next - load) < 1.0) {
                load = next;
                break;
            }
            load = next;
        }
        if (!ok || 1.1 * kg > sl.ship.capacity_kg + 1.0) continue;
        kgs[i] = kg;
        loads[i] = load;
        cost[i] = kg * sl.w.fuel_cr_per_kg + c.T[i] * kTimeUnit / kDay * sl.ship.daily_cost_cr;
    }
    Economic e;
    std::size_t best = n;
    for (std::size_t i = 0; i < n; ++i) {
        if (std::isfinite(cost[i]) && (best == n || cost[i] < cost[best])) best = i;
    }
    if (best == n) return e;
    e.ok = true;
    e.T = c.T[best];
    e.kg = kgs[best];
    e.load = loads[best];
    e.cost = cost[best];
    if (best > 0 && best + 1 < n && std::isfinite(cost[best - 1]) && std::isfinite(cost[best + 1])) {
        // Parabola in log T through the three points.
        const double x0 = std::log(c.T[best - 1]), x1 = std::log(c.T[best]), x2 = std::log(c.T[best + 1]);
        const double y0 = cost[best - 1], y1 = cost[best], y2 = cost[best + 1];
        const double den = (x0 - x1) * (x0 - x2) * (x1 - x2);
        const double A = (x2 * (y1 - y0) + x1 * (y0 - y2) + x0 * (y2 - y1)) / den;
        const double B = (x2 * x2 * (y0 - y1) + x1 * x1 * (y2 - y0) + x0 * x0 * (y1 - y2)) / den;
        if (A > 0.0) {
            const double xv = std::clamp(-B / (2 * A), x0, x2);
            const double yv = y1 + (A * (xv * xv - x1 * x1) + B * (xv - x1));
            const double f = (xv - x1) / ((xv > x1 ? x2 : x0) - x1);
            const std::size_t other = xv > x1 ? best + 1 : best - 1;
            e.T = std::exp(xv);
            e.cost = std::min(yv, cost[best]);
            e.kg = kgs[best] + f * (kgs[other] - kgs[best]);
            e.load = loads[best] + f * (loads[other] - loads[best]);
        }
    }
    return e;
}

struct PathMetrics {
    double sep_max_km {0.0};
    double sep_rms_km {0.0};
    double frac[4] {0.0, 0.0, 0.0, 0.0};
    std::size_t switches {0};
};

// A and B at the same T: dense samples, separation, and (when asked) rows for the path plots.
PathMetrics compare_paths(ShipOnLeg& sl, const Loaded& load, double T, const g::Primer& ideal, const b::Solution& bsol,
    std::string* rows, const std::string& key) {
    b::Settings dense = sl.prop.settings();
    dense.max_step = T / 400.0;
    const b::Propagator prop(dense);
    std::vector<b::Sample> pa, pb;
    prop.follow_ideal(sl.leg.start, ideal, load.s0, load.band, T, &pa);
    const auto end = prop.propagate(sl.leg.start, bsol.primer, load.s0, load.band, T, false, &pb);
    PathMetrics m;
    for (int k = 0; k < 4; ++k) m.frac[k] = end.time_on_arc[k] / T;
    m.switches = end.switches.size();
    double sum2 = 0.0;
    const int N = 400;
    for (int k = 0; k <= N; ++k) {
        const double t = T * k / N;
        const auto ra = position_at(pa, t), rb = position_at(pb, t);
        const double d = std::hypot(ra.x - rb.x, ra.y - rb.y) * kAu / 1000.0;
        m.sep_max_km = std::max(m.sep_max_km, d);
        sum2 += d * d;
        if (rows) {
            for (int which = 0; which < 2; ++which) {
                const auto& path = which == 0 ? pa : pb;
                const auto r = which == 0 ? ra : rb;
                const auto& smp = sample_before(path, t);
                const double a = std::hypot(smp.thrust.x, smp.thrust.y);
                const double m_kg = 2.0 * sl.ship.power_w / (smp.s * kSUnit);
                const double ve = a > 0.0 ? smp.s / a * kSpeedUnit / 1000.0 : 0.0;
                char buf[400];
                std::snprintf(buf, sizeof buf, "%s,%c,%.4f,%.7f,%.7f,%.6e,%.6e,%.6e,%.3f,%.2f,%d\n", key.c_str(), which == 0 ? 'A' : 'B',
                    t * kTimeUnit / kDay, r.x, r.y, a * kAccelUnit, smp.s / load.band.v_max * kAccelUnit,
                    smp.s / load.band.v_min * kAccelUnit, ve, m_kg, static_cast<int>(smp.arc));
                *rows += buf;
            }
        }
    }
    m.sep_rms_km = std::sqrt(sum2 / (N + 1));
    return m;
}

// Resume: legs listed in `skip` (a previous results.csv) are left out; on routes with no finished leg
// only every `stride`-th departure is run.
void run_study(const World& w, const std::filesystem::path& out_dir, int departures, unsigned threads, const std::string& skip,
    int stride) {
    std::set<std::string> done_legs, done_routes;
    if (!skip.empty()) {
        std::ifstream in(skip);
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            std::vector<std::string> f;
            std::stringstream ss(line);
            for (std::string x; std::getline(ss, x, ',');) f.push_back(x);
            if (f.size() > 4) {
                done_legs.insert(f[3] + "," + f[4]);
                done_routes.insert(f[3]);
            }
        }
    }
    std::vector<Leg> legs;
    for (const auto& route : kRoutes) {
        const std::string name = route.from + "-" + route.to;
        for (int k = 0; k < departures; ++k) {
            if (done_legs.contains(name + "," + std::to_string(k))) continue;
            if (!done_routes.contains(name) && k % std::max(1, stride) != 0) continue;
            legs.push_back(make_leg(w, route, k, departures));
        }
    }
    {
        std::ofstream ships(out_dir / "ships.csv");
        ships << "ship,hull,tank,dry_kg,capacity_kg,power_w,payload_kg,m0_kg,a_full_power_250_mps2,a_full_power_50_mps2,daily_cost,fuel_cr_per_kg\n";
        for (const auto& sp : w.ships) {
            const double m0 = sp.dry_kg + sp.payload_kg + sp.capacity_kg;
            ships << std::format("{},{},{},{:.0f},{:.0f},{:.0f},{:.0f},{:.0f},{:.5e},{:.5e},{:.2f},{:.4f}\n", sp.id, sp.hull, sp.tank_factor,
                sp.dry_kg, sp.capacity_kg, sp.power_w, sp.payload_kg, m0, 2.0 * sp.power_w / (m0 * w.v_max_mps),
                2.0 * sp.power_w / (m0 * w.v_min_mps), sp.daily_cost_cr, w.fuel_cr_per_kg);
        }
    }
    std::ofstream results(out_dir / "results.csv");
    results << "ship,hull,tank,route,index,phase_deg,hohmann_d,"
               "A_ok,A_grid_floor,A_tmin_d,A_kg,A_floor_limited,B_ok,B_grid_floor,B_tmin_d,B_kg,"
               "same_T_d,same_A_kg,same_B_kg,same_ideal_kg,coast_frac,ceiling_frac,interior_frac,floor_frac,switches,sep_max_km,sep_rms_km,"
               "econA_ok,econA_T_d,econA_kg,econA_load,econA_cost,econB_ok,econB_T_d,econB_kg,econB_load,econB_cost,"
               "budget_kg,daily_cost,b_solves,b_fail,b_cold,b_ms,a_runs,a_ms\n";
    std::ofstream curves(out_dir / "curves.csv");
    curves << "ship,route,index,phase_deg,T_d,ideal_kg,A_ok,A_kg,B_ok,B_kg,coast_frac,ceiling_frac,floor_frac\n";
    std::ofstream paths(out_dir / "paths.csv");
    paths << "ship,route,index,approach,t_d,x_au,y_au,a_mps2,amin_mps2,amax_mps2,ve_kms,m_kg,arc\n";
    std::ofstream ideal_timing(out_dir / "ideal_curves.csv");
    ideal_timing << "route,index,T_d,ok,ms\n";
    std::mutex io;
    std::atomic<std::size_t> next {0}, done {0};
    const auto worker = [&]() {
        for (;;) {
            const std::size_t li = next++;
            if (li >= legs.size()) return;
            const Leg& leg = legs[li];
            const auto ic = ideal_curve(leg);
            std::string res_rows, curve_rows, path_rows, ideal_rows;
            for (std::size_t i = 0; i < ic.T.size(); ++i) {
                ideal_rows += std::format("{}-{},{},{:.3f},{},{:.3f}\n", leg.route.from, leg.route.to, leg.index, ic.T[i] * kTimeUnit / kDay,
                    ic.primer[i].has_value(), ic.ms[i]);
            }
            for (const auto& ship : w.ships) {
                ShipOnLeg sl {w, leg, ic, ship, b::Propagator {}, b::Options {}};
                const auto load = loaded(w, ship, ship.capacity_kg);
                const auto cv = full_tank_curves(sl, load);
                const auto ma = min_time(sl, cv, false);
                const auto mb = min_time(sl, cv, true);
                const bool base = ship.tank_factor == 1.0;
                const std::string key = std::format("{},{}-{},{}", ship.hull, leg.route.from, leg.route.to, leg.index);
                // Same T: A's shortest time if A flies, else B's.
                double same_T = 0.0, same_a = 0.0, same_b = 0.0, same_ideal = 0.0;
                PathMetrics pm;
                if (ma.ok || mb.ok) {
                    same_T = ma.ok ? ma.T : mb.T;
                    const g::Primer ideal = ma.ok ? ma.ideal : mb.ideal;
                    const auto idsol = ideal_at(leg, same_T, &ideal);
                    if (idsol) {
                        same_ideal = burn_kg(load, load.s0 + idsol->end.effort);
                        same_a = sl.a_at(load, same_T, idsol->primer).kg;
                        std::optional<b::Solution> bs;
                        if (!ma.ok) {
                            bs = mb.bsol;
                        } else {
                            // Warm from the banded solution nearest in T.
                            std::optional<g::Primer> warm;
                            if (mb.ok && mb.bsol) warm = mb.bsol->primer;
                            bs = sl.b_at(load, same_T, idsol->primer, warm ? &*warm : nullptr);
                        }
                        if (bs) {
                            same_b = burn_kg(load, bs->end.s_flown);
                            const bool keep = base && leg.index % std::max(1, departures / 4) == 0;
                            pm = compare_paths(sl, load, same_T, idsol->primer, *bs, keep ? &path_rows : nullptr, key);
                        }
                    }
                }
                const auto ea = economic(sl, cv, false);
                const auto eb = economic(sl, cv, true);
                const double day = kTimeUnit / kDay;
                res_rows += std::format(
                    "{},{},{},{}-{},{},{:.2f},{:.2f},{},{},{:.3f},{:.1f},{},{},{},{:.3f},{:.1f},{:.3f},{:.1f},{:.1f},{:.1f},{:.4f},{:.4f},{:.4f},{:.4f},{},{:.1f},{:.1f},"
                    "{},{:.3f},{:.1f},{:.1f},{:.1f},{},{:.3f},{:.1f},{:.1f},{:.1f},{:.1f},{:.2f},{},{},{},{:.2f},{},{:.2f}\n",
                    ship.id, ship.hull, ship.tank_factor, leg.route.from, leg.route.to, leg.index, leg.phase_deg, leg.hohmann * day,
                    ma.ok, ma.at_grid_floor, ma.T * day, ma.kg, ma.limited_by_floor, mb.ok, mb.at_grid_floor, mb.T * day, mb.kg,
                    same_T * day, same_a, same_b, same_ideal, pm.frac[0], pm.frac[1], pm.frac[2], pm.frac[3], pm.switches, pm.sep_max_km,
                    pm.sep_rms_km, ea.ok, ea.T * day, ea.kg, ea.load, ea.cost, eb.ok, eb.T * day, eb.kg, eb.load, eb.cost, load.budget_kg,
                    ship.daily_cost_cr, sl.b_solves, sl.b_fail, sl.b_cold, sl.b_ms, sl.a_runs, sl.a_ms);
                if (base) {
                    for (std::size_t i = 0; i < ic.T.size(); ++i) {
                        if (!ic.primer[i]) continue;
                        double fr[4] = {0, 0, 0, 0};
                        if (cv.bsol[i]) {
                            for (int k = 0; k < 4; ++k) fr[k] = cv.bsol[i]->end.time_on_arc[k] / ic.T[i];
                        }
                        curve_rows += std::format("{},{:.2f},{:.3f},{:.1f},{},{:.1f},{},{:.1f},{:.4f},{:.4f},{:.4f}\n", key, leg.phase_deg,
                            ic.T[i] * day, burn_kg(load, load.s0 + ic.effort[i]), cv.a[i].ok, cv.a[i].kg, cv.b[i].ok, cv.b[i].kg, fr[0], fr[1],
                            fr[3]);
                    }
                }
            }
            std::lock_guard lock(io);
            results << res_rows;
            curves << curve_rows;
            paths << path_rows;
            ideal_timing << ideal_rows;
            results.flush();
            std::fprintf(stderr, "\r%zu/%zu legs", ++done, legs.size());
        }
    };
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < threads; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    std::fprintf(stderr, "\n");
}

// ---- Timing (one thread) -------------------------------------------------------------------------

// Earliest arrival for a full tank from scratch, as a planner would search it: the ideal minimum
// time for the budget (a lower bound for both approaches), then T up by 5% steps with warm starts
// until the approach fits, then bisection to 0.1%.
struct ScratchResult {
    bool ok {false};
    double T {0.0};
    double ms {0.0};
    std::size_t solves {0};
};

void run_timing(const World& w, const std::filesystem::path& out_dir, int departures) {
    std::ofstream csv(out_dir / "timing.csv");
    csv << "ship,route,index,ideal_ok,ideal_T_d,ideal_ms,A_ok,A_T_d,A_ms,A_solves,B_ok,B_T_d,B_ms,B_solves,B_exact,"
           "cold_ms,warm_ms,B_check_miss_km,B_check_dkg\n";
    for (const auto& hull : kHulls) {
        const auto& ship = base_ship(w, hull);
        for (const auto& route : kRoutes) {
            for (int k = 0; k < departures; ++k) {
                const Leg leg = make_leg(w, route, k * (36 / departures), 36);
                const auto load = loaded(w, ship, ship.capacity_kg);
                const b::Propagator prop;
                const b::Options options;
                const g::TransferRequest req {leg.start, leg.target, g::EndCondition::Rendezvous, kMinRadius};
                // Ideal minimum time for the budget: J = s_f − s0.
                const double s_budget = load.s0 / (1.0 - load.budget_kg / load.m0);
                const double t_lo = std::max(0.05 * leg.hohmann, 20.0 * kDay / kTimeUnit);
                const double t_hi = std::min(4.0 * leg.hohmann, leg.hohmann + 730.0 * kDay / kTimeUnit);
                double t0 = now_ms();
                const auto ideal = g::solve_min_time(ideal_propagator(), req, s_budget - load.s0, t_lo, t_hi);
                const double ideal_ms = now_ms() - t0;
                const bool ideal_ok = ideal.converged && ideal.end.min_radius >= kMinRadius;
                ScratchResult ra, rb;
                double cold_ms = 0.0, warm_ms = 0.0, miss_km = 0.0, dkg = 0.0;
                bool b_exact = false;
                if (ideal_ok) {
                    for (int banded = 0; banded < 2; ++banded) {
                        ScratchResult& r = banded ? rb : ra;
                        t0 = now_ms();
                        g::Primer iw = ideal.primer;
                        std::optional<g::Primer> bw;
                        double T = ideal.T, t_bad = 0.0, t_good = 0.0;
                        std::optional<b::Solution> best;
                        const auto try_at = [&](double TT) -> bool {
                            const auto id = ideal_at(leg, TT, &iw);
                            ++r.solves;
                            if (!id) return false;
                            iw = id->primer;
                            if (!banded) {
                                const auto f = prop.follow_ideal(leg.start, id->primer, load.s0, load.band, TT);
                                return f.ok && f.floor_ok() && burn_kg(load, f.s_end) <= load.budget_kg;
                            }
                            const b::Request breq {leg.start, leg.target, load.s0, load.band, kMinRadius};
                            const double c0 = now_ms();
                            auto sol = b::solve_fixed_time(prop, breq, TT, id->primer, options, bw ? &*bw : nullptr);
                            if (!bw) cold_ms = now_ms() - c0;
                            if (!sol.converged) return false;
                            bw = sol.primer;
                            const bool fits = burn_kg(load, sol.end.s_flown) <= load.budget_kg;
                            if (fits) best = sol;
                            return fits;
                        };
                        bool found = false;
                        for (int step = 0; step < 40 && T <= t_hi * 1.0001; ++step) {
                            if (try_at(T)) {
                                found = true;
                                t_good = T;
                                break;
                            }
                            t_bad = T;
                            T *= 1.05;
                        }
                        if (found && t_bad > 0.0) {
                            for (int it = 0; it < 6; ++it) {
                                const double mid = std::sqrt(t_bad * t_good);
                                (try_at(mid) ? t_good : t_bad) = mid;
                            }
                        }
                        r.ms = now_ms() - t0;
                        r.ok = found;
                        r.T = t_good;
                        if (banded && found && best) {
                            b_exact = best->exact;
                            // Warm solve at a 2% longer time (a neighbouring query).
                            const auto id = ideal_at(leg, t_good * 1.02, &iw);
                            if (id) {
                                const b::Request breq {leg.start, leg.target, load.s0, load.band, kMinRadius};
                                const double c0 = now_ms();
                                const auto sol = b::solve_fixed_time(prop, breq, t_good * 1.02, id->primer, options, &best->primer);
                                warm_ms = sol.converged ? now_ms() - c0 : -1.0;
                            }
                            // Independent check at 1e-12: end miss and fuel change.
                            b::Settings tight;
                            tight.relative_tolerance = 1e-12;
                            tight.absolute_tolerance = 1e-13;
                            const auto chk = b::Propagator(tight).propagate(leg.start, best->primer, load.s0, load.band, best->T, false,
                                nullptr, best->exact ? 0.0 : options.smoothing_min);
                            const auto tp = leg.target(best->T);
                            miss_km = std::hypot(chk.state.r.x - tp.state.r.x, chk.state.r.y - tp.state.r.y) * kAu / 1000.0;
                            dkg = burn_kg(load, chk.s_flown) - burn_kg(load, best->end.s_flown);
                        }
                    }
                }
                const double day = kTimeUnit / kDay;
                csv << std::format("{},{}-{},{},{},{:.3f},{:.2f},{},{:.3f},{:.2f},{},{},{:.3f},{:.2f},{},{},{:.2f},{:.2f},{:.4g},{:.4g}\n", hull,
                    route.from, route.to, leg.index, ideal_ok, ideal.T * day, ideal_ms, ra.ok, ra.T * day, ra.ms, ra.solves, rb.ok, rb.T * day,
                    rb.ms, rb.solves, b_exact, cold_ms, warm_ms, miss_km, dkg);
                csv.flush();
            }
        }
        std::fprintf(stderr, "timing: %s done\n", hull.c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s tune|study <out_dir>\n", argv[0]);
        return 2;
    }
    const std::string mode = argv[1];
    const std::filesystem::path out_dir = argv[2];
    std::filesystem::create_directories(out_dir);
    const World w = load_world();
    if (mode == "tune") {
        run_tune(w, out_dir, argc > 3 ? std::stoi(argv[3]) : -1);
        return 0;
    }
    if (mode == "trace" && argc >= 8) {
        // trace <out> <from> <to> <index> <ship> <T/T_H>
        const Leg leg = make_leg(w, {argv[3], argv[4]}, std::stoi(argv[5]), 36);
        const std::string hull = argv[6];
        const ShipSpec& ship = base_ship(w, hull);
        b::Options options;
        options.trace = true;
        Solver solver {w, leg, loaded(w, ship, ship.capacity_kg), b::Propagator {}, options, {}};
        const auto p = solver.at(std::stod(argv[7]) * leg.hohmann, nullptr, nullptr);
        std::printf("ideal %d A %d (%.0f kg, floor %.3f) B %d (%.0f kg) %.1f ms, s0 %.4f a_min0 %.4g m/s2\n", p.ideal_ok, p.a_ok, p.a_kg,
            p.a_floor, p.b_ok, p.b_kg, p.ms_b, solver.load.s0, solver.load.s0 / solver.load.band.v_max * kAccelUnit);
        if (argc >= 9 && p.ideal_ok) {
            // Profile q/a_min (and q/a_max) of the smoothed solution at the last ε.
            b::Options o = options;
            o.trace = false;
            o.exact_polish = false;
            const double T = std::stod(argv[7]) * leg.hohmann;
            const b::Request req {leg.start, leg.target, solver.load.s0, solver.load.band, kMinRadius};
            const auto sm = b::solve_fixed_time(solver.prop, req, T, p.ideal, o);
            std::printf("smoothed converged %d s %.6f\n", sm.converged, sm.end.s);
            b::Settings st;
            st.max_step = T / 300;
            std::vector<b::Sample> smp;
            b::Propagator(st).propagate(leg.start, sm.primer, solver.load.s0, solver.load.band, T, false, &smp, o.smoothing_min);
            for (std::size_t i = 0; i < smp.size(); i += 3) {
                const double q = std::hypot(smp[i].thrust.x, smp[i].thrust.y);
                std::printf("%.3f:%.4f ", smp[i].t / T, q / (smp[i].s / solver.load.band.v_max));
            }
            std::printf("\n");
        }
        return 0;
    }
    if (mode == "timing") {
        run_timing(w, out_dir, argc > 3 ? std::stoi(argv[3]) : 6);
        return 0;
    }
    if (mode == "study") {
        const int departures = argc > 3 ? std::stoi(argv[3]) : 36;
        const unsigned threads = argc > 4 ? static_cast<unsigned>(std::stoi(argv[4])) : std::max(1u, std::thread::hardware_concurrency());
        run_study(w, out_dir, departures, threads, argc > 5 ? argv[5] : "", argc > 6 ? std::stoi(argv[6]) : 1);
        return 0;
    }
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 2;
}
