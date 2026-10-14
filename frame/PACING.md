# Pacing

How the game hands its eye images to OpenXR, how that was measured headless, and what the numbers are.
Everything here is from the mock runtime (`frame/tools/mockrun.sh`, `frame/tools/xrmock`); nothing in this
file has been seen on the real SteamVR compositor yet.

## The frame loop before this work (Vulkan backend)

The Vulkan backend never used the fork's pacing thread (`SupportsDetachedFrameLoop()` is false), so every
game frame ran the inline flow on the video thread:

1. End of `Present(N-1)` (`PrepareNextOpenXRFrame`): `xrPollEvent`, `xrWaitFrame` (CPU wait for the
   runtime's next frame slot), `xrBeginFrame`, one `xrLocateViews` for that slot's display time. The
   geometry shader takes that pose for every draw of frame N.
2. Frame N is emulated and drawn (both eyes per draw); Dolphin submits command buffers along the way.
3. `Present(N)`: mirror blits into the desktop window, then per eye `xrAcquireSwapchainImage` +
   `xrWaitSwapchainImage` (CPU wait if the runtime still reads it), clear and blit into the eye image.
4. `PresentBackbuffer`: submits the command buffer (game tail, eye blits, mirror) and presents the window.
5. `SubmitFrame`: submits again, with `PRIMEDGUN_XR_WAIT_GPU=1` waiting for the GPU (the drain, CPU wait),
   releases both eye images, `xrEndFrame` with step 1's display time and the poses the frame was drawn with.
6. The deferred finalisation (`PendingXRFrame`, `FinalizePendingXRFrame`, `WaitForPendingFrameFinalization`)
   is Android only (the command buffer worker releases and ends after its submit); on Linux it does nothing.

CPU waits: `xrWaitFrame`, `xrWaitSwapchainImage`, the drain, and the window present. The frame is ended a
whole game frame after the `xrWaitFrame` it was located for, so it always misses that display time.
Under the mock the window present hid everything: Mesa's software WSI (needed on Xvfb) waits for the GPU
inside `vkQueuePresentKHR`, so the eyes were always finished by the release (that is why the release check
never fired in the baseline) and the CPU copy of the window cost the heavy room about 40 % of its frame rate.

## What it does now

`PRIMEFRAME_PACING=pipelined|inline|drain` (default `pipelined`; `PRIMEDGUN_XR_WAIT_GPU=1` without
`PRIMEFRAME_PACING` still means `drain`). In all three, while the OpenXR session runs, the desktop window is
not drawn or presented (`PRIMEFRAME_WINDOW_PRESENT=1` keeps it); `SubmitFrame` submits the frame's work and
advances Dolphin's frame resources instead.

- `pipelined`: two eye swapchain sets. A pacing thread owns `xrWaitFrame`/`xrBeginFrame`/`xrEndFrame` and
  ends a frame every display refresh. The video thread renders frame N into the next free set, submits,
  puts an empty submission with a fence behind it and hands the set over (poses, swapchains, overlay
  quads, the display time its pose was located for); then it locates the pose for frame N+1 and goes on
  emulating. The pacing thread shows a set on the refresh its pose was located for: when that refresh's
  `xrWaitFrame` returns it waits for the set's fence (normally already signalled; at most half a refresh,
  otherwise it repeats the last set and shows the new one a refresh later), releases the images, and ends
  the projection layer with exactly the poses the set was rendered with (both eyes from the one
  `xrLocateViews` of that game frame). On the refreshes between new sets the pacing thread ends no frame
  (it calls `xrWaitFrame` only for the refresh the next set is due on, just after that refresh started) and
  the runtime shows the last frame again itself; `PRIMEFRAME_REPEATS=1` is the first headset build's way,
  a frame every refresh repeating the last layer (see "Headset session H1" below). The pose target is
  the first refresh the next frame can make (now + the game's pose-to-submit time + the GPU tail + 1 ms),
  kept a fixed number of refreshes after the previous target (2 for 60 fps at 120 Hz) when that is not
  earlier: a steady 2:2 cadence.
- `inline`: the old flow with one set; after the submit a fence is waited for right before the release.
- `drain`: the old flow with one set; the submit itself waits for the GPU (the old `PRIMEDGUN_XR_WAIT_GPU`).
- `PRIMEFRAME_NO_GPU_WAIT=1`: no GPU wait at all (pipelined: the video thread releases right after the
  submit). Only for the negative test of the mock's release check.

All OpenXR calls in which SteamVR's client submits on our queue (`xrBeginFrame`, `xrEndFrame`,
`xrAcquireSwapchainImage`, `xrReleaseSwapchainImage`) hold Dolphin's queue lock (`xrEndFrame` in the inline
path did not before).

## Statistics line

One line a second, OpenXR log category, from the video thread:

```
XRSTATS mode=pipelined submitted=60 layers=120 shown=60 late=0 gpu_waits=0 gpu_wait_max_ms=0.00 drain_avg_ms=0.00 drain_max_ms=0.00 gpu_ms=5.93 xrwait_avg_ms=7.14 xrwait_max_ms=8.40 set_waits=0 eye=1728x1728 game_fps=59.9 game_vps=59.9
```

`submitted` eye sets the game finished, `layers` layer stacks ended, `shown` new sets among them, `late` new
sets shown after the refresh their pose was located for or ended more than a refresh after their
`xrWaitFrame`, `gpu_waits` releases that had to wait for the GPU and the longest wait, `drain_*` the time the
video thread itself waited for the GPU (drain and inline), `gpu_ms` GPU busy time per submitted set (kernel
DRM fdinfo `drm-engine-gpu`, whole process: game, eye blits), `xrwait_*` time blocked in `xrWaitFrame`
(pacing thread in pipelined), `set_waits` times the game waited for a free eye set.

## The mock

`MOCK_XR_SKIP=late` (mockrun's default now): a late `xrWaitFrame` returns at once with the refresh that is
running, display times stay in the future (as SteamVR on the Frame). A layer stack lands on the first refresh
at least one period (`MOCK_XR_LATCH_MS`) after its `xrEndFrame`, never before its display time; the summary
line `xrmock: cadence` gives the late layers and how many refreshes each new eye set stayed on screen.

## Results (mock, 120 Hz, eye 1728x1728, InternalResolution 2)

120 s windows (`mockrun.sh --state 0N --seconds 120 --warmup 20 --pacing M`), mock clock `late`, release
check on (`MOCK_XR_BUSY_MS=4`). Holds: refreshes each new eye set stayed on screen (mock), 2 is the goal
for a 60 fps game. Late: app count (XRSTATS) / layer stacks that landed after their display time (mock).
GPU ms: busy GPU time per eye set, whole process. Waits: releases that had to wait for the GPU (count,
longest); video thread: time the game's own thread waited for the GPU per frame (avg / max).

| Run | Game fps avg (steady, min) | Layers/s | New sets/s | Late app / mock | Holds 2 / 1 / 3 / 4+ | GPU ms per set (max) | Release waits (max ms) | Video thread GPU wait ms | Unfinished (mock) | Poses for the refresh shown |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| s01 pipelined | 59.7 (59.9, 59.0) | 120.6 | 59.9 | 3 / 0 | 8318 / 5 / 21 / 3 | 5.89 (5.97) | 1 (2.4) | 0 | 0 of 16703 | 99.7 % |
| s01 inline | 59.7 (59.9, 59.0) | 59.9 | 59.9 | every frame / 8336 of 8336 | 7868 / 221 / 243 / 3 | 6.03 (6.55) | every frame (14.9) | 6.35 / 14.9 | 0 of 16675 | 100 % located for, all shown later |
| s01 drain | 59.7 (59.9, 59.0) | 59.9 | 59.9 | every frame / 8345 of 8345 | 7947 / 187 / 208 / 3 | 6.02 (6.10) | every frame (11.1) | 6.52 / 11.1 | 0 of 16693 | as inline |
| s02 pipelined | 55.2 (55.0, 45.4) | 121.1 | 55.1 | 17 / 2 | 5783 / 169 / 1612 / 35 | 11.84 (21.8) | 132 (6.9) | 0 (520 waits for a free set) | 0 of 15299 | 97.5 % (2.3 % shown a refresh early) |
| s02 inline | 35.3 (34.7, 22.7) | 35.9 | 35.9 | every frame / 4783 of 4783 | 1154 / 15 / 1183 / 2430 | 11.96 (21.9) | every frame (44.1) | 12.33 / 44.1 | 0 of 9569 | as inline |
| s02 drain | 34.9 (34.1, 23.0) | 35.4 | 35.4 | every frame / 4731 of 4731 | 1065 / 15 / 1223 / 2427 | 12.01 (21.9) | every frame (57.9) | 12.50 / 57.9 | 0 of 9465 | as inline |
| s02 drain, window present (the old path) | 34.2 (33.4, 23.0) | 34.7 | 34.7 | every frame / 4642 of 4642 | 914 / 7 / 1287 / 2433 | 12.27 (21.8) | (the window present waited) | 0.12 / 1.1 | 0 of 9287 | as inline |
| Baseline (lane 0, old mock clock): s01 | 59.6 drain on and off | | | | | | | drain 0.13 / 1.45 | 0 (blind, see below) | |
| Baseline: s02 | 31.3 drain, 35.9 no drain | | | | | | | drain 3.09 / 11.0 | 0 (blind) | |

What the numbers say:

- s02 is GPU heavy (12 ms of GPU per frame on average, up to 22), s01 light (6 ms). Inline and drain make the
  video thread wait for all of it every frame (CPU and GPU in series): s02 at 35 fps. Pipelined overlaps the
  GPU with the next frame's emulation: s02 at 55 fps (+57 %), s01 unchanged at 60.
- Inline and drain end every frame a game frame after the `xrWaitFrame` it was located for, so every frame
  misses its display time (late 100 %) and the cadence follows the game's phase (s01: 221 one-refresh holds,
  243 three-refresh holds). Pipelined ends a layer every refresh (120/s), shows each set on the refresh its
  pose was located for, and holds 99.7 % of the s01 sets exactly two refreshes (5 one-refresh holds in
  120 s; one 1-3 flip).
- In s02 the frame time wanders (GPU 12 to 22 ms), so 3-refresh holds are unavoidable (21 %); 169 one-refresh
  holds are the target moving earlier again after slack. The game waited for a free eye set 520 times in
  120 s (two sets; both in flight while the GPU is behind).
- Inline costs the same as drain: the GPU work of a frame is submitted at its end, so a fence right before
  the release waits as long as the drain.
- The desktop window present costs the heavy room most of its frame rate in the mock (Mesa's software WSI
  copies the window on the CPU after waiting for the GPU; the drain row with the window present is the old
  path).

### The release check: positive control

`PRIMEFRAME_NO_GPU_WAIT=1` (pipelined, the video thread releases right after the submit):

| Run | Releases compared | Changed after release (caught) | mockrun |
| --- | --- | --- | --- |
| s02, 60 s | 8912 | 8912 | FAIL (as it must) |
| s01, 60 s | 9513 | 9510 | FAIL |
| s02, 30 s, window present kept | 3532 | 2 | FAIL, but nearly blind |

The last row is why the baseline's "0 unfinished" proved nothing: with the window present, Mesa's software
WSI (needed on Xvfb) waits for the GPU inside `vkQueuePresentKHR`, before the release. With the window
present skipped (the default now) the check sees every early release, and all pipelined, inline and drain
runs above have 0. The mock's release check also had to become thread-safe: the overlay quads are released
on the video thread while the pacing thread polls the check in `xrWaitFrame` (one false report before).

### Opcode replay (re-render each refresh with the fresh pose)

Not measurable in this tree: the fork's `OpenXROpcodeReplay` keeps its capture and replay state, but nothing
calls it (no `EnableCaptureForNextFrame`, `CaptureCommand`, `BeginReplayIteration` caller anywhere;
`IsReplaying()` is always false), so `[VR] OpcodeReplay` does nothing. What the GPU numbers say about it:
re-rendering every refresh at 120 Hz doubles the game's GPU work per game frame. s01 needs 5.9 ms per render,
two renders = 11.8 ms per 16.7 ms game frame (71 %, before SteamVR's compositor at about 0.8 ms per refresh,
1.6 ms per game frame: 80 %); s02 needs 11.8 ms on average and up to 21.8 ms per render, two = 23.7 ms avg,
which cannot fit in 16.7 ms. So: s02 cannot hold 120 eye sets a second at this resolution even before the
replay's CPU cost (re-decoding the frame's opcodes on the video thread, unknown), and s01 would hold it only
without margin. Not a default; wiring it is a separate job.

### Validation and soak

- Validation layer, s02, 60 s, pipelined (`--validate`): 4 messages, all
  `VUID-VkGraphicsPipelineCreateInfo-pStages-00738` (Dolphin's geometry-shader pipelines, as in the baseline);
  nothing from the OpenXR path, the fences, the eye sets or the pacing thread; 0 mock errors, 0 of 4860
  releases unfinished. (The layer slows the game to 35 fps, so its cadence numbers mean nothing.)
- Soak, s02, 600 s, pipelined: 0 mock errors, 0 of 65267 releases unfinished, eyes always from one locate,
  no fatal line, no stall file; game 52.7 fps (first 300 s 53.4, last 300 s 52.0, steady minimum 45.8);
  121.2 layers/s; 2 layer stacks of 74345 landed late (mock), 60 new sets late (app); 97.0 % of new sets on
  the refresh they were located for; holds 2: 20843, 3: 10231, 1: 1137, 4+: 200. GPU time per set stepped
  from about 12 ms to 18.7 ms after two minutes and stayed there (the GPU was then about 99 % busy): either the
  scene in the save state changes or the GPU clock came down (not told apart; the GPU's devfreq was not
  logged). Worth a look in lane B.

## Headset session H1: doubled images when turning the head

The owner, first session (pipelined, IR 3, eye 2160, sysmem, 120 Hz): "bad ghosting when moving my head,
like every image is doubled or tripled"; head still was fine. The old Podman build (inline, a frame per
game frame) never doubled. Checked in this order:

1. **Image and pose from different locates: no.** New exact check (`mockrun.sh --stamp`, xrmock's
   `MOCK_XR_STAMP`): the draws note the locate their geometry shader constants came from, every XFB copy keeps
   the oldest and newest of its draws, and the present writes those of the XFB it shows into a corner block of
   each eye image (`PRIMEFRAME_FRAME_STAMP=1`); the mock compares both with the locate of the poses the layer
   was submitted with. With the owner's settings (`LockHeadPosePerFrame = False`, `ImmediateXFBEnable = True`,
   IR 3, eye 2160, sysmem) and the head turning at 150 deg/s, the first build was already exact: s01 19064 of
   19064 eye images, s02 18902 of 19012 (110 from the load, before any XFB was stamped). Immediate XFB
   presents each XFB at its copy, in FIFO order, so no draw of the next frame runs before the present. The
   check does catch it: with `ImmediateXFBEnable = False` 17442 of 19076 images showed an XFB drawn a game frame
   before the pose submitted with it and 1556 mixed two locates (mockrun FAILS). Keep immediate XFB on.
2. **Display time: yes, every second layer.** At 60 fps on 120 Hz the pacing thread ended a frame every
   refresh and, on the refresh between two new sets, repeated the last layer with the poses located for the
   refresh before: 4790 of 9538 layers (s01) went out with a display time one refresh after their poses
   (xrEndFrame always carried its own refresh's predicted time, as SteamVR expects). The Podman build ended
   each frame with the display time its pose was located for, and SteamVR repeated the frame itself.
3. **Cadence and head-locked content.** PrimedGun draws the helmet HUD into the projection layer with the
   head's unrotated projection (geometry shader paths `cstereo.w` -2 and -3), so whenever a frame is shown
   twice the compositor turns it with the world: the HUD moves on the second refresh. That held for the
   Podman build too (60 frames a second either way), so it does not explain the difference; it is the
   first suspect if the HUD alone still doubles.

The change: a frame is ended only for a refresh that shows a new eye set, and that refresh is the one its
poses were located for. The runtime's wake-up grid is kept as the largest "display time minus the time
`xrWaitFrame` returned" (a late call returns later than the wake-up, never earlier), so a late call made just
after the due refresh started returns that refresh. Mock, owner's settings, head at 150 deg/s:

| Run (60 s) | Layers/s | Layers at their poses' display time | 2-refresh holds | Stamps exact | Errors / unfinished releases |
| --- | --- | --- | --- | --- | --- |
| s01, first build | 121.3 | 4728 of 9538 (49.6 %) | 4712 of 4740 | 19064 of 19064 | 0 / 0 |
| s01, now | 61.0 | 4727 of 4755 (99.4 %) | 4717 of 4746 | 9510 of 9510 | 0 / 0 |
| s02, first build | 122.2 | 4674 of 9506 (49.2 %) | 4597 of 4689 | 18902 of 19012 | 0 / 0 |
| s02, now | 61.1 | 4716 of 4755 (99.2 %) | 4691 of 4745 | 9510 of 9510 | 0 / 0 |

Also with the owner's settings and the head at 150 deg/s: s02 soak, 300 s with the heavy room: 0 errors,
0 of 38245 releases unfinished, stamps 38266 of 38266, 19083 of 19133 layers at their poses' display time,
2-refresh holds 18916 of 19120, game 59.9 fps steady (minimum 56), no stall. `PRIMEFRAME_PACING=drain`
(s01, 60 s): every layer at its poses' display time but 4403 of 4753 landing late, holds 2: 4061, 1: 337,
3: 352; s02 drain held only 40 fps (the opening at IR 3). `PRIMEFRAME_NO_GPU_WAIT=1` still fails the
release check (5894 of 5894 changed after release).

`PRIMEFRAME_REPEATS=1` restores the first build's every-refresh frames for an A/B. On SteamVR, SteamVR's own
timing will now count about half the refreshes as reused app frames: that is expected. XRSTATS `layers`
reads about 60 and `late` should stay near 0; `late` near 60 a second would mean SteamVR's late
`xrWaitFrame` does not return the running refresh as assumed (each set then goes out a refresh after its
poses again).

## Default

`pipelined`: the only mode that does not serialise CPU and GPU (s02 +57 % over inline and drain), the only
one whose frames make their display time, and the cleanest cadence on s01; 0 unfinished releases in every
run with a release check that is shown to catch early releases. The headset session tests exactly this
(`PRIMEFRAME_PACING` unset). `PRIMEFRAME_PACING=drain` is the old path for an A/B, `inline` adds nothing over
it.

## Not known until the headset

Everything about the real compositor: whether SteamVR's `xrWaitFrame`, latch and reprojection behave like
the mock's model (late calls return at once, a layer needs about a refresh before its display), whether the
pacing thread's per-refresh `xrEndFrame` costs what the mock suggests, whether repeating the last layer on
the in-between refresh looks as smooth as expected, the GPU cost of SteamVR's compositor on top (about 0.8 ms
a refresh; s02 is already near the GPU budget), and whether skipping the desktop window present has any side
effect under SteamVR.
