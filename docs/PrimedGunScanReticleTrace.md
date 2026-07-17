# PrimedGun Scan Reticle Trace Notes

Date: 2026-05-30

## Test State

- Repo was reset to `HEAD` before the trace so the failed scan-reticle experiments were removed.
- Temporary interpreter tracing was added in `Source/Core/Core/PowerPC/Interpreter/Interpreter.cpp`.
- Portable Dolphin config was set to interpreter/debug mode for the test.
- Runtime log inspected: `Binary/x64/User/Logs/dolphin.log`.

## What Went Wrong With This Trace

- The game ran extremely slowly because Dolphin was forced through interpreter/debug settings.
- No PPC instruction trace lines were written. The log contains `0` matches for `INTER PC`.
- Reason: `Interpreter::Trace()` logs with `DEBUG_LOG_FMT(POWERPC, ...)`, but this build is not `_DEBUG` or `DEBUGFAST`, so `MAX_EFFECTIVE_LOGLEVEL` is capped at `LINFO`. The trace work ran, but the `LDEBUG` output was filtered out.

## What The Log Still Proved

- The PrimedGun mode probe was active and produced 630+ mode entries.
- During the scan attempt, the runtime saw:
  - `player=8046B97C`
  - `camera=0`
  - `morph=0`
  - `visor=1`
  - `gun_alpha=0.000`
  - `holster=0`
- Pad mode later bounced between `combat` and `lock-on`, which matches the user report that scan/lock behavior is overlapping.
- `visor=1` here comes from the current PrimedGun helper reading `player + 0x330`; it should not be treated as the real `CPlayerState::EPlayerVisor` value.

## Current Code Paths Explaining The Missing Scan Reticle Work

Current runtime code intentionally skips our HMD target and billboard systems during scan:

- `UpdateGunTargeting(...)` returns early when `ScanVisorActive(...)` is true, clears the target scratch, and does not produce a gun/HMD target.
- `UpdateReticleBillboard(...)` returns early when `ScanVisorActive(...)` is true, clears `RETICLE_BILLBOARD_SCRATCH`, and does not write an HMD-facing basis.
- `UpdateScanPitch(...)` still uses controller pitch in scan and writes `PLAYER_FREE_LOOK_PITCH_ANGLE_OFFSET`.

That means the current baseline cannot make the scan reticle follow HMD direction: the two systems we would normally reuse are disabled exactly while the scan mode proxy is active.

## Decomp Cross-Check

PrimeDecomp says the real visor state lives in `CPlayerState`, not directly on the player object:

- `CStateManager::x8b8_playerState`
- `CPlayerState::x14_currentVisor`
- Enum values:
  - `Combat = 0`
  - `XRay = 1`
  - `Scan = 2`
  - `Thermal = 3`

So the next fix should not depend only on `player + 0x330 == 1` for scan. That value appears to correlate with the current scan/lock state in our probes, but it is not the true visor enum.

## Recommended Next Pass

1. Stop using full interpreter instruction tracing for this problem; it is too expensive.
2. Add a narrow PrimedGun runtime probe that reads and logs:
   - real `CPlayerState::x14_currentVisor`
   - `CPlayerState::x18_transitioningVisor`
   - player scan state around `CPlayer::x3a8_scanState`
   - orbit target fields around `CPlayer::x310_orbitTargetId` and `x33c_orbitNextTargetId`
   - reticle/target scratch values
3. Add a separate HMD scan-target path instead of trying to move the existing combat reticle path:
   - only active when the real visor is `Scan`
   - use HMD forward as the ray direction
   - keep controller-pitch scan camera disabled or gated separately
   - do not clear the normal reticle billboard scratch just because scan is active

This should give a targeted, fast log and a cleaner implementation path without forcing the entire CPU through PPC trace output.

## Follow-Up Runtime Log: 2026-05-30 15:48

Runtime log inspected: `Binary/x64/User/Logs/dolphin.log`

The new log still contains no `INTER PC` lines, so it does not include PPC assembly output. It does contain a useful PrimedGun mode window around the scan attempt.

Important scan window:

- `47:55.273`: `visor=1`, `gun_alpha=0.000`, `holster=0`
- `47:55.830`: `visor=1`, `input_flags=04`, `gun_alpha=0.000`, `holster=1`
- `47:55.845` through `47:56.010`: `visor=1`, `input_flags=04`, gun alpha ramps from `0.037` to `0.444`
- `47:56.025`: `visor=0`, `input_flags=04`, `gun_alpha=0.481`, `holster=1`, `gameplay_input=false`
- `47:56.035`: pad mode switches to `classic`
- `47:56.041`: `visor=0`, `input_flags=00`, `gun_alpha=0.519`, `gameplay_input=true`
- `47:56.050`: pad mode switches back to `combat`
- `47:56.250`: `gun_alpha=1.000`, `holster=2`

This suggests our current `player + 0x330` proxy is not a stable "currently in scan visor" value. It appears to go active during the transition/input moment, then drops back to `0` while the gun continues its holster/unholster animation. If we use that value to decide whether scan targeting or billboard writes are allowed, we can easily clear the reticle path at the exact moment we need it.

Next implementation implication:

- Use the real `CPlayerState` visor data as the primary scan/combat gate.
- Treat `player + 0x330` as a transition/orbit/input proxy only, not the scan visor truth.
- Keep scan-target work alive through the holster transition instead of clearing target/billboard scratch as soon as the proxy flips.

## Follow-Up Runtime Log: 2026-05-30 16:00

Runtime log inspected: `Binary/x64/User/Logs/dolphin.log`

This run produced the targeted scan probe correctly.

Main scan findings:

- Real player state pointer resolved: `player_state=80E05D60`
- Real visor state is stable in scan:
  - `current_visor=2`
  - `transition_visor=2`
  - `transition=0.200`
- Old scan proxy remains `proxy_visor=1`, so it matches scan here but should still be treated as a proxy, not truth.
- Player scan fields:
  - `scan_state=0`
  - `scanning_obj=FFFF0000`
- Orbit fields:
  - starts with `orbit_target=FFFF0000`
  - `orbit_next` later becomes `045A0000`
- PrimedGun scratch state:
  - `target_player=00000000`
  - `target_uid_word=00000000`
  - `reticle_enabled=0`
- Pitch/yaw are not being driven during this window:
  - `free_pitch=0.0000`
  - `free_yaw=0.0000`

Conclusion:

The real scan visor flag is now known and usable: `CPlayerState::x14_currentVisor == 2`.

The scan reticle/HMD target path is currently absent, not merely pointed the wrong way. During real scan mode, PrimedGun keeps both the gun target scratch and reticle billboard scratch disabled. The next implementation should add a dedicated scan/HMD target path that stays active when `current_visor == 2`, probably feeding the scan target/orbit-next behavior instead of relying on the normal combat gun target path.

## Reverted Experiment Notes: 2026-05-30

These notes were saved before hard-resetting the local tree.

### Scan/HMD Runtime Experiment

- Added `CPlayerState`-based scan detection:
  - `CStateManager::x8b8_playerState`
  - `CPlayerState::x14_currentVisor`
  - `CPlayerState::x18_transitioningVisor`
  - `CPlayerState::x1c_transitionFactor`
- Added scan/orbit probe fields for:
  - `CPlayer::x3a8_scanState`
  - `CPlayer::x3b4_scanningObject`
  - orbit state/type/target/next target around `x304`, `x308`, `x310`, and `x33c`
  - free-look yaw/pitch around `x3e4` and `x3ec`
- Added `RealScanVisorActive(...)` and used it instead of the older `player + 0x330` scan proxy.
- Tried a scan-specific HMD target path:
  - use HMD pose as ray source/direction while real scan visor is active
  - keep reticle billboard scratch alive in scan mode
  - stop feeding controller pitch into scan free-look camera angles
- Added `SCAN_VISOR_ACTIVE_SCRATCH` so pitch/lock hooks could know when scan mode is active and avoid fighting scan behavior.
- Added runtime logging:
  - `scan_probe`
  - `scan_hmd_probe`

### Scan Window Render Experiment

- Added render-side probes in `VertexManagerBase.cpp`:
  - `scan_window_probe`
  - `scan_marker_probe`
  - `scan_candidate_probe`
  - `scan_texture_probe`
- Added scan-window matching for the 320x224 scan viewport/scissor rectangle:
  - viewport `662,566 320x224`
  - scissor `342,342 981,789`
  - ortho projection `-32000..32000`, `22400..-22400`
- Texture hashes found during the experiment:
  - `d48bcde8ccd23d5d`
  - `a7ab534aaca304b6`
  - `d48bedc8cd23d5d8`
  - `305254df9d22741a`
  - `dae6190d7af6e600`
  - tile texture `ac869b6f32d8bbdb`
- Pixel shader hashes seen:
  - `0000000007ad8d1f`
  - `000000006b6d8d76`
  - `1f68c8e6`
  - `65cdb745`
  - `f491ede7`
- The render-side skip worked for some scan-window rectangles, but it was not a clean source fix and risked affecting other static/headlocked effects. Better next pass: find the PPC/UI source that builds the scan-window quads or identify a stable vertex/object source instead of skipping final GPU draws by texture.

### HMD Directional Movement Toggle

- Added a new runtime setting:
  - `directional_movement_use_hmd_direction`
- Desktop UI addition:
  - movement direction radio buttons: `Controller direction` / `HMD direction`
  - persisted with `QSettings` key `primedgun/directional_movement_use_hmd_direction`
- In-headset menu addition:
  - `MOVE DIRECTION` row showing `CONTROLLER` or `HMD`
  - adjusted numeric-row indices after inserting the new row
- Runtime behavior:
  - if disabled, movement direction uses the selected movement controller pose
  - if enabled, movement direction uses HMD yaw instead
  - reset movement settings returns this toggle to `false`

### Files Touched By The Experiment

- `Source/Core/Common/VR/OpenXRInputState.h`
- `Source/Core/Core/PrimedGun/NativeRuntime.cpp`
- `Source/Core/Core/PrimedGun/NativeRuntime.h`
- `Source/Core/DolphinQt/MainWindow.cpp`
- `Source/Core/VideoBackends/D3D/D3DOpenXR.cpp`
- `Source/Core/VideoCommon/VertexManagerBase.cpp`
