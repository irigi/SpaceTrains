extends Node

# The simulation bridge's client (docs/plans/main_gd_split.md, step 2): starts the bridge process,
# writes its command file, and reads what it writes (the snapshot and its side files: paths,
# designs, statistics, porkchops). Main calls poll() once a frame and handles the signals, which
# are emitted synchronously, in the order the files are read (paths, designs, stats, porkchops,
# then the snapshot).

signal snapshot_received(state: Dictionary, seq: int)
# A snapshot older than the one shown (debug output only).
signal snapshot_rejected(seq: int, game_time_s: float, shown_game_time_s: float)
signal designs_received(designs: Dictionary)
signal stats_received(stats: Dictionary)
signal porkchops_received(data: Dictionary)
signal status_changed(text: String)
# A loaded game: a new timeline, possibly earlier (emitted before its first snapshot).
signal epoch_changed

const BRIDGE_STEP_SECONDS := 0.1
# Some simulation steps plan many trajectories at once (the opening dispatch, fleet reviews):
# say so when no snapshot has come for this long, rather than look frozen.
const BRIDGE_BUSY_NOTICE_S := 2.0
const DisplayClockScript := preload("res://scripts/DisplayClock.gd")

var clock: DisplayClockScript  # for wall_time_s()
# Whether the bridge should write the statistics side file (the command file's stats_open), and
# whether to read it now (both: the Statistics screen or the flow overlay is open; Main's tests).
var stats_wanted: Callable = func() -> bool: return false
var stats_readable: Callable = func() -> bool: return false

var bridge_pid := -1
var snapshot_path := ""
var command_path := ""
var repo_root := ""
var executable_path := ""
var data_root := ""
var atlas_path := ""
var bridge_log_path := ""
var bridge_started := false
# Why the bridge did not start (empty when it did): start() runs before Main's status label
# exists, so Main shows this once its UI is built.
var start_error := ""
# What the command file says: pause, timewarp, the save/load request, the porkchop pairs.
var paused := false
var timewarp := 86400.0
var command_request := 0
var command_save_path := ""
var command_load_path := ""
var porkchop_request := ""
# A command file whose rename failed (Windows: the bridge had it open): written again next frame.
var command_write_pending := false
# The last snapshot accepted, its sequence number and when it arrived (wall seconds).
var state: Dictionary = {}
var snapshot_seq := -1
var snapshot_arrival_s := 0.0
# The sequence number of the last snapshot parsed, accepted or not: a rejected (older) one is
# not parsed again every frame while its file stays.
var _parsed_seq := -1
var epoch := 0
var status_text := ""
var status_until_s := 0.0
# Planned paths by ship id ({path_id, trajectory_path, destination_body_at_arrival}), from
# the bridge's paths file, which changes only when a ship gets a new plan.
var paths: Dictionary = {}
var paths_seq := -1
var designs_seq := -1
var stats_seq := -1
var porkchop_seq := -1

# Profiling hook: Main's _profiled(name, started_usec), called around the snapshot parse.
var profiled: Callable

func start() -> void:
    _resolve_install_paths()
    _remove_stale_session_files()
    var session_id := "%d_%d" % [OS.get_process_id(), Time.get_ticks_usec()]
    snapshot_path = ProjectSettings.globalize_path("user://spacetrains_snapshot_%s.json" % session_id)
    command_path = ProjectSettings.globalize_path("user://spacetrains_commands_%s.json" % session_id)
    write_commands()
    _start_bridge()

func set_paused(value: bool) -> void:
    paused = value
    write_commands()

func set_timewarp(value: float) -> void:
    timewarp = value
    write_commands()

# The pairs to plot ("a>b;c>d"); written only when they change.
func request_porkchops(text: String) -> void:
    if text != porkchop_request:
        porkchop_request = text
        write_commands()

func request_save(path: String) -> void:
    command_request += 1
    command_save_path = path
    command_load_path = ""
    write_commands()
    _show_notice("Saving…")

func request_load(path: String) -> void:
    command_request += 1
    command_save_path = ""
    command_load_path = path
    write_commands()
    _show_notice("Loading…")

func _show_notice(text: String) -> void:
    status_text = text
    status_until_s = clock.wall_time_s() + 3.0
    status_changed.emit(text)

# Once a frame: the side files, then a new snapshot if there is one.
func poll() -> void:
    _read_paths()
    _read_designs()
    _read_stats()
    _read_porkchops()
    _read_snapshot()

# Once a frame, after poll().
func retry_pending_write() -> void:
    if command_write_pending:
        write_commands()

# Once a frame: the bridge's health and the busy notice.
func update_status() -> void:
    if not bridge_started:
        return
    if not OS.is_process_running(bridge_pid):
        bridge_started = false
        status_changed.emit("The simulation bridge stopped.\nSee its log: %s" % bridge_log_path)
        return
    if snapshot_seq < 0:
        return
    if status_text != "" and clock.wall_time_s() > status_until_s:
        status_text = ""
        status_changed.emit("")
    # The bridge writes a snapshot per tick (0.1 day): while paused, or at a slow timewarp,
    # none is due for a while.
    if paused:
        return
    var tick_interval_s: float = DisplayClockScript.SIM_TICK_S / maxf(timewarp, 1.0)
    var waited_s := clock.wall_time_s() - snapshot_arrival_s
    if waited_s > maxf(BRIDGE_BUSY_NOTICE_S, 2.0 * tick_interval_s):
        status_changed.emit("Simulation busy: planning ship missions… %d s" % int(waited_s))

func _exit_tree() -> void:
    if bridge_pid > 0:
        OS.kill(bridge_pid)
    for path in [snapshot_path, snapshot_path + ".tmp", snapshot_path + ".seq", snapshot_path + ".seq.tmp",
            snapshot_path + ".paths", snapshot_path + ".paths.tmp", snapshot_path + ".paths.seq", snapshot_path + ".paths.seq.tmp",
            snapshot_path + ".designs", snapshot_path + ".designs.tmp", snapshot_path + ".designs.seq", snapshot_path + ".designs.seq.tmp",
            snapshot_path + ".stats", snapshot_path + ".stats.tmp", snapshot_path + ".stats.seq", snapshot_path + ".stats.seq.tmp",
            snapshot_path + ".porkchop", snapshot_path + ".porkchop.tmp", snapshot_path + ".porkchop.seq", snapshot_path + ".porkchop.seq.tmp",
            command_path, command_path + ".tmp"]:
        if FileAccess.file_exists(path):
            DirAccess.remove_absolute(path)

# Where the bridge, the data and the atlas are. From the editor (the source checkout): the repo
# layout, build/bin and tests/data. An exported game: next to its executable, in bin/, data/
# and atlas/ (docs/modules/deployment.md).
func _resolve_install_paths() -> void:
    var exe_suffix := ".exe" if OS.get_name() == "Windows" else ""
    if OS.has_feature("template"):
        repo_root = OS.get_executable_path().get_base_dir()
        executable_path = repo_root.path_join("bin/spacetrains_bridge" + exe_suffix)
        data_root = repo_root.path_join("data")
        atlas_path = repo_root.path_join("atlas/variable_isp_atlas.bin")
    else:
        repo_root = ProjectSettings.globalize_path("res://").get_base_dir().get_base_dir()
        executable_path = repo_root.path_join("build/bin/spacetrains_bridge" + exe_suffix)
        data_root = repo_root.path_join("data")
        atlas_path = repo_root.path_join("tests/data/variable_isp/variable_isp_atlas.bin")
    bridge_log_path = ProjectSettings.globalize_path("user://logs/bridge.log")

# Session files left behind by a crash (normally _exit_tree removes them): drop those over a day old.
func _remove_stale_session_files() -> void:
    var dir := DirAccess.open("user://")
    if dir == null:
        return
    var now := Time.get_unix_time_from_system()
    for file_name in dir.get_files():
        if not (file_name.begins_with("spacetrains_snapshot_") or file_name.begins_with("spacetrains_commands_")):
            continue
        var path := "user://" + file_name
        if now - FileAccess.get_modified_time(path) > 86400.0:
            DirAccess.remove_absolute(ProjectSettings.globalize_path(path))

func _start_bridge() -> void:
    if not FileAccess.file_exists(executable_path):
        var hint := "The installation is incomplete." if OS.has_feature("template") else "Build the project first."
        _start_failed("Simulation bridge not found:\n%s\n%s" % [executable_path, hint])
        return
    if not DirAccess.dir_exists_absolute(data_root):
        _start_failed("Game data not found:\n%s" % data_root)
        return

    var args := [
        "--data-root", data_root,
        "--atlas", atlas_path,
        "--log-file", bridge_log_path,
        "--snapshot-file", snapshot_path,
        "--command-file", command_path,
        "--step-seconds", str(BRIDGE_STEP_SECONDS),
        "--opening-cache", ProjectSettings.globalize_path("user://opening_cache"),
        # Porkchop columns computed in earlier sessions (every game starts on the same date).
        "--porkchop-cache", ProjectSettings.globalize_path("user://porkchop_cache"),
        # The bridge exits when this process is gone (no orphan if the UI crashes).
        "--parent-pid", str(OS.get_process_id())
    ]
    bridge_pid = OS.create_process(executable_path, args, false)
    if bridge_pid <= 0:
        _start_failed("Failed to start bridge process.")
        bridge_started = false
    else:
        bridge_started = true

func _start_failed(text: String) -> void:
    start_error = text
    status_changed.emit(text)

# Written aside and renamed, so the bridge never reads a half-written command (a save or
# load request read half-written was lost).
func write_commands() -> void:
    var temp_path := command_path + ".tmp"
    var file := FileAccess.open(temp_path, FileAccess.WRITE)
    if file == null:
        command_write_pending = true
        return
    var payload := {
        "paused": paused,
        "timewarp_factor": timewarp,
        "request": command_request,
        "save_path": command_save_path,
        "load_path": command_load_path,
        "stats_open": bool(stats_wanted.call()),
        "porkchop": porkchop_request,
    }
    file.store_string(JSON.stringify(payload))
    file.close()
    command_write_pending = DirAccess.rename_absolute(temp_path, command_path) != OK

func _read_paths() -> void:
    var seq_text := FileAccess.get_file_as_string(snapshot_path + ".paths.seq")
    if seq_text == "" or int(seq_text) == paths_seq:
        return
    var json := JSON.new()
    if json.parse(FileAccess.get_file_as_string(snapshot_path + ".paths")) != OK or typeof(json.data) != TYPE_DICTIONARY:
        return
    paths_seq = int(seq_text)
    paths = json.data.get("paths", {})

# Ship designs by class id ({name, ..., components: [...]}): the parts of every class for the
# inspector's ship systems section. Written once by the bridge (per-ship fits later).
func _read_designs() -> void:
    var seq_text := FileAccess.get_file_as_string(snapshot_path + ".designs.seq")
    if seq_text == "" or int(seq_text) == designs_seq:
        return
    var json := JSON.new()
    if json.parse(FileAccess.get_file_as_string(snapshot_path + ".designs")) != OK or typeof(json.data) != TYPE_DICTIONARY:
        return
    designs_seq = int(seq_text)
    designs_received.emit(json.data.get("designs", {}))

func _read_porkchops() -> void:
    if porkchop_request == "":
        return
    var seq_text := FileAccess.get_file_as_string(snapshot_path + ".porkchop.seq")
    if seq_text == "" or int(seq_text) == porkchop_seq:
        return
    var json := JSON.new()
    if json.parse(FileAccess.get_file_as_string(snapshot_path + ".porkchop")) != OK or typeof(json.data) != TYPE_DICTIONARY:
        return
    porkchop_seq = int(seq_text)
    porkchops_received.emit(json.data)

# The statistics side file (money journal), written by the bridge while the Statistics screen is open.
func _read_stats() -> void:
    if not bool(stats_readable.call()):
        return
    var seq_text := FileAccess.get_file_as_string(snapshot_path + ".stats.seq")
    if seq_text == "" or int(seq_text) == stats_seq:
        return
    var json := JSON.new()
    if json.parse(FileAccess.get_file_as_string(snapshot_path + ".stats")) != OK or typeof(json.data) != TYPE_DICTIONARY:
        return
    stats_seq = int(seq_text)
    stats_received.emit(json.data)

# Ships in flight carry only a path id; attach the planned path from the paths file.
# Returns how many ships name a path the paths file does not have.
func _attach_paths() -> int:
    var missing := 0
    for ship in state.get("ships", []):
        var path_id := String(ship.get("path_id", ""))
        if path_id == "":
            continue
        var cached: Dictionary = paths.get(String(ship.get("id", "")), {})
        if String(cached.get("path_id", "")) == path_id:
            ship["trajectory_path"] = cached.get("trajectory_path", [])
            if cached.has("destination_body_at_arrival"):
                ship["destination_body_at_arrival"] = cached["destination_body_at_arrival"]
        else:
            missing += 1
    return missing

func _read_snapshot() -> void:
    # The bridge bumps a small sequence file with every snapshot: parse only new ones.
    var seq_text := FileAccess.get_file_as_string(snapshot_path + ".seq")
    if seq_text != "" and (int(seq_text) == snapshot_seq or int(seq_text) == _parsed_seq):
        return
    if not FileAccess.file_exists(snapshot_path):
        return
    var file := FileAccess.open(snapshot_path, FileAccess.READ)
    if file == null:
        return
    var json := JSON.new()
    var parse_started := Time.get_ticks_usec()
    var parse_status := json.parse(file.get_as_text())
    if profiled.is_valid(): profiled.call("  snapshot parse", parse_started)
    if parse_status != OK or typeof(json.data) != TYPE_DICTIONARY:
        return
    var new_seq := int(json.data.get("snapshot_seq", -1))
    _parsed_seq = new_seq
    if new_seq <= snapshot_seq:
        return
    var new_game_time_s := float(json.data.get("game_time_s", 0.0))
    var current_game_time_s := float(state.get("game_time_s", -1.0))
    var bridge_info: Dictionary = json.data.get("bridge", {})
    var status := String(bridge_info.get("status", ""))
    if status != "" and status != String(state.get("bridge", {}).get("status", "")):
        _show_notice(status)
    var new_epoch := int(bridge_info.get("epoch", 0))
    if new_epoch != epoch:
        # A loaded game: a new timeline, possibly earlier. Forget the old one.
        epoch = new_epoch
        epoch_changed.emit()
        current_game_time_s = -1.0
    if current_game_time_s >= 0.0 and new_game_time_s + 0.001 < current_game_time_s:
        snapshot_rejected.emit(new_seq, new_game_time_s, current_game_time_s)
        return
    var arrival_now_s := clock.wall_time_s()
    snapshot_seq = new_seq
    snapshot_arrival_s = arrival_now_s
    state = json.data
    # The bridge writes the paths a snapshot names before the snapshot: one read in between
    # (the paths file read first, the new snapshot after) has an older paths file, read again.
    if _attach_paths() > 0:
        _read_paths()
        _attach_paths()
    snapshot_received.emit(state, new_seq)
