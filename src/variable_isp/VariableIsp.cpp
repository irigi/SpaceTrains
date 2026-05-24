#include "variable_isp/VariableIsp.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace spacetrains::variable_isp {

namespace {

constexpr std::uint64_t kAtlasMagic = 0x3154415053495654ULL;  // "TVISPAT1"
constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 2.0 * kPi;

using State = std::array<double, 8>;
using StateWide = std::array<long double, 8>;

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

void ode_system(const StateWide& y, const CanonicalMissionConfig& config, long double c_theta, StateWide& dydt) {
    const long double r = y[0];
    const long double v_r = y[2];
    const long double v_theta = y[3];
    const long double m = y[4];
    const long double lambda_r = y[5];
    const long double lambda_vr = y[6];
    const long double lambda_vtheta = y[7];

    const long double a_r = static_cast<long double>(config.k_gain) * lambda_vr;
    const long double a_theta = static_cast<long double>(config.k_gain) * lambda_vtheta;
    const long double accel_sq = (a_r * a_r) + (a_theta * a_theta);

    dydt[0] = v_r;
    dydt[1] = v_theta / r;
    dydt[2] = (v_theta * v_theta) / r - (static_cast<long double>(config.mu_m3_s2) / (r * r)) + a_r;
    dydt[3] = -(v_r * v_theta) / r + a_theta;
    dydt[4] = -(m * m / (2.0L * static_cast<long double>(config.power_w))) * accel_sq;
    dydt[5] = (c_theta * v_theta) / (r * r)
        + (lambda_vr * v_theta * v_theta) / (r * r)
        - (2.0L * lambda_vr * static_cast<long double>(config.mu_m3_s2)) / (r * r * r)
        - (lambda_vtheta * v_r * v_theta) / (r * r);
    dydt[6] = -lambda_r + (lambda_vtheta * v_theta) / r;
    dydt[7] = -c_theta / r - (2.0 * lambda_vr * v_theta) / r + (lambda_vtheta * v_r) / r;
}

long double rms_norm(const StateWide& x) {
    long double sum = 0.0L;
    for (const long double value : x) {
        sum += value * value;
    }
    return std::sqrt(sum / static_cast<long double>(x.size()));
}

long double select_initial_step(
    const StateWide& y0,
    const StateWide& f0,
    long double interval_length,
    long double max_step,
    const IntegratorSettings& settings,
    const CanonicalMissionConfig& config,
    long double c_theta) {
    StateWide scale {};
    for (std::size_t idx = 0; idx < y0.size(); ++idx) {
        scale[idx] = static_cast<long double>(settings.absolute_tolerance)
            + std::abs(y0[idx]) * static_cast<long double>(settings.relative_tolerance);
    }

    StateWide y_scaled {};
    StateWide f_scaled {};
    for (std::size_t idx = 0; idx < y0.size(); ++idx) {
        y_scaled[idx] = y0[idx] / scale[idx];
        f_scaled[idx] = f0[idx] / scale[idx];
    }

    const long double d0 = rms_norm(y_scaled);
    const long double d1 = rms_norm(f_scaled);
    long double h0 = (d0 < 1e-5L || d1 < 1e-5L) ? 1e-6L : (0.01L * d0 / d1);
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
    const long double d2 = rms_norm(f_delta) / h0;

    long double h1 = 0.0L;
    if (d1 <= 1e-15L && d2 <= 1e-15L) {
        h1 = std::max(1e-6L, h0 * 1e-3L);
    } else {
        h1 = std::pow(0.01L / std::max(d1, d2), 1.0L / 5.0L);
    }

    return std::min({100.0L * h0, h1, interval_length, max_step});
}

void rk45_step(
    const StateWide& y,
    const StateWide& f,
    long double dt,
    const CanonicalMissionConfig& config,
    long double c_theta,
    std::array<StateWide, 7>& k,
    StateWide& y_new,
    StateWide& f_new) {
    constexpr std::array<std::array<long double, 5>, 6> a {{
        {{0.0L, 0.0L, 0.0L, 0.0L, 0.0L}},
        {{1.0L / 5.0L, 0.0L, 0.0L, 0.0L, 0.0L}},
        {{3.0L / 40.0L, 9.0L / 40.0L, 0.0L, 0.0L, 0.0L}},
        {{44.0L / 45.0L, -56.0L / 15.0L, 32.0L / 9.0L, 0.0L, 0.0L}},
        {{19372.0L / 6561.0L, -25360.0L / 2187.0L, 64448.0L / 6561.0L, -212.0L / 729.0L, 0.0L}},
        {{9017.0L / 3168.0L, -355.0L / 33.0L, 46732.0L / 5247.0L, 49.0L / 176.0L, -5103.0L / 18656.0L}},
    }};
    constexpr std::array<long double, 6> b {35.0L / 384.0L, 0.0L, 500.0L / 1113.0L, 125.0L / 192.0L, -2187.0L / 6784.0L, 11.0L / 84.0L};

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

long double estimate_error_norm(
    const std::array<StateWide, 7>& k,
    long double dt,
    const StateWide& y,
    const StateWide& y_new,
    const IntegratorSettings& settings) {
    constexpr std::array<long double, 7> e {
        -71.0L / 57600.0L, 0.0L, 71.0L / 16695.0L, -71.0L / 1920.0L,
        17253.0L / 339200.0L, -22.0L / 525.0L, 1.0L / 40.0L,
    };

    StateWide scaled_error {};
    for (std::size_t idx = 0; idx < y.size(); ++idx) {
        long double err = 0.0L;
        for (std::size_t stage = 0; stage < e.size(); ++stage) {
            err += e[stage] * k[stage][idx];
        }
        err *= dt;
        const long double scale = static_cast<long double>(settings.absolute_tolerance)
            + std::max(std::abs(y[idx]), std::abs(y_new[idx])) * static_cast<long double>(settings.relative_tolerance);
        scaled_error[idx] = err / scale;
    }
    return rms_norm(scaled_error);
}

StateWide interpolate_dense_output(
    const std::array<StateWide, 7>& k,
    const StateWide& y_old,
    long double t_old,
    long double t_new,
    long double t_query) {
    constexpr std::array<std::array<long double, 4>, 7> p {{
        {{1.0L, -8048581381.0L / 2820520608.0L, 8663915743.0L / 2820520608.0L, -12715105075.0L / 11282082432.0L}},
        {{0.0L, 0.0L, 0.0L, 0.0L}},
        {{0.0L, 131558114200.0L / 32700410799.0L, -68118460800.0L / 10900136933.0L, 87487479700.0L / 32700410799.0L}},
        {{0.0L, -1754552775.0L / 470086768.0L, 14199869525.0L / 1410260304.0L, -10690763975.0L / 1880347072.0L}},
        {{0.0L, 127303824393.0L / 49829197408.0L, -318862633887.0L / 49829197408.0L, 701980252875.0L / 199316789632.0L}},
        {{0.0L, -282668133.0L / 205662961.0L, 2019193451.0L / 616988883.0L, -1453857185.0L / 822651844.0L}},
        {{0.0L, 40617522.0L / 29380423.0L, -110615467.0L / 29380423.0L, 69997945.0L / 29380423.0L}},
    }};

    const long double h = t_new - t_old;
    const long double x = (t_query - t_old) / h;
    const std::array<long double, 4> powers {x, x * x, x * x * x, x * x * x * x};

    StateWide out = y_old;
    for (std::size_t idx = 0; idx < out.size(); ++idx) {
        long double q = 0.0L;
        for (std::size_t stage = 0; stage < p.size(); ++stage) {
            long double stage_poly = 0.0L;
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

IntegrationSummary VariableIspIntegrator::integrate_fixed_time(
    const AtlasSeed& seed,
    const CanonicalMissionConfig& base_config,
    std::size_t sample_count,
    const IntegratorSettings& settings) const {
    if (sample_count < 2) {
        throw std::runtime_error("VariableISP integration requires at least 2 samples");
    }
    if (seed.transfer_time_days <= 0.0) {
        throw std::runtime_error("VariableISP transfer time must be positive");
    }

    CanonicalMissionConfig config = base_config;
    constexpr long double safety = 0.9L;
    constexpr long double min_factor = 0.2L;
    constexpr long double max_factor = 10.0L;
    constexpr long double error_exponent = -1.0L / 5.0L;

    const long double c_theta = seed.params[4];
    const long double transfer_time_s = seed.transfer_time_days * kDayS;
    const long double dt_output = transfer_time_s / static_cast<long double>(sample_count - 1);

    StateWide y {
        static_cast<long double>(config.r0_m),
        0.0L,
        static_cast<long double>(config.vr0_mps),
        static_cast<long double>(config.vtheta0_mps),
        static_cast<long double>(config.m0_kg),
        static_cast<long double>(seed.params[0]),
        static_cast<long double>(seed.params[1]),
        static_cast<long double>(seed.params[2]),
    };

    IntegrationSummary summary;
    summary.samples.reserve(sample_count);
    summary.samples.push_back({0.0, static_cast<double>(y[0]), static_cast<double>(y[1]), static_cast<double>(y[2]), static_cast<double>(y[3]), static_cast<double>(y[4])});

    long double time_s = 0.0L;
    std::size_t next_sample_index = 1;
    StateWide f {};
    ode_system(y, config, c_theta, f);
    long double h_abs = select_initial_step(
        y,
        f,
        transfer_time_s,
        static_cast<long double>(settings.max_step_s),
        settings,
        config,
        c_theta);
    std::array<StateWide, 7> k {};
    StateWide y_new {};
    StateWide f_new {};

    while (summary.samples.size() < sample_count) {
        const long double min_step = 10.0L * std::abs(std::nextafter(time_s, std::numeric_limits<long double>::infinity()) - time_s);
        h_abs = std::clamp(h_abs, min_step, static_cast<long double>(settings.max_step_s));

        bool step_accepted = false;
        bool step_rejected = false;
        while (!step_accepted) {
            if (h_abs < min_step) {
                throw std::runtime_error("VariableISP RK45 step size underflow");
            }

            long double dt = h_abs;
            long double t_new = time_s + dt;
            if (t_new > transfer_time_s) {
                t_new = transfer_time_s;
            }
            dt = t_new - time_s;
            h_abs = std::abs(dt);

            const StateWide y_old = y;
            const long double t_old = time_s;
            rk45_step(y, f, dt, config, c_theta, k, y_new, f_new);
            const long double error_norm = estimate_error_norm(k, dt, y_old, y_new, settings);

            if (error_norm < 1.0L) {
                long double factor = (error_norm == 0.0L)
                    ? max_factor
                    : std::min(max_factor, safety * std::pow(error_norm, error_exponent));
                if (step_rejected) {
                    factor = std::min(1.0L, factor);
                }
                h_abs *= factor;
                step_accepted = true;
                y = y_new;
                f = f_new;
                time_s = t_new;
                summary.accepted_steps += 1;

                while (next_sample_index < sample_count) {
                    const long double sample_time = dt_output * static_cast<long double>(next_sample_index);
                    if (sample_time > time_s + 1e-12L) {
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
