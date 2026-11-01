# deployment

## Purpose

Release packages of SpaceTrains for Linux and Windows 11 that run without compiling anything: an archive to unpack
and start. Everything is built on a Linux machine; Windows binaries are cross-compiled with MinGW-w64
(user's decision, 2026-11-01). Installers (AppImage, Inno Setup), CI and code signing are later steps.

## Package layout

```
SpaceTrains-<version>-<linux|windows>-x86_64/
  SpaceTrains.x86_64 | SpaceTrains.exe     the exported Godot game (double precision)
  SpaceTrains.pck                          its scripts, scenes and textures
  bin/spacetrains_bridge[.exe]             the simulation (C++ runtime linked in; no DLLs on Windows)
  data/**                                  the CSV data (no Python helpers)
  atlas/variable_isp_atlas.bin             the VariableISP atlas (34 MB), outside data/ so the
                                           opening-cache key does not read it
  README.txt                               running, keys, where the user's files are
```

The folder is only read. Saves, caches and logs go to Godot's `user://`
(Windows `%APPDATA%\Godot\app_userdata\SpaceTrains`, Linux `~/.local/share/godot/app_userdata/SpaceTrains`):
`saves/`, `opening_cache/`, `porkchop_cache/`, the session's snapshot and command files, `logs/godot.log` and
`logs/bridge.log`.

## How the game finds its parts

`Main.gd` (`_resolve_install_paths`): an exported game (`OS.has_feature("template")`) looks next to its executable
for `bin/`, `data/` and `atlas/`; run from the editor binary (`godot4 --path godot`) it uses the source checkout:
`build/bin`, `data/`, `tests/data/variable_isp/`. On Windows the bridge name gets `.exe`. The UI passes the bridge
`--atlas` and `--log-file user://logs/bridge.log`; a bridge that stops is reported with the log's path. Session
files over a day old (left by a crash) are removed at start.

## Windows specifics

- Godot starts the bridge with `CREATE_NO_WINDOW`, so the console program shows no window.
- The bridge carries a manifest (`src/bridge/windows/`) that sets the process code page to UTF-8 (Windows 10 1903
  and later): arguments, `fopen` and `std::filesystem` paths then handle a user folder with a non-ASCII name.
- The file transport renames over files the other side may have open, which Windows can refuse: the bridge retries
  its renames (`write_text_file`), and the UI writes its command file again on the next frame.
- The bridge exits when the UI is gone: it waits on the UI's process handle (`--parent-pid`).
- The executables are not signed: SmartScreen warns on first start ("More info", "Run anyway").

## Building

`tools/package/build_release.sh [linux] [windows]` (default both) writes `dist/SpaceTrains-<git describe>-...`
(`.tar.gz` for Linux, `.zip` for Windows). Steps:

1. Godot export templates, double precision, built once from `$GODOT_SRC` (default `~/src/godot-4.3-stable`, the
   tree the editor was built from) with `scons target=template_release precision=double production=yes`
   (Windows: `use_mingw=yes`), copied to `build-release/templates/`. `godot/export_presets.cfg` (committed) points
   its "Linux" and "Windows Desktop" presets there, relative to `godot/`; it leaves out `godot/data/*`.
2. The bridge in `build-release/<platform>/`, Release, `-DSPACETRAINS_STATIC_RUNTIME=ON` (Linux: static libstdc++
   and libgcc; MinGW: `-static`). Windows uses `cmake/mingw-w64-x86_64.cmake` (the posix-thread MinGW compilers;
   `wine` as the emulator, so the cross-compiled tests run with `ctest`).
3. `godot4 --headless --export-release <preset>` into the package folder, then the bridge, the CSVs, the atlas and
   `README.txt` (`tools/package/README.txt`, CRLF on Windows), and the archive.

Needs: `cmake`, `ninja`, `scons`, the double-precision Godot editor (`$GODOT4_BINARY`, default
`~/.local/bin/godot4`), and for Windows `mingw-w64` and `zip`.

## Limits

- Linux: built on Ubuntu 24.04, so it needs glibc 2.39 or newer and a Vulkan driver.
- x86_64 only.
