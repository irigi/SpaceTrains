#include "variable_isp/GeneralTransfer.hpp"

#include <algorithm>
#include <cmath>
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
constexpr std::size_t kWithStm = kBase + 36;   // columns ∂/∂(p0, w0)
constexpr std::size_t kFullStm = kBase + 72;   // columns ∂/∂(r0, v0, p0, w0)

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

    if constexpr (N > kBase) {
        const double inv7 = inv5 / r2;
        for (std::size_t c = 0; c < (N - kBase) / 9; ++c) {
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

// DOP853 (Dormand-Prince 8(5,3)) tableau, from SciPy's dop853_coefficients (Hairer's DOP853).
constexpr std::size_t k853Stages = 12;
constexpr double k853C[12] = {0.0, 0.05260015195876773, 0.0789002279381516, 0.1183503419072274, 0.2816496580927726, 0.3333333333333333, 0.25, 0.3076923076923077, 0.6512820512820513, 0.6, 0.8571428571428571, 1.0};
constexpr double k853B[12] = {0.054293734116568765, 0.0, 0.0, 0.0, 0.0, 4.450312892752409, 1.8915178993145003, -5.801203960010585, 0.3111643669578199, -0.1521609496625161, 0.20136540080403034, 0.04471061572777259};
constexpr double k853E3[13] = {-0.18980075407240762, 0.0, 0.0, 0.0, 0.0, 4.450312892752409, 1.8915178993145003, -5.801203960010585, -0.4226823213237919, -0.1521609496625161, 0.20136540080403034, 0.02265179219836082, 0.0};
constexpr double k853E5[13] = {0.01312004499419488, 0.0, 0.0, 0.0, 0.0, -1.2251564463762044, -0.4957589496572502, 1.6643771824549864, -0.35032884874997366, 0.3341791187130175, 0.08192320648511571, -0.022355307863886294, 0.0};
constexpr double k853A[12][12] = {
    {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
    {0.05260015195876773, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
    {0.0197250569845379, 0.0591751709536137, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
    {0.02958758547680685, 0.0, 0.08876275643042054, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
    {0.2413651341592667, 0.0, -0.8845494793282861, 0.924834003261792, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
    {0.037037037037037035, 0.0, 0.0, 0.17082860872947386, 0.12546768756682242, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
    {0.037109375, 0.0, 0.0, 0.17025221101954405, 0.06021653898045596, -0.017578125, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
    {0.03709200011850479, 0.0, 0.0, 0.17038392571223998, 0.10726203044637328, -0.015319437748624402, 0.008273789163814023, 0.0, 0.0, 0.0, 0.0, 0.0},
    {0.6241109587160757, 0.0, 0.0, -3.3608926294469414, -0.868219346841726, 27.59209969944671, 20.154067550477894, -43.48988418106996, 0.0, 0.0, 0.0, 0.0},
    {0.47766253643826434, 0.0, 0.0, -2.4881146199716677, -0.590290826836843, 21.230051448181193, 15.279233632882423, -33.28821096898486, -0.020331201708508627, 0.0, 0.0, 0.0},
    {-0.9371424300859873, 0.0, 0.0, 5.186372428844064, 1.0914373489967295, -8.149787010746927, -18.52006565999696, 22.739487099350505, 2.4936055526796523, -3.0467644718982196, 0.0, 0.0},
    {2.273310147516538, 0.0, 0.0, -10.53449546673725, -2.0008720582248625, -17.9589318631188, 27.94888452941996, -2.8589982771350235, -8.87285693353063, 12.360567175794303, 0.6433927460157636, 0.0},
};

// Adaptive DOP853 from 0 to T, same contract as integrate(): error control on the first kBase
// entries (SciPy's combined 5th/3rd-order estimate), observer after every accepted step.
template <std::size_t N, typename Observer>
StepStats integrate853(Vec<N>& y, double T, const PropagationSettings& settings, Observer&& observer) {
    StepStats stats;
    std::array<Vec<N>, k853Stages + 1> K {};
    Vec<N> tmp {}, y_new {};
    rhs<N>(y, K[0]);
    double t = 0.0;
    observer(t, y);
    const double r0 = std::hypot(y[0], y[1]);
    double h = std::min(T, 0.05 * std::pow(r0, 1.5));
    const double rtol = settings.relative_tolerance;
    const double atol = settings.absolute_tolerance;
    bool last_rejected = false;
    while (t < T) {
        if (stats.steps >= settings.max_steps) {
            stats.ok = false;
            return stats;
        }
        if (t + h > T) h = T - t;
        for (std::size_t stage = 1; stage < k853Stages; ++stage) {
            for (std::size_t i = 0; i < N; ++i) {
                double acc = 0.0;
                for (std::size_t j = 0; j < stage; ++j) acc += k853A[stage][j] * K[j][i];
                tmp[i] = y[i] + h * acc;
            }
            rhs<N>(tmp, K[stage]);
        }
        for (std::size_t i = 0; i < N; ++i) {
            double acc = 0.0;
            for (std::size_t j = 0; j < k853Stages; ++j) acc += k853B[j] * K[j][i];
            y_new[i] = y[i] + h * acc;
        }
        rhs<N>(y_new, K[k853Stages]);
        double e5 = 0.0, e3 = 0.0;
        for (std::size_t i = 0; i < kBase; ++i) {
            double a5 = 0.0, a3 = 0.0;
            for (std::size_t j = 0; j <= k853Stages; ++j) {
                a5 += k853E5[j] * K[j][i];
                a3 += k853E3[j] * K[j][i];
            }
            const double sc = atol + rtol * std::max(std::abs(y[i]), std::abs(y_new[i]));
            e5 += (a5 / sc) * (a5 / sc);
            e3 += (a3 / sc) * (a3 / sc);
        }
        const double err = (e5 == 0.0 && e3 == 0.0) ? 0.0 : std::abs(h) * e5 / std::sqrt((e5 + 0.01 * e3) * static_cast<double>(kBase));
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
            K[0] = K[k853Stages];
            const double rr = std::hypot(y[0], y[1]);
            stats.min_radius = std::min(stats.min_radius, rr);
            if (rr < settings.min_radius) {
                stats.ok = false;
                return stats;
            }
            observer(t, y);
            double factor = err == 0.0 ? 10.0 : std::min(10.0, 0.9 * std::pow(err, -1.0 / 8.0));
            if (last_rejected) factor = std::min(1.0, factor);
            h *= factor;
            last_rejected = false;
        } else {
            h *= std::max(0.2, 0.9 * std::pow(err, -1.0 / 8.0));
            last_rejected = true;
            if (h < 1e-14 * std::max(1.0, T)) {
                stats.ok = false;
                return stats;
            }
        }
    }
    return stats;
}

template <std::size_t N, typename Observer>
StepStats integrate_with(Vec<N>& y, double T, const PropagationSettings& settings, Observer&& observer) {
    if (settings.high_order) return integrate853<N>(y, T, settings, std::forward<Observer>(observer));
    return integrate<N>(y, T, settings, std::forward<Observer>(observer));
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
        const auto stats = integrate_with<kWithStm>(y, T, settings_, [](double, const auto&) {});
        finish(y, stats);
        for (std::size_t c = 0; c < 4; ++c) {
            for (std::size_t r = 0; r < 9; ++r) end.stm[r][c] = y[kBase + 9 * c + r];
        }
    } else {
        auto y = initial_vector<kBase>(start, primer);
        const auto stats = integrate_with<kBase>(y, T, settings_, [](double, const auto&) {});
        finish(y, stats);
    }
    return end;
}


SegmentEnd Propagator::propagate_segment(const std::array<double, 8>& x0, double dt) const {
    SegmentEnd out;
    Vec<kFullStm> y {};
    for (std::size_t i = 0; i < 8; ++i) {
        y[i] = x0[i];
        y[kBase + 9 * i + i] = 1.0;
    }
    if (!(dt > 0.0)) return out;
    const auto stats = integrate_with<kFullStm>(y, dt, settings_, [](double, const auto&) {});
    for (std::size_t i = 0; i < 8; ++i) out.x[i] = y[i];
    out.effort = y[8];
    out.sweep_rad = y[9];
    out.min_radius = std::min(stats.min_radius, std::hypot(x0[0], x0[1]));
    for (std::size_t c = 0; c < 8; ++c) {
        for (std::size_t r = 0; r < 9; ++r) out.stm[r][c] = y[kBase + 9 * c + r];
    }
    out.steps = stats.steps;
    out.ok = stats.ok && std::isfinite(y[0]) && std::isfinite(y[8]);
    return out;
}

std::vector<Sample> Propagator::sample(const State& start, const Primer& primer, double T, std::size_t count) const {
    // One integration, recording every accepted step; output times are filled in by cubic
    // Hermite interpolation between steps (state and thrust have their derivatives at hand).
    std::vector<double> times;
    std::vector<Vec<kBase>> states;
    auto y = initial_vector<kBase>(start, primer);
    const auto stats = integrate_with<kBase>(y, T, settings_, [&](double t, const Vec<kBase>& state) {
        times.push_back(t);
        states.push_back(state);
    });
    std::vector<Sample> out;
    if (!stats.ok || times.size() < 2 || count < 2) return out;
    out.reserve(count);
    std::size_t step = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const double t = i + 1 == count ? times.back() : T * static_cast<double>(i) / static_cast<double>(count - 1);
        while (step + 2 < times.size() && times[step + 1] < t) ++step;
        const auto& a = states[step];
        const auto& b = states[step + 1];
        const double h = times[step + 1] - times[step];
        const double s = h > 0.0 ? std::clamp((t - times[step]) / h, 0.0, 1.0) : 0.0;
        Vec<kBase> da {}, db {};
        rhs<kBase>(a, da);
        rhs<kBase>(b, db);
        const double s2 = s * s, s3 = s2 * s;
        const double h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s, h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
        Vec<kBase> v {};
        for (std::size_t k = 0; k < kBase; ++k) v[k] = h00 * a[k] + h10 * h * da[k] + h01 * b[k] + h11 * h * db[k];
        out.push_back({t, {{v[0], v[1]}, {v[2], v[3]}}, {v[4], v[5]}, v[8]});
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
        // The budget row is relative: efforts span orders of magnitude.
        const double budget_scale = budget_row ? 1.0 / std::max(options.effort_budget, 1e-300) : 1.0;
        if (budget_row) {
            e.f[4] = e.end.effort * budget_scale - 1.0;
        }
        if (with_jac) {
            e.jac.assign(m * n, 0.0);
            const std::array<std::size_t, 5> stm_rows {0, 1, rows[0], rows[1], 8};
            for (std::size_t r = 0; r < m; ++r) {
                for (std::size_t c = 0; c < 4; ++c) e.jac[r * n + c] = e.end.stm[stm_rows[r]][c] * (r == 4 ? budget_scale : 1.0);
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
                dt[4] *= budget_scale;
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
    solution.initial_sweep = current.end.sweep_rad;
    double lambda = 0.0;
    const double start_norm = current.norm;
    for (; solution.iterations < options.max_iterations; ++solution.iterations) {
        if (current.norm <= options.tolerance) break;
        if (solution.propagations >= options.max_propagations) break;
        if (solution.iterations >= options.stall_iterations && current.norm > options.stall_fraction * start_norm) break;
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
        for (int attempt = 0; attempt < 10 && !accepted && solution.propagations < options.max_propagations; ++attempt) {
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

Solution multiple_shoot(
    const Propagator& propagator,
    const State& start,
    const Target& target,
    EndCondition end_condition,
    double T,
    const std::vector<std::array<double, 8>>& nodes,
    const ShootingOptions& options) {
    Solution solution;
    const std::size_t K = nodes.size();
    if (K == 0) return solution;
    const double dt = T / static_cast<double>(K);
    const std::size_t n = 4 + 8 * (K - 1);
    const std::size_t m = n;
    std::vector<double> x(n);
    for (std::size_t i = 0; i < 4; ++i) x[i] = nodes[0][4 + i];
    for (std::size_t k = 1; k < K; ++k)
        for (std::size_t i = 0; i < 8; ++i) x[4 + 8 * (k - 1) + i] = nodes[k][i];
    const TargetPoint tp = target(T);

    struct Eval {
        bool ok {false};
        std::vector<double> f, jac;
        std::vector<SegmentEnd> segments;
        double norm {std::numeric_limits<double>::infinity()};
    };
    const auto evaluate = [&](const std::vector<double>& xv) {
        Eval e;
        e.f.assign(m, 0.0);
        e.jac.assign(m * n, 0.0);
        e.segments.resize(K);
        for (std::size_t k = 0; k < K; ++k) {
            std::array<double, 8> x0 {};
            if (k == 0) {
                x0 = {start.r.x, start.r.y, start.v.x, start.v.y, xv[0], xv[1], xv[2], xv[3]};
            } else {
                for (std::size_t i = 0; i < 8; ++i) x0[i] = xv[4 + 8 * (k - 1) + i];
            }
            e.segments[k] = propagator.propagate_segment(x0, dt);
            ++solution.propagations;
            if (!e.segments[k].ok) return e;
            const auto& seg = e.segments[k];
            // Columns of this segment's start unknowns.
            const auto col_of = [&](std::size_t i) -> long {
                if (k == 0) return i >= 4 ? static_cast<long>(i - 4) : -1;
                return static_cast<long>(4 + 8 * (k - 1) + i);
            };
            if (k + 1 < K) {
                for (std::size_t r = 0; r < 8; ++r) {
                    const std::size_t row = 8 * k + r;
                    e.f[row] = seg.x[r] - xv[4 + 8 * k + r];
                    for (std::size_t i = 0; i < 8; ++i) {
                        const long c = col_of(i);
                        if (c >= 0) e.jac[row * n + static_cast<std::size_t>(c)] = seg.stm[r][i];
                    }
                    e.jac[row * n + 4 + 8 * k + r] = -1.0;
                }
            } else {
                const std::size_t base = 8 * k;
                std::array<std::size_t, 4> rows {0, 1, 2, 3};
                std::array<double, 4> goal {tp.state.r.x, tp.state.r.y, tp.state.v.x, tp.state.v.y};
                if (end_condition == EndCondition::Intercept) {
                    rows = {0, 1, 4, 5};
                    goal[2] = goal[3] = 0.0;
                }
                for (std::size_t r = 0; r < 4; ++r) {
                    e.f[base + r] = seg.x[rows[r]] - goal[r];
                    for (std::size_t i = 0; i < 8; ++i) {
                        const long c = col_of(i);
                        if (c >= 0) e.jac[(base + r) * n + static_cast<std::size_t>(c)] = seg.stm[rows[r]][i];
                    }
                }
            }
        }
        double mx = 0.0;
        for (const double v : e.f) mx = std::max(mx, std::abs(v));
        e.norm = mx;
        e.ok = true;
        return e;
    };
    const auto sq = [](const std::vector<double>& f) {
        double s2 = 0.0;
        for (const double v : f) s2 += v * v;
        return s2;
    };

    Eval current = evaluate(x);
    if (!current.ok) return solution;
    // Fixed row weights (equilibrate the Jacobian rows at the start) for the merit function and
    // the steps: continuity rows of primer and position differ by orders of magnitude.
    std::vector<double> row_weight(m, 1.0);
    for (std::size_t r = 0; r < m; ++r) {
        double s2 = 0.0;
        for (std::size_t c = 0; c < n; ++c) s2 += current.jac[r * n + c] * current.jac[r * n + c];
        row_weight[r] = s2 > 0.0 ? 1.0 / std::sqrt(s2) : 1.0;
    }
    const auto weigh = [&](Eval& e) {
        for (std::size_t r = 0; r < m; ++r) {
            e.f[r] *= row_weight[r];
            for (std::size_t c = 0; c < n; ++c) e.jac[r * n + c] *= row_weight[r];
        }
    };
    const auto raw_norm = [&](const Eval& e) {
        double mx = 0.0;
        for (std::size_t r = 0; r < m; ++r) mx = std::max(mx, std::abs(e.f[r] / row_weight[r]));
        return mx;
    };
    weigh(current);
    const double start_norm = current.norm;
    double lambda = 0.0;
    for (; solution.iterations < options.max_iterations; ++solution.iterations) {
        if (current.norm <= options.tolerance) break;
        if (solution.propagations >= options.max_propagations * K) break;
        if (solution.iterations >= options.stall_iterations && current.norm > options.stall_fraction * start_norm) break;
        std::vector<double> scale(n, 1.0);
        std::vector<double> jac = current.jac;
        for (std::size_t c = 0; c < n; ++c) {
            double s2 = 0.0;
            for (std::size_t r = 0; r < m; ++r) s2 += jac[r * n + c] * jac[r * n + c];
            scale[c] = s2 > 0.0 ? std::sqrt(s2) : 1.0;
            for (std::size_t r = 0; r < m; ++r) jac[r * n + c] /= scale[c];
        }
        bool accepted = false;
        for (int attempt = 0; attempt < 10 && !accepted; ++attempt) {
            std::vector<double> dx;
            bool solved = false;
            if (lambda == 0.0) {
                std::vector<double> a = jac;
                dx = current.f;
                for (double& v : dx) v = -v;
                solved = solve_dense(a, dx, n);
            } else {
                solved = lm_step(jac, current.f, m, n, lambda, dx);
            }
            if (!solved) {
                lambda = lambda == 0.0 ? 1e-3 : lambda * 10.0;
                continue;
            }
            std::vector<double> trial = x;
            for (std::size_t c = 0; c < n; ++c) trial[c] += dx[c] / scale[c];
            Eval next = evaluate(trial);
            if (next.ok) {
                weigh(next);
                next.norm = raw_norm(next);
            }
            if (next.ok && sq(next.f) < sq(current.f)) {
                x = std::move(trial);
                current = std::move(next);
                lambda *= 0.1;
                if (lambda < 1e-6) lambda = 0.0;
                accepted = true;
            } else {
                lambda = lambda == 0.0 ? 1e-3 : lambda * 10.0;
            }
        }
        if (!accepted) break;
    }
    solution.primer = {{x[0], x[1]}, {x[2], x[3]}};
    solution.T = T;
    solution.residual = current.norm;
    solution.converged = current.norm <= options.tolerance;
    // Assemble the end point from the segments.
    EndPoint end;
    end.ok = current.ok;
    end.min_radius = std::numeric_limits<double>::infinity();
    for (const auto& seg : current.segments) {
        end.effort += seg.effort;
        end.sweep_rad += seg.sweep_rad;
        end.min_radius = std::min(end.min_radius, seg.min_radius);
        end.steps += seg.steps;
    }
    const auto& last = current.segments.back().x;
    end.state = {{last[0], last[1]}, {last[2], last[3]}};
    end.primer = {{last[4], last[5]}, {last[6], last[7]}};
    solution.end = end;
    return solution;
}

Solution continue_in_time(
    const Propagator& propagator,
    const State& start,
    const Target& target,
    EndCondition end_condition,
    double T_to,
    const ContinuationOptions& options,
    const ShootingOptions& shooting) {
    std::size_t propagations = 0;
    double T = std::min(options.T_from, T_to);
    ShootingOptions fixed = shooting;
    fixed.free_time = false;
    Solution current;
    // Short-T start: free-space seed toward the target's state at T.
    for (int attempt = 0; attempt < 6; ++attempt) {
        const auto tp = target(T);
        current = shoot(propagator, start, target, end_condition, free_space_seed(start, tp.state, T), T, fixed);
        propagations += current.propagations;
        if (current.converged) break;
        T *= 0.5;
    }
    if (!current.converged) {
        current.propagations = propagations;
        return current;
    }
    const auto budget_solve = [&](const Solution& from) {
        ShootingOptions budget = shooting;
        budget.free_time = true;
        budget.match_budget = true;
        budget.effort_budget = options.stop_effort;
        auto sol = shoot(propagator, start, target, end_condition, from.primer, from.T, budget);
        propagations += sol.propagations;
        sol.propagations = propagations;
        return sol;
    };
    if (options.stop_effort > 0.0 && current.end.effort <= options.stop_effort) {
        return budget_solve(current);
    }
    double growth = options.initial_growth;
    for (std::size_t step = 0; step < options.max_steps && T < T_to; ++step) {
        const double T_next = std::min(T_to, T * growth);
        // Tangent predictor: F_z dz = −F_T dT, with F_z from the transition matrix at T.
        Primer predicted = current.primer;
        {
            const auto end = propagator.propagate(start, current.primer, T, true);
            ++propagations;
            const auto tp = target(T);
            std::array<std::size_t, 4> rows {0, 1, 2, 3};
            std::array<double, 4> ft {};
            const Vec2 g = gravity(end.state.r);
            ft[0] = end.state.v.x - tp.velocity.x;
            ft[1] = end.state.v.y - tp.velocity.y;
            if (end_condition == EndCondition::Rendezvous) {
                ft[2] = g.x + end.primer.p.x - tp.acceleration.x;
                ft[3] = g.y + end.primer.p.y - tp.acceleration.y;
            } else {
                rows = {0, 1, 4, 5};
                ft[2] = end.primer.w.x;
                ft[3] = end.primer.w.y;
            }
            std::vector<double> a(16), b(4);
            for (std::size_t r = 0; r < 4; ++r) {
                for (std::size_t c = 0; c < 4; ++c) a[r * 4 + c] = end.stm[rows[r]][c];
                b[r] = -ft[r] * (T_next - T);
            }
            if (end.ok && solve_dense(a, b, 4)) {
                predicted.p = predicted.p + Vec2 {b[0], b[1]};
                predicted.w = predicted.w + Vec2 {b[2], b[3]};
            }
        }
        auto next = shoot(propagator, start, target, end_condition, predicted, T_next, fixed);
        propagations += next.propagations;
        if (!next.converged) {
            growth = 1.0 + 0.5 * (growth - 1.0);
            if (growth < options.min_growth) break;
            continue;
        }
        if (options.stop_effort > 0.0 && next.end.effort <= options.stop_effort) {
            return budget_solve(next);
        }
        T = T_next;
        current = std::move(next);
        growth = std::min(options.max_growth, 1.0 + 1.5 * (growth - 1.0));
    }
    current.propagations = propagations;
    if (T < T_to) current.converged = false;
    if (options.stop_effort > 0.0) current.converged = false;  // budget never reached
    return current;
}


Solution continue_in_boundary(
    const Propagator& propagator,
    const State& start,
    const Target& target,
    EndCondition end_condition,
    double T,
    const BoundaryHomotopyOptions& options,
    const ShootingOptions& shooting) {
    std::size_t propagations = 0;
    const auto coast = propagator.propagate(start, {}, T, false);
    ++propagations;
    Solution failed;
    if (!coast.ok) {
        failed.propagations = propagations;
        return failed;
    }
    const TargetPoint real = target(T);
    const State delta {real.state.r - coast.state.r, real.state.v - coast.state.v};
    const auto blended = [&](double lambda) {
        const State st {coast.state.r + lambda * delta.r, coast.state.v + lambda * delta.v};
        return fixed_target(st);
    };
    ShootingOptions corrector = shooting;
    corrector.free_time = false;
    corrector.max_iterations = options.corrector_iterations;
    corrector.stall_iterations = options.corrector_iterations;

    Primer z {};  // exact at λ = 0
    double lambda = 0.0;
    double step = options.first_step;
    Solution current;
    current.converged = true;
    current.T = T;
    while (lambda < 1.0) {
        const double next_lambda = std::min(1.0, lambda + step);
        // Predictor: F_z dz = Δ dλ (the residual's target part moves with λ).
        Primer predicted = z;
        if (lambda > 0.0) {
            const auto end = propagator.propagate(start, z, T, true);
            ++propagations;
            std::array<std::size_t, 4> rows {0, 1, 2, 3};
            std::array<double, 4> d {delta.r.x, delta.r.y, delta.v.x, delta.v.y};
            if (end_condition == EndCondition::Intercept) {
                rows = {0, 1, 4, 5};
                d[2] = d[3] = 0.0;
            }
            std::vector<double> a(16), b(4);
            for (std::size_t r = 0; r < 4; ++r) {
                for (std::size_t c = 0; c < 4; ++c) a[r * 4 + c] = end.stm[rows[r]][c];
                b[r] = d[r] * (next_lambda - lambda);
            }
            if (end.ok && solve_dense(a, b, 4)) {
                predicted.p = predicted.p + Vec2 {b[0], b[1]};
                predicted.w = predicted.w + Vec2 {b[2], b[3]};
            }
        }
        auto sol = shoot(propagator, start, blended(next_lambda), end_condition, predicted, T, corrector);
        propagations += sol.propagations;
        if (sol.converged) {
            lambda = next_lambda;
            z = sol.primer;
            current = std::move(sol);
            step = std::min(1.0, step * 2.0);
        } else {
            step *= 0.5;
            if (step < options.min_step) {
                failed.propagations = propagations;
                return failed;
            }
        }
    }
    // The blended target at λ = 1 is the real end state; for a moving target the residual is the
    // same, so the last corrector result is the answer.
    current.propagations = propagations;
    return current;
}


// ---- Solvers -----------------------------------------------------------------------------------

namespace {

bool valid(const Solution& s, const TransferRequest& request) {
    return s.converged && s.end.ok && s.end.min_radius >= request.min_radius;
}

// Shape seed for a fixed end state: best of the arrival-angle branches.
ShapeResult best_shape(const State& start, const State& end, double T, const SolveOptions& options, bool free_end) {
    ShapeOptions opt = options.shape;
    opt.free_end_velocity = free_end;
    ShapeResult best;
    for (const double th : arrival_angle_candidates(start, end, T, options.branches)) {
        auto sh = shape_solve(start, end, T, th, opt);
        if (sh.ok && (!best.ok || sh.effort < best.effort)) best = std::move(sh);
    }
    return best;
}

}  // namespace

Solution solve_transfer(const Propagator& propagator, const TransferRequest& request, double T,
    const SolveOptions& options, const Primer* warm, const char** stage) {
    Solution best;
    std::size_t propagations = 0;
    const char* best_stage = nullptr;
    const auto consider = [&](Solution s, const char* name) {
        propagations += s.propagations;
        if (valid(s, request) && (!valid(best, request) || s.end.effort < best.end.effort)) {
            best = std::move(s);
            best_stage = name;
        }
    };
    const auto shoot_from = [&](const Primer& seed, const ShootingOptions& shooting = {}) {
        return shoot(propagator, request.start, request.target, request.end_condition, seed, T, shooting);
    };
    if (warm != nullptr) consider(shoot_from(*warm), "warm");
    // The zero primer either converges fast or not at all: a short leash, the shape seed follows.
    ShootingOptions quick;
    quick.max_propagations = options.zero_max_propagations;
    quick.stall_iterations = options.zero_stall_iterations;
    if (!valid(best, request)) consider(shoot_from({}, quick), "zero");
    const bool free_end = request.end_condition == EndCondition::Intercept;
    ShapeResult shape;
    const State end = request.target(T).state;
    if (options.always_try_shape || !valid(best, request)) {
        shape = best_shape(request.start, end, T, options, free_end);
        if (shape.ok) {
            std::vector<double> times;
            std::vector<Vec2> positions, thrust;
            for (std::size_t i = 0; i < options.fit_samples; ++i) {
                const double sv = static_cast<double>(i) / static_cast<double>(options.fit_samples - 1);
                const auto pt = shape_eval(request.start, end, shape, sv, false);
                times.push_back(sv * T);
                positions.push_back(pt.state.r);
                thrust.push_back(pt.thrust);
            }
            consider(shoot_from(fit_primer(times, positions, thrust)), "shape");
        }
    }
    if (options.fallbacks && !valid(best, request)) {
        if (shape.ok) {
            const std::size_t K = std::clamp<std::size_t>(static_cast<std::size_t>(std::ceil(std::abs(shape.theta_end) / 1.5)), 2, 16);
            const std::size_t per = 25;
            std::vector<double> times;
            std::vector<Vec2> positions, thrust;
            std::vector<State> states;
            for (std::size_t i = 0; i <= K * per; ++i) {
                const double sv = static_cast<double>(i) / static_cast<double>(K * per);
                const auto pt = shape_eval(request.start, end, shape, sv, false);
                times.push_back(sv * T);
                positions.push_back(pt.state.r);
                thrust.push_back(pt.thrust);
                states.push_back(pt.state);
            }
            std::vector<Primer> history;
            fit_primer(times, positions, thrust, &history);
            std::vector<std::array<double, 8>> nodes;
            for (std::size_t k = 0; k < K; ++k) {
                const auto& st = states[k * per];
                const auto& pr = history[k * per];
                nodes.push_back({st.r.x, st.r.y, st.v.x, st.v.y, pr.p.x, pr.p.y, pr.w.x, pr.w.y});
            }
            consider(multiple_shoot(propagator, request.start, request.target, request.end_condition, T, nodes), "multiple");
        }
    }
    if (options.fallbacks && !valid(best, request)) {
        consider(continue_in_boundary(propagator, request.start, request.target, request.end_condition, T), "boundary");
    }
    if (options.fallbacks && !valid(best, request)) {
        consider(continue_in_time(propagator, request.start, request.target, request.end_condition, T), "time");
    }
    if (!valid(best, request)) best.converged = false;
    best.propagations = propagations;
    if (stage != nullptr) *stage = best_stage;
    return best;
}

Solution solve_min_time(const Propagator& propagator, const TransferRequest& request, double budget, double T_lo,
    double T_hi, const MinTimeOptions& options) {
    std::size_t propagations = 0;
    const double ratio = std::pow(T_hi / T_lo, 1.0 / static_cast<double>(std::max<std::size_t>(1, options.grid - 1)));
    Solution previous;
    bool have_previous = false;
    double previous_T = 0.0;
    SolveOptions cheap = options.solve;
    for (std::size_t i = 0; i < options.grid; ++i) {
        const double T = T_lo * std::pow(ratio, static_cast<double>(i));
        // Warm start from the previous grid point; the full portfolio when that fails.
        Solution sol;
        if (have_previous) {
            sol = shoot(propagator, request.start, request.target, request.end_condition, previous.primer, T);
            propagations += sol.propagations;
        }
        const bool warm_ok = sol.converged && sol.end.min_radius >= request.min_radius;
        if (!warm_ok || (options.recheck_over_budget && sol.end.effort > budget)) {
            // Another branch may fit the budget at this T: the portfolio, keeping the cheaper.
            auto fresh = solve_transfer(propagator, request, T, cheap);
            propagations += fresh.propagations;
            if (fresh.converged && (!warm_ok || fresh.end.effort < sol.end.effort)) sol = std::move(fresh);
            else if (!warm_ok) sol = std::move(fresh);
        }
        if (!sol.converged) {
            have_previous = false;
            continue;
        }
        if (sol.end.effort <= budget) {
            // Refine the crossing on the budget equation from the feasible side, or keep this
            // grid point when it is the first and already feasible: the answer must not be
            // shorter than T_lo (the caller's minimum transfer time).
            if (i == 0) {
                sol.propagations = propagations;
                return sol;
            }
            const Solution& from = (have_previous && previous.end.effort > budget) ? previous : sol;
            const double T_from = (have_previous && previous.end.effort > budget) ? previous_T : T;
            ShootingOptions budget_shoot;
            budget_shoot.free_time = true;
            budget_shoot.match_budget = true;
            budget_shoot.effort_budget = budget;
            budget_shoot.min_time = 0.5 * T_from;
            // Secant guess between the bracketing grid points (effort falls with T here).
            double T_guess = T;
            if (&from == &previous) {
                const double e0 = std::log(previous.end.effort), e1 = std::log(sol.end.effort);
                const double frac = (e0 - std::log(budget)) / std::max(1e-12, e0 - e1);
                T_guess = std::exp(std::log(previous_T) + frac * (std::log(T) - std::log(previous_T)));
            }
            const Primer& seed = (&from == &previous) ? previous.primer : sol.primer;
            auto refined = shoot(propagator, request.start, request.target, request.end_condition, seed, T_guess, budget_shoot);
            propagations += refined.propagations;
            if (refined.converged && refined.end.min_radius >= request.min_radius && refined.T <= T * (1.0 + 1e-9)
                && refined.T >= T_lo) {
                refined.propagations = propagations;
                return refined;
            }
            sol.propagations = propagations;
            return sol;  // grid point: feasible, slightly later than the true minimum
        }
        previous = sol;
        previous_T = T;
        have_previous = true;
    }
    Solution none;
    none.propagations = propagations;
    return none;
}


double effort_time_slope(const EndPoint& end, const TargetPoint& target) {
    return -2.0 * end.hamiltonian - 2.0 * dot(end.primer.w, target.velocity) + 2.0 * dot(end.primer.p, target.acceleration);
}

Solution fit_effort_by_time(const Propagator& propagator, const State& start, const Target& target,
    EndCondition end_condition, const Solution& from, double budget, double relative_tolerance,
    const ShootingOptions& shooting) {
    std::size_t propagations = 0;
    ShootingOptions fixed = shooting;
    fixed.free_time = false;
    Solution a = from;  // over budget
    if (a.end.effort <= budget) return a;
    const double T0 = a.T;
    const auto slope_at = [&](const Solution& s) { return effort_time_slope(s.end, target(s.T)); };
    // Newton on J(T) = budget with the exact slope. On a regular branch to a moving target the
    // effort falls as T grows; aim 5% past the Newton point (J is usually convex in T, so the
    // tangent undershoots) to land on the feasible side.
    double slope = slope_at(a);
    const double direction = slope <= 0.0 ? 1.0 : -1.0;
    double dT = slope != 0.0 ? 1.05 * (a.end.effort - budget) / std::abs(slope) : 0.05 * T0;
    dT = std::clamp(dT, 1e-3 * T0, 0.3 * T0);
    Solution feasible;
    bool bracketed = false;
    for (int iteration = 0; iteration < 20; ++iteration) {
        const double T = a.T + direction * dT;
        if (!(T > 0.05 * T0)) break;
        auto b = shoot(propagator, start, target, end_condition, a.primer, T, fixed);
        propagations += b.propagations;
        if (!b.converged || b.end.effort >= a.end.effort) {
            // Not converged, or past the effort minimum: shorter step.
            dT *= 0.3;
            if (dT < 2e-4 * T0) break;
            continue;
        }
        if (b.end.effort <= budget) {
            feasible = b;
            bracketed = true;
            if (b.end.effort >= budget * (1.0 - relative_tolerance)) break;
            // Inside [a, b]: Newton from a, regula falsi if it leaves the bracket.
            const double width = std::abs(b.T - a.T);
            const double newton = slope != 0.0 ? (a.end.effort - budget) / std::abs(slope) : -1.0;
            const double frac = (a.end.effort - budget) / (a.end.effort - b.end.effort);
            dT = (newton > 0.0 && newton < width) ? newton : frac * width;
            dT = std::max(1e-4 * T0, dT);
            continue;
        }
        // Still over budget: Newton from b.
        const double slope_b = slope_at(b);
        const double secant = (a.end.effort - b.end.effort) / std::abs(b.T - a.T);
        const double rate = (slope_b * direction < 0.0) ? std::abs(slope_b) : secant;
        // Where the effort is nearly flat in T (many-turn paths) the tangent asks for a huge
        // step: grow by at most 2.5x the last step that worked.
        const double next = std::min(1.05 * (b.end.effort - budget) / rate, 2.5 * std::abs(b.T - a.T));
        a = std::move(b);
        slope = slope_b;
        dT = bracketed ? std::min(next, std::abs(feasible.T - a.T)) : std::clamp(next, 1e-3 * T0, 0.3 * a.T);
    }
    if (!bracketed) {
        Solution none;
        none.propagations = propagations;
        return none;
    }
    feasible.propagations = propagations;
    return feasible;
}

Solution spend_effort_by_time(const Propagator& propagator, const State& start, const Target& target,
    EndCondition end_condition, const Solution& from, double budget, double min_radius, double relative_tolerance,
    const ShootingOptions& shooting) {
    std::size_t propagations = 0;
    ShootingOptions fixed = shooting;
    fixed.free_time = false;
    Solution a = from;  // feasible: effort below the budget
    if (a.end.effort >= budget * (1.0 - relative_tolerance)) return a;
    const double T0 = a.T;
    const auto slope_at = [&](const Solution& s) { return effort_time_slope(s.end, target(s.T)); };
    // Newton on J(T) = budget with the exact slope, toward shorter transfers when the effort
    // falls with T. Aim 5% short of the Newton point: J is usually convex in T, so the tangent
    // overshoots toward shorter times.
    double slope = slope_at(a);
    const double direction = slope <= 0.0 ? -1.0 : 1.0;
    double dT = slope != 0.0 ? 0.95 * (budget - a.end.effort) / std::abs(slope) : 0.05 * T0;
    dT = std::clamp(dT, 1e-3 * T0, 0.3 * T0);
    bool bracketed = false;
    double over_T = 0.0;
    double over_effort = 0.0;
    for (int iteration = 0; iteration < 16; ++iteration) {
        const double T = a.T + direction * dT;
        if (!(T > 0.05 * T0)) break;
        auto b = shoot(propagator, start, target, end_condition, a.primer, T, fixed);
        propagations += b.propagations;
        if (!b.converged || b.end.min_radius < min_radius || b.end.effort <= a.end.effort) {
            dT *= 0.3;
            if (dT < 2e-4 * T0) break;
            continue;
        }
        if (b.end.effort <= budget) {
            const double slope_b = slope_at(b);
            const double step = std::abs(b.T - a.T);
            const double secant = (b.end.effort - a.end.effort) / step;
            a = std::move(b);
            slope = slope_b;
            if (a.end.effort >= budget * (1.0 - relative_tolerance)) break;
            const double rate = (slope_b * direction > 0.0) ? std::abs(slope_b) : secant;
            // Where the effort is nearly flat in T (many-turn paths) the tangent asks for a huge
            // step: grow by at most 2.5x the last step that worked.
            const double next = std::min(0.95 * (budget - a.end.effort) / rate, 2.5 * step);
            dT = bracketed ? std::min(next, 0.9 * std::abs(over_T - a.T)) : std::clamp(next, 1e-3 * T0, 0.3 * a.T);
        } else {
            // Over the budget: Newton from a inside [a, b], regula falsi if it leaves the bracket.
            bracketed = true;
            over_T = b.T;
            over_effort = b.end.effort;
            const double width = std::abs(over_T - a.T);
            const double newton = slope * direction > 0.0 ? (budget - a.end.effort) / std::abs(slope) : -1.0;
            const double frac = (budget - a.end.effort) / (over_effort - a.end.effort);
            dT = (newton > 0.0 && newton < width) ? newton : frac * width;
            dT = std::max(1e-4 * T0, dT);
        }
    }
    a.propagations = from.propagations + propagations;
    return a;
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

ShapePoint shape_eval(const State& start, const State& end, const ShapeResult& shape, double s, bool with_rate) {
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
    if (!with_rate) return out;
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

Primer fit_primer(const std::vector<double>& times, const std::vector<Vec2>& positions, const std::vector<Vec2>& thrust,
    std::vector<Primer>* history) {
    // Propagate the four basis solutions of p'' = G(r(t)) p along the given path (positions at
    // `times`, linearly interpolated inside a step via RK4 on midpoints), then least squares.
    const std::size_t count = times.size();
    std::array<std::array<double, 4>, 4> basis {};  // [solution][p.x, p.y, w.x, w.y]
    for (std::size_t c = 0; c < 4; ++c) basis[c][c] = 1.0;
    std::vector<std::array<std::array<double, 4>, 4>> basis_history;
    if (history != nullptr) basis_history.push_back(basis);
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
        if (history != nullptr) basis_history.push_back(basis);
        accumulate(i);
    }
    std::vector<double> a(ata.begin(), ata.end());
    std::vector<double> b(atb.begin(), atb.end());
    for (std::size_t i = 0; i < 4; ++i) a[i * 4 + i] *= 1.0 + 1e-12;
    if (!solve_dense(a, b, 4)) return {};
    if (history != nullptr) {
        history->clear();
        for (const auto& bs : basis_history) {
            std::array<double, 4> y {};
            for (std::size_t c = 0; c < 4; ++c)
                for (std::size_t j = 0; j < 4; ++j) y[j] += b[c] * bs[c][j];
            history->push_back({{y[0], y[1]}, {y[2], y[3]}});
        }
    }
    return {{b[0], b[1]}, {b[2], b[3]}};
}

}  // namespace spacetrains::variable_isp::general
