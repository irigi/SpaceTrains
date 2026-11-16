# Plan: split `godot/scripts/Main.gd` (Architecture 2)

Written 2026-11-16 from the roadmap's Architecture item 2 (outside review 2026-11-12). Not started.

**Display-only refactor: what is on screen must not change**, and the simulation is not touched (the 365-day
economy audit stays byte-identical by construction).

## Where it stands (main at PR #39)

`Main.gd` (2,631 lines, `extends Node3D`) does five jobs besides scene set-up, input and selection. Line ranges
below are from `main` at PR #39.

| Target | Functions (line) |
|---|---|
| **BridgeClient** | `_resolve_install_paths` 480, `_remove_stale_session_files` 495, `_start_bridge` 507, `_update_bridge_status` 536, `_quicksave_path` 557, `_request_save_or_load` 561, `_show_notice` 568, `_write_bridge_commands` 575, `_read_paths` 599, `_read_designs` 615, `_update_porkchop_request` 628, `_read_porkchops` 640, `_read_stats` 656, `_attach_paths` 700, `_read_snapshot` 715 |
| **PathMotion** | `_cache_orbits` 1064, `_cache_ship_path` 1083, `_cache_helio_polar` 1137, `_sample_rates` 1165, `_offset_rates` 1187, `_planet_of` 1224, `_path_sense` 1236, `_rail_anchor_m` 1256, `_frame_now_m` 1280, `_frame_at_m` 1283, `_drawn_point_m` 1287, `_plus_m`/`_minus_m` 1295-1298, `_rail_point_m` 1302, `_station_blend_m` 1332, `_station_visual_offset_m` 1351, `_helio_point_m` 1363, `_hermite` 1372, `_body_at_m` 1379, `_body_m` 1393, `_station_m` 1410, `_ship_m` 1440, `_path_sample` 1452, `_ship_heading` 1464, `_entity_m` 1470, `_to_render` 1481, `_abs_render` 1484, `_trajectory_path_signature` 2435 |
| **DisplayClock** | `_advance_display_clock` 1026, `_advance_visual_phases` 1427, `_wall_time_s` 2343; the spin clock (inline in `_update_nodes` 887-893); the `sim_pace` update (inline in `_read_snapshot` 763-770) |
| **Berths** | `_place_docked_ship` 1490, `_berth_pose` 1500, `_ease_into_berth` 1519, `_rebuild_docked_slots` 1557, `_free_slot` 1596 (state at 1553-1555) |
| **FineLines** | `_update_fine_rings` 1614, `_trail_material` 1711, `_fine_path_material` 1720, `_update_path_frames` 1728, `_update_path_clocks` 1745, `_update_fine_paths` 1758, `_fine_path_current` 1810, `_box_gap` 1824, `_build_fine_path` 1832, `_segment_distance` 1895, `_sinh_stretch` 1909, `_ensure_fine_ring` 1922, `_update_history_trails` 1951, `_ensure_history_trail_node` 1980, `_rebuild_history_trail` 1994, `_ensure_trail_node` 2380, `_update_ship_trails` 2397, `_trajectory_color` 2442, `_rebuild_trail_mesh` 2445, `_rail_line_m` 2492, `_update_burn_markers` 2522, and the destination ghost (`_make_destination_ghost_material` 2095, `_ensure_destination_body_ghost` 2554, `_update_selected_overlay_positions` 2569, `_destination_ghost_position` 2578, `_update_destination_body_ghost` 2587) |
| **Stays in Main** | `_ready`, `_start_music`, `_setup_ui`, signal handlers, `select_entity`, `_process`, `_unhandled_input`, `_update_flow_overlay`, `_apply_snapshot`, `_upsert_entity`, `_update_nodes`, eclipses, camera obstacles, `_orient_ship`, scale and LOD (2007-2112), `_refresh_ui*`, `_pick_entity`, debug guides, lighting, `_step_timewarp`, focus, `_reset_timeline`, `_exit_tree`, `_jet_power_w` |

Per-frame order (`_process` 390-416), to be kept:
1. `_read_snapshot`: paths, designs, stats, porkchops, then the snapshot → `_apply_snapshot` (orbits and paths,
   entities, `_rebuild_docked_slots`, `_update_ship_trails`, `_refresh_ui`);
2. retry a pending command write; 3. `_update_bridge_status`; 4. `_advance_display_clock`;
5. `_update_nodes` (origin → spin clock → visual phases → body and station positions → entities → docked ships →
   eclipses → berths → LOD and glow → obstacles → history trails → rings → fine rings → path frames → fine paths →
   path clocks → light → environment → overlay positions);
6. `_update_camera_focus`; 7. `map_icons.update`; 8. `_update_map_debug`.

Ties that make the split less than clean:
- `_read_snapshot` also moves the display clock, resets the timeline on a new epoch and calls `_apply_snapshot`;
  `_read_stats`/`_read_porkchops` push straight into `stats_panel`, and `_update_porkchop_request` reads the
  panel's private fields with `stats_panel.get("_tab")` etc.
- `_apply_snapshot` erases entries of the trail, fine-path and history dictionaries directly (803-815).
- `_reset_timeline` clears the clock, `ship_paths`, `economy_history` and the history trails together.
- The entity dictionaries (`entity_nodes`, `entity_details`, `entity_kinds`, `entity_targets`,
  `entity_visual_signatures`, `faction_colors`) are shared with MapIcons by reference (`map_icons.setup`, 206): they
  may be mutated but never reassigned.
- **ShotTour.gd** (931 lines, a child of Main, `main = get_parent()`) uses about 45 of Main's members, including
  private functions of four target groups (`_write_bridge_commands`, `_request_save_or_load`,
  `_update_porkchop_request`, `_wall_time_s`, `_rail_point_m`, `_body_at_m`, `_update_fine_paths`), the
  `command_*` vars, and 28 `main.get("…")` lookups that return null without an error once a name moves.
  **DebugShot.gd** reaches in through `get_tree().current_scene`. No other script reaches back into Main.

## Target

Helpers are preloaded like `EconomyHistory.gd` (`const X := preload(...)`, no `class_name`, no autoloads), each
owning its own state:

| File | Kind | Owns | Interface |
|---|---|---|---|
| `DisplayClock.gd` | RefCounted | `display_time_s`, `snapshot_game_time_s`, `snapshot_wall_s`, `sim_pace`, `spin_clock_s`, `visual_phase`, `wall_time_s()` | `on_snapshot(game_time_s, wall_s, paused, timewarp)`, `advance(delta, paused, timewarp)`, `advance_spin(...)`, `advance_visual_phases(...)`, `reset()` |
| `BridgeClient.gd` | Node (signals, `_exit_tree`) | process and paths, command state (`command_request`, save/load paths, pending write), all `*_seq`, `bridge_state`, `bridge_paths`, `bridge_designs`, `porkchop_data`, status text | `start()`, `poll()` once a frame; `set_paused`, `set_timewarp`, `set_stats_open`, `request_porkchops(pairs)`, `request_save(path)`, `request_load(path)`; signals `snapshot_received(state, seq)`, `paths_received`, `designs_received`, `stats_received(json)`, `porkchops_received(data)`, `status_changed(text)`, `epoch_changed` |
| `PathMotion.gd` | RefCounted | `body_orbits`, `station_orbits`, `ship_paths`, `frame_positions_m`, `origin_m`; holds the clock | `cache_orbits(state)`, `cache_ship_path(...)`, `forget_ship(id)`, `begin_frame(origin)`, `body_m`, `station_m`, `ship_m`, `entity_m`, `ship_heading`, `rail_point_m`, `body_at_m`, `to_render`, `planet_of`, `reset()` |
| `Berths.gd` | RefCounted | `_docked_slots`, `_docked_station`, `_last_berth` | `setup(entity_nodes, entity_details, scale)`, `rebuild(state)`, `place(ship_id, node)`, `ease(ship_id, node, delta)` |
| `FineLines.gd` | Node3D (creates meshes) | rings, fine paths, history and planned trails, burn markers, destination ghost, their materials and constants | `setup(world_root, camera, camera_rig, path_motion, clock)`, `on_snapshot(state)`, `forget_ship(id)`, `update(selected, body_positions, render_origin)`, `update_fine_paths()`, `reset()` |

`Main.gd` keeps scene set-up, input, selection and focus, the per-frame order and the glue: signal handlers that
call `clock.on_snapshot`, `stats_panel.update…` and `_apply_snapshot`. `_reset_timeline` calls `reset()` on each
helper. `_jet_power_w` stays in Main (it serves models and the inspector). The StatisticsPanel gets a small public
getter for what `_update_porkchop_request` and `_update_flow_overlay` read, instead of `get("_tab")`. Expected size:
`Main.gd` about 1,100 lines.

## Steps (one commit each)

0. **Make ShotTour fail loudly.** Replace the 28 `main.get("…")` lookups in `ShotTour.gd` with direct property
   access, and the `main.call("_body_display_scale")` with a direct call. Nothing else changes; the tours run as
   before. From here on a moved name is an error, not a silent null.
1. **DisplayClock** first: the bridge (snapshot timing), PathMotion (`visual_phase`) and FineLines (`display_time_s`)
   all read it. Move the spin-clock lines out of `_update_nodes` and the `sim_pace` lines out of `_read_snapshot`.
2. **BridgeClient.** `_read_snapshot` stops touching the clock and the panels; it emits, and Main's handlers do the
   rest in the same order as today (paths, designs, stats, porkchops, then the snapshot). `_exit_tree` moves with it.
3. **PathMotion.** `_apply_snapshot` calls `path_motion.cache_orbits` / `cache_ship_path`; `_update_nodes` calls
   `path_motion.begin_frame(origin)` where it clears `frame_positions_m` today.
4. **Berths.**
5. **FineLines**, with `forget_ship(id)` replacing the direct erases in `_apply_snapshot` (803-815).
6. **Docs:** the file table in `docs/modules/ui_observer.md`.

In every step, update `ShotTour.gd` and `DebugShot.gd` to the new paths (`main.clock.display_time_s`,
`main.bridge.request_load(...)`, `main.path_motion.rail_point_m(...)`). No forwarding wrappers are left in Main.

## Checks after every commit

- Parse check of every changed script:
  `godot4 --headless --path godot --check-only --script res://scripts/<file>.gd`.
- `ctest` (it runs `tests/test_key_bindings.py`, which scans the scripts for key constants).
- A fixed set of shot tours, run in a window (headless runs of the scene crash on worker-built meshes), once on the
  commit before step 0 as the reference and then after each step, into separate folders:
  `--shot-tour=<dir>` with `--shot-day`, `--shot-pause`, `--shot-close`, `--shot-stations`, `--shot-arrival`,
  `--shot-routes`, `--shot-flows`, `--shot-stats`, `--shot-inspector`, `--shot-saveload` (`ui_observer.md`, "Shot
  tours"). Compare the pictures side by side and the printed `[ShotTour]` numbers (stalled frames, pause spin clock,
  path-end distances to stations, fine-path counts and timings, picking checks). Pixel equality is not expected (the
  display runs on wall time); the same scene, the same lines and numbers within their usual spread are.
- A manual pass of a few minutes: select a ship and a station, focus and close zoom, a ship arriving at a berth,
  F5/F9, the Statistics screen with porkchops and the flows overlay, pause and timewarp keys.
- `--shot-fps`: the frame time is not worse.

## Risks

- **Signals change timing.** Godot signals are synchronous, so emitting from `poll()` keeps today's order within the
  frame; do not use `call_deferred` or `CONNECT_DEFERRED`.
- **Reassigned shared dictionaries** break MapIcons silently. Helpers receive them by reference and only mutate.
- **Typed GDScript.** Keep the existing static types on moved functions; the parse check catches mismatches only
  where types are declared.
- A display bug found during the split is fixed in its own commit after the split, not inside a move.
