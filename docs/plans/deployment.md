# Deployment plan (Linux and Windows 11)

Roadmap step 2. Planned 2026-11-01 on `feature/deployment`; not built yet.

**Goal.** One way to install and run SpaceTrains on Linux and Windows 11 without compiling anything.

**Decisions (user, 2026-11-01).**
- Build everything on the Linux PC; Windows binaries are cross-compiled with MinGW. The user's Win11 laptop is
  for testing only. CI can come later.
- First deliver archives (Linux tar.gz, Windows zip: unpack and run); AppImage and an Inno Setup installer later.

**Starting point.** The game only runs from the git checkout: `godot/scripts/Main.gd` builds the bridge path
(`build/bin/spacetrains_bridge`, no `.exe`) and the data root from the repo; the atlas path is hard-coded as
`data_root/../tests/data/variable_isp/variable_isp_atlas.bin` (`src/simulation/Simulation.cpp`); the bridge
logs to stderr, which is lost without a terminal; there are no export presets, export templates or packaging
scripts. The C++ code is nearly portable: no external dependencies, warning flags only for GCC/Clang, `<format>`
needs GCC 13 (Ubuntu 24.04's MinGW is 13), POSIX calls already behind `#ifdef`s (bridge parent watch, porkchop
thread priority).



## Package layout (same on both platforms)
```
SpaceTrains-<version>-<linux|windows>/
  SpaceTrains.x86_64 | SpaceTrains.exe     exported Godot game (double precision)
  SpaceTrains.pck
  bin/spacetrains_bridge[.exe]             statically linked runtime (libstdc++/libgcc; fully static on Windows)
  data/**                                  only the CSVs (no build_ship_classes.py, no __pycache__)
  atlas/variable_isp_atlas.bin             34 MB, kept out of data/ so the opening-cache hash doesn't read it
  README.txt                               how to run, where saves and logs are
```
Writable files stay in Godot's `user://` (Linux `~/.local/share/godot/app_userdata/SpaceTrains`, Windows
`%APPDATA%\Godot\app_userdata\SpaceTrains`), so the folder can be read-only (e.g. `Program Files` later).

## Code changes
1. **Godot path resolution** (`Main.gd:166-171`): when `OS.has_feature("editor")` or running from the source
   tree, keep today's repo layout; in an exported game use `OS.get_executable_path().get_base_dir()` with
   `bin/`, `data/`, `atlas/`. Add `.exe` when `OS.get_name() == "Windows"`. Pass the new `--atlas` and
   `--log-file` flags. Show a clear error on screen when the bridge or data are missing.
2. **Bridge `--atlas <path>`** (`src/bridge/main.cpp` flag parsing ~:45; through to `Simulation.cpp:164` and
   the opening-cache fingerprint `main.cpp:176`). Default stays the repo path, so headless, tests and tools are
   unchanged.
3. **Bridge `--log-file <path>`**: send the log (`SPACETRAINS_BRIDGE_LOG` output, warnings, errors) to
   `user://logs/bridge.log` (overwritten each start). Godot's "see the terminal" message (`Main.gd:472`) points to that file.
4. **Windows robustness:**
   - rename-over-open: retry `std::filesystem::rename` a few times in `write_text_file` (`main.cpp:76-89`); check
     and retry the rename on the Godot side (`Main.gd:524`);
   - non-ASCII user folders: embed an application manifest with `activeCodePage UTF-8` in the Windows bridge (MinGW
     `windres`), so `std::string` paths are UTF-8;
   - `parse_string_flag` (`main.cpp:201-210`): unescape `\\` and `\"`;
   - make sure no console window flashes up for the bridge (check `create_process(..., false)`; if one does,
     link the bridge as a GUI subsystem app `-mwindows`, since it only logs to the file);
   - startup clean-up of stale `spacetrains_snapshot_*`/`commands_*` files older than a day in `user://`.
   - Anything else the MinGW compile turns up (fixed as found; warning flags already only apply to GNU/Clang).
5. **Godot project**: a committed `godot/export_presets.cfg` with "Linux" and "Windows Desktop" presets that point
   `custom_template/release` at our double-precision templates (remove it from `.gitignore`); exclude
   `godot/data/*.csv` and the debug scenes (DebugShot, ModelGallery) from the export; `application/config/name`
   = SpaceTrains, an icon.

## Build script `tools/package/build_release.sh` (+ `cmake/mingw-w64-x86_64.cmake` toolchain file)
1. Godot export templates (only if missing), in `~/src/godot-4.3-stable`:
   `scons platform=linuxbsd target=template_release precision=double production=yes` and
   `scons platform=windows use_mingw=yes target=template_release precision=double production=yes`.
   Needs `sudo apt install mingw-w64` once (the user runs it; it isn't installed now). First build ~20-40 min each on 16 cores.
2. Bridge: `cmake -B build-release/linux` and `cmake -B build-release/windows --toolchain cmake/mingw-w64-x86_64.cmake`,
   Release, target `spacetrains_bridge` (+ the test executables for the Windows smoke test).
3. Export: `godot4 --headless --path godot --export-release "Linux" …` and `"Windows Desktop"`.
4. Assemble the layout above and pack `dist/SpaceTrains-<git describe>-linux-x86_64.tar.gz` and `…-windows-x86_64.zip`.
   `dist/` and `build-release/` go in `.gitignore`.

## Docs
`docs/modules/deployment.md` (layout, build script, toolchain needs, where user files live, known limits:
Linux binary needs glibc ≥ 2.39, i.e. Ubuntu 24.04 era or newer), a short README section, the roadmap step marked
done with what's left (AppImage, Inno Setup installer, CI, maybe code signing so Windows SmartScreen doesn't warn), a memory note.

## Verification
- `ctest` and the 365-day economy audit stay byte-identical (path handling must not change results).
- Windows on Linux: the cross-compiled test suites run under `wine`; the Windows bridge runs under wine with
  `--data-root/--atlas/--log-file` for a few seconds and writes snapshots.
- Linux package: unpack to a temp folder outside the repo, start it (opens a game window, I warn first and use a
  short timeout), check the bridge log, ships move, F5/F9 save, quit, and `pgrep -x spacetrains_brid` finds nothing.
- Win11 laptop (the user): unzip, start, watch a few days pass, F5/F9, quit, check Task Manager has no
  `spacetrains_bridge.exe` left; also try a user folder with a non-ASCII name if possible. Send me
  `%APPDATA%\Godot\app_userdata\SpaceTrains\logs\` if anything fails.
- Commit on `feature/deployment`; no push or PR until asked.
