# Kepler Trajectory Rendering

## Scope

- Keep live runtime planning Kepler-only for this pass.
- Leave the VariableISP atlas/integrator as verified standalone reference code, not runtime ship mission logic.
- Make selected trajectory lines use the same mission path that ship render positions use.
- Remove stale implementation notes and update current docs.

## Implementation

- Timed `TrajectoryPlan` samples are copied into `MissionAssignment` when a mission starts.
- `Simulation::get_ship_render_position()` interpolates the active mission path by absolute sample time.
- Bridge JSON includes `trajectory_path` for awaiting/in-transit ships and `destination_body_at_arrival` for selected-path presentation.
- Godot selected trajectory rendering consumes `trajectory_path` directly, adds a selected-ship overlay, and draws a transparent destination-body ghost from bridge data.
- Active ships render near-white with stronger white emission; stranded ships keep red coloring.
- `KeplerTrajectoryPlanner` uses local direct transfers for same-parent stations, and launch-windowed Hohmann timing, two-impulse delta-v, rocket-equation propellant estimates, and deterministic endpoints for interplanetary transfers.

## Acceptance Criteria

- C++ tests cover Kepler sampled path endpoints, Hohmann transfer time, rocket-equation propellant usage, insufficient-propellant infeasibility, simulation render interpolation on the mission path, and bridge `trajectory_path` output.
- Existing `spacetrains_tests` and `spacetrains_variable_isp_tests` pass.
- In Godot, selecting an in-transit ship displays a continuous line from bridge-provided path samples.
- The selected trajectory line does not rebuild or wiggle unless the bridge-provided path changes.
- The in-transit ship position lies on the displayed selected trajectory path.
