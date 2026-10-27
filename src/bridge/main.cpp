#include <cerrno>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "simulation/Simulation.hpp"
#include "trajectory/Porkchop.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#endif

namespace {

struct BridgeConfig {
    std::string data_root;
    std::string snapshot_file;
    std::string command_file;
    bool once {false};
    std::string opening_cache_dir;  // where the state after the opening dispatch is kept
    double step_seconds {0.1};
    int startup_steps {0};
    long long parent_pid {0};  // the UI's process: the bridge exits when it is gone (0: not watched)
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
        } else if (arg == "--opening-cache" && i + 1 < argc) {
            config.opening_cache_dir = argv[++i];
        } else if (arg == "--once") {
            config.once = true;
        } else if (arg == "--step-seconds" && i + 1 < argc) {
            config.step_seconds = std::stod(argv[++i]);
        } else if (arg == "--startup-steps" && i + 1 < argc) {
            config.startup_steps = std::stoi(argv[++i]);
        } else if (arg == "--parent-pid" && i + 1 < argc) {
            config.parent_pid = std::stoll(argv[++i]);
        }
    }
    return config;
}

// Write aside and rename, so a reader never sees a half-written file and a failed or
// interrupted write leaves the old file whole. Returns false if the write failed.
bool write_text_file(const std::string& path, const std::string& contents) {
    const std::string temp_path = path + ".tmp";
    {
        std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
        file << contents;
        file.close();
        if (!file) {
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temp_path, path, error);
    return !error;
}

// The UI writes its command file as one JSON object; until the closing brace is there the
// file is still being written, and is read again on the next loop.
bool complete_command(const std::string& text) {
    const auto last = text.find_last_not_of(" \t\r\n");
    return last != std::string::npos && text[last] == '}';
}

// The bridge must not outlive the UI that started it: an orphan keeps simulating at full
// speed. Linux signals it when the parent dies; elsewhere (and as a backstop) the loop checks.
class ParentWatch {
public:
    explicit ParentWatch(long long parent_pid) : parent_pid_(parent_pid) {
#if defined(_WIN32)
        if (parent_pid_ > 0) {
            handle_ = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(parent_pid_));
        }
#else
        initial_parent_ = getppid();
#if defined(__linux__)
        prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
#endif
    }
    ~ParentWatch() {
#if defined(_WIN32)
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
#endif
    }
    ParentWatch(const ParentWatch&) = delete;
    ParentWatch& operator=(const ParentWatch&) = delete;

    [[nodiscard]] bool parent_gone() const {
#if defined(_WIN32)
        return handle_ != nullptr && WaitForSingleObject(handle_, 0) == WAIT_OBJECT_0;
#else
        if (getppid() != initial_parent_) {
            return true;  // re-parented: the process that started the bridge has exited
        }
        return parent_pid_ > 0 && kill(static_cast<pid_t>(parent_pid_), 0) != 0 && errno == ESRCH;
#endif
    }

private:
    long long parent_pid_;
#if defined(_WIN32)
    HANDLE handle_ {nullptr};
#else
    pid_t initial_parent_ {0};
#endif
};

std::string read_text_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// The opening cache's key: every data file's path and contents (a few hundred KB), and the
// atlas file's size and time (it lives next to the data, in tests/data). Saves use the
// narrower Simulation::data_fingerprint, so a save still loads after, say, a price tweak;
// the opening has to be planned again after any data change.
std::string opening_fingerprint(const std::filesystem::path& data_root) {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&](const std::string& text) {
        for (const char ch : text) {
            hash ^= static_cast<unsigned char>(ch);
            hash *= 1099511628211ULL;
        }
    };
    std::error_code error;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(data_root, error)) {
        if (entry.is_regular_file(error)) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        mix(std::filesystem::relative(file, data_root, error).generic_string());
        mix("\n" + read_text_file(file.string()) + "\n");
    }
    const auto atlas = data_root.parent_path() / "tests" / "data" / "variable_isp" / "variable_isp_atlas.bin";
    if (std::filesystem::exists(atlas, error)) {
        mix(std::format("atlas|{}|{}", std::filesystem::file_size(atlas, error),
            std::filesystem::last_write_time(atlas, error).time_since_epoch().count()));
    }
    return std::format("{:016x}", hash);
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

// "key":"value" (the UI writes paths without quotes or backslashes).
std::string parse_string_flag(const std::string& text, const std::string& key) {
    const auto marker = "\"" + key + "\":\"";
    const auto index = text.find(marker);
    if (index == std::string::npos) {
        return {};
    }
    const auto start = index + marker.size();
    const auto end = text.find('"', start);
    return end == std::string::npos ? std::string {} : text.substr(start, end - start);
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
    const ParentWatch parent_watch(config.parent_pid);
    auto simulation = spacetrains::simulation::Simulation::from_data_root(config.data_root);
    const auto bridge_start = std::chrono::steady_clock::now();
    std::uint64_t snapshot_seq = 0;

    bool paused = false;
    double timewarp = simulation.timewarp_factor();
    for (int i = 0; i < config.startup_steps; ++i) {
        simulation.step(config.step_seconds);
    }

    // Save/load results for the UI, and an epoch that changes with every load (the UI then
    // drops what it remembered of the old timeline).
    int epoch = 0;
    std::string command_status;
    std::string paths_signature = "-";
    std::uint64_t paths_seq = 0;
    std::string designs_signature = "-";
    std::uint64_t designs_seq = 0;
    // Statistics (the UI's Statistics screen): a side file, written only while the screen is open,
    // at most once a second of real time and whenever a month's sample closes.
    bool stats_open = false;
    bool stats_due = false;
    std::uint64_t stats_seq = 0;
    std::size_t stats_samples = 0;
    auto last_stats = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    // Porkchop plots (Statistics > Routes, the station inspector's windows): the UI names planet
    // pairs ("porkchop":"earth>mars;earth>venus"); a background thread fills their grids and the
    // bridge writes them to a side file as columns arrive, at most twice a second.
    spacetrains::trajectory::PorkchopService porkchops(simulation.universe());
    std::vector<std::pair<std::string, std::string>> porkchop_pairs;
    std::uint64_t porkchop_version = 0;
    std::uint64_t porkchop_seq = 0;
    double porkchop_day = -1.0;
    auto last_porkchop = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    const auto write_snapshot = [&]() {
        const auto now = std::chrono::steady_clock::now();
        const double snapshot_real_time_s = std::chrono::duration<double>(now - bridge_start).count();
        const auto seq = snapshot_seq++;
        // Planned paths change only when a ship gets a mission: they go to their own file,
        // written before the snapshot that refers to them.
        if (auto signature = simulation.bridge_paths_signature(); signature != paths_signature) {
            paths_signature = std::move(signature);
            write_text_file(config.snapshot_file + ".paths", simulation.build_bridge_paths_json());
            write_text_file(config.snapshot_file + ".paths.seq", std::to_string(paths_seq++));
        }
        // Ship designs (classes and their parts) are static until ships carry their own fits.
        if (auto signature = simulation.bridge_designs_signature(); signature != designs_signature) {
            designs_signature = std::move(signature);
            write_text_file(config.snapshot_file + ".designs", simulation.build_bridge_designs_json());
            write_text_file(config.snapshot_file + ".designs.seq", std::to_string(designs_seq++));
        }
        if (stats_open) {
            const auto samples = simulation.money_journal().history().size();
            if (stats_due || samples != stats_samples || now - last_stats >= std::chrono::seconds(1)) {
                write_text_file(config.snapshot_file + ".stats", simulation.build_stats_json());
                write_text_file(config.snapshot_file + ".stats.seq", std::to_string(stats_seq++));
                stats_samples = samples;
                last_stats = now;
                stats_due = false;
            }
        }
        if (!porkchop_pairs.empty()) {
            const double day = simulation.game_time_s() / 86400.0;
            porkchops.request(porkchop_pairs, simulation.game_time_s());
            if ((porkchops.version() != porkchop_version || std::abs(day - porkchop_day) >= 1.0)
                && now - last_porkchop >= std::chrono::milliseconds(500)) {
                porkchop_version = porkchops.version();
                porkchop_day = day;
                last_porkchop = now;
                write_text_file(config.snapshot_file + ".porkchop", porkchops.json());
                write_text_file(config.snapshot_file + ".porkchop.seq", std::to_string(porkchop_seq++));
            }
        }
        auto json = simulation.build_bridge_snapshot_json(paused, seq, snapshot_real_time_s, false);
        json.pop_back();  // the closing brace
        json += std::format(",\"bridge\":{{\"epoch\":{},\"status\":\"{}\"}}}}", epoch, command_status);
        write_text_file(config.snapshot_file, json);
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
    double last_request = -1.0;
    auto last_snapshot = std::chrono::steady_clock::now();
    bool dirty = false;  // a change not yet in a snapshot
    double owed_real_s = 0.0;  // real time the simulation has not caught up yet
    auto last_loop = std::chrono::steady_clock::now();
    const auto tick = [&]() {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed_s = std::min(kMaxCatchUpS, std::chrono::duration<double>(now - last_loop).count());
        last_loop = now;
        bool changed = false;
        const std::string command_text = read_text_file(config.command_file);
        if (complete_command(command_text) && command_text != last_command_text) {
            last_command_text = command_text;
            paused = parse_bool_flag(command_text, "paused", paused);
            timewarp = parse_number_flag(command_text, "timewarp_factor", timewarp);
            const bool open = parse_bool_flag(command_text, "stats_open", false);
            stats_due = stats_due || (open && !stats_open);
            stats_open = open;
            {
                std::vector<std::pair<std::string, std::string>> pairs;
                const auto list = parse_string_flag(command_text, "porkchop");
                std::size_t start = 0;
                while (start < list.size()) {
                    const auto end = std::min(list.find(';', start), list.size());
                    const auto item = list.substr(start, end - start);
                    if (const auto arrow = item.find('>'); arrow != std::string::npos) {
                        pairs.emplace_back(item.substr(0, arrow), item.substr(arrow + 1));
                    }
                    start = end + 1;
                }
                if (pairs != porkchop_pairs) {
                    porkchop_pairs = std::move(pairs);
                    porkchop_day = -1.0;   // write the new selection at once
                    porkchops.request(porkchop_pairs, simulation.game_time_s());
                }
            }
            simulation.set_timewarp(timewarp);
            changed = true;
            // Save and load requests carry a request number, so the same path can be used again.
            const double request = parse_number_flag(command_text, "request", -1.0);
            if (request > last_request) {
                last_request = request;
                if (const auto path = parse_string_flag(command_text, "save_path"); !path.empty()) {
                    command_status = write_text_file(path, simulation.save_state_json())
                        ? std::format("Saved day {:.1f}", simulation.game_time_s() / 86400.0) : "Save failed";
                } else if (const auto path = parse_string_flag(command_text, "load_path"); !path.empty()) {
                    const auto text = read_text_file(path);
                    try {
                        if (text.empty()) {
                            throw std::runtime_error("no save file");
                        }
                        simulation.load_state_json(text);
                        simulation.set_timewarp(timewarp);
                        ++epoch;
                        stats_due = true;
                        command_status = std::format("Loaded day {:.1f}", simulation.game_time_s() / 86400.0);
                    } catch (const std::exception& error) {
                        command_status = std::string("Load failed: ") + error.what();
                        for (auto& ch : command_status) {
                            ch = ch == '"' || ch == '\\' ? '\'' : ch;
                        }
                    }
                }
            }
        }
        if (!paused) {
            // Ticks one at a time, for at most 100 ms of work per loop: when the simulation
            // runs slower than the timewarp asks (a fleet review), snapshots keep coming and
            // the UI slows down smoothly; the rest of the time is carried over (up to
            // kMaxCatchUpS) instead of being run in one long burst.
            // The cap is at least one tick: at the slowest timewarp (600x) a tick is 14.4 s of
            // real time, and a 4 s cap alone would never let one run.
            const double tick_real_s = spacetrains::simulation::Simulation::TICK_S / std::max(1.0, timewarp);
            owed_real_s = std::min(std::max(kMaxCatchUpS, tick_real_s), owed_real_s + elapsed_s);
            const double before_s = simulation.game_time_s();
            const auto work_start = std::chrono::steady_clock::now();
            while (owed_real_s >= tick_real_s - 1e-9
                && std::chrono::steady_clock::now() - work_start < std::chrono::milliseconds(100)) {
                simulation.step(tick_real_s);
                owed_real_s -= tick_real_s;
            }
            changed = changed || simulation.game_time_s() != before_s;
            // Debug aid: SPACETRAINS_BRIDGE_LOG=1 reports slow loops (stderr).
            static const bool log_slow = std::getenv("SPACETRAINS_BRIDGE_LOG") != nullptr;
            const double work_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - work_start).count();
            if (log_slow && work_s > 0.3) {
                std::cerr << std::format("[bridge] day {:.2f}: {:.0f} ticks took {:.2f} s (timewarp {:.0f}, {:.2f} s owed)\n",
                    simulation.game_time_s() / 86400.0, (simulation.game_time_s() - before_s) / spacetrains::simulation::Simulation::TICK_S,
                    work_s, timewarp, owed_real_s);
            }
        }
        dirty = dirty || changed;
        if (dirty && (now - last_snapshot >= std::chrono::milliseconds(100) || paused)) {
            write_snapshot();
            last_snapshot = now;
            dirty = false;
        }
    };

    // The first tick dispatches the whole fleet at once (a few seconds of trajectory
    // planning): show the starting state before it, so the UI does not wait on a blank
    // screen, and start the clock after it, so its planning time is not caught up later.
    write_snapshot();
    // The opening is the same for the same data and the same build, so it is kept: a later
    // start loads it instead of planning again (the user's "pre-calculate and save").
    std::string opening_path;
    if (!config.opening_cache_dir.empty()) {
        std::error_code error;
        const auto build_time = std::filesystem::last_write_time(argv[0], error);
        opening_path = (std::filesystem::path(config.opening_cache_dir)
            / std::format("opening_{}_{}.json", opening_fingerprint(config.data_root),
                error ? 0 : build_time.time_since_epoch().count())).string();
    }
    bool opened_from_cache = false;
    if (const auto cached = opening_path.empty() ? std::string {} : read_text_file(opening_path); !cached.empty()) {
        try {
            simulation.load_state_json(cached);
            simulation.set_timewarp(timewarp);
            opened_from_cache = true;
        } catch (const std::exception&) {
            // A stale or damaged cache: plan the opening again below.
        }
    }
    if (!opened_from_cache) {
        simulation.step(spacetrains::simulation::Simulation::TICK_S / simulation.timewarp_factor());
        if (!opening_path.empty()) {
            std::error_code error;
            const auto directory = std::filesystem::path(opening_path).parent_path();
            std::filesystem::create_directories(directory, error);
            // Openings of older builds or data are stale: keep only this one.
            for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
                const auto name = entry.path().filename().string();
                if (name.starts_with("opening_") && entry.path() != std::filesystem::path(opening_path)) {
                    std::filesystem::remove(entry.path(), error);
                }
            }
            write_text_file(opening_path, simulation.save_state_json());
        }
    }
    write_snapshot();
    if (config.once) {
        return 0;
    }
    last_loop = std::chrono::steady_clock::now();

    const auto loop_period = std::chrono::duration<double>(std::min(config.step_seconds, 0.05));
    while (!parent_watch.parent_gone()) {
        const auto loop_start = std::chrono::steady_clock::now();
        tick();
        std::this_thread::sleep_until(loop_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(loop_period));
    }
    std::cerr << "[bridge] the UI is gone; exiting\n";
    return 0;
}
