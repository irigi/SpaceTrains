# SpaceTrains — Solar System Simulation

A self-playing 3D space simulation set in the Solar System, built with Godot 4.x. Watch factions manage stations, dispatch cargo ships, and trade commodities across the solar system.

## Requirements

- **Godot 4.2+** (standard build, no paid plugins)
- **Windows 11** (primary target, but should run on any Godot-supported platform)

## Setup

1. Open Godot 4.2+
2. Import the project by selecting the `project.godot` file
3. Press F5 or click Play to run

## Controls

### Camera
| Input | Action |
|-------|--------|
| Right Mouse + Drag | Rotate camera |
| Middle Mouse + Drag | Pan camera |
| Scroll Wheel | Zoom in/out |
| F | Focus on selected object |

### Selection
| Input | Action |
|-------|--------|
| Left Click | Select planet/station/ship |
| Left Click (empty space) | Deselect |

### Time
| Input | Action |
|-------|--------|
| Space | Pause / Resume |
| 1 | Speed 1× |
| 2 | Speed 5× |
| 3 | Speed 20× |
| 4 | Speed 50× |

### Other
| Input | Action |
|-------|--------|
| H | Toggle help overlay |
| Ctrl+S | Save game |
| Ctrl+L | Load game |

## Architecture

### Simulation Layer (`sim/`)
- `Simulation.gd` — Main tick loop, world setup, save/load
- `WorldState.gd` — Pure data containers (bodies, stations, ships, factions)
- `EventBus.gd` — Global signal bus (autoload)
- `systems/OrbitSystem.gd` — Analytic Keplerian orbits
- `systems/ShipMovementSystem.gd` — Ship travel with smoothstep interpolation
- `systems/StationEconomySystem.gd` — Production/consumption of commodities
- `systems/MissionSystem.gd` — Cargo mission creation and dispatch

### View Layer (`view/`)
- `SolarSystemRenderer.gd` — Creates and updates all 3D visual nodes
- `OrbitCamera.gd` — Orbit camera with rotate/pan/zoom
- `FloatingOrigin.gd` — Precision maintenance for large-scale rendering

### UI Layer (`ui/`)
- `SelectionPanel.gd` — Entity detail inspector
- `EventLog.gd` — Filtered event feed
- `TimeControls.gd` — Pause/speed controls
- `HelpOverlay.gd` — Controls reference

### Scenes (`scenes/`)
- `Main.tscn` — Root scene, entry point

## Simulation Details

### Celestial Bodies
Sun, Mercury, Venus, Earth, Mars, Jupiter, Saturn, Uranus, Neptune, and Ceres. All follow simplified circular orbits (analytic, not n-body).

### Factions
- **Sol Federation** — Earth-based, high lawfulness, balanced trade/security
- **Mars Corp** — Mars-based, trade-focused, moderate lawfulness
- **Independent** — Neutral stations, trade-oriented

### Stations (10)
Distributed across Earth, Mars, Venus, Mercury, Ceres, Jupiter, Saturn orbit. Each has modules (Docks, Farm, Refinery, Life Support, etc.), inventory, and a ship roster.

### Ships
Light Freighters execute cargo delivery missions between stations. Ships launch, travel (with smoothstep interpolation), dock, and deliver commodities.

### Commodities
Food, Water, Oxygen, Metals, Fuel, Electronics, Medical

### Economy
Stations produce commodities based on modules (Farm → Food, Refinery → Metals→Fuel) and consume based on population. Surplus triggers cargo missions to needy stations.

## Save/Load
Games save to `user://saves/savegame.json` in JSON format. All entity IDs are stable across saves.

## Version Roadmap

- **v0.1 (Current)** — MVP: Solar system view, stations, cargo ships, event log, time controls
- **v0.2** — Pirates, patrols, inspections, bounties
- **v0.3** — Production chains, shortages, ship services
- **v0.4** — Visual polish, trails, better UI filtering
