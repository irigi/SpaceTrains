#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

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

int main() {
    const auto repo_root = std::filesystem::current_path();
    VariableIspAtlas atlas;
    atlas.load_binary((repo_root / "tests/data/variable_isp/variable_isp_atlas.bin").string());
    require(!atlas.rho_grid().empty(), "atlas rho grid should load");
    require(atlas.solved_count() > 1000, "atlas solved coverage unexpectedly low");

    VariableIspIntegrator integrator;
    verify_dataset(repo_root / "tests/data/variable_isp/reference_unit.json", atlas, integrator, false);
    verify_dataset(repo_root / "tests/data/variable_isp/reference_validation.json", atlas, integrator, false);

    std::cout << "VariableISP tests passed.\n";
    return 0;
}
