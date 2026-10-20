# Every key and mouse binding of the game, shown by the help panel (H).
# tests/test_key_bindings.py fails when a script handles a KEY_* or MOUSE_BUTTON_* input that is
# not listed here, or when an input listed here is handled nowhere.
extends RefCounted

const BINDINGS := [
    {"group": "Time", "keys": "Space", "inputs": [KEY_SPACE], "action": "Pause or resume"},
    {"group": "Time", "keys": "1 / 2 / 3", "inputs": [KEY_1, KEY_2, KEY_3], "action": "Time warp 1 hour / 6 hours / 1 day per second"},
    {"group": "Time", "keys": ", / .", "inputs": [KEY_COMMA, KEY_PERIOD], "action": "Time warp one step slower / faster"},
    {"group": "Map", "keys": "Left click", "inputs": [MOUSE_BUTTON_LEFT], "action": "Select a ship, station or body"},
    {"group": "Map", "keys": "Double click / F", "inputs": [KEY_F], "action": "Focus the camera on the selection"},
    {"group": "Map", "keys": "Right drag", "inputs": [MOUSE_BUTTON_RIGHT], "action": "Rotate the camera"},
    {"group": "Map", "keys": "Middle drag", "inputs": [MOUSE_BUTTON_MIDDLE], "action": "Pan the camera"},
    {"group": "Map", "keys": "Mouse wheel", "inputs": [MOUSE_BUTTON_WHEEL_UP, MOUSE_BUTTON_WHEEL_DOWN, MOUSE_BUTTON_WHEEL_LEFT, MOUSE_BUTTON_WHEEL_RIGHT],
        "action": "Zoom the map; over a panel, scroll the panel"},
    {"group": "Panels", "keys": "M", "inputs": [KEY_M], "action": "Economy & markets table (red = scarce, green = cheap)"},
    {"group": "Panels", "keys": "H", "inputs": [KEY_H], "action": "This list of key bindings"},
    {"group": "Panels", "keys": "Esc", "inputs": [KEY_ESCAPE], "action": "Close the open panel"},
    {"group": "Game", "keys": "F5", "inputs": [KEY_F5], "action": "Quick save"},
    {"group": "Game", "keys": "F9", "inputs": [KEY_F9], "action": "Quick load"},
    {"group": "Debug", "keys": "F10", "inputs": [KEY_F10], "action": "Print the map state to the console"},
    {"group": "Debug", "keys": "F11", "inputs": [KEY_F11], "action": "Map debug logging on / off"},
]
