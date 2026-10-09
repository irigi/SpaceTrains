#include "variable_isp/VariableIsp.hpp"

#include "util/Profiling.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace spacetrains::variable_isp {

namespace {

constexpr std::uint64_t kAtlasMagic = 0x3154415053495654ULL;  // "TVISPAT1"
constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 2.0 * kPi;

using State = std::array<double, 8>;
using StateWide = std::array<double, 8>;

template <typename T>
T read_value(std::ifstream& stream) {
    T value {};
    stream.read(reinterpret_cast<char*>(&value), sizeof(T));
    if (!stream) {
        throw std::runtime_error("Unexpected end of VariableISP binary atlas");
    }
    return value;
}

void read_exact(std::ifstream& stream, void* buffer, std::size_t size) {
    stream.read(reinterpret_cast<char*>(buffer), static_cast<std::streamsize>(size));
    if (!stream) {
        throw std::runtime_error("Unexpected end of VariableISP binary atlas");
    }
}

double wrapped_delta(double a, double b) {
    return VariableIspIntegrator::normalize_angle(a - b);
}

void ode_system(const StateWide& y, const CanonicalMissionConfig& config, double c_theta, StateWide& dydt) {
    const double r = y[0];
    const double v_r = y[2];
    const double v_theta = y[3];
    const double m = y[4];
    const double lambda_r = y[5];
    const double lambda_vr = y[6];
    const double lambda_vtheta = y[7];

    const double a_r = static_cast<double>(config.k_gain) * lambda_vr;
    const double a_theta = static_cast<double>(config.k_gain) * lambda_vtheta;
    const double accel_sq = (a_r * a_r) + (a_theta * a_theta);

    dydt[0] = v_r;
    dydt[1] = v_theta / r;
    dydt[2] = (v_theta * v_theta) / r - (static_cast<double>(config.mu_m3_s2) / (r * r)) + a_r;
    dydt[3] = -(v_r * v_theta) / r + a_theta;
    dydt[4] = -(m * m / (2.0 * static_cast<double>(config.power_w))) * accel_sq;
    dydt[5] = (c_theta * v_theta) / (r * r)
        + (lambda_vr * v_theta * v_theta) / (r * r)
        - (2.0 * lambda_vr * static_cast<double>(config.mu_m3_s2)) / (r * r * r)
        - (lambda_vtheta * v_r * v_theta) / (r * r);
    dydt[6] = -lambda_r + (lambda_vtheta * v_theta) / r;
    dydt[7] = -c_theta / r - (2.0 * lambda_vr * v_theta) / r + (lambda_vtheta * v_r) / r;
}

double rms_norm(const StateWide& x) {
    double sum = 0.0;
    for (const double value : x) {
        sum += value * value;
    }
    return std::sqrt(sum / static_cast<double>(x.size()));
}

double select_initial_step(
    const StateWide& y0,
    const StateWide& f0,
    double interval_length,
    double max_step,
    const IntegratorSettings& settings,
    const CanonicalMissionConfig& config,
    double c_theta) {
    StateWide scale {};
    for (std::size_t idx = 0; idx < y0.size(); ++idx) {
        scale[idx] = static_cast<double>(settings.absolute_tolerance)
            + std::abs(y0[idx]) * static_cast<double>(settings.relative_tolerance);
    }

    StateWide y_scaled {};
    StateWide f_scaled {};
    for (std::size_t idx = 0; idx < y0.size(); ++idx) {
        y_scaled[idx] = y0[idx] / scale[idx];
        f_scaled[idx] = f0[idx] / scale[idx];
    }

    const double d0 = rms_norm(y_scaled);
    const double d1 = rms_norm(f_scaled);
    double h0 = (d0 < 1e-5 || d1 < 1e-5) ? 1e-6 : (0.01 * d0 / d1);
    h0 = std::min(h0, interval_length);

    StateWide y1 = y0;
    for (std::size_t idx = 0; idx < y0.size(); ++idx) {
        y1[idx] += h0 * f0[idx];
    }

    StateWide f1 {};
    ode_system(y1, config, c_theta, f1);

    StateWide f_delta {};
    for (std::size_t idx = 0; idx < y0.size(); ++idx) {
        f_delta[idx] = (f1[idx] - f0[idx]) / scale[idx];
    }
    const double d2 = rms_norm(f_delta) / h0;

    double h1 = 0.0;
    if (d1 <= 1e-15 && d2 <= 1e-15) {
        h1 = std::max(1e-6, h0 * 1e-3);
    } else {
        h1 = std::pow(0.01 / std::max(d1, d2), 1.0 / 5.0);
    }

    return std::min({100.0 * h0, h1, interval_length, max_step});
}

void rk45_step(
    const StateWide& y,
    const StateWide& f,
    double dt,
    const CanonicalMissionConfig& config,
    double c_theta,
    std::array<StateWide, 7>& k,
    StateWide& y_new,
    StateWide& f_new) {
    constexpr std::array<std::array<double, 5>, 6> a {{
        {{0.0, 0.0, 0.0, 0.0, 0.0}},
        {{1.0 / 5.0, 0.0, 0.0, 0.0, 0.0}},
        {{3.0 / 40.0, 9.0 / 40.0, 0.0, 0.0, 0.0}},
        {{44.0 / 45.0, -56.0 / 15.0, 32.0 / 9.0, 0.0, 0.0}},
        {{19372.0 / 6561.0, -25360.0 / 2187.0, 64448.0 / 6561.0, -212.0 / 729.0, 0.0}},
        {{9017.0 / 3168.0, -355.0 / 33.0, 46732.0 / 5247.0, 49.0 / 176.0, -5103.0 / 18656.0}},
    }};
    constexpr std::array<double, 6> b {35.0 / 384.0, 0.0, 500.0 / 1113.0, 125.0 / 192.0, -2187.0 / 6784.0, 11.0 / 84.0};

    k[0] = f;
    for (std::size_t stage = 1; stage < 6; ++stage) {
        StateWide y_stage = y;
        for (std::size_t prev = 0; prev < stage; ++prev) {
            for (std::size_t idx = 0; idx < y.size(); ++idx) {
                y_stage[idx] += dt * a[stage][prev] * k[prev][idx];
            }
        }
        ode_system(y_stage, config, c_theta, k[stage]);
    }

    y_new = y;
    for (std::size_t idx = 0; idx < y.size(); ++idx) {
        for (std::size_t stage = 0; stage < 6; ++stage) {
            y_new[idx] += dt * b[stage] * k[stage][idx];
        }
    }
    ode_system(y_new, config, c_theta, f_new);
    k[6] = f_new;
}

double estimate_error_norm(
    const std::array<StateWide, 7>& k,
    double dt,
    const StateWide& y,
    const StateWide& y_new,
    const IntegratorSettings& settings) {
    constexpr std::array<double, 7> e {
        -71.0 / 57600.0, 0.0, 71.0 / 16695.0, -71.0 / 1920.0,
        17253.0 / 339200.0, -22.0 / 525.0, 1.0 / 40.0,
    };

    StateWide scaled_error {};
    for (std::size_t idx = 0; idx < y.size(); ++idx) {
        double err = 0.0;
        for (std::size_t stage = 0; stage < e.size(); ++stage) {
            err += e[stage] * k[stage][idx];
        }
        err *= dt;
        const double scale = static_cast<double>(settings.absolute_tolerance)
            + std::max(std::abs(y[idx]), std::abs(y_new[idx])) * static_cast<double>(settings.relative_tolerance);
        scaled_error[idx] = err / scale;
    }
    return rms_norm(scaled_error);
}

StateWide interpolate_dense_output(
    const std::array<StateWide, 7>& k,
    const StateWide& y_old,
    double t_old,
    double t_new,
    double t_query) {
    constexpr std::array<std::array<double, 4>, 7> p {{
        {{1.0, -8048581381.0 / 2820520608.0, 8663915743.0 / 2820520608.0, -12715105075.0 / 11282082432.0}},
        {{0.0, 0.0, 0.0, 0.0}},
        {{0.0, 131558114200.0 / 32700410799.0, -68118460800.0 / 10900136933.0, 87487479700.0 / 32700410799.0}},
        {{0.0, -1754552775.0 / 470086768.0, 14199869525.0 / 1410260304.0, -10690763975.0 / 1880347072.0}},
        {{0.0, 127303824393.0 / 49829197408.0, -318862633887.0 / 49829197408.0, 701980252875.0 / 199316789632.0}},
        {{0.0, -282668133.0 / 205662961.0, 2019193451.0 / 616988883.0, -1453857185.0 / 822651844.0}},
        {{0.0, 40617522.0 / 29380423.0, -110615467.0 / 29380423.0, 69997945.0 / 29380423.0}},
    }};

    const double h = t_new - t_old;
    const double x = (t_query - t_old) / h;
    const std::array<double, 4> powers {x, x * x, x * x * x, x * x * x * x};

    StateWide out = y_old;
    for (std::size_t idx = 0; idx < out.size(); ++idx) {
        double q = 0.0;
        for (std::size_t stage = 0; stage < p.size(); ++stage) {
            double stage_poly = 0.0;
            for (std::size_t order = 0; order < powers.size(); ++order) {
                stage_poly += p[stage][order] * powers[order];
            }
            q += k[stage][idx] * stage_poly;
        }
        out[idx] += h * q;
    }
    return out;
}

std::array<double, 6> unpack_seed(const AtlasSeed& seed) {
    return {
        seed.params[0],
        seed.params[1],
        seed.params[2],
        seed.params[3],
        seed.params[4],
        seed.transfer_time_days,
    };
}

}  // namespace

void VariableIspAtlas::load_binary(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Could not open VariableISP atlas: " + path);
    }

    const std::uint64_t magic = read_value<std::uint64_t>(stream);
    if (magic != kAtlasMagic) {
        throw std::runtime_error("Unexpected VariableISP atlas magic in " + path);
    }

    const auto rho_count = static_cast<std::size_t>(read_value<std::uint64_t>(stream));
    const auto kappa_count = static_cast<std::size_t>(read_value<std::uint64_t>(stream));
    const auto theta_count = static_cast<std::size_t>(read_value<std::uint64_t>(stream));

    rho_grid_.resize(rho_count);
    kappa_grid_.resize(kappa_count);
    theta_grid_.resize(theta_count);
    read_exact(stream, rho_grid_.data(), rho_grid_.size() * sizeof(double));
    read_exact(stream, kappa_grid_.data(), kappa_grid_.size() * sizeof(double));
    read_exact(stream, theta_grid_.data(), theta_grid_.size() * sizeof(double));

    const auto total_cells = rho_count * kappa_count * theta_count;
    solved_mask_.resize(total_cells);
    read_exact(stream, solved_mask_.data(), solved_mask_.size() * sizeof(std::uint8_t));

    records_.resize(total_cells * kRecordWidth);
    read_exact(stream, records_.data(), records_.size() * sizeof(double));
}

std::size_t VariableIspAtlas::solved_count() const {
    return static_cast<std::size_t>(std::count(solved_mask_.begin(), solved_mask_.end(), std::uint8_t {1}));
}

std::size_t VariableIspAtlas::cell_index(std::size_t i, std::size_t j, std::size_t k) const {
    return ((i * kappa_grid_.size()) + j) * theta_grid_.size() + k;
}

bool VariableIspAtlas::is_solved(std::size_t i, std::size_t j, std::size_t k) const {
    return solved_mask_.at(cell_index(i, j, k)) != 0;
}

AtlasSeed VariableIspAtlas::seed_at(std::size_t i, std::size_t j, std::size_t k) const {
    if (!is_solved(i, j, k)) {
        throw std::runtime_error("Requested unsolved VariableISP atlas cell");
    }

    const auto offset = cell_index(i, j, k) * kRecordWidth;
    AtlasSeed seed;
    for (std::size_t idx = 0; idx < seed.params.size(); ++idx) {
        seed.params[idx] = records_.at(offset + idx);
    }
    seed.transfer_time_days = records_.at(offset + seed.params.size());
    return seed;
}

std::size_t VariableIspAtlas::lower_cell_index(const std::vector<double>& axis, double value) const {
    if (axis.size() < 2) {
        throw std::runtime_error("VariableISP axis must contain at least 2 entries");
    }
    if (value <= axis.front()) {
        return 0;
    }
    if (value >= axis.back()) {
        return axis.size() - 2;
    }

    const auto upper = std::lower_bound(axis.begin(), axis.end(), value);
    const auto index = static_cast<std::size_t>(std::distance(axis.begin(), upper));
    if (*upper == value) {
        return std::min(index, axis.size() - 2);
    }
    return index - 1;
}

Index3 VariableIspAtlas::nearest_solved_index(double rho, double kappa, double theta_rad, std::size_t search_radius) const {
    const auto i0 = lower_cell_index(rho_grid_, rho);
    const auto j0 = lower_cell_index(kappa_grid_, kappa);
    const auto k0 = lower_cell_index(theta_grid_, theta_rad);

    double best_score = std::numeric_limits<double>::infinity();
    Index3 best {};
    bool found = false;

    const auto i_min = (i0 > search_radius) ? i0 - search_radius : 0;
    const auto j_min = (j0 > search_radius) ? j0 - search_radius : 0;
    const auto k_min = (k0 > search_radius) ? k0 - search_radius : 0;
    const auto i_max = std::min(rho_grid_.size() - 1, i0 + search_radius + 1);
    const auto j_max = std::min(kappa_grid_.size() - 1, j0 + search_radius + 1);
    const auto k_max = std::min(theta_grid_.size() - 1, k0 + search_radius + 1);

    for (std::size_t i = i_min; i <= i_max; ++i) {
        for (std::size_t j = j_min; j <= j_max; ++j) {
            for (std::size_t k = k_min; k <= k_max; ++k) {
                if (!is_solved(i, j, k)) {
                    continue;
                }

                const double score = std::abs(std::log(rho_grid_[i] / rho))
                    + std::abs(std::log(kappa_grid_[j] / kappa))
                    + std::abs(wrapped_delta(theta_grid_[k], theta_rad));
                if (score < best_score) {
                    best_score = score;
                    best = {i, j, k};
                    found = true;
                }
            }
        }
    }

    if (!found) {
        for (std::size_t i = 0; i < rho_grid_.size(); ++i) {
            for (std::size_t j = 0; j < kappa_grid_.size(); ++j) {
                for (std::size_t k = 0; k < theta_grid_.size(); ++k) {
                    if (!is_solved(i, j, k)) {
                        continue;
                    }
                    const double score = std::abs(std::log(rho_grid_[i] / rho))
                        + std::abs(std::log(kappa_grid_[j] / kappa))
                        + std::abs(wrapped_delta(theta_grid_[k], theta_rad));
                    if (score < best_score) {
                        best_score = score;
                        best = {i, j, k};
                        found = true;
                    }
                }
            }
        }
    }

    if (!found) {
        throw std::runtime_error("No solved VariableISP atlas cell found in atlas");
    }
    return best;
}

AtlasSeed VariableIspAtlas::query(double rho, double kappa, double theta_rad, std::size_t search_radius) const {
    const auto i0 = lower_cell_index(rho_grid_, rho);
    const auto j0 = lower_cell_index(kappa_grid_, kappa);
    const auto k0 = lower_cell_index(theta_grid_, theta_rad);

    const std::size_t i1 = std::min(i0 + 1, rho_grid_.size() - 1);
    const std::size_t j1 = std::min(j0 + 1, kappa_grid_.size() - 1);
    const std::size_t k1 = std::min(k0 + 1, theta_grid_.size() - 1);

    const bool have_full_cube =
        is_solved(i0, j0, k0) && is_solved(i1, j0, k0) &&
        is_solved(i0, j1, k0) && is_solved(i1, j1, k0) &&
        is_solved(i0, j0, k1) && is_solved(i1, j0, k1) &&
        is_solved(i0, j1, k1) && is_solved(i1, j1, k1);

    if (!have_full_cube) {
        const auto nearest = nearest_solved_index(rho, kappa, theta_rad, search_radius);
        return seed_at(nearest.i, nearest.j, nearest.k);
    }

    const double tx = (rho - rho_grid_[i0]) / (rho_grid_[i1] - rho_grid_[i0]);
    const double ty = (kappa - kappa_grid_[j0]) / (kappa_grid_[j1] - kappa_grid_[j0]);
    const double tz = (theta_rad - theta_grid_[k0]) / (theta_grid_[k1] - theta_grid_[k0]);

    const auto c000 = unpack_seed(seed_at(i0, j0, k0));
    const auto c100 = unpack_seed(seed_at(i1, j0, k0));
    const auto c010 = unpack_seed(seed_at(i0, j1, k0));
    const auto c110 = unpack_seed(seed_at(i1, j1, k0));
    const auto c001 = unpack_seed(seed_at(i0, j0, k1));
    const auto c101 = unpack_seed(seed_at(i1, j0, k1));
    const auto c011 = unpack_seed(seed_at(i0, j1, k1));
    const auto c111 = unpack_seed(seed_at(i1, j1, k1));

    AtlasSeed out;
    for (std::size_t idx = 0; idx < kRecordWidth; ++idx) {
        const double c00 = c000[idx] * (1.0 - tx) + c100[idx] * tx;
        const double c10 = c010[idx] * (1.0 - tx) + c110[idx] * tx;
        const double c01 = c001[idx] * (1.0 - tx) + c101[idx] * tx;
        const double c11 = c011[idx] * (1.0 - tx) + c111[idx] * tx;
        const double c0 = c00 * (1.0 - ty) + c10 * ty;
        const double c1 = c01 * (1.0 - ty) + c11 * ty;
        const double value = c0 * (1.0 - tz) + c1 * tz;

        if (idx < out.params.size()) {
            out.params[idx] = value;
        } else {
            out.transfer_time_days = value;
        }
    }
    return out;
}

CanonicalMissionConfig VariableIspIntegrator::canonical_config(double rho, double kappa) {
    (void)rho;
    constexpr double kGainFixed = -3.725e-6 - 4.91294688e-06;
    constexpr double delta_inverse_mass = (1.0 / kCanonicalDryMassKg) - (1.0 / kCanonicalM0Kg);
    const double kappa_scale_factor = std::pow(kCanonicalR0SI, 2.5) / std::pow(kMuSunSI, 1.5);
    const double j_capacity = kappa / kappa_scale_factor;
    const double power = j_capacity / (2.0 * delta_inverse_mass);

    CanonicalMissionConfig config;
    config.mu_m3_s2 = kMuSunSI;
    config.power_w = power;
    config.m_dry_kg = kCanonicalDryMassKg;
    config.m0_kg = kCanonicalM0Kg;
    config.r0_m = kCanonicalR0SI;
    config.vr0_mps = 0.0;
    config.vtheta0_mps = std::sqrt(config.mu_m3_s2 / config.r0_m);
    config.k_gain = kGainFixed;
    return config;
}

double VariableIspIntegrator::normalize_angle(double angle_rad) {
    return std::atan2(std::sin(angle_rad), std::cos(angle_rad));
}

namespace {

// Scales of the shooting unknowns (lambda_r, lambda_vr, lambda_vtheta, C_theta),
// taken from the Python generator's SOLUTION0 so all unknowns are O(1).
constexpr std::array<double, 4> kCostateScale {-9.04177133e-05, -2.23208767e+01, -2.82272150e+03, -1.56907920e+08};
constexpr std::array<std::size_t, 4> kCostateIndex {0, 1, 2, 4};  // params[3] is the gauge, fixed at 0
constexpr double kYearDays = 365.0;

using ShootVector = std::array<double, 5>;     // scaled unknowns
using ShootResidual = std::array<double, 4>;

ShootVector to_unknowns(const AtlasSeed& seed) {
    ShootVector z {};
    for (std::size_t i = 0; i < 4; ++i) {
        z[i] = seed.params[kCostateIndex[i]] / kCostateScale[i];
    }
    z[4] = seed.transfer_time_days / kYearDays;
    return z;
}

AtlasSeed from_unknowns(const ShootVector& z, const AtlasSeed& base) {
    AtlasSeed seed = base;
    for (std::size_t i = 0; i < 4; ++i) {
        seed.params[kCostateIndex[i]] = z[i] * kCostateScale[i];
    }
    seed.transfer_time_days = z[4] * kYearDays;
    return seed;
}

double norm(const ShootResidual& f) {
    double sum = 0.0;
    for (const double v : f) sum += v * v;
    return std::sqrt(sum);
}

// Solve the 4x4 system a * x = b by Gaussian elimination with partial pivoting.
bool solve4(std::array<std::array<double, 4>, 4> a, ShootResidual b, ShootResidual& x) {
    for (std::size_t col = 0; col < 4; ++col) {
        std::size_t pivot = col;
        for (std::size_t row = col + 1; row < 4; ++row) {
            if (std::abs(a[row][col]) > std::abs(a[pivot][col])) pivot = row;
        }
        if (std::abs(a[pivot][col]) < 1e-300) return false;
        std::swap(a[col], a[pivot]);
        std::swap(b[col], b[pivot]);
        for (std::size_t row = col + 1; row < 4; ++row) {
            const double factor = a[row][col] / a[col][col];
            for (std::size_t k = col; k < 4; ++k) a[row][k] -= factor * a[col][k];
            b[row] -= factor * b[col];
        }
    }
    for (std::size_t row = 4; row-- > 0;) {
        double sum = b[row];
        for (std::size_t k = row + 1; k < 4; ++k) sum -= a[row][k] * x[k];
        x[row] = sum / a[row][row];
    }
    return true;
}

}  // namespace

ShootingResult VariableIspIntegrator::refine_seed(
    const AtlasSeed& seed,
    const CanonicalMissionConfig& config,
    double r_target_m,
    double theta_target_rad,
    const ShootingSettings& settings) const {
    return refine_seed(
        seed, config, [&](double) { return std::pair {r_target_m, theta_target_rad}; }, settings);
}

ShootingResult VariableIspIntegrator::refine_seed(
    const AtlasSeed& seed,
    const CanonicalMissionConfig& config,
    const MovingTarget& target,
    const ShootingSettings& settings) const {
    const profiling::Scope profile_scope(profiling::Phase::VariableIspRefine);

    // Only endpoints matter here, so let RK45 take long steps; endpoints agree
    // with the 0.5-day rendering step to ~3e-5 relative.
    IntegratorSettings integration;
    integration.max_step_s = 10.0 * kDayS;
    integration.max_steps = 20000;
    const double v_scale = std::sqrt(config.mu_m3_s2 / target(seed.transfer_time_days).first);

    // Endpoint residual scaled so each tolerance maps to ~1e-3; nullopt if the
    // integration fails (e.g. the trajectory dives into the Sun).
    const auto residual = [&](const ShootVector& z) -> std::optional<ShootResidual> {
        if (z[4] <= 0.0) return std::nullopt;
        try {
            const auto summary = integrate_fixed_time(from_unknowns(z, seed), config, 2, integration);
            const auto& end = summary.samples.back();
            if (!std::isfinite(end.r_m) || end.r_m <= 0.0) return std::nullopt;
            const auto [r_target_m, theta_target_rad] = target(z[4] * kYearDays);
            return ShootResidual {
                (end.r_m / r_target_m - 1.0) * (1e-3 / settings.r_tolerance_rel),
                (end.theta_rad - theta_target_rad) * (1e-3 / settings.theta_tolerance_rad),
                end.vr_mps / v_scale * (1e-3 / settings.velocity_tolerance_rel),
                (end.vtheta_mps - std::sqrt(config.mu_m3_s2 / end.r_m)) / v_scale * (1e-3 / settings.velocity_tolerance_rel),
            };
        } catch (const std::runtime_error&) {
            return std::nullopt;
        }
    };
    const auto converged = [](const ShootResidual& f) {
        return std::all_of(f.begin(), f.end(), [](double v) { return std::abs(v) <= 1e-3; });
    };

    ShootingResult result;
    ShootVector z = to_unknowns(seed);
    auto f = residual(z);
    if (!f) {
        return result;
    }
    for (; result.iterations < settings.max_iterations && !converged(*f); ++result.iterations) {
        // Forward-difference Jacobian J (4 residuals x 5 unknowns).
        std::array<ShootVector, 4> jac {};
        for (std::size_t c = 0; c < 5; ++c) {
            ShootVector zp = z;
            const double h = settings.finite_difference_step * std::max(1.0, std::abs(z[c]));
            zp[c] += h;
            const auto fp = residual(zp);
            if (!fp) {
                return result;
            }
            for (std::size_t r = 0; r < 4; ++r) jac[r][c] = ((*fp)[r] - (*f)[r]) / h;
        }
        // Column scaling (like scipy's x_scale="jac"): seeds far from SOLUTION0 have
        // costates 10-100x the nominal scale, which leaves J badly conditioned.
        ShootVector column_norm {};
        for (std::size_t c = 0; c < 5; ++c) {
            for (std::size_t r = 0; r < 4; ++r) column_norm[c] += jac[r][c] * jac[r][c];
            column_norm[c] = std::sqrt(column_norm[c]);
            if (column_norm[c] <= 0.0) column_norm[c] = 1.0;
            for (std::size_t r = 0; r < 4; ++r) jac[r][c] /= column_norm[c];
        }
        // Minimum-norm Gauss-Newton step in the scaled unknowns,
        // dz = J^T (J J^T)^-1 (-f): the smallest change that fixes the endpoint
        // to first order.
        std::array<std::array<double, 4>, 4> jjt {};
        for (std::size_t a = 0; a < 4; ++a) {
            for (std::size_t b = 0; b < 4; ++b) {
                for (std::size_t c = 0; c < 5; ++c) jjt[a][b] += jac[a][c] * jac[b][c];
            }
        }
        ShootResidual rhs {};
        for (std::size_t r = 0; r < 4; ++r) rhs[r] = -(*f)[r];
        ShootResidual y {};
        if (!solve4(jjt, rhs, y)) {
            return result;
        }
        ShootVector dz {};
        for (std::size_t c = 0; c < 5; ++c) {
            for (std::size_t r = 0; r < 4; ++r) dz[c] += jac[r][c] * y[r];
            dz[c] /= column_norm[c];
        }
        // Backtracking: accept the first step length that reduces the residual.
        bool improved = false;
        for (double step = 1.0; step >= 1.0 / 64.0; step *= 0.5) {
            ShootVector trial = z;
            for (std::size_t c = 0; c < 5; ++c) trial[c] += step * dz[c];
            const auto ft = residual(trial);
            if (ft && norm(*ft) < norm(*f)) {
                z = trial;
                f = ft;
                improved = true;
                break;
            }
        }
        if (!improved) {
            break;
        }
    }

    result.seed = from_unknowns(z, seed);
    result.residual_norm = norm(*f);
    result.converged = converged(*f);
    return result;
}

IntegrationSummary VariableIspIntegrator::integrate_fixed_time(
    const AtlasSeed& seed,
    const CanonicalMissionConfig& base_config,
    std::size_t sample_count,
    const IntegratorSettings& settings) const {
    const profiling::Scope profile_scope(profiling::Phase::VariableIspIntegrate);
    if (sample_count < 2) {
        throw std::runtime_error("VariableISP integration requires at least 2 samples");
    }
    if (seed.transfer_time_days <= 0.0) {
        throw std::runtime_error("VariableISP transfer time must be positive");
    }

    CanonicalMissionConfig config = base_config;
    constexpr double safety = 0.9;
    constexpr double min_factor = 0.2;
    constexpr double max_factor = 10.0;
    constexpr double error_exponent = -1.0 / 5.0;

    const double c_theta = seed.params[4];
    const double transfer_time_s = seed.transfer_time_days * kDayS;
    const double dt_output = transfer_time_s / static_cast<double>(sample_count - 1);

    StateWide y {
        static_cast<double>(config.r0_m),
        0.0,
        static_cast<double>(config.vr0_mps),
        static_cast<double>(config.vtheta0_mps),
        static_cast<double>(config.m0_kg),
        static_cast<double>(seed.params[0]),
        static_cast<double>(seed.params[1]),
        static_cast<double>(seed.params[2]),
    };

    IntegrationSummary summary;
    summary.samples.reserve(sample_count);
    summary.samples.push_back({0.0, static_cast<double>(y[0]), static_cast<double>(y[1]), static_cast<double>(y[2]), static_cast<double>(y[3]), static_cast<double>(y[4])});

    double time_s = 0.0;
    std::size_t next_sample_index = 1;
    StateWide f {};
    ode_system(y, config, c_theta, f);
    double h_abs = select_initial_step(
        y,
        f,
        transfer_time_s,
        static_cast<double>(settings.max_step_s),
        settings,
        config,
        c_theta);
    std::array<StateWide, 7> k {};
    StateWide y_new {};
    StateWide f_new {};

    while (summary.samples.size() < sample_count) {
        const double min_step = 10.0 * std::abs(std::nextafter(time_s, std::numeric_limits<double>::infinity()) - time_s);
        h_abs = std::clamp(h_abs, min_step, static_cast<double>(settings.max_step_s));

        bool step_accepted = false;
        bool step_rejected = false;
        while (!step_accepted) {
            if (h_abs < min_step) {
                throw std::runtime_error("VariableISP RK45 step size underflow");
            }
            if (summary.accepted_steps + summary.rejected_steps >= settings.max_steps) {
                throw std::runtime_error("VariableISP RK45 step budget exhausted");
            }

            double dt = h_abs;
            double t_new = time_s + dt;
            if (t_new > transfer_time_s) {
                t_new = transfer_time_s;
            }
            dt = t_new - time_s;
            h_abs = std::abs(dt);

            const StateWide y_old = y;
            const double t_old = time_s;
            rk45_step(y, f, dt, config, c_theta, k, y_new, f_new);
            const double error_norm = estimate_error_norm(k, dt, y_old, y_new, settings);

            if (error_norm < 1.0) {
                double factor = (error_norm == 0.0)
                    ? max_factor
                    : std::min(max_factor, safety * std::pow(error_norm, error_exponent));
                if (step_rejected) {
                    factor = std::min(1.0, factor);
                }
                h_abs *= factor;
                step_accepted = true;
                y = y_new;
                f = f_new;
                time_s = t_new;
                summary.accepted_steps += 1;

                while (next_sample_index < sample_count) {
                    // The last sample is exactly the end: dt_output x (n - 1) can round to a
                    // hair above it, and the loop then spun on zero-length steps until the
                    // step budget threw (it did for some sample counts, e.g. 600).
                    const double sample_time = next_sample_index + 1 == sample_count
                        ? transfer_time_s
                        : std::min(transfer_time_s, dt_output * static_cast<double>(next_sample_index));
                    if (sample_time > time_s + 1e-12) {
                        break;
                    }
                    const StateWide y_sample = (sample_time == time_s)
                        ? y
                        : interpolate_dense_output(k, y_old, t_old, time_s, sample_time);
                    summary.samples.push_back({
                        static_cast<double>(sample_time),
                        static_cast<double>(y_sample[0]),
                        static_cast<double>(y_sample[1]),
                        static_cast<double>(y_sample[2]),
                        static_cast<double>(y_sample[3]),
                        static_cast<double>(y_sample[4]),
                    });
                    next_sample_index += 1;
                }
            } else {
                h_abs *= std::max(min_factor, safety * std::pow(error_norm, error_exponent));
                step_rejected = true;
                summary.rejected_steps += 1;
            }
        }
    }

    summary.samples.back().time_s = static_cast<double>(transfer_time_s);
    return summary;
}

}  // namespace spacetrains::variable_isp
