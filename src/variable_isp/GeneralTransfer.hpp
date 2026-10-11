#pragma once

// General-boundary transfers for a constant-power, variable-Isp ship (prototype).
//
// The atlas solver (VariableIsp.hpp) only flies circular orbit to circular orbit. This module
// solves the same physics between arbitrary states: from any (r0, v0) to any (rf, vf), to a
// target that moves with the arrival time (a station, or another ship), or to a position only
// (intercept, free arrival velocity). It is the primitive a chase needs.
//
// Model. With the power P fixed and the exhaust speed free, minimum propellant is minimum
// effort J = ∫|a|² dt (1/m_f − 1/m0 = J/2P), so the path never depends on the mass. The
// optimal thrust is the primer p = a, with w = p′ and w′ = G(r)p (G the gravity gradient):
// the indirect method needs only (p0, w0) as unknowns, whatever the boundary states.
//
// Units are normalised: μ = 1 and the caller's length unit R, so times are in sqrt(R³/μ)
// and accelerations in μ/R². Effort J is then the dimensionless κ of the atlas when R = 1 AU
// (κ = J_SI·R^2.5/μ^1.5).

#include <array>
#include <cstddef>
#include <functional>
#include <vector>

namespace spacetrains::variable_isp::general {

struct Vec2 {
    double x {0.0};
    double y {0.0};
};

inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(double s, Vec2 a) { return {s * a.x, s * a.y}; }
inline double dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
double norm(Vec2 a);

struct State {
    Vec2 r;
    Vec2 v;
};

// Primer at departure: p0 is the initial thrust acceleration, w0 its rate of change.
struct Primer {
    Vec2 p;
    Vec2 w;
};

// Target state at arrival time T after departure, with its time derivative (needed for the
// Jacobian when the target moves). A fixed target has zero derivatives.
struct TargetPoint {
    State state;
    Vec2 velocity;      // d r_target / dT
    Vec2 acceleration;  // d v_target / dT
};
using Target = std::function<TargetPoint(double T)>;
Target fixed_target(const State& state);
// A target that coasts on a Kepler orbit (μ = 1) from `state` at departure.
Target coasting_target(const State& state);

enum class EndCondition {
    Rendezvous,  // match position and velocity
    Intercept,   // match position, arrival velocity free (transversality p(T) = 0)
};

struct PropagationSettings {
    double relative_tolerance {1e-10};
    double absolute_tolerance {1e-11};
    double min_radius {2e-3};          // numerical singularity guard, not a physical limit
    std::size_t max_steps {20000};
    bool high_order {true};            // DOP853 (else DOPRI5): ~25% faster at 1e-10, equal at 1e-8
};

struct EndPoint {
    State state;
    Primer primer;
    double effort {0.0};       // ∫|p|² dt
    double sweep_rad {0.0};    // unwrapped heliocentric angle swept
    double min_radius {0.0};
    double hamiltonian {0.0};  // h = −w·v + p·g + |p|²/2 (conserved; dJ/dT = −2h)
    std::size_t steps {0};
    // ∂(r, v, p, w, E)(T)/∂(p0, w0): 9 rows x 4 columns (when requested).
    std::array<std::array<double, 4>, 9> stm {};
    bool ok {false};
};

struct Sample {
    double t {0.0};
    State state;
    Vec2 thrust;  // = p
    double effort {0.0};
};

// One multiple-shooting segment: full state (r, v, p, w) after dt and ∂(r, v, p, w, E)/∂(start).
struct SegmentEnd {
    std::array<double, 8> x {};
    double effort {0.0};
    double sweep_rad {0.0};
    double min_radius {0.0};
    std::array<std::array<double, 8>, 9> stm {};
    std::size_t steps {0};
    bool ok {false};
};

class Propagator {
public:
    explicit Propagator(PropagationSettings settings = {}) : settings_(settings) {}

    EndPoint propagate(const State& start, const Primer& primer, double T, bool with_stm) const;
    SegmentEnd propagate_segment(const std::array<double, 8>& x0, double dt) const;
    // Dense rollout for paths and independent checks.
    std::vector<Sample> sample(const State& start, const Primer& primer, double T, std::size_t count) const;

    [[nodiscard]] const PropagationSettings& settings() const { return settings_; }

private:
    PropagationSettings settings_;
};

struct ShootingOptions {
    std::size_t max_iterations {30};
    std::size_t max_propagations {60};
    // Give up when the residual has not fallen below this fraction of the start residual
    // after `stall_iterations` iterations (a seed in the wrong basin rarely recovers).
    std::size_t stall_iterations {8};
    double stall_fraction {1e-2};
    double tolerance {1e-9};  // max abs residual, normalised units
    // T becomes an unknown. With match_budget the effort must equal effort_budget (minimum
    // time for a budget, square system); without, the step is minimum-norm over (p0, w0, T).
    bool free_time {false};
    bool match_budget {true};
    double effort_budget {0.0};
    double min_time {1e-3};
};

struct Solution {
    bool converged {false};
    Primer primer;
    double T {0.0};
    EndPoint end;
    std::size_t iterations {0};
    std::size_t propagations {0};
    double residual {0.0};
    double initial_sweep {0.0};  // angle swept by the seed (first evaluation), for branch checks
};

// Newton (Levenberg-Marquardt) shooting on (p0, w0[, T]) with the analytic transition matrix.
Solution shoot(
    const Propagator& propagator,
    const State& start,
    const Target& target,
    EndCondition end_condition,
    const Primer& seed,
    double T,
    const ShootingOptions& options = {});

// Multiple shooting at fixed T over K equal segments. `nodes[k]` is the guessed (r, v, p, w) at
// t = k T / K (nodes[0]'s r, v are ignored: the start is fixed). Unknowns are (p0, w0) and the
// interior node states; residuals are the continuity defects and the end condition.
Solution multiple_shoot(
    const Propagator& propagator,
    const State& start,
    const Target& target,
    EndCondition end_condition,
    double T,
    const std::vector<std::array<double, 8>>& nodes,
    const ShootingOptions& options = {});

// Continuation in transfer time: solve at a short T_from, where the free-space solution is
// nearly exact, and follow the solution branch to T_to with a tangent predictor. With
// stop_effort > 0 it stops at the first T whose effort falls to stop_effort and returns the
// minimum-time solution for that budget there.
struct ContinuationOptions {
    double T_from {0.02};
    double initial_growth {1.5};
    double max_growth {2.5};
    double min_growth {1.01};
    double stop_effort {0.0};
    std::size_t max_steps {200};
};
Solution continue_in_time(
    const Propagator& propagator,
    const State& start,
    const Target& target,
    EndCondition end_condition,
    double T_to,
    const ContinuationOptions& options = {},
    const ShootingOptions& shooting = {});

// Continuation in the boundary condition at fixed T: start on the coasting arc (zero primer is
// exact when the target is the ballistic end state) and move the target to the real one,
// λ: 0 → 1, with a tangent predictor. A single full step is Newton from the zero primer,
// i.e. the linear-quadratic (Gramian) solution about the ballistic arc.
struct BoundaryHomotopyOptions {
    double first_step {1.0};
    double min_step {1.0 / 64.0};
    std::size_t corrector_iterations {8};
};
Solution continue_in_boundary(
    const Propagator& propagator,
    const State& start,
    const Target& target,
    EndCondition end_condition,
    double T,
    const BoundaryHomotopyOptions& options = {},
    const ShootingOptions& shooting = {});

// Lengthen a converged fixed-T solution until its effort fits `budget` (effort falls with T on
// the usual branch): safeguarded secant search over fixed-T, warm-started shootings. Returns
// the feasible side (effort ≤ budget, within `relative_tolerance`); not converged if the
// effort stops falling first (no time at this departure fits the budget).
Solution fit_effort_by_time(const Propagator& propagator, const State& start, const Target& target,
    EndCondition end_condition, const Solution& from, double budget, double relative_tolerance = 1e-3,
    const ShootingOptions& shooting = {});

// ---- Seeds ----------------------------------------------------------------------------------

// Gravity-free minimum-effort cubic (closed form), the free-space seed.
Primer free_space_seed(const State& start, const State& end, double T);
double free_space_effort(const State& start, const State& end, double T);

// Direct shape method: polar r(t), θ(t) as cubic Hermite + `terms` bubble functions each,
// effort minimised by Gauss-Newton. The arrival angle is θ_end (unwrapped, relative to the
// start's polar angle). Feasible by construction; the thrust is whatever the path needs.
struct ShapeResult {
    bool ok {false};
    double effort {0.0};
    double T {0.0};
    double theta_end {0.0};
    std::vector<double> coefficients;  // r terms, θ terms[, free arrival dr/ds, dθ/ds]
    std::size_t terms {0};
    bool free_end {false};
    std::size_t iterations {0};
    double min_radius {0.0};
};
struct ShapeOptions {
    std::size_t terms {6};
    std::size_t nodes {24};
    std::size_t max_iterations {30};
    bool free_end_velocity {false};  // intercept: arrival velocity becomes free
};
ShapeResult shape_solve(
    const State& start, const State& end, double T, double theta_end, const ShapeOptions& options = {},
    const std::vector<double>* warm = nullptr);
// The shape path's state and thrust at normalised time s ∈ [0, 1].
struct ShapePoint {
    State state;
    Vec2 thrust;
    Vec2 thrust_rate;
};
ShapePoint shape_eval(const State& start, const State& end, const ShapeResult& shape, double s, bool with_rate = true);

// Angle-parametrised shape: r(θ) and t(θ) as cubic Hermite + bubbles over θ ∈ [0, θ_end], so
// fast perihelion passages stay smooth. Needs the angular rate to keep one sign (no
// retrograde ends). Same result type; terms counts the bubbles per function.
ShapeResult angle_shape_solve(
    const State& start, const State& end, double T, double theta_end, const ShapeOptions& options = {});
struct TimedShapePoint {
    double t {0.0};
    State state;
    Vec2 thrust;
};
TimedShapePoint angle_shape_eval(const State& start, const State& end, const ShapeResult& shape, double sigma);

// Least-squares fit of (p0, w0) so the primer propagated along a fixed reference path matches
// a thrust history: the primer equations are linear along a given path.
Primer fit_primer(const std::vector<double>& times, const std::vector<Vec2>& positions,
    const std::vector<Vec2>& thrust, std::vector<Primer>* history = nullptr);

// Unwrapped arrival angle candidates: the arrival direction plus whole turns, nearest first
// to a rough angular-rate estimate.
std::vector<double> arrival_angle_candidates(const State& start, const State& end, double T, int count);

// ---- Solvers -----------------------------------------------------------------------------------

struct TransferRequest {
    State start;
    Target target;
    EndCondition end_condition {EndCondition::Rendezvous};
    double min_radius {0.0};  // reject solutions passing closer to the Sun (normalised)
};

struct SolveOptions {
    // Fixed-T portfolio. Each stage runs only if the previous ones found nothing valid, except
    // that the shape seed always runs after the zero-primer seed to catch a better branch.
    bool always_try_shape {true};
    bool fallbacks {true};    // multiple shooting, boundary homotopy, time continuation
    int branches {3};         // arrival-angle candidates for the shape seed
    ShapeOptions shape {};
    std::size_t fit_samples {80};
};

// Minimum-effort transfer at a fixed T; `warm` is tried first when given. `stage` (optional)
// receives the name of the seed that produced the answer.
Solution solve_transfer(const Propagator& propagator, const TransferRequest& request, double T,
    const SolveOptions& options = {}, const Primer* warm = nullptr, const char** stage = nullptr);

// Earliest arrival whose minimum effort fits `budget` (minimum time for a fuel load), searched
// over [T_lo, T_hi] on a geometric grid with warm starts, then refined on the budget equation.
// (Following the effort curve up from one solve at T_lo instead of a grid was tried: fewer
// requests solved, no faster.)
struct MinTimeOptions {
    std::size_t grid {16};
    SolveOptions solve {};
};
Solution solve_min_time(const Propagator& propagator, const TransferRequest& request, double budget, double T_lo,
    double T_hi, const MinTimeOptions& options = {});

}  // namespace spacetrains::variable_isp::general
