#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <string>
#include <vector>

namespace spacetrains::variable_isp {

struct SimilarityRequest {
    double rho {1.0};
    double kappa {1.0};
    double theta_rad {0.0};
};

struct CanonicalMissionConfig {
    double mu_m3_s2 {0.0};
    double power_w {0.0};
    double m_dry_kg {0.0};
    double m0_kg {0.0};
    double r0_m {0.0};
    double vr0_mps {0.0};
    double vtheta0_mps {0.0};
    double k_gain {0.0};
};

struct AtlasSeed {
    std::array<double, 5> params {};
    double transfer_time_days {0.0};
};

struct TrajectorySample {
    double time_s {0.0};
    double r_m {0.0};
    double theta_rad {0.0};
    double vr_mps {0.0};
    double vtheta_mps {0.0};
    double mass_kg {0.0};
};

struct IntegratorSettings {
    double relative_tolerance {1e-8};
    double absolute_tolerance {1e-9};
    double max_step_s {43200.0};
    double min_step_s {1e-6};
    double initial_step_s {10.0};
    // Hard cap on accepted + rejected RK45 steps. A bad seed can drive the
    // trajectory into the Sun, where the adaptive step shrinks without bound.
    std::size_t max_steps {200000};
};

struct IntegrationSummary {
    std::vector<TrajectorySample> samples;
    std::size_t accepted_steps {0};
    std::size_t rejected_steps {0};
};

struct Index3 {
    std::size_t i {0};
    std::size_t j {0};
    std::size_t k {0};
};

class VariableIspAtlas {
public:
    static constexpr std::uint64_t kRecordWidth = 6;

    void load_binary(const std::string& path);

    [[nodiscard]] const std::vector<double>& rho_grid() const { return rho_grid_; }
    [[nodiscard]] const std::vector<double>& kappa_grid() const { return kappa_grid_; }
    [[nodiscard]] const std::vector<double>& theta_grid() const { return theta_grid_; }
    [[nodiscard]] std::size_t solved_count() const;

    [[nodiscard]] bool is_solved(std::size_t i, std::size_t j, std::size_t k) const;
    [[nodiscard]] AtlasSeed seed_at(std::size_t i, std::size_t j, std::size_t k) const;
    [[nodiscard]] Index3 nearest_solved_index(
        double rho,
        double kappa,
        double theta_rad,
        std::size_t search_radius = 1) const;
    [[nodiscard]] AtlasSeed query(double rho, double kappa, double theta_rad, std::size_t search_radius = 1) const;

private:
    [[nodiscard]] std::size_t cell_index(std::size_t i, std::size_t j, std::size_t k) const;
    [[nodiscard]] std::size_t lower_cell_index(const std::vector<double>& axis, double value) const;

    std::vector<double> rho_grid_;
    std::vector<double> kappa_grid_;
    std::vector<double> theta_grid_;
    std::vector<std::uint8_t> solved_mask_;
    std::vector<double> records_;
};

// Endpoint correction of an atlas seed (indirect shooting). The thrust law has a
// fixed gain, so the path depends only on the costates and T, never on kappa:
// kappa only decides whether the fuel suffices, which the caller checks.
struct ShootingSettings {
    std::size_t max_iterations {12};
    double r_tolerance_rel {1e-4};       // |r_end / r_target - 1|
    double theta_tolerance_rad {1e-4};
    double velocity_tolerance_rel {1e-3};  // radial / circular-mismatch speed over v_circ(target)
    double finite_difference_step {1e-6};  // in scaled unknowns
};

struct ShootingResult {
    bool converged {false};
    AtlasSeed seed;                 // corrected costates and transfer time
    std::size_t iterations {0};
    double residual_norm {0.0};     // scaled; < ~1e-3 when converged
};

class VariableIspIntegrator {
public:
    static constexpr double kAstronomicalUnitM = 1.495978707e11;
    static constexpr double kDayS = 86400.0;
    static constexpr double kMuSunSI = 1.32712440018e20;
    static constexpr double kCanonicalR0SI = kAstronomicalUnitM;
    static constexpr double kCanonicalM0Kg = 3000.0;
    static constexpr double kCanonicalDryMassKg = 1000.0;

    [[nodiscard]] static CanonicalMissionConfig canonical_config(double rho, double kappa);
    [[nodiscard]] static double normalize_angle(double angle_rad);

    // Newton-correct `seed` so the canonical trajectory ends at r_target_m on a
    // circular orbit (v_r = 0, v_theta = v_circ) at swept angle theta_target_rad.
    // Pass theta_target on the branch the seed already flies (unwrapped).
    [[nodiscard]] ShootingResult refine_seed(
        const AtlasSeed& seed,
        const CanonicalMissionConfig& config,
        double r_target_m,
        double theta_target_rad,
        const ShootingSettings& settings = {}) const;

    // Same, for a target that moves with the transfer time (e.g. the destination
    // station's position at departure + T). `target(T_days)` returns the canonical
    // {r_m, theta_rad}; theta must be on the branch the seed flies.
    using MovingTarget = std::function<std::pair<double, double>(double transfer_time_days)>;
    [[nodiscard]] ShootingResult refine_seed(
        const AtlasSeed& seed,
        const CanonicalMissionConfig& config,
        const MovingTarget& target,
        const ShootingSettings& settings = {}) const;

    [[nodiscard]] IntegrationSummary integrate_fixed_time(
        const AtlasSeed& seed,
        const CanonicalMissionConfig& config,
        std::size_t sample_count,
        const IntegratorSettings& settings = {}) const;
};

}  // namespace spacetrains::variable_isp
