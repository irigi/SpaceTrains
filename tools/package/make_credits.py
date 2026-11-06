#!/usr/bin/env python3
"""Writes godot/assets/CREDITS.txt: the music (from godot/assets/music/manifest.csv), the planet maps
and the fonts. The game shows this file in the help panel (H, Credits) and build_release.sh copies it
into the package. Run after changing the manifest; --check fails if the file is out of date.
"""
import csv
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MANIFEST = REPO / "godot/assets/music/manifest.csv"
OUT = REPO / "godot/assets/CREDITS.txt"

LICENCE_URLS = {
    "CC BY 3.0": "https://creativecommons.org/licenses/by/3.0/",
    "CC BY 4.0": "https://creativecommons.org/licenses/by/4.0/",
}

HEADER = """SpaceTrains credits
===================

Music
-----
Recordings as listed (title, composer, performer, licence, source). All were converted to
OGG Vorbis and had leading and trailing silence trimmed; the music itself is unchanged.
"""

FOOTER = """
Planet maps
-----------
Solar System Scope (https://www.solarsystemscope.com/textures/), CC BY 4.0
(https://creativecommons.org/licenses/by/4.0/), based on NASA elevation and imagery data.
Changed: Venus and Mars desaturated, Mars brightened, Saturn's ring resampled, all re-encoded as JPEG.
Europa and Ganymede: stylised maps generated for this project (no photographic imagery).
"""


def music_section() -> str:
    lines = []
    with MANIFEST.open(newline="", encoding="utf-8") as f:
        for row in csv.DictReader(f):
            licence = row["licence"]
            url = LICENCE_URLS.get(licence)
            if url is None and not licence.lower().startswith(("public domain", "cc0")):
                sys.exit(f"make_credits: no licence link for {licence!r} ({row['file']})")
            licence_text = f"{licence} ({url})" if url else licence
            lines.append(
                f"\n\"{row['title']}\" by {row['composer']}\n"
                f"  Performed by {row['performer']}\n"
                f"  Licence: {licence_text}\n"
                f"  Source: {row['source_url']}\n"
            )
    return "".join(lines)


def main() -> None:
    text = HEADER + music_section() + FOOTER
    if "--check" in sys.argv[1:]:
        if not OUT.exists() or OUT.read_text(encoding="utf-8") != text:
            sys.exit(f"make_credits: {OUT.relative_to(REPO)} is out of date; run tools/package/make_credits.py")
        return
    OUT.write_text(text, encoding="utf-8")
    print(f"wrote {OUT.relative_to(REPO)}")


if __name__ == "__main__":
    main()
