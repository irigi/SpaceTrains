#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "simulation/Simulation.hpp"

namespace {

struct BridgeConfig {
    std::string data_root;
    std::string snapshot_file;
    std::string command_file;
    bool once {false};
    double step_seconds {0.1};
    int startup_steps {0};
};

BridgeConfig parse_args(int argc, char** argv) {
    BridgeConfig config;
    config.data_root = (std::filesystem::current_path() / "data").string();
    config.snapshot_file = (std::filesystem::temp_directory_path() / "spacetrains_snapshot.json").string();
    config.command_file = (std::filesystem::temp_directory_path() / "spacetrains_commands.json").string();

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--data-root" && i + 1 < argc) {
            config.data_root = argv[++i];
        } else if (arg == "--snapshot-file" && i + 1 < argc) {
            config.snapshot_file = argv[++i];
        } else if (arg == "--command-file" && i + 1 < argc) {
            config.command_file = argv[++i];
        } else if (arg == "--once") {
            config.once = true;
        } else if (arg == "--step-seconds" && i + 1 < argc) {
            config.step_seconds = std::stod(argv[++i]);
        } else if (arg == "--startup-steps" && i + 1 < argc) {
            config.startup_steps = std::stoi(argv[++i]);
        }
    }
    return config;
}

void write_text_file(const std::string& path, const std::string& contents) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << contents;
}

std::string read_text_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool parse_bool_flag(const std::string& text, const std::string& key, bool fallback) {
    const auto marker = "\"" + key + "\":";
    const auto index = text.find(marker);
    if (index == std::string::npos) {
        return fallback;
    }
    const auto value_index = index + marker.size();
    if (text.compare(value_index, 4, "true") == 0) {
        return true;
    }
    if (text.compare(value_index, 5, "false") == 0) {
        return false;
    }
    return fallback;
}

double parse_number_flag(const std::string& text, const std::string& key, double fallback) {
    const auto marker = "\"" + key + "\":";
    const auto index = text.find(marker);
    if (index == std::string::npos) {
        return fallback;
    }
    const auto value_index = index + marker.size();
    try {
        return std::stod(text.substr(value_index));
    } catch (...) {
        return fallback;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const auto config = parse_args(argc, argv);
    auto simulation = spacetrains::simulation::Simulation::from_data_root(config.data_root);

    bool paused = false;
    double timewarp = simulation.timewarp_factor();
    for (int i = 0; i < config.startup_steps; ++i) {
        simulation.step(config.step_seconds);
    }

    auto tick = [&]() {
        const std::string command_text = read_text_file(config.command_file);
        if (!command_text.empty()) {
            paused = parse_bool_flag(command_text, "paused", paused);
            timewarp = parse_number_flag(command_text, "timewarp_factor", timewarp);
            simulation.set_timewarp(timewarp);
        }

        if (!paused) {
            simulation.step(config.step_seconds);
        }
        write_text_file(config.snapshot_file, simulation.build_bridge_snapshot_json(paused));
    };

    if (config.once) {
        tick();
        return 0;
    }

    while (true) {
        tick();
        std::this_thread::sleep_for(std::chrono::duration<double>(config.step_seconds));
    }
}
