# Plan: the simulation as a GDExtension (Architecture 3, conditional)

Written 2026-11-16 from the roadmap's Architecture item 3 (outside review 2026-11-12). **Not to be built unless its
trigger is met.**

- **Trigger:** the user decides that the player gives commands (the parked "thin player stake": buy, sell, re-home a
  ship, set a reserve). An observer does not need it: the file bridge works, and its costs are known and paid.
- **Prerequisite:** Architecture 2 (`main_gd_split.md`). Its `BridgeClient.gd` is where a second transport plugs in.
  Architecture 1 is not required but makes a command handler easier to place.
- A first version of the player stake can use the command file (see "Without the extension" below); the extension is
  for when commands need replies and state on demand.

## Where it stands (main at PR #39)

- **Transport.** `spacetrains_bridge` (`src/bridge/main.cpp`, 562 lines) runs the simulation in its own process.
  Godot starts it with `OS.create_process` (`Main.gd:507-534`) and both sides talk through files in `user://`:
  - the UI writes a command file (aside, then rename; on failure it retries next frame, `Main.gd:575-592`);
  - the bridge writes the snapshot, `.paths`, `.designs`, `.stats` and `.porkchop`, each with a `.seq` file, by
    write-aside-and-rename with up to 5 retries 2 ms apart on Windows (`main.cpp:83-103`); the snapshot at most every
    100 ms, the others only when their signature changes (`main.cpp:352-398`);
  - the UI polls the `.seq` files every frame and parses the JSON in GDScript.
  - `ParentWatch` (`main.cpp:114-156`) ends the bridge when Godot dies (`PR_SET_PDEATHSIG` on Linux, a parent-pid
    check elsewhere, a process handle on Windows); the UI kills it and deletes the session files in `_exit_tree`.
- **Commands today:** `paused`, `timewarp_factor`, `stats_open`, `porkchop` (pairs), and `request` with `save_path`
  or `load_path` (`main.cpp:421-472`), parsed by hand-written string search. Replies exist only as a status string in
  the snapshot (`Loaded day X`, `Load failed: …`) and an epoch counter. **No gameplay commands.**
- **Size:** a snapshot was ~600 KB at 90 ships before paths moved to their own file (they were 70%), parsed up to 10
  times a second.
- **Packaging:** `tools/package/build_release.sh` builds a **double-precision** Godot 4.3 export and a static
  `spacetrains_bridge[.exe]` (MinGW for Windows, `cmake/mingw-w64-x86_64.cmake`); see `modules/deployment.md`.
- **No GDExtension exists** in this repo. A working one (Rust, godot-rust) is in
  `~/Projects/TheLongBurn/godot/tlb.gdextension`, useful for the export and packaging steps.

## Design

### Library

- **godot-cpp**, not godot-rust: the core is C++20 and links directly. Pin godot-cpp to the 4.3 branch and build it
  with `precision=double` (it must match the engine; a single-precision extension in a double engine does not load
  or corrupts vectors).
- Built by CMake as a shared library `spacetrains_gdext` (`.so` / `.dll`) that links the static `spacetrains_core`;
  godot-cpp added with `FetchContent` at a pinned commit. MinGW cross-build for Windows as for the bridge.
- `godot/spacetrains.gdextension` lists the libraries per platform; the export copies them next to the executable.

### `SimHost` (the one class Godot sees)

```
class SimHost : public godot::Node
  start(data_root, atlas_path)        builds Simulation on a worker thread (from_data_root, opening cache as today)
  stop()                              joins the thread; also on NOTIFICATION_PREDELETE
  poll() -> void                      main thread, once a frame: emits signals for whatever is new
  set_paused(bool), set_timewarp(float), set_stats_open(bool), request_porkchops(PackedStringArray)
  send_command(Dictionary) -> int     returns a command id
  signals: snapshot_ready(Dictionary), paths_ready(Dictionary), designs_ready(Dictionary),
           stats_ready(Dictionary), porkchops_ready(Dictionary), command_done(id, ok, message), status_changed(text)
```

- **Threading.** The simulation thread runs the same loop as `bridge/main.cpp` (whole ticks, at most 100 ms of work
  per loop, owed time capped at 4 s). After a batch of ticks it builds an immutable C++ snapshot (the same data as
  `build_bridge_snapshot_json`) and swaps it into a mutex-guarded slot. `poll()` on the main thread takes the latest
  slot and converts it to Godot types; no Godot object is touched from the simulation thread.
- **Change detection** stays as today: paths, designs and stats are produced only when their signatures change;
  porkchops come from the existing `PorkchopService` thread.
- **Commands** go into a queue with ids and are applied by the simulation thread **at the start of a tick**, never
  mid-tick, so a game stays deterministic given its commands. Each applied command is written to a command log in
  the state (game time, command) and saved, so a save, load and continue still equals never stopping. The reply
  (`command_done`) carries ok/refused and a message (e.g. "not enough credit").
- **Saves and loads** become commands like any other, replied to with `command_done` instead of the status string
  and epoch.

### On the Godot side

- One interface, two transports: `FileTransport.gd` (today's `BridgeClient.gd`) and `ExtensionTransport.gd`
  (wraps `SimHost`). Both emit the same signals, so `Main.gd` does not know which runs.
- The extension is the default once it ships; `--bridge=file` keeps the file bridge for debugging and for headless
  comparison. `spacetrains_bridge` stays in the build.

### Without the extension

Commands with replies can also go over the file bridge: an `id` per command in the command file, and a
`replies: [{id, ok, message}]` block in the next snapshot. That is enough for buy/sell/re-home (a reply within 100
ms). Use it for a first player-stake prototype; build the extension when the stake proves worth keeping or the
parsing cost shows in the frame time.

## Steps

1. **Spike.** An empty extension (one `SimHost` that returns a version string) loads in the editor and in the
   exported game on Linux and on Windows 11, double precision. Packaging script updated. Stop here if the export or
   the MinGW build fights back; record why.
2. **JSON passthrough.** `SimHost` runs the simulation and hands over the same JSON strings the bridge writes, with
   no files. `ExtensionTransport.gd` parses them as today. Proves threading, shutdown and packaging; the parsing cost
   is unchanged.
3. **Commands with replies**, starting with the existing ones (pause, timewarp, save, load).
4. **Godot types instead of JSON** for the snapshot: Dictionaries built in C++, and packed arrays for positions and
   path samples. Measure the frame time before and after.
5. **The first player command** (from the player-stake design, a separate plan).

## Checks

- The same shot tours through both transports (`--bridge=file` and the default) give the same scenes and
  `[ShotTour]` numbers.
- Save in one transport, load in the other.
- Determinism: a headless run and an extension run with the same commands give the same save at day 365.
- Frame time spent on the snapshot (`_profiled` in `Main.gd`) before and after step 4.
- Quitting the game, killing it, and loading a bad save leave no thread or process behind (Linux and Windows).
- `ctest` unchanged; the bridge keeps its own coverage.

## Risks

- **Crashes take Godot down.** Today a simulation crash kills only the bridge and the UI reports it. With the
  extension the whole game goes. Keep sanitizer runs of the headless app and keep `--bridge=file` available.
- **Double precision** must match across godot-cpp, the engine and the export templates; a mismatch fails at load.
- **Godot upgrades** need a godot-cpp rebuild at the matching version.
- **Windows:** the DLL must be static-runtime like the bridge (no MinGW DLLs next to it).
