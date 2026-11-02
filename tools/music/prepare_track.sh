#!/usr/bin/env bash
# Convert one downloaded recording into a game track and print its manifest gain.
#   tools/music/prepare_track.sh <input file> <output .ogg>
# Output: OGG Vorbis quality 4 (~110-125 kbps), 44.1 kHz stereo, metadata stripped,
# silence trimmed at both ends. Gain aims at -20 LUFS without lifting a true peak above -1 dBTP;
# put it in godot/assets/music/manifest.csv (gain_db), the file itself is not changed.
set -euo pipefail
in=$1 out=$2
trim="silenceremove=start_periods=1:start_threshold=-60dB:start_silence=0.3,areverse,"
trim+="silenceremove=start_periods=1:start_threshold=-60dB:start_silence=1.0,areverse"
ffmpeg -nostdin -v error -y -i "$in" -map 0:a:0 -map_metadata -1 -af "$trim" \
    -ar 44100 -ac 2 -c:a libvorbis -q:a 4 "$out"
ffmpeg -nostdin -hide_banner -i "$out" -af ebur128=peak=true -f null - 2>&1 | awk -v f="$out" '
    /Integrated loudness/ {i = 1} i && /I:/ {I = $2}
    /True peak/ {p = 1} p && /Peak:/ {P = $2}
    END {g = -20 - I; if (g > -1 - P) g = -1 - P; printf "%s  %.1f LUFS  peak %.1f dBTP  gain_db %.1f\n", f, I, P, g}'
