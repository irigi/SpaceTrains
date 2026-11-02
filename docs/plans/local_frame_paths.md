# Plan: cislunar and other planet-system paths drawn in the planet's frame (2026-11-02)

**Wish (user, 2026-11-02):** the Earth–Luna ferries draw heliocentric paths. The two ends are so close
together that what the player sees is Earth's own motion, not the transfer. Draw these paths in Earth's
frame instead.

**Status:** built 2026-11-02 as designed, inferred in the UI (no bridge field needed). Burn markers also
shrink to a few pixels close up (their fixed ~96,000 km size swamped the Earth–Moon view). Checked with
`--shot-local-paths` before/after (Kepler ellipse, plasma spiral, LEO hop). Checked by the user 2026-11-02, after a fix: `planet_system_path` thinned the samples by the heliocentric
shape (nearly straight), so the drawn transfer wobbled by up to ~2,500 km; it now thins them around the planet.

## What happened before

- The planners already fly these hops around the planet, not the Sun: `TrajectoryPlanner.cpp` ("Different
  bodies of one planetary system", `keplerian_planet_system`: a Hohmann half ellipse around Earth timed for the
  Moon's phase) and `VariableIspTrajectoryPlanner.cpp` (`variable_isp_planet_system`: a spiral).
  Same-body hops (`keplerian_local`) are the same case.
- `planet_system_path` builds the path around the planet, then adds the planet's position at each sample time,
  so `sampled_path` is heliocentric. That is right for the simulation (ship positions, police meeting a ship
  on its path with `PathMotion`, saves) and stays as it is.
- In Godot (`Main.gd`) the ship itself is placed correctly: `_rail_anchor_m` blends the anchor from Earth to
  the Moon, and the offsets carry the rest. **The drawn line is the problem:** `_rail_line_m` returns absolute
  positions at each time, and the mesh sits in `world_root` (the Sun's frame). In a ~5-day Hohmann hop Earth
  moves ~13 million km, ~35 times the Earth–Moon distance, so the line is a long streak along Earth's orbit
  with the transfer as a small wobble in it. The fine drawing near the camera (`_build_fine_path`), the
  history trail (`ship_trail_history`) and the destination ghost have the same problem.

## Design

A path whose origin and destination stations belong to the same planet system (`_planet_of(origin body) ==
_planet_of(destination body)`, not a derelict or patrol path) gets a **frame body**: that planet. Everything
drawn for it lives in the planet's frame and moves with the planet:

- Rail anchor = the planet's position at time t (no Earth→Moon blend), so the offsets are the
  planet-centred path. The ship's position stays exactly what it is now (planet(t) + offset(t)).
- The line = offsets drawn under a node that follows the planet each frame: `planet(now) + offset(t_i)`. It
  shows the ellipse or spiral around Earth from the Moon's orbit, as a player would picture it.
- Fine line near the camera: the same, with the focus taken relative to the planet.
- History trail of a ship in such a flight: stored relative to the planet and drawn in its frame.
- Destination ghost (the Moon at arrival): `planet(now) + moon_relative(arrival)`, on the drawn path's end.
- Burn markers: at the path's ends, in the planet's frame.

Display only: no change to the planners, the bridge's path samples, saves or the economy (audit
byte-identical). The UI infers the frame from the stations' bodies, as it already does for the `helio` flag;
if that is fragile, the bridge can add a `frame_body` field to the paths file instead (a small change, no
save impact).

Heliocentric flights (Earth→Mars) stay as they are; the stretch near Earth at departure is out of scope.

## Steps

1. `_cache_ship_path`: set `frame` (planet id or "") next to `helio`; `_rail_anchor_m` returns the frame
   planet's position for framed paths.
2. `_rail_line_m` / `_rebuild_trail_mesh`: framed paths keep their points as offsets; the trail node's
   position follows the planet in `_process` (one vector per framed ship per frame).
3. `_update_fine_paths` / `_build_fine_path`: bounding boxes and focus in the frame for framed paths.
4. History trail, destination ghost, burn markers in the frame.
5. Checks: Godot parse check; existing shot tour shots of heliocentric paths unchanged; a new shot of an
   Earth→Luna ferry close up (the Kepler ellipse) and a plasma spiral, before and after. The shot tour
   opens a visible Godot window, so the user is told before it runs. The 365-day economy audit
   is unchanged (no C++ change; confirm if the bridge field is added).
6. Docs: `docs/modules/ui_observer.md` (paths section).

## Open for the user

- The same treatment applies to Jupiter's and Saturn's moon hops (Europa↔Ganymede etc.) and same-body hops:
  planned, unless the user wants Earth only.
