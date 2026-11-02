#!/usr/bin/env bash
# Builds the release packages of SpaceTrains on Linux (docs/modules/deployment.md):
#   dist/SpaceTrains-<version>-linux-x86_64.tar.gz
#   dist/SpaceTrains-<version>-windows-x86_64.zip   (cross-compiled with MinGW-w64)
#
# Usage: tools/package/build_release.sh [linux] [windows]     (default: both)
# Environment:
#   GODOT_SRC        Godot 4.3 source tree for the export templates (default ~/src/godot-4.3-stable)
#   GODOT4_BINARY    the double-precision Godot editor (default ~/.local/bin/godot4)
#   JOBS             parallel build jobs (default: all cores)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GODOT_SRC="${GODOT_SRC:-$HOME/src/godot-4.3-stable}"
GODOT="${GODOT4_BINARY:-$HOME/.local/bin/godot4}"
JOBS="${JOBS:-$(nproc)}"
RELEASE="$REPO/build-release"
DIST="$REPO/dist"

platforms=("$@")
if [ ${#platforms[@]} -eq 0 ]; then
    platforms=(linux windows)
fi

die() { echo "build_release: $*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "$1 not found. $2"; }

need cmake "Install cmake."
need ninja "Install ninja-build."
need scons "Install scons (Godot's build tool)."
[ -x "$GODOT" ] || die "Godot editor not found at $GODOT (set GODOT4_BINARY)."
[ -d "$GODOT_SRC" ] || die "Godot source not found at $GODOT_SRC (set GODOT_SRC)."
for p in "${platforms[@]}"; do
    case "$p" in
        linux) ;;
        windows)
            need x86_64-w64-mingw32-g++-posix "Install MinGW-w64: sudo apt install mingw-w64"
            need zip "Install zip." ;;
        *) die "unknown platform '$p' (linux, windows)" ;;
    esac
done

# <date of the commit>-<short hash>[-dirty]; the repo's tags are milestones, not versions.
VERSION="$(git -C "$REPO" log -1 --format=%cd --date=format:%Y%m%d)-$(git -C "$REPO" rev-parse --short HEAD)"
if [ -n "$(git -C "$REPO" status --porcelain --untracked-files=no)" ]; then
    VERSION="$VERSION-dirty"
fi
mkdir -p "$RELEASE/templates" "$DIST"

# Godot export templates in double precision, built once (each takes a while).
build_template() {
    local platform="$1" file="$2"; shift 2
    if [ ! -f "$GODOT_SRC/bin/$file" ]; then
        echo "== Godot export template $file (first build only, this takes a while)"
        (cd "$GODOT_SRC" && scons -j"$JOBS" platform="$platform" target=template_release \
            precision=double production=yes "$@")
    fi
    cp -u "$GODOT_SRC/bin/$file" "$RELEASE/templates/$file"
}

# Godot accepts only the posix-thread MinGW under the plain tool names. Instead of switching the
# system's alternatives, a private prefix links those names to the -posix compilers.
posix_mingw_prefix() {
    local prefix="$RELEASE/mingw-posix"
    mkdir -p "$prefix/bin"
    for tool in /usr/bin/x86_64-w64-mingw32-*; do
        ln -sf "$tool" "$prefix/bin/$(basename "$tool")"
    done
    for tool in gcc g++ c++ cpp gcc-ar gcc-nm gcc-ranlib; do
        if [ -x "/usr/bin/x86_64-w64-mingw32-$tool-posix" ]; then
            ln -sf "/usr/bin/x86_64-w64-mingw32-$tool-posix" "$prefix/bin/x86_64-w64-mingw32-$tool"
        fi
    done
    echo "$prefix"
}

build_bridge() {
    local platform="$1"; shift
    echo "== Bridge ($platform)"
    cmake -S "$REPO" -B "$RELEASE/$platform" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DSPACETRAINS_STATIC_RUNTIME=ON "$@" >/dev/null
    cmake --build "$RELEASE/$platform" --target spacetrains_bridge -j"$JOBS"
}

# The package folder: the exported game, bin/, data/ (CSVs only), atlas/, README.txt and CREDITS.txt.
assemble() {
    local platform="$1" game_file="$2" bridge_file="$3" preset="$4"
    local name="SpaceTrains-$VERSION-$platform-x86_64"
    local out="$DIST/$name"
    rm -rf "$out"
    mkdir -p "$out/bin" "$out/atlas"
    echo "== Godot export ($preset)" >&2
    (cd "$REPO/godot" && "$GODOT" --headless --path . --export-release "$preset" "$out/$game_file") >&2
    [ -f "$out/$game_file" ] && [ -f "$out/SpaceTrains.pck" ] || die "Godot export ($preset) failed."
    cp "$RELEASE/$platform/bin/$bridge_file" "$out/bin/"
    (cd "$REPO" && find data -name '*.csv' -print0 | xargs -0 -I{} install -D -m 644 {} "$out/{}")
    cp "$REPO/tests/data/variable_isp/variable_isp_atlas.bin" "$out/atlas/"
    sed "s/@VERSION@/$VERSION/" "$REPO/tools/package/README.txt" > "$out/README.txt"
    cp "$REPO/godot/assets/CREDITS.txt" "$out/CREDITS.txt"
    echo "$out"
}

# The credits must match the music manifest.
python3 "$REPO/tools/package/make_credits.py" --check || die "CREDITS.txt is out of date."

for p in "${platforms[@]}"; do
    case "$p" in
        linux)
            build_template linuxbsd godot.linuxbsd.template_release.double.x86_64
            build_bridge linux
            out="$(assemble linux SpaceTrains.x86_64 spacetrains_bridge Linux | tail -1)"
            (cd "$DIST" && tar czf "$(basename "$out").tar.gz" "$(basename "$out")")
            echo "== $out.tar.gz" ;;
        windows)
            build_template windows godot.windows.template_release.double.x86_64.exe use_mingw=yes \
                mingw_prefix="$(posix_mingw_prefix)"
            build_bridge windows --toolchain "$REPO/cmake/mingw-w64-x86_64.cmake"
            out="$(assemble windows SpaceTrains.exe spacetrains_bridge.exe "Windows Desktop" | tail -1)"
            (cd "$DIST" && sed -i 's/$/\r/' "$out/README.txt" "$out/CREDITS.txt" && rm -f "$(basename "$out").zip" \
                && zip -qr "$(basename "$out").zip" "$(basename "$out")")
            echo "== $out.zip" ;;
    esac
done
