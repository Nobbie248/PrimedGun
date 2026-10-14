# Pacing

How the game hands its eye images to OpenXR, how that was measured headless, and what the numbers are.
The tables are from the mock runtime (`frame/tools/mockrun.sh`, `frame/tools/xrmock`); what the headset
showed is in the "Headset session" sections.

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

`PRIMEFRAME_PACING=cycle|pipelined|drain|inline` (default `cycle`; `PRIMEDGUN_XR_WAIT_GPU=1` without
`PRIMEFRAME_PACING` still means `drain`). In all of them, while the OpenXR session runs, the desktop window
is not drawn or presented (`PRIMEFRAME_WINDOW_PRESENT=1` keeps it); `SubmitFrame` submits the frame's work
and advances Dolphin's frame resources instead.

- `cycle`: one eye set; every game frame runs in its own `xrWaitFrame` cycle, in the drain's order as the
  runtime sees it: `xrWaitFrame`, `xrBeginFrame`, one `xrLocateViews` for the display time that
  `xrWaitFrame` returned, the frame's draws, the release, `xrEndFrame` with that display time and those
  poses, then the next frame's `xrWaitFrame`. The frame begins at its first draw that reads the head pose
  (the presenter arms it at the end of a present, OpenXRManager starts it inside the pose read; the
  present begins it itself when no draw did; `PRIMEFRAME_CYCLE_LAZY=0` begins it at the end of the last
  present, as the drain). At the present the video thread submits, puts a fence behind the work and hands
  the frame to a releaser thread, which waits for the fence, releases the images and ends the frame; the
  video thread goes on emulating, and the next frame begins only once the last one ended. No CPU drain.
- `pipelined`: two eye swapchain sets. A pacing thread owns `xrWaitFrame`/`xrBeginFrame`/`xrEndFrame`. The
  video thread renders frame N into the next free set, submits, puts a fence behind it and hands the set
  over (poses, swapchains, overlay quads, the display time its poses were located for); then it locates
  the pose for frame N+1 and goes on emulating. The pose target is the first slot of the runtime's grid
  the frame can make: its wake-up (display time minus the runtime's lead) after now + the game's
  pose-to-submit time + the GPU tail + `PRIMEFRAME_PACE_MARGIN_MS` (3), kept a fixed number of slots after
  the previous target when that is not earlier. The pacing thread releases a set's images as soon as its
  fence signalled (`PRIMEFRAME_EARLY_RELEASE=0`: at its slot), which frees the set for the game, and
  ends the frame on the slot its poses were located for, right after that slot's `xrWaitFrame` returned,
  with exactly the poses the set was drawn with. When the set is due on the very next slot the runtime
  hands out (SteamVR throttling the app to every second refresh) `xrWaitFrame` is called at once and
  blocks until the runtime's wake-up; when slots are skipped (60 sets a second on 120 Hz slots) it is
  called just after the due slot's wake-up, because a later call returns at once with the slot that is
  running. A slot handed out earlier than a set's poses (a wrong wake-up estimate) is begun and left
  unended (the next `xrBeginFrame` discards it; XRSTATS `discards`) instead of ending the last set with a
  display time its poses were not located for. The slot spacing is measured from consecutive display
  times of blocking `xrWaitFrame` calls; a blocking call also gives the exact lead. `PRIMEFRAME_REPEATS=1`
  is the first headset build's way, a frame every refresh repeating the last layer.
- `drain`: the old flow with one set; the submit itself waits for the GPU (the old `PRIMEDGUN_XR_WAIT_GPU`).
- `inline`: as drain, but a fence waited for right before the release.
- `PRIMEFRAME_NO_GPU_WAIT=1`: no GPU wait at all (cycle and pipelined: the video thread releases right
  after the submit). Only for the negative test of the mock's release check.

All OpenXR calls in which SteamVR's client submits on our queue (`xrBeginFrame`, `xrEndFrame`,
`xrAcquireSwapchainImage`, `xrReleaseSwapchainImage`) hold Dolphin's queue lock.

## Statistics line

One line a second, OpenXR log category, from the video thread:

```
XRSTATS mode=cycle submitted=60 layers=60 shown=60 late=60 gpu_waits=60 gpu_wait_max_ms=7.64 drain_avg_ms=0.00 drain_max_ms=0.00 gpu_ms=6.76 xrwait_avg_ms=0.13 xrwait_max_ms=0.17 set_waits=0 discards=0 end_avg_ms=18.86 end_max_ms=19.70 end_late=60 age_avg_ms=16.51 age_max_ms=17.35 lead_avg_ms=22.62 pose_off=0 eye=2160x2160 game_fps=59.9 game_vps=59.9
```

`submitted` eye sets the game finished, `layers` layer stacks ended, `shown` new sets among them, `late` new
sets shown after the refresh their pose was located for or ended more than a refresh after their
`xrWaitFrame`, `gpu_waits` releases that had to wait for the GPU and the longest wait, `drain_*` the time the
video thread itself waited for the GPU (drain and inline), `gpu_ms` GPU busy time per submitted set (kernel
DRM fdinfo `drm-engine-gpu`, whole process: game, eye blits), `xrwait_*` time blocked in `xrWaitFrame`,
`set_waits` times the game waited for a free eye set, `discards` slots begun and left unended (pipelined).
From the per-frame timing of the frames ended with layers: `end_*` the runtime's wake-up for the frame's
display time (its lead measured as below) to its `xrEndFrame`, `end_late` frames ended more than
`PRIMEFRAME_ONTIME_MS` (2.5) after it, `age_*` the poses' `xrLocateViews` call to the `xrEndFrame`,
`lead_avg_ms` display time minus the `xrWaitFrame` return, `pose_off` frames whose poses were located for
another display time than the one they were ended with (must be 0).

### Frame log

One `XRF` line per `xrEndFrame` (all modes; `PRIMEFRAME_FRAME_LOG=0` turns it off: at 60 lines a second the
log grows by about 20 MB an hour, so it is for test sessions), times in ms:

```
XRF n=3000 blk=0.18 lead=24.48 r=0.02 rg=0.51 age=28.32 pdt=0.00 dd=2 per=8.333
```

`blk` how long its `xrWaitFrame` blocked, `lead` the display time it returned minus its return, `r` its
return to the `xrEndFrame`, `rg` the runtime's wake-up for the display time (display time minus the lead
of the last blocking call, or the largest seen) to the `xrEndFrame` (what SteamVR's timing calls poses to
frame ready), `age` the poses' `xrLocateViews` call to the `xrEndFrame` (-1 unknown), `pdt` the poses'
display time minus the end's, `dd` slots since the last ended display time in display periods, `per` the
`predictedDisplayPeriod`. `empty` marks an end without layers. XrTime and the steady clock are taken to be
the same clock (both CLOCK_MONOTONIC on SteamVR and the mock); `lead` would show it if not.

## The mock

`MOCK_XR_SKIP=late` (mockrun's default now): a late `xrWaitFrame` returns at once with the refresh that is
running, display times stay in the future (as SteamVR on the Frame). A layer stack lands on the first refresh
at least one period (`MOCK_XR_LATCH_MS`) after its `xrEndFrame`, never before its display time; the summary
line `xrmock: cadence` gives the late layers and how many refreshes each new eye set stayed on screen.
`MOCK_XR_THROTTLE=1` hands out every second refresh (SteamVR's framesToThrottle). The `xrmock: latency` line:
for new eye sets, the poses' `xrLocateViews` call to the refresh they land on, and the `xrEndFrame` after
the wake-up of their display time (with how many came later than `MOCK_XR_ONTIME_MS`, 2.5: the headset
showed such frames in a 1-3 cadence, see "Headset sessions H2"). The mock does not model that cadence.

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

## Headset sessions H2: still doubled after the H1 fix, clean but lagging with the drain

Four short launches (light room, s01, 120 Hz, eye 2160, IR 3, sysmem). SteamVR's per-second timing
(vrtiming.log; "poses to ready" is SteamVR's time from the frame's wake-up to its `xrEndFrame`, "vsyncs" its
vsyncs to the first view) against the owner's verdict. Seconds at 50 to 70 frames, grouped by poses to ready:

| Launch | Loop, framesToThrottle | Poses to ready | Seconds | Presents >1 per frame | Mispresented per frame | Vsyncs | Owner |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 212604 | pipelined (H1 fix), 0 | < 1.5 ms | 5 | 0.82 | 1.29 | 1.08 | doubled |
| | | 2.5 to 12 ms | 19 | 0.52 | 1.81 | 1.40 | |
| 221215 | same, motion smoothing global | < 1.5 ms | 16 | 0.86 | 1.26 | 1.09 | doubled |
| | | 1.5 to 2.5 ms | 37 | 0.77 | 1.37 | 1.15 | |
| | | 2.5 to 12 ms | 69 | 0.58 | 1.83 | 1.39 | |
| 223413 | same, 1 | < 12 ms | 25 | 0.50 to 0.56 | 1.0 | 1.5 | doubled |
| | | about 16 ms | 3 | 1.00 | 1.00 | 2.0 | |
| 223501 | drain, 1 | 17 to 29 ms | 56 | 0.95 | 1.61 | 3.2 (2.0 at 17 ms, 4.0 at 25 ms) | clean, lag |
| GalaxyFrame | every refresh, 0 | 0.3 to 1 ms | | 0 (120 fps) | 0 | 1.00 | clean |

What the numbers say:

- At 60 frames on 120 Hz a clean 2:2 cadence is 1.0 presents >1 per frame and 1 mispresent per frame (the
  repeat). 0.5 with 2 mispresents is a 1-3 cadence: every second frame shown once, the next three times, and
  every present on another vsync than SteamVR predicted ("doubled or tripled"). The pipelined launches were
  1-3 whenever their frames ended later than about 2.5 ms after SteamVR's wake-up for them (and at
  framesToThrottle 1 even earlier), and close to 2:2 only below 1.5 ms. The drain, its frames always ended
  a whole frame after their wake-up, was 2:2 all the time (shown 2 to 4 vsyncs after, hence the lag).
- Poses to ready in the pipelined launches is a sawtooth (1 to 8-11 ms over about 8 s, then back): the
  ends followed the game, not SteamVR's refresh. The game waited for a free eye set in most frames
  (XRSTATS set_waits 30 to 60 in most seconds; 221215: 75 s with 40 or more had 0.61 presents >1 per frame
  and 4.2 ms poses to ready, 21 s with under 10 had 0.73 and 2.6 ms; the mock had none), and a waiting game made the pacing thread show
  the pending set at once: at the game's present, with the display time of the running refresh, not the
  one its poses were located for (XRSTATS counted only sets shown late, not early ones). The game's 59.94
  Hz against SteamVR's 60 slots moves that point by 1 ms a second (one refresh in 8.3 s): the sawtooth.
  In 221215 1-3 seconds also came with set_waits 0 (poses to ready 2.7 ms, max 5.8): the pacing thread's
  estimate of SteamVR's wake-up (the largest lead ever seen, from calls that never block) was late.
- H1 (repeats, a frame every refresh): SteamVR's own count (vrcompositor.txt, cumulative stats) was 9 and
  28 reprojected of 17788 and 8402 presents: it took every repeated layer as a new frame and showed it as it
  was. In H2 it reprojected 82, 80 and 51 % (pipelined) and 85 % (drain): the in-between refreshes are
  SteamVR's repeats now.
- Motion smoothing at the global setting never engaged (vrtiming motion 0) and changed nothing.

So the doubling after the H1 fix is the 1-3 cadence of frames ended late (after SteamVR's wake-up) but not a
whole refresh late; both causes are in the pacing thread: sets shown early, at the game's phase, for a waiting
game, and a wake-up estimate that never learns when SteamVR moves its wake-up. The drain is clean because its
frames are consistently a whole frame late (and lag for the same reason). What the logs could not tell:
whether a pipelined set ended within 1 ms of the wake-up for every frame looks clean in the headset (the
seconds below 1.5 ms were mostly 2:2, but the owner saw the launch as a whole); SteamVR's per-frame data
(which present belongs to which frame) was not logged.

### What changed after H2

- New default loop `cycle`: the drain's frame order as SteamVR sees it, without the CPU drain (see "What it
  does now"). In the mock it has the drain's latency: the game draws through the whole frame (the first
  draw that reads the head pose comes right after the last present), so beginning the frame at that draw
  instead of at the end of the present gains nothing measurable here (table below). What it gains is the
  CPU drain: the video thread goes on with the next frame while the GPU finishes.
- Pipelined: sets are freed for the game when their images are released (at their fence, before their
  slot), never shown early for a waiting game, a slot handed out too early is discarded rather than ended
  with the wrong poses, and when the due set is for the very next slot (SteamVR throttling the app to 60)
  the pacing thread blocks in `xrWaitFrame` for SteamVR's own wake-up instead of estimating it.
- Per-frame timing in the log (`XRF`) and in XRSTATS, so that a session shows, frame by frame, when each
  frame ended against SteamVR's wake-up and whether its poses matched its display time.

Mock, owner's settings (IR 3, eye 2160, sysmem, `ImmediateXFBEnable`, head at 150 deg/s, `--stamp`), 60 s:

| Run | framesToThrottle (mock) | Game fps, steady | Poses' locate call to the refresh shown, avg (min, max) ms | xrEndFrame after the wake-up: avg ms, later than 2.5 ms | Layers landing after their display time | 2-refresh holds (1 slot at throttle 1) | Stamps exact | Errors, unfinished releases |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| s01 drain | 0 | 59.9 | 29.2 (20.6, 270) | 20.9, all | 4419 of 4755 | 4085 of 4754 (334 1-3 flips) | 9510 of 9510 | 0, 0 |
| s01 cycle | 0 | 59.9 | 29.1 (19.7, 108) | 20.8, all | 4667 of 4759 | 4590 of 4758 (63 flips) | 9518 of 9518 | 0, 0 |
| s01 cycle | 1 | 59.9 | 28.8 (23.1, 110) | 23.3, all | 4275 of 4759 | 4583 of 4758 | 9518 of 9518 | 0, 0 |
| s02 drain | 0 | 40.3 | 37.6 (23.7, 267) | 29.3, all | 3154 of 3160 | 930 of 3159 | 6320 of 6320 | 0, 0 |
| s02 cycle | 0 | 46.9 | 33.1 (24.0, 93) | 24.9, all | 3797 of 3804 | 1890 of 3803 | 7608 of 7608 | 0, 0 |
| s01 pipelined | 0 | 59.9 | 57.0 (51.8, 307) | 0.57, 4 of 4704 | 0 | 4655 of 4703 | 9422 of 9422 | 0, 0 |
| s01 pipelined | 1 | 59.9 | 61.6 (51.7, 284) | 0.28, 4 of 4704 | 0 | 4672 of 4703 | 9414 of 9414 | 0, 0 |
| s02 pipelined | 0 | 59.9 | 58.3 (50.1, 271) | 0.63, 5 of 4701 | 0 | 4612 of 4700 | 9412 of 9412 | 0, 0 |
| s02 pipelined | 1 | 59.9 | 62.3 (49.9, 287) | 0.29, 8 of 4686 | 1 | 4640 of 4685 | 9378 of 9378 | 0, 0 |

Negative tests (`--nowait`, s02, 30 s): cycle 5922 of 5922 releases changed after release, pipelined 5900 of
5900: both FAIL, as they must. In the pipelined runs after the first ten seconds: no waits for a free set,
no discarded slots, 0 to 4 sets ended with poses for another display time (sets that missed their slot after
a hitch).

What the table says:

- Cycle and drain show the same poses equally old: about 29 ms from the `xrLocateViews` call to the refresh
  that shows them in the light room, of which the frame itself is about 16.5 ms (XRF `age`: the game draws
  from right after one present to the next). In the mock both land a refresh after their display time
  nearly every frame; on the headset the drain was 2 to 4 vsyncs after it. Beginning the frame at its
  first pose read does not make it later here. Cycle is faster than the drain in the heavy room (47 against
  40 frames a second) because the video thread does not wait for the GPU at the present, only for the last
  frame's end at the next frame's first draw.
- Pipelined predicts the pose for the refresh it is shown on, 50 to 62 ms ahead (the game's frame, the GPU
  tail, the 3 ms margin, SteamVR's lead of about 25 ms), and ends each set within about 0.6 ms of the wake-up
  (0.3 ms when `xrWaitFrame` blocks for the next slot at throttle 1), the window the headset showed clean.
  It holds 60 in the heavy room.

## Default

`cycle`: the drain's frame order (the only one the headset has shown clean with a 60 fps game, at
framesToThrottle 1) without the CPU drain. The next headset session decides whether `pipelined`, which shows
each frame on the refresh its poses were predicted for, can replace it (lower latency: the world does not
trail the head by the 2 to 4 refreshes the drain's frames came late).

## Next headset session (two launches, s01, under a minute each)

1. `PRIMEFRAME_PACING=pipelined`, `PRIMEFRAME_THROTTLE=1`. 2. `PRIMEFRAME_PACING=cycle`,
`PRIMEFRAME_THROTTLE=1`. Same SteamVR settings for both (120 Hz, motion smoothing off, eye 2160), so only the
loop differs. In each: turn the head steadily left and right for about ten seconds looking at an edge (a
door frame): doubled or tripled edges? Then quick turns and stops: does the world trail the head (lag) or
swing past and settle back (prediction overshoot)? Then which of the two lagged less.

The logs must show, launch 1: XRSTATS `discards` 0, `set_waits` 0, `pose_off` 0, `end_avg_ms` under 1 and
`end_late` near 0; XRF `blk` several ms (the call blocks for SteamVR's wake-up), `rg` under 1 ms, `pdt` 0,
`per` and `dd` (SteamVR's period and slot spacing at throttle 1, not known yet); vrtiming: presents >1 equal to
frames (60 of 60), mispresented about 60, vsyncs to photons the same every second. Launch 2: `pose_off` 0,
`end_avg_ms` 17 to 33, vrtiming like the drain's launch (60 of 60 reused, vsyncs 2 to 4).

How it decides: launch 1 clean with vrtiming at a steady 2:2 -> pipelined becomes the default (and
framesToThrottle 1 the wrapper's). Launch 1 doubled although vrtiming shows a steady 2:2 at the first vsync ->
SteamVR shows a 60 fps app cleanly only with frames ended in their own cycle: cycle stays. Launch 1 in 1-3 with
`rg` under 1 ms -> SteamVR's throttled slots and the targets disagree: XRF `per`/`dd` show how. Launch 2
doubled -> cycle differs from the drain on the headset (the frame begins at the first draw and a releaser
thread ends it): `PRIMEFRAME_CYCLE_LAZY=0`, then `drain`.

## Not known

Whether a pipelined set ended within a millisecond of SteamVR's wake-up looks clean (the H2 seconds below
1.5 ms were mostly 2:2, but the owner judged whole launches); SteamVR's period and slot spacing at
framesToThrottle 1 (the pipelined loop measures the spacing itself); whether SteamVR's prediction 50 to 60 ms
ahead overshoots when the head stops; how the cycle loop's order (`xrEndFrame` on the releaser thread,
`xrWaitFrame` and `xrBeginFrame` on the video thread) looks to SteamVR; and which present belongs to which
frame in SteamVR's timing (vrtiming.log is per second).
