#!/usr/bin/env python3
"""The key bindings help (H in the game) lists every key and mouse button the game handles.

Fails when a Godot script uses a KEY_* or MOUSE_BUTTON_* constant, or an input action, that
godot/scripts/ui/KeyBindings.gd does not list, or when that table lists an input nothing handles.

    python3 tests/test_key_bindings.py   (also run by ctest)
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = ROOT / "godot" / "scripts"
TABLE = SCRIPTS / "ui" / "KeyBindings.gd"
# Dev tools that synthesise input events (screenshot tour); they bind nothing.
DEV_TOOLS = {"ShotTour.gd"}

INPUT = re.compile(r"\b(?:KEY|MOUSE_BUTTON)_[A-Z0-9_]+\b")
ACTION = re.compile(r"""is_action(?:_pressed|_released|_just_pressed|_just_released)?\(\s*["']([^"']+)["']""")
STRING = re.compile(r'"(?:[^"\\]|\\.)*"' + r"|'(?:[^'\\]|\\.)*'")


def code_only(text: str) -> str:
    """The script without strings and comments (a KEY_ in a tooltip is not a binding)."""
    lines = []
    for line in text.splitlines():
        line = STRING.sub('""', line)
        lines.append(line.split("#", 1)[0])
    return "\n".join(lines)


def table_inputs() -> tuple[set[str], set[str]]:
    text = TABLE.read_text()
    inputs: set[str] = set()
    actions: set[str] = set()
    for match in re.finditer(r'"inputs":\s*\[([^\]]*)\]', text):
        for item in match.group(1).split(","):
            item = item.strip()
            if not item:
                continue
            if item.startswith('"'):
                actions.add(item.strip('"'))
            else:
                inputs.add(item)
    if not inputs:
        sys.exit(f"{TABLE.relative_to(ROOT)}: no bindings found")
    return inputs, actions


def used_inputs() -> tuple[dict[str, list[str]], dict[str, list[str]]]:
    inputs: dict[str, list[str]] = {}
    actions: dict[str, list[str]] = {}
    for path in sorted(SCRIPTS.rglob("*.gd")):
        if path == TABLE or path.name in DEV_TOOLS:
            continue
        raw = path.read_text()
        where = str(path.relative_to(ROOT))
        for name in INPUT.findall(code_only(raw)):
            inputs.setdefault(name, []).append(where)
        for name in ACTION.findall(raw):
            actions.setdefault(name, []).append(where)
    # Input map actions defined in the project settings count as bindings too.
    project = (ROOT / "godot" / "project.godot").read_text()
    section = re.search(r"^\[input\]\n(.*?)(?=^\[|\Z)", project, re.M | re.S)
    if section:
        for name in re.findall(r"^([A-Za-z0-9_]+)\s*=", section.group(1), re.M):
            actions.setdefault(name, []).append("godot/project.godot")
    return inputs, actions


def main() -> None:
    listed_inputs, listed_actions = table_inputs()
    inputs, actions = used_inputs()
    errors = []
    for name, files in sorted(inputs.items()):
        if name not in listed_inputs:
            errors.append(f"{name} is handled in {', '.join(sorted(set(files)))} but missing from the key bindings table")
    for name, files in sorted(actions.items()):
        if name not in listed_actions:
            errors.append(f"input action '{name}' is used in {', '.join(sorted(set(files)))} but missing from the key bindings table")
    for name in sorted(listed_inputs - inputs.keys()):
        errors.append(f"{name} is in the key bindings table but no script handles it")
    for name in sorted(listed_actions - actions.keys()):
        errors.append(f"input action '{name}' is in the key bindings table but nothing uses it")
    if errors:
        sys.exit(f"Key bindings table ({TABLE.relative_to(ROOT)}) is out of date:\n  " + "\n  ".join(errors))
    print(f"Key bindings table covers all {len(inputs)} inputs.")


if __name__ == "__main__":
    main()
