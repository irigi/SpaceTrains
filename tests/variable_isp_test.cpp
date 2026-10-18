#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "trajectory/ChaseSolver.hpp"
#include "variable_isp/GeneralTransfer.hpp"
#include "variable_isp/VariableIsp.hpp"

namespace {

using spacetrains::variable_isp::AtlasSeed;
using spacetrains::variable_isp::CanonicalMissionConfig;
using spacetrains::variable_isp::IntegrationSummary;
using spacetrains::variable_isp::TrajectorySample;
using spacetrains::variable_isp::VariableIspAtlas;
using spacetrains::variable_isp::VariableIspIntegrator;

struct JsonValue {
    enum class Type { Null, Number, String, Object, Array, Bool };

    Type type {Type::Null};
    double number_value {0.0};
    bool bool_value {false};
    std::string string_value;
    std::vector<std::pair<std::string, JsonValue>> object_value;
    std::vector<JsonValue> array_value;

    [[nodiscard]] const JsonValue& at(const std::string& key) const {
        for (const auto& [name, value] : object_value) {
            if (name == key) {
                return value;
            }
        }
        throw std::runtime_error("Missing JSON key: " + key);
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string input) : input_(std::move(input)) {}

    JsonValue parse() {
        skip_ws();
        JsonValue value = parse_value();
        skip_ws();
        if (pos_ != input_.size()) {
            throw std::runtime_error("Unexpected trailing JSON content");
        }
        return value;
    }

private:
    JsonValue parse_value() {
        skip_ws();
        if (match("null")) {
            return JsonValue {};
        }
        if (match("true")) {
            JsonValue value;
            value.type = JsonValue::Type::Bool;
            value.bool_value = true;
            return value;
        }
        if (match("false")) {
            JsonValue value;
            value.type = JsonValue::Type::Bool;
            value.bool_value = false;
            return value;
        }
        if (peek() == '"') {
            JsonValue value;
            value.type = JsonValue::Type::String;
            value.string_value = parse_string();
            return value;
        }
        if (peek() == '{') {
            return parse_object();
        }
        if (peek() == '[') {
            return parse_array();
        }
        return parse_number();
    }

    JsonValue parse_object() {
        expect('{');
        JsonValue value;
        value.type = JsonValue::Type::Object;
        skip_ws();
        if (peek() == '}') {
            expect('}');
            return value;
        }
        while (true) {
            const std::string key = parse_string();
            skip_ws();
            expect(':');
            value.object_value.push_back({key, parse_value()});
            skip_ws();
            if (peek() == '}') {
                expect('}');
                break;
            }
            expect(',');
        }
        return value;
    }

    JsonValue parse_array() {
        expect('[');
        JsonValue value;
        value.type = JsonValue::Type::Array;
        skip_ws();
        if (peek() == ']') {
            expect(']');
            return value;
        }
        while (true) {
            value.array_value.push_back(parse_value());
            skip_ws();
            if (peek() == ']') {
                expect(']');
                break;
            }
            expect(',');
        }
        return value;
    }

    JsonValue parse_number() {
        const std::size_t start = pos_;
        if (peek() == '-') {
            ++pos_;
        }
        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            ++pos_;
        }
        if (peek() == '.') {
            ++pos_;
            while (std::isdigit(static_cast<unsigned char>(peek()))) {
                ++pos_;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') {
                ++pos_;
            }
            while (std::isdigit(static_cast<unsigned char>(peek()))) {
                ++pos_;
            }
        }

        JsonValue value;
        value.type = JsonValue::Type::Number;
        value.number_value = std::stod(input_.substr(start, pos_ - start));
        return value;
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (true) {
            if (pos_ >= input_.size()) {
                throw std::runtime_error("Unterminated JSON string");
            }
            const char ch = input_[pos_++];
            if (ch == '"') {
                break;
            }
            if (ch == '\\') {
                const char escaped = input_.at(pos_++);
                switch (escaped) {
                    case '"':
                    case '\\':
                    case '/':
                        out.push_back(escaped);
                        break;
                    case 'b':
                        out.push_back('\b');
                        break;
                    case 'f':
                        out.push_back('\f');
                        break;
                    case 'n':
                        out.push_back('\n');
                        break;
                    case 'r':
                        out.push_back('\r');
                        break;
                    case 't':
                        out.push_back('\t');
                        break;
                    default:
                        throw std::runtime_error("Unsupported JSON escape");
                }
            } else {
                out.push_back(ch);
            }
        }
        return out;
    }

    bool match(const char* token) {
        const std::size_t len = std::char_traits<char>::length(token);
        if (input_.substr(pos_, len) == token) {
            pos_ += len;
            return true;
        }
        return false;
    }

    void expect(char token) {
        skip_ws();
        if (peek() != token) {
            throw std::runtime_error(std::string("Expected JSON token: ") + token);
        }
        ++pos_;
    }

    char peek() const {
        if (pos_ >= input_.size()) {
            return '\0';
        }
        return input_[pos_];
    }

    void skip_ws() {
        while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) {
            ++pos_;
        }
    }

    std::string input_;
    std::size_t pos_ {0};
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        throw std::runtime_error("Could not open " + path.string());
    }
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

double rel_scaled_error(double actual, double expected, double scale) {
    return std::abs(actual - expected) / std::max(scale, 1.0);
}

TrajectorySample sample_from_json(const JsonValue& sample) {
    return {
        sample.at("time_s").number_value,
        sample.at("r_m").number_value,
        sample.at("theta_rad").number_value,
        sample.at("vr_mps").number_value,
        sample.at("vtheta_mps").number_value,
        sample.at("mass_kg").number_value,
    };
}

AtlasSeed seed_from_json(const JsonValue& seed_json) {
    AtlasSeed seed;
    require(seed_json.array_value.size() == 6, "Expected 6 seed values");
    for (std::size_t idx = 0; idx < 5; ++idx) {
        seed.params[idx] = seed_json.array_value[idx].number_value;
    }
    seed.transfer_time_days = seed_json.array_value[5].number_value;
    return seed;
}

void verify_dataset(
    const std::filesystem::path& dataset_path,
    const VariableIspAtlas& atlas,
    const VariableIspIntegrator& integrator,
    bool use_runtime_lookup) {
    const JsonValue root = JsonParser(read_text(dataset_path)).parse();
    const auto& trajectories = root.at("trajectories").array_value;
    require(!trajectories.empty(), "VariableISP dataset must not be empty");

    double worst_component_error = 0.0;
    for (const auto& entry : trajectories) {
        const double rho = entry.at("rho").number_value;
        const double kappa = entry.at("kappa").number_value;
        const double theta = entry.at("theta_rad").number_value;
        const AtlasSeed expected_seed = seed_from_json(entry.at("seed"));
        const AtlasSeed loaded_seed = use_runtime_lookup ? atlas.query(rho, kappa, theta) : expected_seed;
        const CanonicalMissionConfig config = VariableIspIntegrator::canonical_config(rho, kappa);
        const IntegrationSummary result = integrator.integrate_fixed_time(
            loaded_seed,
            config,
            entry.at("samples").array_value.size());

        require(result.samples.size() == entry.at("samples").array_value.size(), "Sample count mismatch");

        double max_error = 0.0;
        double mean_error = 0.0;
        std::size_t worst_sample_index = 0;
        std::string worst_component;
        double worst_expected = 0.0;
        double worst_actual = 0.0;
        const double velocity_scale = std::sqrt(config.mu_m3_s2 / VariableIspIntegrator::kAstronomicalUnitM);
        for (std::size_t idx = 0; idx < result.samples.size(); ++idx) {
            const TrajectorySample expected = sample_from_json(entry.at("samples").array_value[idx]);
            const TrajectorySample actual = result.samples[idx];
            const std::array<double, 5> component_errors {
                rel_scaled_error(actual.r_m, expected.r_m, expected.r_m),
                std::abs(VariableIspIntegrator::normalize_angle(actual.theta_rad - expected.theta_rad)),
                rel_scaled_error(actual.vr_mps, expected.vr_mps, velocity_scale),
                rel_scaled_error(actual.vtheta_mps, expected.vtheta_mps, velocity_scale),
                rel_scaled_error(actual.mass_kg, expected.mass_kg, expected.mass_kg),
            };
            const std::array<std::string, 5> component_names {"r", "theta", "vr", "vtheta", "mass"};
            const std::array<double, 5> expected_values {expected.r_m, expected.theta_rad, expected.vr_mps, expected.vtheta_mps, expected.mass_kg};
            const std::array<double, 5> actual_values {actual.r_m, actual.theta_rad, actual.vr_mps, actual.vtheta_mps, actual.mass_kg};
            for (std::size_t component = 0; component < component_errors.size(); ++component) {
                const double value = component_errors[component];
                if (value > max_error) {
                    max_error = value;
                    worst_sample_index = idx;
                    worst_component = component_names[component];
                    worst_expected = expected_values[component];
                    worst_actual = actual_values[component];
                }
                mean_error += value;
            }
        }
        mean_error /= static_cast<double>(result.samples.size() * 5);
        worst_component_error = std::max(worst_component_error, max_error);
        require(
            mean_error <= 2e-4,
            "VariableISP mean error exceeded tolerance for " + dataset_path.string()
                + " at rho=" + std::to_string(rho)
                + " kappa=" + std::to_string(kappa)
                + " theta=" + std::to_string(theta)
                + " seed0=" + std::to_string(loaded_seed.params[0])
                + " seed1=" + std::to_string(loaded_seed.params[1])
                + " seed2=" + std::to_string(loaded_seed.params[2])
                + " seed3=" + std::to_string(loaded_seed.params[3])
                + " seed4=" + std::to_string(loaded_seed.params[4])
                + " tf_days=" + std::to_string(loaded_seed.transfer_time_days)
                + " mean=" + std::to_string(mean_error)
                + " max=" + std::to_string(max_error)
                + " worst_sample=" + std::to_string(worst_sample_index)
                + " component=" + worst_component
                + " expected=" + std::to_string(worst_expected)
                + " actual=" + std::to_string(worst_actual));
        require(
            max_error <= 5e-3,
            "VariableISP max error exceeded tolerance for " + dataset_path.string()
                + " at rho=" + std::to_string(rho)
                + " kappa=" + std::to_string(kappa)
                + " theta=" + std::to_string(theta)
                + " seed0=" + std::to_string(loaded_seed.params[0])
                + " seed1=" + std::to_string(loaded_seed.params[1])
                + " seed2=" + std::to_string(loaded_seed.params[2])
                + " seed3=" + std::to_string(loaded_seed.params[3])
                + " seed4=" + std::to_string(loaded_seed.params[4])
                + " tf_days=" + std::to_string(loaded_seed.transfer_time_days)
                + " mean=" + std::to_string(mean_error)
                + " max=" + std::to_string(max_error)
                + " worst_sample=" + std::to_string(worst_sample_index)
                + " component=" + worst_component
                + " expected=" + std::to_string(worst_expected)
                + " actual=" + std::to_string(worst_actual));
    }

    std::cout << dataset_path.filename().string()
              << " verified, worst component error = " << worst_component_error << "\n";
}

}  // namespace

// The output samples are interpolated, so the RK45 steps do not depend on how many are
// asked for, and the last one is exactly the end. (Until 2026-10-08 the last sample time,
// dt x (n - 1), could round a hair above the end and the loop spun on zero-length steps until
// the step budget threw: some sample counts, e.g. 600, failed where 4000 worked.)
void verify_sample_counts(const std::filesystem::path& dataset_path, const VariableIspIntegrator& integrator) {
    const JsonValue root = JsonParser(read_text(dataset_path)).parse();
    for (const auto& entry : root.at("trajectories").array_value) {
        const AtlasSeed seed = seed_from_json(entry.at("seed"));
        const auto config = VariableIspIntegrator::canonical_config(entry.at("rho").number_value, entry.at("kappa").number_value);
        std::size_t steps = 0;
        double end_r = 0.0;
        for (const std::size_t count : {2, 600, 601, 999, 4000}) {
            const auto result = integrator.integrate_fixed_time(seed, config, count);
            require(result.samples.size() == count, "integrate_fixed_time should return the samples asked for");
            require(result.samples.back().time_s == seed.transfer_time_days * VariableIspIntegrator::kDayS,
                "the last sample should be exactly the end of the transfer");
            if (count == 2) {
                steps = result.accepted_steps + result.rejected_steps;
                end_r = result.samples.back().r_m;
            } else {
                require(result.accepted_steps + result.rejected_steps == steps,
                    "the RK45 steps should not depend on the number of output samples");
                require(result.samples.back().r_m == end_r, "the end state should not depend on the number of output samples");
            }
        }
    }
}


// General-boundary solver (GeneralTransfer.hpp): the atlas costates map exactly to the Cartesian
// primer, the STM refine agrees with the finite-difference one, and arbitrary-state transfers
// solve and survive an independent rollout.
void verify_general_transfer(const std::filesystem::path& dataset_path, const VariableIspIntegrator& integrator) {
    namespace g = spacetrains::variable_isp::general;
    const g::Propagator propagator;
    const JsonValue root = JsonParser(read_text(dataset_path)).parse();
    std::size_t refined = 0, checked = 0, grazing = 0;
    for (const auto& entry : root.at("trajectories").array_value) {
        const AtlasSeed seed = seed_from_json(entry.at("seed"));
        const auto config = VariableIspIntegrator::canonical_config(entry.at("rho").number_value, entry.at("kappa").number_value);
        const double length = config.r0_m;
        const double time = std::sqrt(length * length * length / config.mu_m3_s2);
        const double accel = length / (time * time);
        const double k = config.k_gain;
        const g::Primer primer {
            {k * seed.params[1] / accel, k * seed.params[2] / accel},
            {-k * seed.params[0] / (accel / time), -k * (seed.params[4] + config.vtheta0_mps * seed.params[1]) / length / (accel / time)},
        };
        const g::State start {{1.0, 0.0}, {0.0, config.vtheta0_mps / (length / time)}};
        const double T = seed.transfer_time_days * VariableIspIntegrator::kDayS / time;
        const auto atlas_end = integrator.integrate_fixed_time(seed, config, 2).samples.back();
        const auto end = propagator.propagate(start, primer, T, false);
        ++checked;
        if (!end.ok) {
            // Atlas arcs that graze the Sun hit the propagator's singularity guard.
            const auto dense = integrator.integrate_fixed_time(seed, config, 2000).samples;
            const auto closest = std::min_element(dense.begin(), dense.end(), [](const auto& x, const auto& y) { return x.r_m < y.r_m; });
            require(closest->r_m < 0.01 * length, "primer rollout of an atlas seed should succeed");
            ++grazing;
            continue;
        }
        const double effort_from_mass = 2.0 * config.power_w * (1.0 / atlas_end.mass_kg - 1.0 / config.m0_kg) / (accel * accel * time);
        const double err = std::max({std::abs(g::norm(end.state.r) * length / atlas_end.r_m - 1.0), std::abs(end.sweep_rad - atlas_end.theta_rad),
            std::abs(end.effort / effort_from_mass - 1.0)});
        // Near-singular solar passes amplify the two integrators' differences.
        require(err < (end.min_radius > 0.01 ? 1e-4 : 1e-2), "primer rollout should match the atlas trajectory and mass use");

        // STM refine onto a slightly moved target agrees with the finite-difference refine.
        const double r_target = atlas_end.r_m * 1.01;
        const double theta_target = atlas_end.theta_rad + 0.01;
        const auto fd = integrator.refine_seed(seed, config, r_target, theta_target);
        const auto stm = integrator.refine_seed_stm(seed, config, [&](double) { return std::pair {r_target, theta_target}; });
        if (fd.converged && stm.converged && end.min_radius > 0.05) {  // grazing arcs: integrators differ
            const auto e = integrator.integrate_fixed_time(stm.seed, config, 2).samples.back();
            require(std::abs(e.r_m / r_target - 1.0) < 1e-4, "refine_seed_stm should hit the target radius");
            require(std::abs(e.theta_rad - theta_target) < 1e-4, "refine_seed_stm should hit the target angle");
            require(std::abs(e.vr_mps) < 1e-3 * config.vtheta0_mps, "refine_seed_stm should arrive with no radial speed");
            require(std::abs(stm.seed.transfer_time_days / fd.seed.transfer_time_days - 1.0) < 0.05,
                "refine_seed_stm and refine_seed should agree on the transfer time");
            ++refined;
        }
    }
    require(refined > 0, "refine_seed_stm should converge on the reference set");
    require(grazing * 2 < checked, "most atlas reference arcs should not graze the Sun");

    // A chase: from a perturbed circular orbit at 1.5 to a target coasting on an ellipse.
    const g::State chaser {{1.5, 0.0}, {0.05, 0.85}};
    const g::State quarry {{1.2, 0.6}, {-0.4, 0.75}};
    for (const auto end_condition : {g::EndCondition::Rendezvous, g::EndCondition::Intercept}) {
        const g::TransferRequest request {chaser, g::coasting_target(quarry), end_condition, 0.1};
        const double T = 1.3;
        const auto sol = g::solve_transfer(propagator, request, T);
        require(sol.converged, "solve_transfer should solve the chase");
        g::PropagationSettings tight;
        tight.relative_tolerance = 1e-12;
        tight.absolute_tolerance = 1e-13;
        const auto rollout = g::Propagator(tight).propagate(chaser, sol.primer, T, false);
        const auto goal = request.target(T).state;
        require(g::norm(rollout.state.r - goal.r) < 1e-6, "chase rollout should reach the target");
        if (end_condition == g::EndCondition::Rendezvous) {
            require(g::norm(rollout.state.v - goal.v) < 1e-6, "rendezvous rollout should match the target velocity");
        } else {
            require(g::norm(rollout.primer.p) < 1e-6, "intercept should end with zero thrust (free arrival velocity)");
        }

        // Multiple shooting from the converged solution's nodes, perturbed, converges back to it.
        {
            std::vector<std::array<double, 8>> nodes;
            for (int k = 0; k < 4; ++k) {
                const auto e = propagator.propagate(chaser, sol.primer, T * k / 4.0, false);
                const auto& st = k == 0 ? chaser : e.state;
                const auto& pr = k == 0 ? sol.primer : e.primer;
                nodes.push_back({st.r.x * 1.002, st.r.y, st.v.x, st.v.y * 0.998, pr.p.x * 1.02, pr.p.y, pr.w.x, pr.w.y * 0.98});
            }
            const auto ms = g::multiple_shoot(propagator, chaser, request.target, end_condition, T, nodes);
            require(ms.converged && std::abs(ms.end.effort / sol.end.effort - 1.0) < 1e-6, "multiple shooting should recover the solution");
        }

        // Time reversal: flying back with both velocities negated costs the same effort.
        if (end_condition == g::EndCondition::Rendezvous) {
            const g::State back_start {goal.r, -1.0 * goal.v};
            const g::State back_end {chaser.r, -1.0 * chaser.v};
            const g::TransferRequest back {back_start, g::fixed_target(back_end), end_condition, 0.1};
            const auto reverse = g::solve_transfer(propagator, back, T);
            require(reverse.converged && std::abs(reverse.end.effort / sol.end.effort - 1.0) < 1e-6,
                "the reversed transfer should cost the same effort");
        }

        // Spending twice the effort along the branch shortens the transfer and uses the budget.
        const auto spent = g::spend_effort_by_time(propagator, chaser, request.target, end_condition, sol, 2.0 * sol.end.effort, 0.1);
        require(spent.converged && spent.T < T && spent.end.effort <= 2.0 * sol.end.effort * (1.0 + 1e-9)
                && spent.end.effort > 2.0 * sol.end.effort * 0.99,
            "spend_effort_by_time should shorten the transfer to use the budget");

        // Twice the effort buys an earlier arrival, which uses the budget exactly.
        const auto fast = g::solve_min_time(propagator, request, 2.0 * sol.end.effort, 0.05, 2.0 * T);
        require(fast.converged && fast.T < T, "a larger budget should arrive earlier");
        require(std::abs(fast.end.effort / (2.0 * sol.end.effort) - 1.0) < 1e-6, "the minimum-time transfer should use the budget");
    }

    // Free-space effort formula against a direct quadrature of u = A + C t.
    const g::State a {{1.0, 0.2}, {0.1, 0.9}};
    const g::State b {{-0.5, 1.1}, {-0.6, -0.2}};
    const double T = 0.7;
    const auto seed = g::free_space_seed(a, b, T);
    double quad = 0.0;
    for (int i = 0; i < 1000; ++i) {
        const double t = (i + 0.5) * T / 1000.0;
        const g::Vec2 u = seed.p + t * seed.w;
        quad += g::dot(u, u) * T / 1000.0;
    }
    require(std::abs(quad / g::free_space_effort(a, b, T) - 1.0) < 1e-5, "free-space effort formula");
    std::cout << "general transfer verified (" << checked << " atlas arcs, " << grazing << " Sun-grazing skipped, " << refined
              << " STM refines)\n";
}

// The game-facing chase adapter: a plasma freighter leaving Earth's orbit intercepts a ship
// coasting outward on a Hohmann-like ellipse, given as a sampled path in SI units.
void verify_chase_adapter() {
    namespace g = spacetrains::variable_isp::general;
    using spacetrains::math::Vec3d;
    namespace tr = spacetrains::trajectory;
    const double au = VariableIspIntegrator::kAstronomicalUnitM;
    const double mu = VariableIspIntegrator::kMuSunSI;
    const tr::ChaseUnits units {mu, au};
    // Quarry: perihelion at 1 AU, aphelion 1.52 AU, starting 0.3 rad ahead of the chaser.
    const double a = 1.26 * au, e = 0.52 / 2.52;
    spacetrains::domain::TrajectoryPlan quarry;
    const double period = 2.0 * 3.14159265358979 * std::sqrt(a * a * a / mu);
    for (int i = 0; i <= 400; ++i) {
        const double t = period * 0.5 * i / 400.0;
        // Kepler's equation for the eccentric anomaly.
        const double M = 2.0 * 3.14159265358979 * t / period;
        double E = M;
        for (int k = 0; k < 30; ++k) E = M + e * std::sin(E);
        const double nu = 2.0 * std::atan2(std::sqrt(1 + e) * std::sin(E / 2), std::sqrt(1 - e) * std::cos(E / 2));
        const double r = a * (1 - e * std::cos(E));
        quarry.sampled_path.push_back(Vec3d {r * std::cos(nu + 0.3), 0.0, r * std::sin(nu + 0.3)});
        quarry.sampled_times_s.push_back(t);
    }
    const double v_earth = std::sqrt(mu / au);
    tr::ChaseRequest request;
    request.position_m = {au, 0.0, 0.0};
    request.velocity_mps = {0.0, 0.0, v_earth};
    request.depart_time_s = 10.0 * 86400.0;
    request.power_w = 293.0 * 34100.0;
    request.dry_mass_kg = 34100.0 + 15000.0;  // half the tanks stay as reserve and payload
    request.initial_mass_kg = 34100.0 + 30000.0;
    for (const auto end_condition : {g::EndCondition::Intercept, g::EndCondition::Rendezvous}) {
        request.end_condition = end_condition;
        const auto target = tr::path_target(quarry, request.depart_time_s, units);
        const auto plan = tr::plan_chase(request, target, units);
        require(plan.feasible, "plasma freighter should catch the coasting ship");
        require(plan.propellant_kg <= 15000.0 * 1.0001, "the chase should fit the propellant budget");
        const auto goal = target(plan.transfer_time_s / units.time_s()).state;
        require((plan.path_m.back() - units.position_m(goal.r)).length() < 1e-4 * au, "the chase path should end at the quarry");
        std::cout << "chase " << (end_condition == g::EndCondition::Intercept ? "intercept" : "rendezvous") << ": "
                  << plan.transfer_time_s / 86400.0 << " d, " << plan.propellant_kg << " kg\n";
    }
}

int main() {
    const auto repo_root = std::filesystem::current_path();
    VariableIspAtlas atlas;
    atlas.load_binary((repo_root / "tests/data/variable_isp/variable_isp_atlas.bin").string());
    require(!atlas.rho_grid().empty(), "atlas rho grid should load");
    require(atlas.solved_count() > 1000, "atlas solved coverage unexpectedly low");

    VariableIspIntegrator integrator;
    verify_dataset(repo_root / "tests/data/variable_isp/reference_unit.json", atlas, integrator, false);
    verify_dataset(repo_root / "tests/data/variable_isp/reference_validation.json", atlas, integrator, false);

    verify_sample_counts(repo_root / "tests/data/variable_isp/reference_unit.json", integrator);
    verify_general_transfer(repo_root / "tests/data/variable_isp/reference_unit.json", integrator);
    verify_chase_adapter();

    std::cout << "VariableISP tests passed.\n";
    return 0;
}
