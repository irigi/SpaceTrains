#pragma once

// Minimum-propellant transfers for a constant-power drive whose exhaust speed is confined to a
// band [v_min, v_max] (study code: docs/studies/banded_transfer). Separate from the ideal solver
// (GeneralTransfer.hpp), whose public types and seeds it reuses; nothing in the game links it.
//
// Model (normalised units, μ = 1, caller's length unit R; speeds in sqrt(μ/R)). The drive runs
// at jet power P or not at all: thrust acceleration a and exhaust speed v_e are tied by
// a = 2P/(m v_e). With s = 2P/m the band allows a ∈ [a_min, a_max] = [s/v_max, s/v_min] at full
// power; below a_min the drive can only run at v_max and part power (or pulse), so the mass
// flow is
//     s' = a · max(a, a_min)        (= a² inside the band: the ideal engine)
// and minimum propellant is minimum s(T) (m_f = 2P / s(T)).
//
// Pontryagin. With the primer p (= −λ_v/2, as in the ideal solver), w = p', w' = G(r) p, and λ_s
// the costate of s (homogeneous costates: λ_s(0) = 1, so with no band a = p exactly), the
// control minimises −2|p| a + λ_s l(a) over a ∈ [0, a_max], l(a) = a·max(a, a_min):
//     q = |p| / λ_s
//     q ≥ a_max          floor:    a = a_max          λ_s' = (2|p| − 2λ_s a_max) / v_min
//     a_min ≤ q < a_max  interior: a = q              λ_s' = 0
//     a_min/2 < q < a_min ceiling: a = a_min (v_max)  λ_s' = (2|p| − 2λ_s a_min) / v_max
//     q ≤ a_min/2        coast:    a = 0              λ_s' = 0
// so under the ceiling the optimum coasts or burns at full power and v_max, never throttles.
// Arc changes are located as events; the Jacobian of the end state with respect to (p0, w0)
// comes from forward-mode dual numbers, with the jump of the right-hand side at a coast switch
// carried by the saltation term.

#include <array>
#include <cstddef>
#include <vector>

#include "variable_isp/GeneralTransfer.hpp"

namespace spacetrains::variable_isp::banded {

using general::Primer;
using general::State;
using general::Target;
using general::TargetPoint;
using general::Vec2;

// Exhaust-speed band in normalised speed units. 0 on a side means no limit there.
struct Band {
    double v_min {0.0};
    double v_max {0.0};
    [[nodiscard]] bool limited() const { return v_min > 0.0 || v_max > 0.0; }
    // The band tightened by λ ∈ [0, 1] for continuation: limits at v_max/λ and λ·v_min
    // (λ = 0: no band, λ = 1: this band).
    [[nodiscard]] Band scaled(double lambda) const;
};

enum class Arc : int { Coast = 0, Ceiling = 1, Interior = 2, Floor = 3 };

struct Settings {
    double relative_tolerance {1e-10};
    double absolute_tolerance {1e-11};
    double min_radius {2e-3};  // numerical guard
    std::size_t max_steps {20000};
    std::size_t max_switches {400};
    bool high_order {true};    // DOP853, else DOPRI5
    bool gravity {true};       // off: free space (closed-form tests)
    double max_step {0.0};     // > 0: cap on the step (dense samples for plots)
};

struct Switch {
    double t {0.0};
    Arc from {Arc::Interior};
    Arc to {Arc::Interior};
};

struct Sample {
    double t {0.0};
    State state;
    Vec2 thrust;          // acceleration vector
    double s {0.0};       // 2P/m
    double lambda_s {0.0};
    Arc arc {Arc::Interior};
};

struct EndPoint {
    State state;
    Primer primer;          // p, w at T
    double s {0.0};         // the optimised state (with smoothing: the smoothed cost)
    double s_flown {0.0};   // the drive's real s' = a·max(a, s/v_max) along the same thrust (fuel to report)
    double lambda_s {0.0};
    double sweep_rad {0.0};
    double min_radius {0.0};
    double hamiltonian {0.0};   // 2w·v − 2p·(g + a) + λ_s l(a), at T
    double time_on_arc[4] {0.0, 0.0, 0.0, 0.0};  // by Arc
    std::vector<Switch> switches;
    std::size_t steps {0};
    // ∂(r, v)(T) / ∂(p0, w0), rows r.x, r.y, v.x, v.y.
    std::array<std::array<double, 4>, 4> jacobian {};
    bool ok {false};
};

// Approach A: the ideal path (thrust = the ideal primer) flown within the band, throttled at
// v_max where it asks for less than a_min.
struct FollowResult {
    double s_end {0.0};
    double ideal_s_end {0.0};   // same path with no band (s0 + ∫a²)
    double floor_load {0.0};    // max a / a_max (> 1: needs an exhaust below the band)
    double throttled_time {0.0};
    State end;
    std::size_t steps {0};
    bool ok {false};
    [[nodiscard]] bool floor_ok() const { return floor_load <= 1.0 + 1e-9; }
};

class Propagator {
public:
    explicit Propagator(Settings settings = {}) : settings_(settings) {}

    // The optimal-control flow from (start, primer, s0, λ_s = 1) over [0, T]. With smoothing ε > 0
    // the ceiling's cost a·max(a, a_min) is replaced by a·smax_ε(a, a_min), smax_ε(a, m) =
    // (a + m + sqrt((a − m)² + ε²m²))/2, and the control is the exact minimiser of that
    // Hamiltonian: continuous in the state, so no events and no square-root sensitivity when an
    // arc is born (continuation runs ε → 0, then polishes with ε = 0 and exact switching).
    EndPoint propagate(const State& start, const Primer& primer, double s0, const Band& band, double T,
        bool with_jacobian, std::vector<Sample>* samples = nullptr, double smoothing = 0.0) const;
    // The ideal flow (a = p) with the banded mass flow (approach A).
    FollowResult follow_ideal(const State& start, const Primer& ideal, double s0, const Band& band, double T,
        std::vector<Sample>* samples = nullptr) const;

    [[nodiscard]] const Settings& settings() const { return settings_; }

private:
    Settings settings_;
};

struct Request {
    State start;
    Target target;          // rendezvous: match position and velocity at T
    double s0 {0.0};        // 2P/m0 (normalised: accel × speed)
    Band band;
    double min_radius {0.0};
};

struct Options {
    std::size_t max_iterations {14};
    double tolerance {1e-9};               // max abs residual (normalised r, v)
    double continuation_tolerance {1e-6};  // on intermediate continuation steps
    double first_step {0.25};              // λ step of the band continuation
    double growth {1.8};
    double shrink {0.35};
    double min_step {1.0 / 512.0};
    std::size_t max_continuation_steps {60};
    bool predictor {true};                 // secant predictor in λ
    bool finite_difference {false};        // Jacobian by forward differences (check / tuning)
    double fd_step {1e-7};
    bool trace {false};                    // continuation and Newton steps to stderr
    // Fallback when the exact continuation fails: the band continuation with the coast/ceiling
    // switch smoothed by ε = smoothing, ε reduced by smoothing_factor down to smoothing_min, then
    // the exact problem (0: no fallback).
    double smoothing {0.3};
    double smoothing_min {1e-2};
    double smoothing_factor {0.3};
    bool exact_polish {true};              // finish with ε = 0 and exact switching
    std::size_t max_smoothing_splits {3};
    double smoothed_tolerance {1e-7};      // residual accepted for a smoothed plan (≈ 15 km, 3 mm/s)
    std::size_t max_propagations {800};    // budget of one solve
};

struct Solution {
    bool converged {false};
    Primer primer;
    double T {0.0};
    EndPoint end;
    double residual {0.0};
    std::size_t iterations {0};
    std::size_t propagations {0};
    std::size_t continuation_steps {0};
    bool from_warm {false};
    bool exact {false};      // ε = 0 with exact switching; else the smallest smoothing (a flyable,
                             // near-optimal plan: its intermediate thrust is throttling at v_max)
    // The warm start or the continuation ended on an extremal that burns more than flying the
    // ideal path throttled (another branch): rejected.
    bool rejected_worse {false};
};

// m_f / m0 = s0 / s(T).
[[nodiscard]] inline double final_mass_fraction(double s0, double s_end) { return s0 / s_end; }

// Newton (damped) on (p0, w0) at the full band from `guess`.
Solution shoot(const Propagator& propagator, const Request& request, double T, const Primer& guess,
    const Options& options = {}, double tolerance = 0.0, double smoothing = 0.0);

// Minimum-propellant transfer at fixed T. With `warm` (a banded primer, e.g. from a neighbouring
// T or departure) Newton runs at the full band first; otherwise, or if that fails, the band is
// tightened from the ideal solution `ideal` (general::solve_transfer at this T) in steps.
Solution solve_fixed_time(const Propagator& propagator, const Request& request, double T, const Primer& ideal,
    const Options& options = {}, const Primer* warm = nullptr);

}  // namespace spacetrains::variable_isp::banded
