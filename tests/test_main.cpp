#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include "data_loader/DataLoader.hpp"
#include "simulation/Simulation.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

}  // namespace

int main() {
    const auto repo_root = std::filesystem::current_path();
    spacetrains::data_loader::DataLoader loader;
    const auto universe = loader.load_universe(repo_root / "data");

    require(universe.bodies.size() >= 6, "expected seeded bodies");
    require(universe.stations.size() >= 6, "expected seeded stations");
    require(universe.ship_seeds.size() >= 3, "expected seeded ships");

    auto sim = spacetrains::simulation::Simulation::from_data_root((repo_root / "data").string());
    const auto before = sim.snapshot();
    require(!before.stations.empty(), "snapshot should include stations");

    for (int i = 0; i < 90; ++i) {
        sim.step(1.0);
    }

    const auto after = sim.snapshot();
    require(after.game_time_s > before.game_time_s, "time should advance");
    require(!after.recent_events.empty(), "simulation should emit events");
    const auto bridge_json = sim.build_bridge_snapshot_json(false, 1, 0.1);
    require(bridge_json.find("\"bodies\"") != std::string::npos, "bridge snapshot should include bodies");
    require(bridge_json.find("\"ships\"") != std::string::npos, "bridge snapshot should include ships");

    bool found_in_transit_or_arrival_signal = false;
    for (const auto& event : after.recent_events) {
        if (event.text.find("departed") != std::string::npos || event.text.find("arrived") != std::string::npos) {
            found_in_transit_or_arrival_signal = true;
        }
    }
    require(found_in_transit_or_arrival_signal, "simulation should dispatch at least one mission");

    std::cout << "All SpaceTrains tests passed.\n";
    return 0;
}
