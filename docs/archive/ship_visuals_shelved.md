# Shelved: Shipyard Panel and 3D Ship Close-ups

Status: archived (user, 2026-10-11). Not in the pipeline and not planned soon. These were proposals B and C of the
ship visuals proposal. Proposals A (the inspector schematic) and D (map hints) were built; see
`docs/modules/ui_observer.md`.

## B: the fleet at one scale ("Shipyard" panel)

![](fleet_to_scale.png)

(Mock-up from a one-off script, removed in the 2026-10 clean-up; git history has it.)

- A panel with every hull at one scale. Each row gives Δv (laden and empty), α/κ, crew, hold, price and the
  number in service.
- It would open from the market panel or with a key (Y).
- It shows the design space the economy chooses from: why fleet investment bought a Plasma Bulk Tanker, or why
  Ganymede ships are plasma.
- Tank variants appear as ghosted outlines.
- In tier 2 (modules and refits, `docs/modules/ships_and_fleet.md`) it would become the refit and module browser.
- Estimated at half a day, reusing `ShipSchematic` per row.

## C: a 3D ship close-up built from components

(Partly revived 2026-10-23: the map models are now built from the components with the schematic's layout and
shown in detail when the camera is near; see `docs/modules/ui_observer.md`. The inspector turntable stays shelved.)

The map can't show a ship closely: `OrbitCamera.gd` clamps the near plane to at least 0.0002 world units, and a
model is about 2·10⁻⁵ units.
- Render the selected ship in its own small SubViewport in the inspector, as a turntable with its own camera and
  light, at metre scale. Add an "expand" button for a larger view.
- `EntityVisuals` would build the model from the design components with the schematic's layout rules (cylinders,
  boxes, wings and nozzle sized by volume and area). The map models would be generated the same way, so a bulk
  tanker looks like one even at 40 px.
- Live state: radiator emission by temperature, tank frost, reactor glow while burning, and the plume.
- Estimated at 1-1.5 days.
