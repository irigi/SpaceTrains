#include "variable_isp/GeneralTransfer.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <mutex>

namespace spacetrains::variable_isp::general {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 2.0 * kPi;

// ---- Small dense linear algebra --------------------------------------------------------------

// Solves a·x = b in place by Gaussian elimination with partial pivoting (a is n x n, row-major).
bool solve_dense(std::vector<double>& a, std::vector<double>& b, std::size_t n) {
    for (std::size_t col = 0; col < n; ++col) {
        std::size_t pivot = col;
        for (std::size_t row = col + 1; row < n; ++row) {
            if (std::abs(a[row * n + col]) > std::abs(a[pivot * n + col])) pivot = row;
        }
        if (!(std::abs(a[pivot * n + col]) > 1e-300)) return false;
        if (pivot != col) {
            for (std::size_t k = 0; k < n; ++k) std::swap(a[col * n + k], a[pivot * n + k]);
            std::swap(b[col], b[pivot]);
        }
        const double inv = 1.0 / a[col * n + col];
        for (std::size_t row = col + 1; row < n; ++row) {
            const double factor = a[row * n + col] * inv;
            if (factor == 0.0) continue;
            for (std::size_t k = col; k < n; ++k) a[row * n + k] -= factor * a[col * n + k];
            b[row] -= factor * b[col];
        }
    }
    for (std::size_t row = n; row-- > 0;) {
        double sum = b[row];
        for (std::size_t k = row + 1; k < n; ++k) sum -= a[row * n + k] * b[k];
        b[row] = sum / a[row * n + row];
    }
    return true;
}

// Levenberg-Marquardt step for residual f (m) and Jacobian jac (m x n, row-major):
// (JᵀJ + λ diag(JᵀJ)) dx = −Jᵀf. Returns false if singular.
bool lm_step(const std::vector<double>& jac, const std::vector<double>& f, std::size_t m, std::size_t n,
    double lambda, std::vector<double>& dx) {
    std::vector<double> a(n * n, 0.0);
    dx.assign(n, 0.0);
    for (std::size_t r = 0; r < m; ++r) {
        const double* row = &jac[r * n];
        for (std::size_t i = 0; i < n; ++i) {
            if (row[i] == 0.0) continue;
            dx[i] -= row[i] * f[r];
            for (std::size_t j = 0; j < n; ++j) a[i * n + j] += row[i] * row[j];
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        a[i * n + i] += lambda * std::max(a[i * n + i], 1e-12);
    }
    return solve_dense(a, dx, n);
}

// Minimum-norm (damped) Gauss-Newton step for an underdetermined system (m < n):
// dx = −Jᵀ (JJᵀ + λ I)⁻¹ f.
bool min_norm_step(const std::vector<double>& jac, const std::vector<double>& f, std::size_t m, std::size_t n,
    double lambda, std::vector<double>& dx) {
    std::vector<double> a(m * m, 0.0);
    std::vector<double> y(f);
    for (std::size_t i = 0; i < m; ++i) {
        for (std::size_t j = 0; j < m; ++j) {
            for (std::size_t c = 0; c < n; ++c) a[i * m + j] += jac[i * n + c] * jac[j * n + c];
        }
    }
    for (std::size_t i = 0; i < m; ++i) a[i * m + i] *= 1.0 + lambda;
    if (!solve_dense(a, y, m)) return false;
    dx.assign(n, 0.0);
    for (std::size_t c = 0; c < n; ++c) {
        for (std::size_t r = 0; r < m; ++r) dx[c] -= jac[r * n + c] * y[r];
    }
    return true;
}

// ---- Dynamics ----------------------------------------------------------------------------------

// y: r(0,1) v(2,3) p(4,5) w(6,7) E(8) sweep(9), then the 9x4 transition matrix column-major
// by unknown: entries 10 + 9*c + row.
constexpr std::size_t kBase = 10;
constexpr std::size_t kWithStm = kBase + 36;

template <std::size_t N>
using Vec = std::array<double, N>;

template <std::size_t N>
void rhs(const Vec<N>& y, Vec<N>& dy) {
    const double x = y[0];
    const double z = y[1];
    const double r2 = x * x + z * z;
    const double rr = std::sqrt(r2);
    const double inv3 = 1.0 / (r2 * rr);
    const double inv5 = inv3 / r2;
    const double px = y[4];
    const double pz = y[5];
    const double rp = x * px + z * pz;

    dy[0] = y[2];
    dy[1] = y[3];
    dy[2] = -x * inv3 + px;
    dy[3] = -z * inv3 + pz;
    dy[4] = y[6];
    dy[5] = y[7];
    // w' = G p, G = −I/r³ + 3 r rᵀ/r⁵
    dy[6] = -px * inv3 + 3.0 * x * rp * inv5;
    dy[7] = -pz * inv3 + 3.0 * z * rp * inv5;
    dy[8] = px * px + pz * pz;
    dy[9] = (x * y[3] - z * y[2]) / r2;

    if constexpr (N == kWithStm) {
        const double inv7 = inv5 / r2;
        for (std::size_t c = 0; c < 4; ++c) {
            const double* s = &y[kBase + 9 * c];
            double* ds = &dy[kBase + 9 * c];
            const double drx = s[0], drz = s[1];
            const double dpx = s[4], dpz = s[5];
            const double r_dr = x * drx + z * drz;
            const double r_dp = x * dpx + z * dpz;
            const double dr_p = drx * px + drz * pz;
            ds[0] = s[2];
            ds[1] = s[3];
            // δv' = G δr + δp
            ds[2] = -drx * inv3 + 3.0 * x * r_dr * inv5 + dpx;
            ds[3] = -drz * inv3 + 3.0 * z * r_dr * inv5 + dpz;
            ds[4] = s[6];
            ds[5] = s[7];
            // δw' = G δp + H[δr] p
            const double hx = 3.0 * inv5 * (px * r_dr + drx * rp + x * dr_p) - 15.0 * inv7 * x * rp * r_dr;
            const double hz = 3.0 * inv5 * (pz * r_dr + drz * rp + z * dr_p) - 15.0 * inv7 * z * rp * r_dr;
            ds[6] = -dpx * inv3 + 3.0 * x * r_dp * inv5 + hx;
            ds[7] = -dpz * inv3 + 3.0 * z * r_dp * inv5 + hz;
            ds[8] = 2.0 * (px * dpx + pz * dpz);
        }
    }
}

// Dormand-Prince 5(4) tableau.
constexpr double a21 = 1.0 / 5.0;
constexpr double a31 = 3.0 / 40.0, a32 = 9.0 / 40.0;
constexpr double a41 = 44.0 / 45.0, a42 = -56.0 / 15.0, a43 = 32.0 / 9.0;
constexpr double a51 = 19372.0 / 6561.0, a52 = -25360.0 / 2187.0, a53 = 64448.0 / 6561.0, a54 = -212.0 / 729.0;
constexpr double a61 = 9017.0 / 3168.0, a62 = -355.0 / 33.0, a63 = 46732.0 / 5247.0, a64 = 49.0 / 176.0,
                 a65 = -5103.0 / 18656.0;
constexpr double b1 = 35.0 / 384.0, b3 = 500.0 / 1113.0, b4 = 125.0 / 192.0, b5 = -2187.0 / 6784.0, b6 = 11.0 / 84.0;
constexpr double e1 = -71.0 / 57600.0, e3 = 71.0 / 16695.0, e4 = -71.0 / 1920.0, e5 = 17253.0 / 339200.0,
                 e6 = -22.0 / 525.0, e7 = 1.0 / 40.0;

struct StepStats {
    std::size_t steps {0};
    double min_radius {std::numeric_limits<double>::infinity()};
    bool ok {true};
};

// Adaptive DOPRI5 from 0 to T; error control on the trajectory and primer (first kBase entries).
// `observer(t, y)` is called after every accepted step (and at t = 0).
template <std::size_t N, typename Observer>
StepStats integrate(Vec<N>& y, double T, const PropagationSettings& settings, Observer&& observer) {
    StepStats stats;
    Vec<N> k1 {}, k2 {}, k3 {}, k4 {}, k5 {}, k6 {}, k7 {}, tmp {}, y_new {};
    rhs<N>(y, k1);
    double t = 0.0;
    observer(t, y);
    // Initial step: a small fraction of the local orbital period.
    const double r0 = std::hypot(y[0], y[1]);
    double h = std::min(T, 0.01 * std::pow(r0, 1.5));
    const double rtol = settings.relative_tolerance;
    const double atol = settings.absolute_tolerance;
    bool last_rejected = false;
    while (t < T) {
        if (stats.steps >= settings.max_steps) {
            stats.ok = false;
            return stats;
        }
        if (t + h > T) h = T - t;
        for (std::size_t i = 0; i < N; ++i) tmp[i] = y[i] + h * a21 * k1[i];
        rhs<N>(tmp, k2);
        for (std::size_t i = 0; i < N; ++i) tmp[i] = y[i] + h * (a31 * k1[i] + a32 * k2[i]);
        rhs<N>(tmp, k3);
        for (std::size_t i = 0; i < N; ++i) tmp[i] = y[i] + h * (a41 * k1[i] + a42 * k2[i] + a43 * k3[i]);
        rhs<N>(tmp, k4);
        for (std::size_t i = 0; i < N; ++i)
            tmp[i] = y[i] + h * (a51 * k1[i] + a52 * k2[i] + a53 * k3[i] + a54 * k4[i]);
        rhs<N>(tmp, k5);
        for (std::size_t i = 0; i < N; ++i)
            tmp[i] = y[i] + h * (a61 * k1[i] + a62 * k2[i] + a63 * k3[i] + a64 * k4[i] + a65 * k5[i]);
        rhs<N>(tmp, k6);
        for (std::size_t i = 0; i < N; ++i)
            y_new[i] = y[i] + h * (b1 * k1[i] + b3 * k3[i] + b4 * k4[i] + b5 * k5[i] + b6 * k6[i]);
        rhs<N>(y_new, k7);
        double err = 0.0;
        for (std::size_t i = 0; i < kBase; ++i) {
            const double e = h * (e1 * k1[i] + e3 * k3[i] + e4 * k4[i] + e5 * k5[i] + e6 * k6[i] + e7 * k7[i]);
            const double sc = atol + rtol * std::max(std::abs(y[i]), std::abs(y_new[i]));
            err = std::max(err, std::abs(e) / sc);
        }
        ++stats.steps;
        if (!std::isfinite(err)) {
            h *= 0.2;
            last_rejected = true;
            if (h < 1e-14 * std::max(1.0, T)) {
                stats.ok = false;
                return stats;
            }
            continue;
        }
        if (err <= 1.0) {
            t += h;
            y = y_new;
            k1 = k7;
            const double rr = std::hypot(y[0], y[1]);
            stats.min_radius = std::min(stats.min_radius, rr);
            if (rr < settings.min_radius) {
                stats.ok = false;
                return stats;
            }
            observer(t, y);
            double factor = err == 0.0 ? 5.0 : std::min(5.0, 0.9 * std::pow(err, -0.2));
            if (last_rejected) factor = std::min(1.0, factor);
            h *= factor;
            last_rejected = false;
        } else {
            h *= std::max(0.2, 0.9 * std::pow(err, -0.2));
            last_rejected = true;
            if (h < 1e-14 * std::max(1.0, T)) {
                stats.ok = false;
                return stats;
            }
        }
    }
    return stats;
}

template <std::size_t N>
Vec<N> initial_vector(const State& start, const Primer& primer) {
    Vec<N> y {};
    y[0] = start.r.x;
    y[1] = start.r.y;
    y[2] = start.v.x;
    y[3] = start.v.y;
    y[4] = primer.p.x;
    y[5] = primer.p.y;
    y[6] = primer.w.x;
    y[7] = primer.w.y;
    if constexpr (N == kWithStm) {
        // ∂(p0, w0): unit perturbation of p (rows 4, 5) and w (rows 6, 7).
        y[kBase + 9 * 0 + 4] = 1.0;
        y[kBase + 9 * 1 + 5] = 1.0;
        y[kBase + 9 * 2 + 6] = 1.0;
        y[kBase + 9 * 3 + 7] = 1.0;
    }
    return y;
}

Vec2 gravity(Vec2 r) {
    const double rr = norm(r);
    return (-1.0 / (rr * rr * rr)) * r;
}

}  // namespace

double norm(Vec2 a) { return std::hypot(a.x, a.y); }

Target fixed_target(const State& state) {
    return [state](double) { return TargetPoint {state, {}, {}}; };
}

Target coasting_target(const State& state) {
    return [state](double T) {
        const Propagator propagator;
        const auto end = propagator.propagate(state, {}, T, false);
        return TargetPoint {end.state, end.state.v, gravity(end.state.r)};
    };
}

EndPoint Propagator::propagate(const State& start, const Primer& primer, double T, bool with_stm) const {
    EndPoint end;
    const auto finish = [&](const auto& y, const StepStats& stats) {
        end.state = {{y[0], y[1]}, {y[2], y[3]}};
        end.primer = {{y[4], y[5]}, {y[6], y[7]}};
        end.effort = y[8];
        end.sweep_rad = y[9];
        end.min_radius = std::min(stats.min_radius, norm(start.r));
        end.steps = stats.steps;
        end.ok = stats.ok && std::isfinite(y[0]) && std::isfinite(y[8]);
        const Vec2 g = gravity(end.state.r);
        end.hamiltonian = -dot(end.primer.w, end.state.v) + dot(end.primer.p, g) + 0.5 * dot(end.primer.p, end.primer.p);
    };
    if (!(T > 0.0)) {
        return end;
    }
    if (with_stm) {
        auto y = initial_vector<kWithStm>(start, primer);
        const auto stats = integrate<kWithStm>(y, T, settings_, [](double, const auto&) {});
        finish(y, stats);
        for (std::size_t c = 0; c < 4; ++c) {
            for (std::size_t r = 0; r < 9; ++r) end.stm[r][c] = y[kBase + 9 * c + r];
        }
    } else {
        auto y = initial_vector<kBase>(start, primer);
        const auto stats = integrate<kBase>(y, T, settings_, [](double, const auto&) {});
        finish(y, stats);
    }
    return end;
}

std::vector<Sample> Propagator::sample(const State& start, const Primer& primer, double T, std::size_t count) const {
    // Integrate between output times (each leg adaptive): simple and accurate enough for
    // checks and rendering.
    std::vector<Sample> out;
    out.reserve(count);
    auto y = initial_vector<kBase>(start, primer);
    out.push_back({0.0, start, primer.p, 0.0});
    for (std::size_t i = 1; i < count; ++i) {
        const double dt = T / static_cast<double>(count - 1);
        const double effort = y[8];
        const double sweep = y[9];
        y[8] = 0.0;
        y[9] = 0.0;
        const auto stats = integrate<kBase>(y, dt, settings_, [](double, const auto&) {});
        y[8] += effort;
        y[9] += sweep;
        if (!stats.ok) break;
        out.push_back({dt * static_cast<double>(i), {{y[0], y[1]}, {y[2], y[3]}}, {y[4], y[5]}, y[8]});
    }
    return out;
}

// ---- Shooting ---------------------------------------------------------------------------------

Solution shoot(
    const Propagator& propagator,
    const State& start,
    const Target& target,
    EndCondition end_condition,
    const Primer& seed,
    double T,
    const ShootingOptions& options) {
    Solution solution;
    const std::size_t n = options.free_time ? 5 : 4;
    const bool budget_row = options.free_time && options.match_budget;
    const std::size_t m = budget_row ? 5 : 4;

    std::vector<double> x {seed.p.x, seed.p.y, seed.w.x, seed.w.y, T};
    struct Eval {
        bool ok {false};
        std::vector<double> f;
        std::vector<double> jac;
        EndPoint end;
        double norm {std::numeric_limits<double>::infinity()};
    };
    const auto evaluate = [&](const std::vector<double>& xv, bool with_jac) {
        Eval e;
        const double t_end = options.free_time ? xv[4] : T;
        if (!(t_end > options.min_time)) return e;
        const Primer primer {{xv[0], xv[1]}, {xv[2], xv[3]}};
        e.end = propagator.propagate(start, primer, t_end, with_jac);
        ++solution.propagations;
        if (!e.end.ok) return e;
        const TargetPoint tp = target(t_end);
        const auto& s = e.end.state;
        e.f.assign(m, 0.0);
        e.f[0] = s.r.x - tp.state.r.x;
        e.f[1] = s.r.y - tp.state.r.y;
        std::array<std::size_t, 2> rows {};
        if (end_condition == EndCondition::Rendezvous) {
            e.f[2] = s.v.x - tp.state.v.x;
            e.f[3] = s.v.y - tp.state.v.y;
            rows = {2, 3};
        } else {
            e.f[2] = e.end.primer.p.x;
            e.f[3] = e.end.primer.p.y;
            rows = {4, 5};
        }
        if (budget_row) {
            e.f[4] = e.end.effort - options.effort_budget;
        }
        if (with_jac) {
            e.jac.assign(m * n, 0.0);
            const std::array<std::size_t, 5> stm_rows {0, 1, rows[0], rows[1], 8};
            for (std::size_t r = 0; r < m; ++r) {
                for (std::size_t c = 0; c < 4; ++c) e.jac[r * n + c] = e.end.stm[stm_rows[r]][c];
            }
            if (options.free_time) {
                // Autonomous dynamics: ∂y(T)/∂T = f(y(T)); minus the target's motion.
                const Vec2 g = gravity(s.r);
                const Vec2 p = e.end.primer.p;
                const Vec2 w = e.end.primer.w;
                std::array<double, 5> dt {s.v.x - tp.velocity.x, s.v.y - tp.velocity.y, 0.0, 0.0, dot(p, p)};
                if (end_condition == EndCondition::Rendezvous) {
                    dt[2] = g.x + p.x - tp.acceleration.x;
                    dt[3] = g.y + p.y - tp.acceleration.y;
                } else {
                    dt[2] = w.x;
                    dt[3] = w.y;
                }
                for (std::size_t r = 0; r < m; ++r) e.jac[r * n + 4] = dt[r];
            }
        }
        double sum = 0.0;
        for (const double v : e.f) sum = std::max(sum, std::abs(v));
        e.norm = sum;
        e.ok = true;
        return e;
    };
    const auto sq = [](const std::vector<double>& f) {
        double s = 0.0;
        for (const double v : f) s += v * v;
        return s;
    };

    Eval current = evaluate(x, true);
    if (!current.ok) {
        return solution;
    }
    double lambda = 0.0;
    for (; solution.iterations < options.max_iterations; ++solution.iterations) {
        if (current.norm <= options.tolerance) break;
        // Column scaling (Jacobian-norm), like scipy's x_scale="jac".
        std::vector<double> scale(n, 1.0);
        std::vector<double> jac = current.jac;
        for (std::size_t c = 0; c < n; ++c) {
            double s = 0.0;
            for (std::size_t r = 0; r < m; ++r) s += jac[r * n + c] * jac[r * n + c];
            scale[c] = s > 0.0 ? std::sqrt(s) : 1.0;
            for (std::size_t r = 0; r < m; ++r) jac[r * n + c] /= scale[c];
        }
        bool accepted = false;
        for (int attempt = 0; attempt < 10 && !accepted; ++attempt) {
            std::vector<double> dx;
            if (!(m < n ? min_norm_step(jac, current.f, m, n, lambda, dx) : lm_step(jac, current.f, m, n, lambda, dx))) {
                lambda = lambda == 0.0 ? 1e-3 : lambda * 10.0;
                continue;
            }
            std::vector<double> trial = x;
            for (std::size_t c = 0; c < n; ++c) trial[c] += dx[c] / scale[c];
            if (options.free_time && trial[4] < 0.5 * x[4]) {
                trial[4] = 0.5 * x[4];
            }
            Eval next = evaluate(trial, true);
            if (next.ok && sq(next.f) < sq(current.f)) {
                x = trial;
                current = std::move(next);
                lambda = lambda * 0.1;
                if (lambda < 1e-6) lambda = 0.0;
                accepted = true;
            } else {
                lambda = lambda == 0.0 ? 1e-3 : lambda * 10.0;
            }
        }
        if (!accepted) break;
    }
    solution.primer = {{x[0], x[1]}, {x[2], x[3]}};
    solution.T = options.free_time ? x[4] : T;
    solution.end = current.end;
    solution.residual = current.norm;
    solution.converged = current.norm <= options.tolerance;
    return solution;
}

// ---- Seeds ------------------------------------------------------------------------------------

Primer free_space_seed(const State& start, const State& end, double T) {
    const Vec2 d = end.r - start.r;
    const Vec2 a = (6.0 / (T * T)) * d - (1.0 / T) * (4.0 * start.v + 2.0 * end.v);
    const Vec2 c = (-12.0 / (T * T * T)) * d + (6.0 / (T * T)) * (start.v + end.v);
    return {a, c};
}

double free_space_effort(const State& start, const State& end, double T) {
    const Vec2 d = end.r - start.r;
    const double q = dot(start.v, start.v) + dot(start.v, end.v) + dot(end.v, end.v);
    return 12.0 * dot(d, d) / (T * T * T) - 12.0 * dot(d, start.v + end.v) / (T * T) + 4.0 * q / T;
}

std::vector<double> arrival_angle_candidates(const State& start, const State& end, double T, int count) {
    const double base = std::atan2(cross(start.r, end.r), dot(start.r, end.r));
    const double w0 = cross(start.r, start.v) / dot(start.r, start.r);
    const double wf = cross(end.r, end.v) / dot(end.r, end.r);
    const double estimate = 0.5 * (w0 + wf) * T;
    const double n0 = std::round((estimate - base) / kTau);
    std::vector<double> out;
    for (int k = -count; k <= count; ++k) out.push_back(base + kTau * (n0 + k));
    std::sort(out.begin(), out.end(), [&](double a, double b) { return std::abs(a - estimate) < std::abs(b - estimate); });
    out.resize(static_cast<std::size_t>(std::max(1, count)));
    return out;
}

namespace {

// Gauss-Legendre nodes and weights on [0, 1].
const std::pair<std::vector<double>, std::vector<double>>& gauss_legendre(std::size_t n) {
    static std::mutex mutex;
    static std::map<std::size_t, std::pair<std::vector<double>, std::vector<double>>> cache;
    const std::lock_guard lock(mutex);
    auto it = cache.find(n);
    if (it != cache.end()) return it->second;
    std::vector<double> x(n), w(n);
    for (std::size_t i = 0; i < n; ++i) {
        double z = std::cos(kPi * (static_cast<double>(i) + 0.75) / (static_cast<double>(n) + 0.5));
        double dp = 0.0;
        for (int iter = 0; iter < 100; ++iter) {
            double p0 = 1.0, p1 = z;
            for (std::size_t k = 2; k <= n; ++k) {
                const double p2 = ((2.0 * k - 1.0) * z * p1 - (k - 1.0) * p0) / static_cast<double>(k);
                p0 = p1;
                p1 = p2;
            }
            dp = static_cast<double>(n) * (z * p1 - p0) / (z * z - 1.0);
            const double dz = p1 / dp;
            z -= dz;
            if (std::abs(dz) < 1e-15) break;
        }
        x[i] = 0.5 * (1.0 - z);
        w[i] = 1.0 / ((1.0 - z * z) * dp * dp);  // 2/((1-z²)P'²) scaled by 1/2
    }
    return cache.emplace(n, std::make_pair(std::move(x), std::move(w))).first->second;
}

// Bubble basis B_k(s) = s²(1−s)² P_k(2s−1) and its first two s-derivatives.
void bubble_basis(double s, std::size_t terms, double* b, double* db, double* ddb) {
    const double x = 2.0 * s - 1.0;
    const double q = s * s * (1.0 - s) * (1.0 - s);
    const double dq = 2.0 * s * (1.0 - s) * (1.0 - 2.0 * s);
    const double ddq = 2.0 * (1.0 - 6.0 * s + 6.0 * s * s);
    double p_prev = 0.0, dp_prev = 0.0, ddp_prev = 0.0;
    double p = 1.0, dp = 0.0, ddp = 0.0;  // Legendre in x and its x-derivatives
    for (std::size_t k = 0; k < terms; ++k) {
        b[k] = q * p;
        db[k] = dq * p + q * 2.0 * dp;
        ddb[k] = ddq * p + 2.0 * dq * 2.0 * dp + q * 4.0 * ddp;
        // P_{k+1} = ((2k+1) x P_k − k P_{k−1}) / (k+1); P'_{k+1} = P'_{k−1} + (2k+1) P_k.
        const double kk = static_cast<double>(k);
        const double p_next = ((2.0 * kk + 1.0) * x * p - kk * p_prev) / (kk + 1.0);
        const double dp_next = dp_prev + (2.0 * kk + 1.0) * p;
        const double ddp_next = ddp_prev + (2.0 * kk + 1.0) * dp;
        p_prev = p;
        dp_prev = dp;
        ddp_prev = ddp;
        p = p_next;
        dp = dp_next;
        ddp = ddp_next;
    }
}

struct Hermite {
    double h00, h10, h01, h11;
    double d00, d10, d01, d11;
    double dd00, dd10, dd01, dd11;
};
Hermite hermite(double s) {
    const double s2 = s * s, s3 = s2 * s;
    return {
        2 * s3 - 3 * s2 + 1, s3 - 2 * s2 + s, -2 * s3 + 3 * s2, s3 - s2,
        6 * s2 - 6 * s, 3 * s2 - 4 * s + 1, -6 * s2 + 6 * s, 3 * s2 - 2 * s,
        12 * s - 6, 6 * s - 4, -12 * s + 6, 6 * s - 2,
    };
}

struct Polar {
    double r, vr, w;  // radius, radial speed, angular rate
};
Polar polar(const State& st) {
    const double rr = norm(st.r);
    return {rr, dot(st.r, st.v) / rr, cross(st.r, st.v) / (rr * rr)};
}

// Shape path quantities at s for coefficients c (r terms, θ terms[, free end rates]).
struct ShapeLocal {
    double r, rs, rss, th, ths, thss;
};

ShapeLocal shape_local(const Polar& a, const Polar& b, double T, double theta_end, const std::vector<double>& c,
    std::size_t terms, bool free_end, double s, double* bb, double* dbb, double* ddbb, Hermite& h) {
    h = hermite(s);
    bubble_basis(s, terms, bb, dbb, ddbb);
    const double rfs = free_end ? c[2 * terms] : b.vr * T;
    const double thfs = free_end ? c[2 * terms + 1] : b.w * T;
    const double r0s = a.vr * T;
    const double th0s = a.w * T;
    ShapeLocal l {};
    l.r = h.h00 * a.r + h.h10 * r0s + h.h01 * b.r + h.h11 * rfs;
    l.rs = h.d00 * a.r + h.d10 * r0s + h.d01 * b.r + h.d11 * rfs;
    l.rss = h.dd00 * a.r + h.dd10 * r0s + h.dd01 * b.r + h.dd11 * rfs;
    l.th = h.h10 * th0s + h.h01 * theta_end + h.h11 * thfs;
    l.ths = h.d10 * th0s + h.d01 * theta_end + h.d11 * thfs;
    l.thss = h.dd10 * th0s + h.dd01 * theta_end + h.dd11 * thfs;
    for (std::size_t k = 0; k < terms; ++k) {
        l.r += c[k] * bb[k];
        l.rs += c[k] * dbb[k];
        l.rss += c[k] * ddbb[k];
        l.th += c[terms + k] * bb[k];
        l.ths += c[terms + k] * dbb[k];
        l.thss += c[terms + k] * ddbb[k];
    }
    return l;
}

}  // namespace

ShapeResult shape_solve(
    const State& start, const State& end, double T, double theta_end, const ShapeOptions& options,
    const std::vector<double>* warm) {
    ShapeResult result;
    result.T = T;
    result.theta_end = theta_end;
    const std::size_t terms = options.terms;
    const bool free_end = options.free_end_velocity;
    const std::size_t n = 2 * terms + (free_end ? 2 : 0);
    const auto& [nodes, weights] = gauss_legendre(options.nodes);
    const std::size_t m = 2 * nodes.size();
    const Polar a = polar(start);
    const Polar b = polar(end);

    std::vector<double> c(n, 0.0);
    if (warm != nullptr && warm->size() == n) {
        c = *warm;
    } else if (free_end) {
        c[2 * terms] = b.vr * T;
        c[2 * terms + 1] = b.w * T;
    }
    std::vector<double> bb(terms), dbb(terms), ddbb(terms);
    std::vector<double> f(m), jac(m * n);

    const auto evaluate = [&](const std::vector<double>& cv, bool with_jac, double& min_r) {
        double cost = 0.0;
        min_r = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            Hermite h {};
            const auto l = shape_local(a, b, T, theta_end, cv, terms, free_end, nodes[i], bb.data(), dbb.data(), ddbb.data(), h);
            if (!(l.r > 1e-3)) return std::numeric_limits<double>::infinity();
            min_r = std::min(min_r, l.r);
            const double rd = l.rs / T, rdd = l.rss / (T * T);
            const double wd = l.ths / T, wdd = l.thss / (T * T);
            const double ar = rdd - l.r * wd * wd + 1.0 / (l.r * l.r);
            const double at = l.r * wdd + 2.0 * rd * wd;
            const double sw = std::sqrt(T * weights[i]);
            f[2 * i] = sw * ar;
            f[2 * i + 1] = sw * at;
            cost += T * weights[i] * (ar * ar + at * at);
            if (!with_jac) continue;
            double* jr = &jac[(2 * i) * n];
            double* jt = &jac[(2 * i + 1) * n];
            const auto r_column = [&](double dr, double drs, double drss) {
                const double ddr = drs / T, dddr = drss / (T * T);
                return std::pair {
                    sw * (dddr - wd * wd * dr - 2.0 / (l.r * l.r * l.r) * dr),
                    sw * (wdd * dr + 2.0 * wd * ddr),
                };
            };
            const auto th_column = [&](double dths, double dthss) {
                const double dw = dths / T, dwdd = dthss / (T * T);
                return std::pair {
                    sw * (-2.0 * l.r * wd * dw),
                    sw * (l.r * dwdd + 2.0 * rd * dw),
                };
            };
            for (std::size_t k = 0; k < terms; ++k) {
                const auto [cr, ct] = r_column(bb[k], dbb[k], ddbb[k]);
                jr[k] = cr;
                jt[k] = ct;
                const auto [tr, tt] = th_column(dbb[k], ddbb[k]);
                jr[terms + k] = tr;
                jt[terms + k] = tt;
            }
            if (free_end) {
                const auto [cr, ct] = r_column(h.h11, h.d11, h.dd11);
                jr[2 * terms] = cr;
                jt[2 * terms] = ct;
                const auto [tr, tt] = th_column(h.d11, h.dd11);
                jr[2 * terms + 1] = tr;
                jt[2 * terms + 1] = tt;
            }
        }
        return cost;
    };

    double min_r = 0.0;
    double cost = evaluate(c, true, min_r);
    if (!std::isfinite(cost)) return result;
    double lambda = 1e-6;
    std::vector<double> dx;
    for (; result.iterations < options.max_iterations; ++result.iterations) {
        std::vector<double> f_saved = f;
        std::vector<double> jac_saved = jac;
        bool accepted = false;
        double new_cost = cost;
        for (int attempt = 0; attempt < 12; ++attempt) {
            if (!lm_step(jac_saved, f_saved, m, n, lambda, dx)) {
                lambda = std::max(lambda * 10.0, 1e-6);
                continue;
            }
            std::vector<double> trial = c;
            for (std::size_t k = 0; k < n; ++k) trial[k] += dx[k];
            double trial_min_r = 0.0;
            new_cost = evaluate(trial, true, trial_min_r);
            if (std::isfinite(new_cost) && new_cost <= cost) {
                c = std::move(trial);
                min_r = trial_min_r;
                lambda = std::max(lambda * 0.2, 1e-9);
                accepted = true;
                break;
            }
            lambda = std::max(lambda * 10.0, 1e-6);
        }
        if (!accepted) {
            // Restore f/jac of the current point.
            f = std::move(f_saved);
            jac = std::move(jac_saved);
            break;
        }
        const double improvement = cost - new_cost;
        cost = new_cost;
        if (improvement <= 1e-10 * std::max(cost, 1e-12)) {
            ++result.iterations;
            break;
        }
    }
    result.ok = true;
    result.terms = terms;
    result.free_end = free_end;
    result.effort = cost;
    result.coefficients = std::move(c);
    result.min_radius = min_r;
    return result;
}

ShapePoint shape_eval(const State& start, const State& end, const ShapeResult& shape, double s) {
    const Polar a = polar(start);
    const Polar b = polar(end);
    const std::size_t terms = shape.terms;
    const bool is_free = shape.free_end;
    std::vector<double> bb(terms + 1), dbb(terms + 1), ddbb(terms + 1);
    const double phi0 = std::atan2(start.r.y, start.r.x);
    const double T = shape.T;
    const auto at = [&](double ss) {
        Hermite h {};
        const auto l = shape_local(a, b, T, shape.theta_end, shape.coefficients, terms, is_free, ss, bb.data(), dbb.data(),
            ddbb.data(), h);
        return l;
    };
    const auto point = [&](const ShapeLocal& l, State& st, Vec2& thrust) {
        const double rd = l.rs / T, rdd = l.rss / (T * T);
        const double wd = l.ths / T, wdd = l.thss / (T * T);
        const double ar = rdd - l.r * wd * wd + 1.0 / (l.r * l.r);
        const double atn = l.r * wdd + 2.0 * rd * wd;
        const double ang = phi0 + l.th;
        const Vec2 er {std::cos(ang), std::sin(ang)};
        const Vec2 et {-er.y, er.x};
        st.r = l.r * er;
        st.v = rd * er + (l.r * wd) * et;
        thrust = ar * er + atn * et;
    };
    ShapePoint out;
    point(at(s), out.state, out.thrust);
    // Thrust rate by central difference in time.
    const double ds = 1e-5;
    State s1, s2;
    Vec2 t1, t2;
    const double sa = std::max(0.0, s - ds), sb = std::min(1.0, s + ds);
    point(at(sa), s1, t1);
    point(at(sb), s2, t2);
    out.thrust_rate = (1.0 / ((sb - sa) * T)) * (t2 - t1);
    return out;
}


namespace {

// Levenberg-Marquardt on a residual function with a forward-difference Jacobian.
// Returns the final sum of squares (infinity if the start is invalid).
using ResidualFn = std::function<bool(const std::vector<double>& x, std::vector<double>& f)>;
double lm_minimize(const ResidualFn& residual, std::vector<double>& x, std::size_t max_iterations, std::size_t& iterations) {
    const std::size_t n = x.size();
    std::vector<double> f;
    if (!residual(x, f)) return std::numeric_limits<double>::infinity();
    const std::size_t m = f.size();
    const auto sumsq = [](const std::vector<double>& v) {
        double s = 0.0;
        for (const double e : v) s += e * e;
        return s;
    };
    double cost = sumsq(f);
    double lambda = 1e-4;
    std::vector<double> jac(m * n), fp, dx, trial;
    for (iterations = 0; iterations < max_iterations; ++iterations) {
        for (std::size_t c = 0; c < n; ++c) {
            std::vector<double> xp = x;
            const double h = 1e-7 * std::max(1.0, std::abs(x[c]));
            xp[c] += h;
            if (!residual(xp, fp)) {
                xp[c] = x[c] - h;
                if (!residual(xp, fp)) return cost;
                for (std::size_t r = 0; r < m; ++r) jac[r * n + c] = (f[r] - fp[r]) / h;
                continue;
            }
            for (std::size_t r = 0; r < m; ++r) jac[r * n + c] = (fp[r] - f[r]) / h;
        }
        bool accepted = false;
        double new_cost = cost;
        for (int attempt = 0; attempt < 12; ++attempt) {
            if (!lm_step(jac, f, m, n, lambda, dx)) {
                lambda *= 10.0;
                continue;
            }
            trial = x;
            for (std::size_t k = 0; k < n; ++k) trial[k] += dx[k];
            if (residual(trial, fp) && (new_cost = sumsq(fp)) <= cost) {
                x = trial;
                f = fp;
                lambda = std::max(lambda * 0.2, 1e-10);
                accepted = true;
                break;
            }
            lambda *= 10.0;
        }
        if (!accepted) break;
        const double improvement = cost - new_cost;
        cost = new_cost;
        if (improvement <= 1e-10 * std::max(cost, 1e-14)) {
            ++iterations;
            break;
        }
    }
    return cost;
}

struct AngleLocal {
    double r, rs, rss, t, ts, tss;
};

AngleLocal angle_local(const ShapeResult& shape, double r0, double r0s, double rf, double rfs, double t0s, double tfs,
    double sigma, double* bb, double* dbb, double* ddbb) {
    const Hermite h = hermite(sigma);
    const std::size_t terms = shape.terms;
    bubble_basis(sigma, terms, bb, dbb, ddbb);
    AngleLocal l {};
    l.r = h.h00 * r0 + h.h10 * r0s + h.h01 * rf + h.h11 * rfs;
    l.rs = h.d00 * r0 + h.d10 * r0s + h.d01 * rf + h.d11 * rfs;
    l.rss = h.dd00 * r0 + h.dd10 * r0s + h.dd01 * rf + h.dd11 * rfs;
    l.t = h.h10 * t0s + h.h01 * shape.T + h.h11 * tfs;
    l.ts = h.d10 * t0s + h.d01 * shape.T + h.d11 * tfs;
    l.tss = h.dd10 * t0s + h.dd01 * shape.T + h.dd11 * tfs;
    const auto& c = shape.coefficients;
    for (std::size_t k = 0; k < terms; ++k) {
        l.r += c[k] * bb[k];
        l.rs += c[k] * dbb[k];
        l.rss += c[k] * ddbb[k];
        l.t += c[terms + k] * bb[k];
        l.ts += c[terms + k] * dbb[k];
        l.tss += c[terms + k] * ddbb[k];
    }
    return l;
}

struct AngleEnds {
    bool ok {false};
    double r0, r0s, rf, rfs, t0s, tfs;
};
AngleEnds angle_ends(const State& start, const State& end, double theta_end) {
    const Polar a = polar(start);
    const Polar b = polar(end);
    AngleEnds e;
    if (!(a.w * theta_end > 0.0) || !(b.w * theta_end > 0.0)) return e;
    e.r0 = a.r;
    e.rf = b.r;
    e.r0s = a.vr / a.w * theta_end;
    e.rfs = b.vr / b.w * theta_end;
    e.t0s = theta_end / a.w;
    e.tfs = theta_end / b.w;
    e.ok = true;
    return e;
}

// Thrust in the polar frame from the σ-derivatives (θ = θ_end σ).
std::pair<double, double> angle_thrust(const AngleLocal& l, double theta_end) {
    const double ts = l.ts;
    const double rd = l.rs / ts;
    const double wd = theta_end / ts;
    const double rdd = (l.rss * ts - l.rs * l.tss) / (ts * ts * ts);
    const double wdd = -theta_end * l.tss / (ts * ts * ts);
    return {rdd - l.r * wd * wd + 1.0 / (l.r * l.r), l.r * wdd + 2.0 * rd * wd};
}

}  // namespace

ShapeResult angle_shape_solve(
    const State& start, const State& end, double T, double theta_end, const ShapeOptions& options) {
    ShapeResult shape;
    shape.T = T;
    shape.theta_end = theta_end;
    shape.terms = options.terms;
    const auto ends = angle_ends(start, end, theta_end);
    if (!ends.ok) return shape;
    const auto& [nodes, weights] = gauss_legendre(options.nodes);
    std::vector<double> bb(options.terms), dbb(options.terms), ddbb(options.terms);
    ShapeResult work = shape;
    double min_r = 0.0;
    const ResidualFn residual = [&](const std::vector<double>& x, std::vector<double>& f) {
        work.coefficients = x;
        f.resize(2 * nodes.size());
        min_r = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const auto l = angle_local(work, ends.r0, ends.r0s, ends.rf, ends.rfs, ends.t0s, ends.tfs, nodes[i], bb.data(),
                dbb.data(), ddbb.data());
            if (!(l.r > 1e-3) || !(l.ts * theta_end > 0.0 ? true : false) || !(l.ts > 0.0)) return false;
            min_r = std::min(min_r, l.r);
            const auto [ar, at] = angle_thrust(l, theta_end);
            const double sw = std::sqrt(weights[i] * l.ts);
            f[2 * i] = sw * ar;
            f[2 * i + 1] = sw * at;
        }
        return true;
    };
    // t(σ) must increase: θ_end and the end rates share a sign, so ts > 0 needs θ_end > 0
    // in this parametrisation; mirror retrograde requests by flipping the sign of θ.
    if (theta_end < 0.0) return shape;
    std::vector<double> x(2 * options.terms, 0.0);
    std::size_t iterations = 0;
    const double cost = lm_minimize(residual, x, options.max_iterations, iterations);
    if (!std::isfinite(cost)) return shape;
    std::vector<double> f;
    residual(x, f);  // refresh min_r for the final point
    shape.ok = true;
    shape.effort = cost;
    shape.coefficients = x;
    shape.iterations = iterations;
    shape.min_radius = min_r;
    return shape;
}

TimedShapePoint angle_shape_eval(const State& start, const State& end, const ShapeResult& shape, double sigma) {
    const auto ends = angle_ends(start, end, shape.theta_end);
    std::vector<double> bb(shape.terms + 1), dbb(shape.terms + 1), ddbb(shape.terms + 1);
    const auto l = angle_local(shape, ends.r0, ends.r0s, ends.rf, ends.rfs, ends.t0s, ends.tfs, sigma, bb.data(), dbb.data(),
        ddbb.data());
    const auto [ar, at] = angle_thrust(l, shape.theta_end);
    const double ang = std::atan2(start.r.y, start.r.x) + shape.theta_end * sigma;
    const Vec2 er {std::cos(ang), std::sin(ang)};
    const Vec2 et {-er.y, er.x};
    TimedShapePoint out;
    out.t = l.t;
    out.state.r = l.r * er;
    out.state.v = (l.rs / l.ts) * er + (l.r * shape.theta_end / l.ts) * et;
    out.thrust = ar * er + at * et;
    return out;
}

Primer fit_primer(const std::vector<double>& times, const std::vector<Vec2>& positions, const std::vector<Vec2>& thrust) {
    // Propagate the four basis solutions of p'' = G(r(t)) p along the given path (positions at
    // `times`, linearly interpolated inside a step via RK4 on midpoints), then least squares.
    const std::size_t count = times.size();
    std::array<std::array<double, 4>, 4> basis {};  // [solution][p.x, p.y, w.x, w.y]
    for (std::size_t c = 0; c < 4; ++c) basis[c][c] = 1.0;
    std::array<double, 16> ata {};
    std::array<double, 4> atb {};
    const auto accumulate = [&](std::size_t i) {
        // rows: p.x, p.y of each basis solution
        for (int comp = 0; comp < 2; ++comp) {
            const double target = comp == 0 ? thrust[i].x : thrust[i].y;
            for (std::size_t a = 0; a < 4; ++a) {
                atb[a] += basis[a][comp] * target;
                for (std::size_t b = 0; b < 4; ++b) ata[a * 4 + b] += basis[a][comp] * basis[b][comp];
            }
        }
    };
    const auto g_times = [](Vec2 r, double px, double py) {
        const double r2 = dot(r, r);
        const double rr = std::sqrt(r2);
        const double inv3 = 1.0 / (r2 * rr);
        const double rp = r.x * px + r.y * py;
        return Vec2 {-px * inv3 + 3.0 * r.x * rp * inv3 / r2, -py * inv3 + 3.0 * r.y * rp * inv3 / r2};
    };
    accumulate(0);
    for (std::size_t i = 1; i < count; ++i) {
        const double h = times[i] - times[i - 1];
        const Vec2 r0 = positions[i - 1], r1 = positions[i];
        const Vec2 rm = 0.5 * (r0 + r1);
        for (auto& s : basis) {
            const auto deriv = [&](const std::array<double, 4>& y, Vec2 r) {
                const Vec2 gw = g_times(r, y[0], y[1]);
                return std::array<double, 4> {y[2], y[3], gw.x, gw.y};
            };
            const auto k1 = deriv(s, r0);
            std::array<double, 4> t {};
            for (int j = 0; j < 4; ++j) t[j] = s[j] + 0.5 * h * k1[j];
            const auto k2 = deriv(t, rm);
            for (int j = 0; j < 4; ++j) t[j] = s[j] + 0.5 * h * k2[j];
            const auto k3 = deriv(t, rm);
            for (int j = 0; j < 4; ++j) t[j] = s[j] + h * k3[j];
            const auto k4 = deriv(t, r1);
            for (int j = 0; j < 4; ++j) s[j] += h / 6.0 * (k1[j] + 2 * k2[j] + 2 * k3[j] + k4[j]);
        }
        accumulate(i);
    }
    std::vector<double> a(ata.begin(), ata.end());
    std::vector<double> b(atb.begin(), atb.end());
    for (std::size_t i = 0; i < 4; ++i) a[i * 4 + i] *= 1.0 + 1e-12;
    if (!solve_dense(a, b, 4)) return {};
    return {{b[0], b[1]}, {b[2], b[3]}};
}

}  // namespace spacetrains::variable_isp::general
