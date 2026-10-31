#include "variable_isp/BandedTransfer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>

namespace spacetrains::variable_isp::banded {

Band Band::scaled(double lambda) const {
    Band out;
    out.v_max = (v_max > 0.0 && lambda > 0.0) ? v_max / lambda : 0.0;
    out.v_min = v_min * lambda;
    return out;
}

namespace {

// ---- Forward-mode dual numbers: value and derivatives along the 4 unknowns (p0, w0) ----------

struct Dual {
    double v {0.0};
    std::array<double, 4> d {};
    Dual() = default;
    Dual(double value) : v(value) {}  // NOLINT(google-explicit-constructor): constants mix in freely
};
inline Dual operator+(const Dual& a, const Dual& b) {
    Dual r(a.v + b.v);
    for (int k = 0; k < 4; ++k) r.d[k] = a.d[k] + b.d[k];
    return r;
}
inline Dual operator-(const Dual& a, const Dual& b) {
    Dual r(a.v - b.v);
    for (int k = 0; k < 4; ++k) r.d[k] = a.d[k] - b.d[k];
    return r;
}
inline Dual operator-(const Dual& a) {
    Dual r(-a.v);
    for (int k = 0; k < 4; ++k) r.d[k] = -a.d[k];
    return r;
}
inline Dual operator*(const Dual& a, const Dual& b) {
    Dual r(a.v * b.v);
    for (int k = 0; k < 4; ++k) r.d[k] = a.d[k] * b.v + a.v * b.d[k];
    return r;
}
inline Dual operator*(double a, const Dual& b) {
    Dual r(a * b.v);
    for (int k = 0; k < 4; ++k) r.d[k] = a * b.d[k];
    return r;
}
inline Dual operator/(const Dual& a, const Dual& b) {
    const double inv = 1.0 / b.v;
    Dual r(a.v * inv);
    for (int k = 0; k < 4; ++k) r.d[k] = (a.d[k] - r.v * b.d[k]) * inv;
    return r;
}
inline Dual sqrt(const Dual& a) {
    Dual r(std::sqrt(a.v));
    const double h = r.v > 0.0 ? 0.5 / r.v : 0.0;
    for (int k = 0; k < 4; ++k) r.d[k] = a.d[k] * h;
    return r;
}
inline double val(double x) { return x; }
inline double val(const Dual& x) { return x.v; }
using std::sqrt;

// ---- State and dynamics ------------------------------------------------------------------------

// r(0,1) v(2,3) p(4,5) w(6,7) s(8) λ_s(9) sweep(10) s_flown(11); error control on the first 10.
// (approach A reuses slot 9 for the ideal s.)
constexpr std::size_t kN = 12;
constexpr std::size_t kControlled = 10;
template <typename S>
using Y = std::array<S, kN>;

double inv_or_zero(double v) { return v > 0.0 ? 1.0 / v : 0.0; }

Arc classify(double pn, double lambda_s, double s, const Band& band) {
    if (lambda_s <= 0.0) return Arc::Floor;
    const double q = pn / lambda_s;
    if (band.v_min > 0.0 && q * band.v_min >= s) return Arc::Floor;
    const double a_min = s * inv_or_zero(band.v_max);
    if (q >= a_min) return Arc::Interior;
    if (q > 0.5 * a_min) return Arc::Ceiling;
    return Arc::Coast;
}

// Boundary k (between arcs k and k+1) as c_p |p| − c_l λ_s s, positive on the k+1 side.
struct BoundaryCoefficients {
    double c_p;
    double c_l;
};
BoundaryCoefficients boundary(int k, const Band& band) {
    switch (k) {
        case 0: return {2.0, inv_or_zero(band.v_max)};  // coast | ceiling
        case 1: return {1.0, inv_or_zero(band.v_max)};  // ceiling | interior
        default: return {band.v_min, 1.0};              // interior | floor
    }
}
template <typename S>
S boundary_value(const BoundaryCoefficients& b, const Y<S>& y) {
    const S pn = sqrt(y[4] * y[4] + y[5] * y[5]);
    return b.c_p * pn - b.c_l * (y[9] * y[8]);
}

template <typename S>
S thrust_on_arc(Arc arc, const S& pn, const S& lambda_s, const S& s, const Band& band) {
    switch (arc) {
        case Arc::Coast: return S(0.0);
        case Arc::Ceiling: return inv_or_zero(band.v_max) * s;
        case Arc::Interior: return pn / lambda_s;
        case Arc::Floor: return inv_or_zero(band.v_min) * s;
    }
    return S(0.0);
}

struct Gravity {
    // g and G·p for position (x, z) and primer (px, pz).
    template <typename S>
    static void eval(bool on, const S& x, const S& z, const S& px, const S& pz, S& gx, S& gz, S& gpx, S& gpz) {
        if (!on) {
            gx = gz = gpx = gpz = S(0.0);
            return;
        }
        const S r2 = x * x + z * z;
        const S rr = sqrt(r2);
        const S inv3 = S(1.0) / (r2 * rr);
        const S inv5 = inv3 / r2;
        const S rp = x * px + z * pz;
        gx = -(x * inv3);
        gz = -(z * inv3);
        gpx = -(px * inv3) + 3.0 * (x * rp * inv5);
        gpz = -(pz * inv3) + 3.0 * (z * rp * inv5);
    }
};

// Optimal-control flow on a given arc.
struct OptimalFlow {
    Band band;
    bool gravity {true};
    template <typename S>
    void operator()(const Y<S>& y, Arc arc, Y<S>& dy) const {
        S gx, gz, gpx, gpz;
        Gravity::eval(gravity, y[0], y[1], y[4], y[5], gx, gz, gpx, gpz);
        const S pn = sqrt(y[4] * y[4] + y[5] * y[5]);
        const S a = thrust_on_arc(arc, pn, y[9], y[8], band);
        S ux(0.0), uz(0.0);
        if (val(pn) > 0.0 && val(a) > 0.0) {
            const S k = a / pn;
            ux = k * y[4];
            uz = k * y[5];
        }
        dy[0] = y[2];
        dy[1] = y[3];
        dy[2] = gx + ux;
        dy[3] = gz + uz;
        dy[4] = y[6];
        dy[5] = y[7];
        dy[6] = gpx;
        dy[7] = gpz;
        dy[8] = a * a;  // l(a): a² inside the band, a·a_min = a² on the ceiling arc (a = a_min)
        if (arc == Arc::Ceiling) {
            dy[9] = inv_or_zero(band.v_max) * (2.0 * pn - 2.0 * (y[9] * a));
        } else if (arc == Arc::Floor) {
            dy[9] = inv_or_zero(band.v_min) * (2.0 * pn - 2.0 * (y[9] * a));
        } else {
            dy[9] = S(0.0);
        }
        const S r2 = y[0] * y[0] + y[1] * y[1];
        dy[10] = (y[0] * y[3] - y[1] * y[2]) / r2;
        dy[11] = dy[8];
    }
};

// Smoothed flow (ε > 0): l(a) = a·smax(a, m), m = a_min = s/v_max; a minimises
// −2|p| a + λ_s l(a) on [0, a_max] (strictly convex: a continuous control).
struct SmoothFlow {
    Band band;
    bool gravity {true};
    double eps {0.1};

    struct Pieces {
        double a {0.0};
        bool clamped {false};
    };
    // Root of φ'(a) = −2P + L (smax + a smax_a) by Newton in doubles.
    Pieces solve(double P, double L, double m, double a_max) const {
        Pieces out;
        if (L <= 0.0) {
            out.a = a_max;
            out.clamped = true;
            return out;
        }
        if (m <= 0.0) {
            out.a = std::min(P / L, a_max);
            out.clamped = P / L >= a_max;
            return out;
        }
        const double e2m2 = eps * eps * m * m;
        const auto dphi = [&](double a, double* d2) {
            const double d = a - m, q = std::sqrt(d * d + e2m2);
            const double smax = 0.5 * (a + m + q), sa = 0.5 * (1.0 + d / q), saa = 0.5 * e2m2 / (q * q * q);
            if (d2) *d2 = L * (2.0 * sa + a * saa);
            return -2.0 * P + L * (smax + a * sa);
        };
        if (dphi(0.0, nullptr) >= 0.0) return out;  // coast
        // φ' ≥ −2P + L a (smax ≥ a, smax_a ≥ 0): the root is below 2P/L.
        const double bound = 2.0 * P / L;
        if (a_max < bound && dphi(a_max, nullptr) <= 0.0) {
            out.a = a_max;
            out.clamped = true;
            return out;
        }
        double lo = 0.0, hi = std::min(a_max, bound);
        double a = std::clamp(std::max(P / L, m), lo, hi);
        for (int it = 0; it < 60; ++it) {
            double d2 = 0.0;
            const double f = dphi(a, &d2);
            (f > 0.0 ? hi : lo) = a;
            double next = a - f / d2;
            if (!(next > lo && next < hi)) next = 0.5 * (lo + hi);
            if (std::abs(next - a) <= 1e-15 * (1.0 + a)) {
                a = next;
                break;
            }
            a = next;
        }
        out.a = a;
        return out;
    }

    template <typename S>
    void operator()(const Y<S>& y, Arc, Y<S>& dy) const {
        S gx, gz, gpx, gpz;
        Gravity::eval(gravity, y[0], y[1], y[4], y[5], gx, gz, gpx, gpz);
        const S pn = sqrt(y[4] * y[4] + y[5] * y[5]);
        const S& s = y[8];
        const S& L = y[9];
        const S m = inv_or_zero(band.v_max) * s;
        const double a_max_v = band.v_min > 0.0 ? val(s) / band.v_min : std::numeric_limits<double>::infinity();
        const auto pieces = solve(val(pn), val(L), val(m), a_max_v);
        S a(0.0), l(0.0), ls_dot(0.0);
        if (pieces.a > 0.0) {
            if (pieces.clamped) {
                a = inv_or_zero(band.v_min) * s;
            } else if (val(m) <= 0.0) {
                a = pn / L;
            } else {
                // One Newton step in dual arithmetic at the root carries the derivatives.
                const double a0 = pieces.a;
                const S d = S(a0) - m;
                const S q = sqrt(d * d + eps * eps * (m * m));
                const S smax = 0.5 * (S(a0) + m + q);
                const S sa = 0.5 * (S(1.0) + d / q);
                const S saa = 0.5 * (eps * eps * (m * m)) / (q * q * q);
                const S f = -(2.0 * pn) + L * (smax + a0 * sa);
                const S f2 = L * (2.0 * sa + a0 * saa);
                a = S(a0) - f / f2;
            }
            if (val(m) > 0.0) {
                const S d = a - m;
                const S q = sqrt(d * d + eps * eps * (m * m));
                const S smax = 0.5 * (a + m + q);
                const S sm = 0.5 * (S(1.0) + (eps * eps * m - d) / q);  // ∂smax/∂m
                l = a * smax;
                ls_dot = -(inv_or_zero(band.v_max) * (L * a * sm));
                if (pieces.clamped) {
                    const S sa = 0.5 * (S(1.0) + d / q);
                    const S mu = 2.0 * pn - L * (smax + a * sa);
                    ls_dot = ls_dot + inv_or_zero(band.v_min) * mu;
                }
            } else {
                l = a * a;
                if (pieces.clamped) ls_dot = inv_or_zero(band.v_min) * (2.0 * pn - 2.0 * (L * a));
            }
        }
        S ux(0.0), uz(0.0);
        if (val(pn) > 0.0 && val(a) > 0.0) {
            const S k = a / pn;
            ux = k * y[4];
            uz = k * y[5];
        }
        dy[0] = y[2];
        dy[1] = y[3];
        dy[2] = gx + ux;
        dy[3] = gz + uz;
        dy[4] = y[6];
        dy[5] = y[7];
        dy[6] = gpx;
        dy[7] = gpz;
        dy[8] = l;
        dy[9] = ls_dot;
        const S r2 = y[0] * y[0] + y[1] * y[1];
        dy[10] = (y[0] * y[3] - y[1] * y[2]) / r2;
        const double a_v = val(a);
        dy[11] = a_v * std::max(a_v, val(y[11]) * inv_or_zero(band.v_max));
    }
};

// Approach A: the ideal thrust a = p with the banded mass flow s' = a·max(a, s/v_max); slot 9
// carries the ideal s (s' = a²).
struct FollowFlow {
    Band band;
    bool gravity {true};
    void operator()(const Y<double>& y, Arc, Y<double>& dy) const {
        double gx, gz, gpx, gpz;
        Gravity::eval(gravity, y[0], y[1], y[4], y[5], gx, gz, gpx, gpz);
        const double a = std::hypot(y[4], y[5]);
        const double a_min = y[8] * inv_or_zero(band.v_max);
        dy[0] = y[2];
        dy[1] = y[3];
        dy[2] = gx + y[4];
        dy[3] = gz + y[5];
        dy[4] = y[6];
        dy[5] = y[7];
        dy[6] = gpx;
        dy[7] = gpz;
        dy[8] = a * std::max(a, a_min);
        dy[9] = a * a;
        dy[10] = (y[0] * y[3] - y[1] * y[2]) / (y[0] * y[0] + y[1] * y[1]);
        dy[11] = 0.0;
    }
};

// ---- Runge-Kutta steps -------------------------------------------------------------------------

constexpr double d5a[7][6] = {
    {0, 0, 0, 0, 0, 0},
    {1.0 / 5.0, 0, 0, 0, 0, 0},
    {3.0 / 40.0, 9.0 / 40.0, 0, 0, 0, 0},
    {44.0 / 45.0, -56.0 / 15.0, 32.0 / 9.0, 0, 0, 0},
    {19372.0 / 6561.0, -25360.0 / 2187.0, 64448.0 / 6561.0, -212.0 / 729.0, 0, 0},
    {9017.0 / 3168.0, -355.0 / 33.0, 46732.0 / 5247.0, 49.0 / 176.0, -5103.0 / 18656.0, 0},
    {35.0 / 384.0, 0, 500.0 / 1113.0, 125.0 / 192.0, -2187.0 / 6784.0, 11.0 / 84.0},
};
constexpr double d5b[6] = {35.0 / 384.0, 0, 500.0 / 1113.0, 125.0 / 192.0, -2187.0 / 6784.0, 11.0 / 84.0};
constexpr double d5e[7] = {-71.0 / 57600.0, 0, 71.0 / 16695.0, -71.0 / 1920.0, 17253.0 / 339200.0, -22.0 / 525.0, 1.0 / 40.0};

// DOP853 (Hairer), from SciPy's dop853_coefficients.
constexpr std::size_t k8S = 12;
constexpr double k8B[12] = {0.054293734116568765, 0.0, 0.0, 0.0, 0.0, 4.450312892752409, 1.8915178993145003, -5.801203960010585, 0.3111643669578199, -0.1521609496625161, 0.20136540080403034, 0.04471061572777259};
constexpr double k8E3[13] = {-0.18980075407240762, 0.0, 0.0, 0.0, 0.0, 4.450312892752409, 1.8915178993145003, -5.801203960010585, -0.4226823213237919, -0.1521609496625161, 0.20136540080403034, 0.02265179219836082, 0.0};
constexpr double k8E5[13] = {0.01312004499419488, 0.0, 0.0, 0.0, 0.0, -1.2251564463762044, -0.4957589496572502, 1.6643771824549864, -0.35032884874997366, 0.3341791187130175, 0.08192320648511571, -0.022355307863886294, 0.0};
constexpr double k8A[12][12] = {
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

// One step of size h from (y, k1 = f(y)); writes y_new and k_end = f(y_new) and returns the
// scaled error (≤ 1: acceptable).
template <typename S, typename Flow>
double rk_step(const Flow& flow, const Y<S>& y, const Y<S>& k1, double h, Arc arc, bool high_order, double rtol,
    double atol, Y<S>& y_new, Y<S>& k_end) {
    Y<S> tmp;
    if (high_order) {
        std::array<Y<S>, k8S + 1> K;
        K[0] = k1;
        for (std::size_t stage = 1; stage < k8S; ++stage) {
            for (std::size_t i = 0; i < kN; ++i) {
                S acc(0.0);
                for (std::size_t j = 0; j < stage; ++j) {
                    if (k8A[stage][j] != 0.0) acc = acc + k8A[stage][j] * K[j][i];
                }
                tmp[i] = y[i] + h * acc;
            }
            flow(tmp, arc, K[stage]);
        }
        for (std::size_t i = 0; i < kN; ++i) {
            S acc(0.0);
            for (std::size_t j = 0; j < k8S; ++j) {
                if (k8B[j] != 0.0) acc = acc + k8B[j] * K[j][i];
            }
            y_new[i] = y[i] + h * acc;
        }
        flow(y_new, arc, K[k8S]);
        k_end = K[k8S];
        double e5 = 0.0, e3 = 0.0;
        for (std::size_t i = 0; i < kControlled; ++i) {
            double a5 = 0.0, a3 = 0.0;
            for (std::size_t j = 0; j <= k8S; ++j) {
                a5 += k8E5[j] * val(K[j][i]);
                a3 += k8E3[j] * val(K[j][i]);
            }
            const double sc = atol + rtol * std::max(std::abs(val(y[i])), std::abs(val(y_new[i])));
            e5 += (a5 / sc) * (a5 / sc);
            e3 += (a3 / sc) * (a3 / sc);
        }
        if (e5 == 0.0 && e3 == 0.0) return 0.0;
        return std::abs(h) * e5 / std::sqrt((e5 + 0.01 * e3) * static_cast<double>(kControlled));
    }
    std::array<Y<S>, 7> K;
    K[0] = k1;
    for (std::size_t stage = 1; stage < 6; ++stage) {
        for (std::size_t i = 0; i < kN; ++i) {
            S acc(0.0);
            for (std::size_t j = 0; j < stage; ++j) acc = acc + d5a[stage][j] * K[j][i];
            tmp[i] = y[i] + h * acc;
        }
        flow(tmp, arc, K[stage]);
    }
    for (std::size_t i = 0; i < kN; ++i) {
        S acc(0.0);
        for (std::size_t j = 0; j < 6; ++j) {
            if (d5b[j] != 0.0) acc = acc + d5b[j] * K[j][i];
        }
        y_new[i] = y[i] + h * acc;
    }
    flow(y_new, arc, K[6]);
    k_end = K[6];
    double err = 0.0;
    for (std::size_t i = 0; i < kControlled; ++i) {
        double e = 0.0;
        for (std::size_t j = 0; j < 7; ++j) e += d5e[j] * val(K[j][i]);
        e *= h;
        const double sc = atol + rtol * std::max(std::abs(val(y[i])), std::abs(val(y_new[i])));
        err = std::max(err, std::abs(e) / sc);
    }
    return err;
}

template <typename S>
Y<double> values(const Y<S>& y) {
    Y<double> out;
    for (std::size_t i = 0; i < kN; ++i) out[i] = val(y[i]);
    return out;
}

struct Run {
    std::size_t steps {0};
    double min_radius {std::numeric_limits<double>::infinity()};
    double time_on_arc[4] {0.0, 0.0, 0.0, 0.0};
    std::vector<Switch> switches;
    bool ok {true};
};

// Adaptive integration over [0, T] on arcs. With `events`, an arc change inside a step is located
// (Illinois on the boundary function, trial steps in the old arc's flow, values only), the step
// is cut there and, for dual numbers, the saltation term is added. `observer(t, y_values, arc)`
// runs at t = 0, after every accepted step and at every switch.
template <typename S, typename Flow, typename Observer>
Run integrate(const Flow& flow, Y<S>& y, double T, Arc arc, const Band& band, bool events, const Settings& settings,
    Observer&& observer) {
    Run run;
    Y<S> k1, y_new, k_end;
    flow(y, arc, k1);
    double t = 0.0;
    observer(t, values(y), arc);
    const double r0 = std::hypot(val(y[0]), val(y[1]));
    double h = std::min(T, (settings.high_order ? 0.05 : 0.01) * std::pow(std::max(r0, 1e-3), 1.5));
    if (!settings.gravity) h = std::min(T, 0.01 * T);
    const double rtol = settings.relative_tolerance;
    const double atol = settings.absolute_tolerance;
    const double order = settings.high_order ? 8.0 : 5.0;
    const double max_factor = settings.high_order ? 10.0 : 5.0;
    const double h_floor = 1e-14 * std::max(1.0, T);
    bool last_rejected = false;
    while (t < T) {
        if (run.steps >= settings.max_steps || run.switches.size() > settings.max_switches) {
            run.ok = false;
            return run;
        }
        if (settings.max_step > 0.0) h = std::min(h, settings.max_step);
        if (t + h > T) h = T - t;
        const double err = rk_step(flow, y, k1, h, arc, settings.high_order, rtol, atol, y_new, k_end);
        ++run.steps;
        if (!std::isfinite(err) || err > 1.0) {
            h *= std::isfinite(err) ? std::max(0.2, 0.9 * std::pow(err, -1.0 / order)) : 0.2;
            last_rejected = true;
            if (h < h_floor) {
                run.ok = false;
                return run;
            }
            continue;
        }
        if (events) {
            const auto yn = values(y_new);
            const Arc next = classify(std::hypot(yn[4], yn[5]), yn[9], yn[8], band);
            if (next != arc) {
                const int dir = static_cast<int>(next) > static_cast<int>(arc) ? 1 : -1;
                const Arc adjacent = static_cast<Arc>(static_cast<int>(arc) + dir);
                const auto bc = boundary(std::min(static_cast<int>(arc), static_cast<int>(adjacent)), band);
                const auto y0 = values(y);
                const auto k0 = values(k1);
                const double sgn = dir;  // g·sgn goes from ≤ 0 to > 0
                double ga = sgn * boundary_value(bc, y0);
                double gb = sgn * boundary_value(bc, yn);
                double ha = 0.0, hb = h;
                double h_event = 0.0;
                if (ga < 0.0 && gb > 0.0) {
                    // Illinois.
                    int side = 0;
                    Y<double> yt, kt;
                    for (int it = 0; it < 40; ++it) {
                        const double hc = (ha * gb - hb * ga) / (gb - ga);
                        rk_step(flow, y0, k0, hc, arc, settings.high_order, rtol, atol, yt, kt);
                        const double gc = sgn * boundary_value(bc, yt);
                        if (gc > 0.0) {
                            hb = hc;
                            gb = gc;
                            if (side == 1) ga *= 0.5;
                            side = 1;
                        } else {
                            ha = hc;
                            ga = gc;
                            if (side == -1) gb *= 0.5;
                            side = -1;
                        }
                        if (hb - ha < 1e-13 * std::max(1.0, T) || std::abs(gc) < 1e-15) break;
                    }
                    // The end nearer the root (Illinois often converges from one side only).
                    h_event = std::abs(ga) < std::abs(gb) ? ha : hb;
                } else if (gb <= 0.0) {
                    // The arc label moved without crossing the adjacent boundary (several
                    // boundaries at once are handled one per pass): accept the step as is.
                    h_event = -1.0;
                }
                if (h_event >= 0.0) {
                    Y<S> y_event = y;
                    Y<S> f_minus = k1;
                    if (h_event > 0.0) {
                        rk_step(flow, y, k1, h_event, arc, settings.high_order, rtol, atol, y_event, f_minus);
                    }
                    Y<S> f_plus;
                    flow(y_event, adjacent, f_plus);
                    if constexpr (std::is_same_v<S, Dual>) {
                        // δx+ = δx− + (f− − f+) δt*, δt* = −∇g·δx− / ∇g·f−.
                        const auto ye = values(y_event);
                        const auto fm = values(f_minus);
                        const double pn = std::hypot(ye[4], ye[5]);
                        const double gdot = (pn > 0.0 ? bc.c_p * (ye[4] * fm[4] + ye[5] * fm[5]) / pn : 0.0)
                            - bc.c_l * (fm[9] * ye[8] + ye[9] * fm[8]);
                        if (std::abs(gdot) > 1e-14) {
                            const Dual g = boundary_value(bc, y_event);
                            for (int k = 0; k < 4; ++k) {
                                const double dt = -g.d[k] / gdot;
                                for (std::size_t i = 0; i < kN; ++i) {
                                    y_event[i].d[k] += (fm[i] - val(f_plus[i])) * dt;
                                }
                            }
                        }
                    }
                    run.time_on_arc[static_cast<int>(arc)] += h_event;
                    t += h_event;
                    y = y_event;
                    run.switches.push_back({t, arc, adjacent});
                    arc = adjacent;
                    flow(y, arc, k1);
                    observer(t, values(y), arc);
                    last_rejected = true;  // do not grow h right after a switch
                    continue;
                }
            }
        }
        run.time_on_arc[static_cast<int>(arc)] += h;
        t += h;
        y = y_new;
        k1 = k_end;
        const double rr = std::hypot(val(y[0]), val(y[1]));
        run.min_radius = std::min(run.min_radius, rr);
        if (settings.gravity && rr < settings.min_radius) {
            run.ok = false;
            return run;
        }
        observer(t, values(y), arc);
        double factor = err == 0.0 ? max_factor : std::min(max_factor, 0.9 * std::pow(err, -1.0 / order));
        if (last_rejected) factor = std::min(1.0, factor);
        h *= factor;
        last_rejected = false;
    }
    return run;
}

template <typename S>
Y<S> initial_state(const State& start, const Primer& primer, double s0) {
    Y<S> y;
    y[0] = start.r.x;
    y[1] = start.r.y;
    y[2] = start.v.x;
    y[3] = start.v.y;
    y[4] = primer.p.x;
    y[5] = primer.p.y;
    y[6] = primer.w.x;
    y[7] = primer.w.y;
    y[8] = s0;
    y[9] = 1.0;
    y[10] = 0.0;
    y[11] = s0;
    if constexpr (std::is_same_v<S, Dual>) {
        y[4].d[0] = 1.0;
        y[5].d[1] = 1.0;
        y[6].d[2] = 1.0;
        y[7].d[3] = 1.0;
    }
    return y;
}

Sample make_sample(double t, const Y<double>& y, Arc arc, const Band& band) {
    Sample sample;
    sample.t = t;
    sample.state = {{y[0], y[1]}, {y[2], y[3]}};
    sample.s = y[8];
    sample.lambda_s = y[9];
    sample.arc = arc;
    const double pn = std::hypot(y[4], y[5]);
    const double a = thrust_on_arc(arc, pn, y[9], y[8], band);
    if (pn > 0.0) sample.thrust = {a * y[4] / pn, a * y[5] / pn};
    return sample;
}

}  // namespace

EndPoint Propagator::propagate(const State& start, const Primer& primer, double s0, const Band& band, double T,
    bool with_jacobian, std::vector<Sample>* samples, double smoothing) const {
    EndPoint out;
    const bool smooth = smoothing > 0.0 && band.v_max > 0.0;
    const Arc arc0 = smooth ? Arc::Interior : classify(std::hypot(primer.p.x, primer.p.y), 1.0, s0, band);
    const auto observer = [&](double t, const Y<double>& y, Arc arc) {
        if (samples) samples->push_back(make_sample(t, y, arc, band));
    };
    const auto finish = [&](const auto& y, const Run& run, Arc arc_end) {
        const auto yv = values(y);
        out.state = {{yv[0], yv[1]}, {yv[2], yv[3]}};
        out.primer = {{yv[4], yv[5]}, {yv[6], yv[7]}};
        out.s = yv[8];
        out.s_flown = yv[11];
        out.lambda_s = yv[9];
        out.sweep_rad = yv[10];
        out.min_radius = run.min_radius;
        out.steps = run.steps;
        out.switches = run.switches;
        for (int k = 0; k < 4; ++k) out.time_on_arc[k] = run.time_on_arc[k];
        double gx, gz, gpx, gpz;
        Gravity::eval(settings_.gravity, yv[0], yv[1], yv[4], yv[5], gx, gz, gpx, gpz);
        const double pn = std::hypot(yv[4], yv[5]);
        const double a = thrust_on_arc(arc_end, pn, yv[9], yv[8], band);
        const double ux = pn > 0.0 ? a * yv[4] / pn : 0.0, uz = pn > 0.0 ? a * yv[5] / pn : 0.0;
        out.hamiltonian = 2.0 * (yv[6] * yv[2] + yv[7] * yv[3]) - 2.0 * (yv[4] * (gx + ux) + yv[5] * (gz + uz))
            + yv[9] * a * a;
        out.ok = run.ok;
    };
    const auto run_flow = [&](const auto& flow, bool events) {
        if (with_jacobian) {
            auto y = initial_state<Dual>(start, primer, s0);
            Arc arc = arc0;
            const auto run = integrate<Dual>(flow, y, T, arc, band, events, settings_,
                [&](double t, const Y<double>& yv, Arc a) { arc = a; observer(t, yv, a); });
            finish(y, run, arc);
            for (int row = 0; row < 4; ++row) {
                for (int k = 0; k < 4; ++k) out.jacobian[row][k] = y[row].d[k];
            }
        } else {
            auto y = initial_state<double>(start, primer, s0);
            Arc arc = arc0;
            const auto run = integrate<double>(flow, y, T, arc, band, events, settings_,
                [&](double t, const Y<double>& yv, Arc a) { arc = a; observer(t, yv, a); });
            finish(y, run, arc);
        }
    };
    if (smooth) {
        run_flow(SmoothFlow {band, settings_.gravity, smoothing}, false);
    } else {
        run_flow(OptimalFlow {band, settings_.gravity}, band.limited());
    }
    return out;
}

FollowResult Propagator::follow_ideal(const State& start, const Primer& ideal, double s0, const Band& band, double T,
    std::vector<Sample>* samples) const {
    FollowResult out;
    const FollowFlow flow {band, settings_.gravity};
    auto y = initial_state<double>(start, ideal, s0);
    y[9] = s0;
    double last_t = 0.0;
    bool last_throttled = false;
    const auto run = integrate<double>(flow, y, T, Arc::Interior, band, false, settings_,
        [&](double t, const Y<double>& yv, Arc) {
            const double a = std::hypot(yv[4], yv[5]);
            if (band.v_min > 0.0) out.floor_load = std::max(out.floor_load, a * band.v_min / yv[8]);
            const bool throttled = band.v_max > 0.0 && a * band.v_max < yv[8];
            if (last_throttled && throttled) out.throttled_time += t - last_t;
            last_throttled = throttled;
            last_t = t;
            if (samples) {
                Sample sample;
                sample.t = t;
                sample.state = {{yv[0], yv[1]}, {yv[2], yv[3]}};
                sample.thrust = {yv[4], yv[5]};
                sample.s = yv[8];
                sample.lambda_s = 1.0;
                sample.arc = throttled ? Arc::Ceiling : (band.v_min > 0.0 && a * band.v_min >= yv[8] ? Arc::Floor : Arc::Interior);
                samples->push_back(sample);
            }
        });
    out.s_end = y[8];
    out.ideal_s_end = y[9];
    out.end = {{y[0], y[1]}, {y[2], y[3]}};
    out.steps = run.steps;
    out.ok = run.ok;
    return out;
}

// ---- Newton ------------------------------------------------------------------------------------

namespace {

bool solve4(std::array<std::array<double, 4>, 4> A, std::array<double, 4> b, std::array<double, 4>& x) {
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r) {
            if (std::abs(A[r][c]) > std::abs(A[piv][c])) piv = r;
        }
        if (!(std::abs(A[piv][c]) > 1e-300)) return false;
        std::swap(A[c], A[piv]);
        std::swap(b[c], b[piv]);
        for (int r = c + 1; r < 4; ++r) {
            const double q = A[r][c] / A[c][c];
            for (int k = c; k < 4; ++k) A[r][k] -= q * A[c][k];
            b[r] -= q * b[c];
        }
    }
    for (int r = 3; r >= 0; --r) {
        double acc = b[r];
        for (int k = r + 1; k < 4; ++k) acc -= A[r][k] * x[k];
        x[r] = acc / A[r][r];
    }
    return std::all_of(x.begin(), x.end(), [](double v) { return std::isfinite(v); });
}

std::array<double, 4> to_z(const Primer& p) { return {p.p.x, p.p.y, p.w.x, p.w.y}; }
Primer from_z(const std::array<double, 4>& z) { return {{z[0], z[1]}, {z[2], z[3]}}; }

std::array<double, 4> residual(const EndPoint& end, const TargetPoint& target) {
    return {end.state.r.x - target.state.r.x, end.state.r.y - target.state.r.y, end.state.v.x - target.state.v.x,
        end.state.v.y - target.state.v.y};
}
double max_abs(const std::array<double, 4>& f) {
    double m = 0.0;
    for (double v : f) m = std::max(m, std::abs(v));
    return m;
}
double norm2(const std::array<double, 4>& f) {
    double m = 0.0;
    for (double v : f) m += v * v;
    return std::sqrt(m);
}

bool usable(const EndPoint& end, const Request& request) {
    return end.ok && end.min_radius >= request.min_radius;
}

}  // namespace

Solution shoot(const Propagator& propagator, const Request& request, double T, const Primer& guess,
    const Options& options, double tolerance, double smoothing) {
    Solution out;
    out.T = T;
    const double tol = tolerance > 0.0 ? tolerance : options.tolerance;
    const TargetPoint target = request.target(T);
    auto z = to_z(guess);
    const auto evaluate = [&](const std::array<double, 4>& zz, bool jac) {
        ++out.propagations;
        auto end = propagator.propagate(request.start, from_z(zz), request.s0, request.band, T, jac && !options.finite_difference,
            nullptr, smoothing);
        if (jac && options.finite_difference && end.ok) {
            const auto f0 = residual(end, target);
            for (int k = 0; k < 4; ++k) {
                auto zp = zz;
                const double h = options.fd_step * std::max(1.0, std::abs(zz[k]));
                zp[k] += h;
                ++out.propagations;
                const auto ep = propagator.propagate(request.start, from_z(zp), request.s0, request.band, T, false, nullptr, smoothing);
                if (!ep.ok) {
                    end.ok = false;
                    break;
                }
                const auto fp = residual(ep, target);
                for (int row = 0; row < 4; ++row) end.jacobian[row][k] = (fp[row] - f0[row]) / h;
            }
        }
        return end;
    };
    auto end = evaluate(z, true);
    if (!usable(end, request)) return out;
    auto f = residual(end, target);
    for (out.iterations = 0; out.iterations < options.max_iterations; ++out.iterations) {
        if (options.trace) {
            std::fprintf(stderr, "    newton %zu |f| %.3e switches %zu s %.6f\n", out.iterations, max_abs(f), end.switches.size(), end.s);
        }
        if (max_abs(f) < tol) break;
        std::array<double, 4> dz {};
        std::array<double, 4> rhs {-f[0], -f[1], -f[2], -f[3]};
        if (!solve4(end.jacobian, rhs, dz)) break;
        bool accepted = false;
        // The full step with its Jacobian (usually accepted); shorter steps without, and the
        // Jacobian then for the accepted one.
        for (double step = 1.0; step >= 1.0 / 64.0; step *= 0.5) {
            std::array<double, 4> zt {};
            for (int k = 0; k < 4; ++k) zt[k] = z[k] + step * dz[k];
            auto trial = evaluate(zt, step == 1.0);
            if (!usable(trial, request)) continue;
            const auto ft = residual(trial, target);
            if (norm2(ft) < (1.0 - 1e-4 * step) * norm2(f) || max_abs(ft) < tol) {
                if (step != 1.0 && max_abs(ft) >= tol) {
                    trial = evaluate(zt, true);
                    if (!usable(trial, request)) continue;
                }
                z = zt;
                f = ft;
                end = std::move(trial);
                accepted = true;
                break;
            }
        }
        if (!accepted) break;
    }
    out.primer = from_z(z);
    out.end = std::move(end);
    out.residual = max_abs(f);
    out.converged = out.residual < tol;
    return out;
}

namespace {

struct Counters {
    std::size_t propagations {0};
    std::size_t iterations {0};
    std::size_t steps {0};
    void add(const Solution& s) {
        propagations += s.propagations;
        iterations += s.iterations;
    }
};

// Band tightness λ: 0 (the ideal solution z) → 1 at smoothing ε, secant predictor, adaptive steps.
std::optional<Solution> continue_band(const Propagator& propagator, const Request& request, double T, std::array<double, 4> z,
    double eps, double final_tolerance, const Options& options, Counters& counters) {
    double lambda = 0.0, step = options.first_step, lambda_prev = -1.0;
    std::array<double, 4> z_prev {};
    Solution last;
    while (lambda < 1.0 && counters.steps < options.max_continuation_steps && counters.propagations < options.max_propagations) {
        const double next = std::min(1.0, lambda + step);
        auto guess = z;
        if (options.predictor && lambda_prev >= 0.0) {
            const double f = (next - lambda) / (lambda - lambda_prev);
            for (int k = 0; k < 4; ++k) guess[k] = z[k] + f * (z[k] - z_prev[k]);
        }
        Request scaled = request;
        scaled.band = request.band.scaled(next);
        const double tol = next >= 1.0 ? final_tolerance : options.continuation_tolerance;
        auto solved = shoot(propagator, scaled, T, from_z(guess), options, tol, eps);
        if (!solved.converged && options.predictor && lambda_prev >= 0.0) {
            counters.add(solved);
            solved = shoot(propagator, scaled, T, from_z(z), options, tol, eps);
        }
        counters.add(solved);
        ++counters.steps;
        if (options.trace) {
            std::fprintf(stderr, "  lambda %.5f eps %.3g -> %s (|f| %.2e, %zu it)\n", next, eps, solved.converged ? "ok" : "FAIL",
                solved.residual, solved.iterations);
        }
        if (solved.converged) {
            lambda_prev = lambda;
            z_prev = z;
            lambda = next;
            z = to_z(solved.primer);
            last = std::move(solved);
            step = std::min(1.0, step * options.growth);
        } else {
            step *= options.shrink;
            if (step < options.min_step) break;
        }
    }
    if (lambda < 1.0) return std::nullopt;
    return last;
}

}  // namespace

Solution solve_fixed_time(const Propagator& propagator, const Request& request, double T, const Primer& ideal,
    const Options& options, const Primer* warm) {
    Counters counters;
    // The throttled ideal path is a feasible control (when it respects the floor): an extremal
    // that burns more is on another branch.
    double bound = std::numeric_limits<double>::infinity();
    if (request.band.limited()) {
        const auto followed = propagator.follow_ideal(request.start, ideal, request.s0, request.band, T);
        ++counters.propagations;
        if (followed.ok && followed.floor_ok()) bound = followed.s_end * (1.0 + 1e-9);
    }
    bool rejected = false;
    const auto accept = [&](Solution sol, bool exact, bool from_warm) -> std::optional<Solution> {
        if (sol.end.s_flown > bound) {
            rejected = true;
            return std::nullopt;
        }
        sol.exact = exact;
        sol.from_warm = from_warm;
        sol.converged = true;
        return sol;
    };
    const auto finish = [&](std::optional<Solution> sol) {
        Solution out = sol ? std::move(*sol) : Solution {};
        out.converged = sol.has_value();
        out.rejected_worse = rejected;
        out.propagations = counters.propagations;
        out.iterations = counters.iterations;
        out.continuation_steps = counters.steps;
        out.T = T;
        return out;
    };
    // 1. Warm start (a neighbouring solution), exact switching.
    if (warm != nullptr) {
        auto direct = shoot(propagator, request, T, *warm, options);
        counters.add(direct);
        if (direct.converged) {
            if (auto ok = accept(std::move(direct), true, true)) return finish(std::move(ok));
        }
    }
    // 2. Band continuation from the ideal solution with exact switching.
    if (auto sol = continue_band(propagator, request, T, to_z(ideal), 0.0, options.tolerance, options, counters)) {
        if (auto ok = accept(std::move(*sol), true, false)) return finish(std::move(ok));
    }
    // 3. Fallback: smoothed band continuation, smoothing reduced, exact polish (or the smallest
    //    smoothing's plan when a grazing switch defeats the exact problem).
    if (!(options.smoothing > 0.0) || !(request.band.v_max > 0.0)) return finish(std::nullopt);
    auto smooth = continue_band(propagator, request, T, to_z(ideal), options.smoothing, options.continuation_tolerance, options, counters);
    if (!smooth) return finish(std::nullopt);
    double eps = options.smoothing;
    std::array<double, 4> z = to_z(smooth->primer), z_prev = z;
    double eps_prev = -1.0;
    std::size_t splits = 0;
    Solution last = std::move(*smooth);
    while (eps > options.smoothing_min * 1.0001 && counters.steps < options.max_continuation_steps
        && counters.propagations < options.max_propagations) {
        double target = std::max(options.smoothing_min, eps * options.smoothing_factor);
        bool ok = false;
        while (!ok) {
            auto guess = z;
            if (options.predictor && eps_prev > 0.0) {
                const double f = std::log(target / eps) / std::log(eps / eps_prev);
                for (int k = 0; k < 4; ++k) guess[k] = z[k] + f * (z[k] - z_prev[k]);
            }
            auto solved = shoot(propagator, request, T, from_z(guess), options, options.continuation_tolerance, target);
            counters.add(solved);
            ++counters.steps;
            if (options.trace) {
                std::fprintf(stderr, "  eps %.3g -> %s (|f| %.2e, %zu it)\n", target, solved.converged ? "ok" : "FAIL", solved.residual,
                    solved.iterations);
            }
            if (counters.propagations >= options.max_propagations) break;
            if (solved.converged) {
                eps_prev = eps;
                z_prev = z;
                eps = target;
                z = to_z(solved.primer);
                last = std::move(solved);
                ok = true;
            } else {
                const double mid = std::sqrt(eps * target);
                if (mid / target < 1.05 || ++splits > options.max_smoothing_splits) break;
                target = mid;
            }
        }
        if (!ok) return finish(std::nullopt);
    }
    if (options.exact_polish) {
        auto exact = shoot(propagator, request, T, from_z(z), options, options.tolerance, 0.0);
        counters.add(exact);
        ++counters.steps;
        if (options.trace) std::fprintf(stderr, "  exact polish -> %s (|f| %.2e)\n", exact.converged ? "ok" : "FAIL", exact.residual);
        if (exact.converged) {
            if (auto ok = accept(std::move(exact), true, false)) return finish(std::move(ok));
        }
    }
    // The smallest smoothing's plan, polished to the full tolerance.
    auto polished = shoot(propagator, request, T, from_z(z), options, options.tolerance, eps);
    counters.add(polished);
    if (polished.end.ok && polished.residual < last.residual) last = std::move(polished);
    if (max_abs(residual(last.end, request.target(T))) > options.smoothed_tolerance) return finish(std::nullopt);
    return finish(accept(std::move(last), false, false));
}

}  // namespace spacetrains::variable_isp::banded
