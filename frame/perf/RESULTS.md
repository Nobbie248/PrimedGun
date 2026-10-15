# GPU and resolution

What the GPU does per frame, which settings change it, the resolution table and the defaults chosen from it.
Everything here is from the mock runtime on the Frame itself (`frame/tools/mockrun.sh`, pipelined pacing,
120 Hz, Adreno 750 with Turnip). SteamVR's compositor is not in these numbers: it needs about 0.8 ms of GPU
per refresh at 120 Hz (1.6 ms per 60 Hz game frame), which is the margin kept below.

## Method

- `frame/tools/mockrun.sh` options added for this work (new options only): `--eye N` (eye image size the
  mock recommends), `--sysmem` / `--tudebug V` (Turnip's `TU_DEBUG`), `--set F:Section.Key=Value` (ini edits in
  the scratch user dir; a game ini value wins over GFX.ini), `--clocklog` (GPU clock, its thermal cap, GPU and
  CPU temperatures once a second, `clock.log` in the run dir) and `--cool C` (start when every GPU thermal
  zone is below C degrees).
- `frame/perf/matrix.sh` runs the table, `summary.py` makes its rows, `timeline.py` lines up game fps, GPU
  time, GPU clock, temperatures and the culled draw count, `hitches.py` reads the per frame log
  (`PRIMEDFRAME_FRAMELOG=<file>`, one line per presented frame), `gputrace.py` and `passes.py` read Turnip's
  own GPU trace (`MESA_GPU_TRACES=print`) and give the GPU time per render pass.
- "GPU ms" is the busy time per eye set from the kernel (XRSTATS `gpu_ms`). The GPU governor
  (`simple_ondemand`) lowers the clock when the GPU is not saturated, which makes the busy time of a light
  scene look longer than it is; "at 903 MHz" scales each second by its mean clock (from devfreq's time in
  each state) to the full clock. In every heavy room run the clock sat at 903 MHz, so both columns agree.
- **The heavy state is two scenes.** `GM8E01.s02` starts in the game's opening (the gunship flies to the
  frigate, world view, GPU 2.5 to 7 ms) and about 95 s after the load the player view starts (Samus on the
  landing site next to the ship, HUD up, about 1300 draws, nothing to cull): that view is the heavy room and
  it does not change while the state keeps running (no input). All s02 numbers below are 120 s of that view
  (105 s to 225 s after the first frame); s01 is 10 s to 130 s.
- Picture checks: the mock writes the released eye images at a given time (`MOCK_XR_DUMP`, quarter size),
  so they show whether a setting changes the picture grossly, not sharpness. The mock's head turns
  (+-17 degrees, 0.5 Hz), so images of two runs differ a little in yaw even at the same time.

## The soak step (lane A's 10 minute soak: 12 ms to 18.7 ms after two minutes)

A scene change, not throttling. Rerun (6 min, s02, IR 2, eye 1728, Turnip's default tiled rendering) with
the clock logged:

| Time after load | Game fps | GPU ms per set | GPU clock MHz | Thermal cap MHz | GPU max C | Draws culled per frame | View |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0 to 30 s | 56 to 59 | 4.4 to 6.2 | 304 to 512 | 903 | 68 | 1160 to 1500 | opening, looking up at the frigate |
| 30 to 100 s | 48 to 58 | 6.5 to 20.2 | 629 to 903 | 903 | 69 | 108 to 1360 | opening, the ship approaching |
| 105 to 380 s | 50.8 to 52.3 | 18.6 to 19.1 | 903 | 903 | 69 | 86 to 91 | player view (heavy room) |

The GPU clock never left 903 MHz in the heavy part, the thermal cap stayed at 903 MHz (cooling state 0),
the GPU never passed 69 C (passive trip at 95 C). The GPU time follows the view: when the head cone culls
1400 draws the frame costs 4 ms, when it culls 90 it costs 19 ms. Lane A's soak shows the same: about 4 ms
while the opening's views culled 1400 draws, 17 to 21 ms from the player view on (60 to 120 culled). The CPU zones reach 95 to 97 C in every run (the emulation
threads), without a frame rate effect in these runs.

## Where the GPU time goes (heavy room, IR 2, eye 1728)

Turnip's trace, 1.5 s of the heavy room, per game frame:

| Pass | Tiled (Turnip's default): GPU ms | Draws | Direct (`TU_DEBUG=sysmem`): GPU ms | Draws |
| --- | --- | --- | --- | --- |
| The game's EFB (1280x1056, colour + depth, two layers): the world, HUD and effects | 16.70 | 1260 | 4.53 | 1365 |
| The two eye images (1728x1728 each: a clear, then the blit) | 1.28 (0.55 of it clears) | 2 | 1.40 (0.63 clears) | 2 |
| The XFB copy (1280x896) | 0.12 | 1 | 0.12 | 1 |
| EFB copies (256x256, 64x64: the game's bloom and visor textures) | 0.03 | 3 | 0.03 | 3 |
| Total per frame | 18.1 | | 6.1 | |

- One render pass of about 1250 draws is 92 % of the frame. Tiled, it runs in 9 GMEM bins and every draw's
  vertex and stereo geometry shader work is repeated for each bin it touches; direct rendering draws it
  once. Turnip's autotune already chose tiled for that pass and direct for the small ones, so
  `TU_DEBUG=gmem` would only move the small passes (0.3 ms); not run.
- The EFB copies the game makes (bloom, visor) cost 0.03 ms on the GPU and nothing on the CPU: with
  `EFBToTextureEnable = True` they stay on the GPU; in 5 minutes not one EFB copy was read back to RAM
  (`PRIMEDFRAME_FRAMELOG` counts them). `EFBToTextureEnable`, `DeferEFBCopies` (only matters for copies to RAM)
  and `EFBScaledCopy` (scaled copies are 0.03 ms) have nothing to win; left as they are.
- The eye clears are pure waste: the blit covers the whole image. 0.55 to 0.63 ms per frame at 1728, more at
  2160 (the 1728 to 2160 step costs 0.85 ms per frame in total). The clear is in `Present.cpp`
  (`BlitCurrentSourceToOpenXREyes`, `SetAndClearFramebuffer`), lane A's file: not changed here.
- Eye image size and internal resolution are separate: the game renders its EFB at 640x528 times the
  internal resolution (IR 2: 1280x1056, IR 3: 1920x1584, IR 4: 2560x2112) and that image is scaled into the
  eye image. At IR 2 a 1728 eye image is a 1.35x horizontal and 1.64x vertical upscale; raising only the eye
  size adds no detail. The panel is 2160 per eye; IR 4 is about the panel's own resolution.

## Settings checked

| Setting | What it does now | GPU effect | Picture |
| --- | --- | --- | --- |
| `TU_DEBUG=sysmem` | Turnip renders straight to memory instead of GMEM tiles | heavy room 18.8 to 6.0 ms (-68 %), light room 5.9 to 3.2 ms at full clock (-46 %) | identical (eye images at the same time) |
| `[VR] DisableCPUCull = True` | turns off Dolphin's stock CPU cull in OpenXR. Has no effect while `[VR] HeadCPUCull` is on (default True): the head cull replaces the stock test for every draw | none | none |
| `[VR] HeadCPUCull` (default True) with the in-VR menu's frustum culling (default on, cone 115 degrees, floored at the rendered field of view plus 4 degrees) | drops draws outside a cone around the head pose the frame is rendered with, plus the game's own frustum patch | off vs on, IR 3, eye 2160, at full clock: opening views 6.45 vs 2.56 ms, 7.16 vs 3.83 ms; heavy room 9.42 vs 9.04 ms | no pop-in visible in the mock images; the headset decides (below) |
| `[VR] DetectSkybox` (default True) | draws with no translation are treated as sky: no eye offset, and the head cull skips them | none measurable (a constant per draw) | sky at infinity instead of in the room; not changed |
| `ShaderCompilationMode = 3` | asynchronous, **skip** drawing what is not compiled yet (2 is asynchronous with ubershaders, 1 synchronous ubershaders, 0 synchronous) | none once compiled; ubershaders would cost GPU on Adreno | an object can be missing for a few frames the first time it is seen |
| Shader cache | was never written by the NoGUI build (no `Cache/` folder, `CreateDir` of `Cache/Shaders/` failed); fixed here | every session compiled all shaders again (54 in s02's first 100 s); now 0 on the second run | fewer first-sight skips |
| `MaxAnisotropy` | default (1x), not in the ini | not measured; costs texture bandwidth | candidate for later (owner's call) |
| Bloom and visor effects | the game's own, from EFB copies; the fork adds no post effect | 0.03 ms | none |

## The table

s02 = heavy room (120 s of the player view), s01 = light state (120 s). Pipelined pacing, mock 120 Hz.
"Late": new sets shown after the refresh their pose was located for (app count). Holds: the share of new
sets the mock showed exactly two refreshes (the goal for 60 fps at 120 Hz), whole session.

| State | Eye | IR | Turnip | Game fps avg (min) | Layers/s | New sets/s | GPU ms per set avg / p99 | at 903 MHz avg / p99 | GPU busy | Late | GPU MHz | GPU max C | Holds 2 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| s02 | 1728 | 2 | tiled | 51.9 (46.8) | 121.1 | 51.9 | 18.77 / 21.12 | 18.77 / 21.12 | 98 % | 11 | 903 | 68.9 | 70 % |
| s02 | 1728 | 2 | sysmem | 59.9 (59.3) | 120.2 | 60.0 | 6.02 / 6.44 | 6.02 / 6.44 | 36 % | 1 | 903 | 66.6 | 99.0 % |
| s02 | 2160 | 2 | sysmem | 59.9 (59.5) | 120.2 | 60.0 | 6.85 / 7.26 | 6.85 / 7.26 | 41 % | 0 | 903 | 68.5 | 99.1 % |
| s02 | 1728 | 3 | sysmem | 59.9 (59.4) | 120.2 | 60.0 | 8.18 / 8.61 | 8.18 / 8.61 | 49 % | 0 | 903 | 73.6 | 99.1 % |
| s02 | 2160 | 3 | sysmem | 59.9 (59.7) | 120.2 | 60.0 | 9.03 / 9.43 | 9.03 / 9.43 | 54 % | 0 | 903 | 75.5 | 99.1 % |
| s02 | 2160 | 4 | sysmem | 59.9 (59.0) | 120.2 | 60.0 | 12.15 / 12.53 | 12.15 / 12.53 | 73 % | 0 | 903 | 84.1 | 99.0 % |
| s01 | 1728 | 2 | tiled | 59.9 (59.0) | 120.1 | 60.0 | 5.89 / 5.98 | 5.89 / 5.98 | 35 % | 1 | 903 | 66.2 | 99.5 % |
| s01 | 1728 | 2 | sysmem | 59.9 (59.0) | 120.1 | 60.0 | 4.94 / 5.01 | 3.17 / 3.29 | 30 % | 2 | 579 | 60.0 | 99.6 % |
| s01 | 2160 | 2 | sysmem | 59.9 (59.0) | 120.1 | 60.0 | 5.16 / 5.31 | 4.05 / 4.45 | 31 % | 1 | 709 | 59.6 | 99.6 % |
| s01 | 1728 | 3 | sysmem | 59.9 (59.0) | 120.1 | 60.0 | 5.63 / 5.76 | 5.63 / 5.76 | 34 % | 1 | 903 | 72.8 | 99.5 % |
| s01 | 2160 | 3 | sysmem | 59.9 (59.0) | 120.1 | 60.0 | 6.42 / 6.49 | 6.42 / 6.49 | 38 % | 1 | 903 | 68.1 | 99.5 % |
| s01 | 2160 | 4 | sysmem | 59.9 (59.9) | 120.1 | 60.0 | 9.20 / 9.27 | 9.20 / 9.27 | 55 % | 1 | 903 | 79.8 | 99.5 % |

Not run: tiled at IR 3 or eye 2160 (tiled IR 2 at 1728 is already 18.8 ms in s02), `TU_DEBUG=gmem` (see above),
IR 5 and up (IR 4 is about the panel's resolution and already 12 ms). Every run: 0 mock errors, 0 images
released before the GPU finished them.

Longer runs, s02 player view, eye 2160, sysmem (6 minutes of it, 465 s sessions):

| IR | Game fps avg (min) | GPU ms avg / p99 | GPU clock | GPU C at 2 / 4 / 6 min | Holds 2 |
| --- | --- | --- | --- | --- | --- |
| 3 | 59.9 (59.3) | 9.02 / 9.44 | 903 MHz throughout | 76 / 77 / 77 (flat) | 99.2 % (0 late layers) |
| 4 | 59.9 (59.2) | 12.13 / 12.50 | 903 MHz throughout | 84 / 86 / 86 (87 at 7 min, still rising) | 99.1 % |

## Hitches (s02, IR 3, eye 2160, sysmem, per frame log)

Player view, 5 minutes (17982 frames):

| Frame time ms | Frames | Share |
| --- | --- | --- |
| below 14 | 75 | 0.4 % |
| 14 to 18 | 17519 | 97.4 % |
| 18 to 22 | 378 | 2.1 % |
| 22 to 26 | 10 | 0.06 % |
| 26 and more | 0 | 0 % |

p50 16.68, p99 18.69, p99.9 20.97, max 23.80 ms: no frame of 25 ms or more in the room itself. The whole session (450 s
from the load) had 8 frames of 25 ms or more, all in the opening's scene changes, at the same moments in
every run (40.5, 65.3, 69.0, 78.6, 96.3, 108.0 s): game work at a cut, not GPU. The worst (75 ms at 96.3 s, the
step from the opening into the player view) came with 15 new shaders on a cold cache and stays at 78 ms on
a warm cache (0 shaders created), so it is the game's transition, not shader compilation. No EFB copy was
read back to RAM in any run; disc reads (176 per session) all fall in the opening and none lines up with a
long frame. Before the shader cache fix every session compiled its shaders again; with
`ShaderCompilationMode = 3` that shows as objects skipped for a few frames, not as stalls.

## Defaults (seed `Config/GFX.ini`, `GameSettings/GM8E01.ini`, `primedframe.sh`)

- **Turnip direct rendering on** (`TU_DEBUG=sysmem` set by the wrapper; `PRIMEDFRAME_SYSMEM=0` turns it off,
  `TU_DEBUG` flags from a `session.env` are kept). It cuts the heavy room's GPU time by two thirds with an
  identical picture, and is what makes anything above today's quality possible.
- **InternalResolution 3** (was 2), with **2160 eye images** (the panel's size; set by SteamVR's per-app
  `resolutionOverride`, which is the wrapper's settings line, not changed here: see below). Heavy room 9.0 ms
  avg, 9.4 ms p99 per frame over 6 minutes: 7 ms under the 16.7 ms frame and 5 ms under the 14.5 ms target
  that keeps the compositor's 1.6 ms, GPU flat at 77 C. 1.5x the pixels of today in each direction.
- Not IR 4, although it fits the target in this room (12.1 / 12.5 ms): it leaves 2 ms beyond the
  compositor's share, so a room about 20 % heavier than s02 (the owner remembers rooms of 1800 draws; this
  one has 1300) would drop frames, and the GPU was at 87 C and still rising after 7 minutes without the
  headset's own display and compositor load (passive trip at 95 C; a clock step down would cost about a
  third of the frame rate at once). It is the first thing to try in the headset: `InternalResolution = 4` in
  `GameSettings/GM8E01.ini`.
- Kept: `ShaderCompilationMode = 3`, `EnableGPUTextureDecoding = False`, `EFBToTextureEnable`,
  `DeferEFBCopies`, `EFBScaledCopy` as they are, culling on (`HeadCPUCull` default), `DisableCPUCull = True`
  (no effect while the head cull is on).

## For the headset session

- Picture: IR 3 should look sharper than before (text on the HUD, distant edges); nothing else should
  change. Direct rendering gave an identical picture in the mock.
- Pop-in at the edges of the view when turning the head quickly (the head cull drops draws outside a cone
  built from the pose the frame is drawn with, plus 4 degrees; SteamVR shows the same image again on the
  in-between refresh, reprojected): look for objects appearing late at the left and right edges. If there is
  any, the in-VR menu's frustum culling angle can be raised.
- Session logs: XRSTATS `gpu_ms` should read about 9 in a room like s02 at IR 3; SteamVR's own timing in
  `vrtiming.log` gives the compositor's share. If any room drops below 60, its `gpu_ms` says whether the
  GPU is the reason.
- Temperature in a long session: not logged by the wrapper; the mock runs above say 77 C at IR 3.

## Not known until the headset

SteamVR's compositor cost at 2160 eye images, the real temperature with the display and compositor
running, rooms heavier than s02, how IR 3 against IR 4 looks on the panel, and the behaviour of
`TU_DEBUG=sysmem` with SteamVR's FDM layer in the chain (the mock has no foveation; Valve's
`XR_APILAYER_VALVE_fdm_injection` loads in front of it but applies nothing there).
