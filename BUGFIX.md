# BUGFIX: Sun Halo and Planet Motion Pulsing

## Summary
This bugfix has two targets:

- **Sun rendering**: the current `main`-inspired Sun setup is functioning, but it does not produce a visible halo in this project. The fix must prove whether HDR bloom is merely too subtle or whether a dedicated halo layer is required.
- **Planet motion pulsing**: the visible slowing and speeding at `86400x` is most likely a frontend and bridge cadence bug. The current renderer interpolates using a fixed assumed interval while the bridge writes snapshots on a different cadence, which can create periodic speed modulation.

The implementation should first add short-lived diagnostics, then fix the motion cadence, then finalize Sun visuals using the lightest solution that works.

## Implementation Changes

### 1. Add temporary diagnostics
Add throttled debug output for:
- bridge snapshot sequence and arrival timing
- snapshot `game_time_s` delta
- computed interpolation alpha
- rendered position and apparent speed for one tracked body
- Sun material emission settings
- environment glow settings
- renderer method and tonemap mode
- Sun screen-space size and distance to camera

Add a simple debug mode switch for the Sun:
- `off`
- `main`
- `exaggerated`

`exaggerated` should force a clearly visible bloom if the bloom pipeline is working.

### 2. Fix the motion pulsing
Keep the current file bridge, but change interpolation to be based on **actual snapshot timing**.

Bridge snapshot JSON must include:
- `snapshot_seq`
- `snapshot_real_time_s` or another monotonic bridge timestamp
- existing `game_time_s`

Godot must track:
- previous snapshot payload
- current snapshot payload
- previous arrival wall time
- current arrival wall time

Interpolation must use:
- actual elapsed wall time since current snapshot arrival
- actual wall-time interval between previous and current snapshots
- clamped alpha from those real intervals

Also change polling so it does not depend on a mismatched fixed interval:
- preferred: poll every frame and only parse when the snapshot file changed
- preserve timer remainder if any timer remains in use

### 3. Validate motion before removing diagnostics
Run and observe at:
- `3600x`
- `21600x`
- `86400x`

Focus Earth or Earth L1 and verify:
- no 1–2 second periodic pulsing
- smooth monotonic orbital motion
- stable nearby station and ship motion in the same frame

Remove motion diagnostics only after this is confirmed.

### 4. Finalize Sun rendering
Start from the current `main`-style emissive sphere and dark scene.

Step 1:
- raise Sun emissive HDR contribution in `exaggerated` mode
- lower bloom threshold enough to make failure obvious
- keep distant directional light disabled

Step 2:
- if exaggerated mode produces visible bloom, tune down to a restrained final level
- if exaggerated mode still does not create a readable halo, add one subtle dedicated Sun halo layer:
  - single layer only
  - warm white and yellow
  - slightly larger than the Sun
  - soft alpha falloff
  - no multi-shell look

The final Sun should be `main`-inspired in mood, not a literal copy of `main`, because `main` itself had no true halo.

## Test Cases
- Verify a runtime debug block reports active glow, tonemapping, and Sun emissive settings.
- Verify exaggerated Sun mode creates an obvious bloom or conclusively proves bloom is insufficient.
- Verify Earth motion remains smooth for at least 15 seconds at `86400x`.
- Verify no distant directional light remains.
- Verify the final Sun looks warm and dominant without reverting to the earlier multi-sphere glow look.

## Assumptions
- The current architecture stays unchanged.
- The file-based bridge remains in place.
- Temporary debug code and debug UI and terminal output are acceptable during diagnosis and should be removed after validation.
- The motion issue is assumed to be a render and bridge cadence problem unless diagnostics prove otherwise.
