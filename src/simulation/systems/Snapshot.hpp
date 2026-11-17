#pragma once

// The snapshot, the bridge's JSON (snapshot, paths, designs) and the text report
// (docs/plans/simulation_split.md, step 13; docs/modules/ui_observer.md). Read only.

#include <cstdint>
#include <string>

#include "simulation/World.hpp"

namespace spacetrains::simulation::systems {

[[nodiscard]] domain::SimulationSnapshot snapshot(const World& w);
[[nodiscard]] std::string build_report(const World& w);
// The UI snapshot. Without paths, ships in flight carry only a "path_id"; their planned
// paths are in build_bridge_paths_json(), which the bridge writes when
// bridge_paths_signature() changes (paths were 70% of a 600 KB snapshot at 90 ships).
[[nodiscard]] std::string build_bridge_snapshot_json(const World& w, bool paused, std::uint64_t snapshot_seq,
    double snapshot_real_time_s, bool with_paths = true);
[[nodiscard]] std::string build_bridge_paths_json(const World& w);
// Ship designs: every class with its parts (ship_components.csv), for the ship inspector.
// Static today, so the bridge writes it once as <snapshot>.designs; with refittable
// modules (tier 2) it changes when a ship's fit does, keyed by bridge_designs_signature().
[[nodiscard]] std::string build_bridge_designs_json(const World& w);
[[nodiscard]] std::string bridge_paths_signature(const World& w);

}  // namespace spacetrains::simulation::systems
