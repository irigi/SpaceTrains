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
    // Write aside and rename, so the UI never reads a half-written snapshot.
    const std::string temp_path = path + ".tmp";
    {
        std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
        file << contents;
    }
    std::error_code error;
    std::filesystem::rename(temp_path, path, error);
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
    const auto bridge_start = std::chrono::steady_clock::now();
    std::uint64_t snapshot_seq = 0;

    bool paused = false;
    double timewarp = simulation.timewarp_factor();
    for (int i = 0; i < config.startup_steps; ++i) {
        simulation.step(config.step_seconds);
    }

    const auto write_snapshot = [&]() {
        const auto now = std::chrono::steady_clock::now();
        const double snapshot_real_time_s = std::chrono::duration<double>(now - bridge_start).count();
        const auto seq = snapshot_seq++;
        write_text_file(config.snapshot_file, simulation.build_bridge_snapshot_json(paused, seq, snapshot_real_time_s));
        // The UI polls this small file every frame and reads the snapshot only when it changes.
        write_text_file(config.snapshot_file + ".seq", std::to_string(seq));
    };

    // Game time follows real time x timewarp: each loop hands the simulation the real time
    // since the last one, and it runs the whole ticks due. A slow tick (mission planning)
    // delays the next snapshot but not the pace; the UI keeps the picture moving meanwhile.
    // A slow tick (a ship purchase, a few seconds at most) is caught up in the next loop:
    // the UI runs a few seconds ahead meanwhile and should not have to wait for dropped time.
    // Longer stalls (a debugger, a suspended laptop) drop time rather than race.
    constexpr double kMaxCatchUpS = 4.0;
    std::string last_command_text;
    auto last_loop = std::chrono::steady_clock::now();
    const auto tick = [&]() {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed_s = std::min(kMaxCatchUpS, std::chrono::duration<double>(now - last_loop).count());
        last_loop = now;
        bool changed = false;
        const std::string command_text = read_text_file(config.command_file);
        if (!command_text.empty() && command_text != last_command_text) {
            last_command_text = command_text;
            paused = parse_bool_flag(command_text, "paused", paused);
            timewarp = parse_number_flag(command_text, "timewarp_factor", timewarp);
            simulation.set_timewarp(timewarp);
            changed = true;
        }
        if (!paused) {
            const double before_s = simulation.game_time_s();
            simulation.step(elapsed_s);
            changed = changed || simulation.game_time_s() != before_s;
        }
        if (changed) {
            write_snapshot();
        }
    };

    // The first tick dispatches the whole fleet at once (a few seconds of trajectory
    // planning): show the starting state before it, so the UI does not wait on a blank
    // screen, and start the clock after it, so its planning time is not caught up later.
    write_snapshot();
    simulation.step(spacetrains::simulation::Simulation::TICK_S / simulation.timewarp_factor());
    write_snapshot();
    if (config.once) {
        return 0;
    }
    last_loop = std::chrono::steady_clock::now();

    const auto loop_period = std::chrono::duration<double>(std::min(config.step_seconds, 0.05));
    while (true) {
        const auto loop_start = std::chrono::steady_clock::now();
        tick();
        std::this_thread::sleep_until(loop_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(loop_period));
    }
}
