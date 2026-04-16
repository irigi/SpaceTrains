#include <filesystem>
#include <iostream>

#include "simulation/Simulation.hpp"

int main(int argc, char** argv) {
    const std::filesystem::path repo_root = (argc > 1) ? argv[1] : std::filesystem::current_path();
    auto sim = spacetrains::simulation::Simulation::from_data_root((repo_root / "data").string());
    sim.set_timewarp(24.0 * 3600.0);

    for (int i = 0; i < 30; ++i) {
        sim.step(1.0);
    }

    std::cout << sim.build_report();
    return 0;
}
