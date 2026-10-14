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
  video thread goes on emulating. No CPU drain. Since the heavy room work (section "The heavy room"):
  the next frame's `xrWaitFrame` does not wait for the last frame's end, its `xrBeginFrame` follows that
  end on the releaser thread (`PRIMEFRAME_CYCLE_OVERLAP=0`: the next frame begins only once the last one
  ended, as before); the GPU gets the frame's draws every 100 draws (`PRIMEFRAME_KICK_DRAWS`, 0 off); the
  eye images are not cleared before the blit (`PRIMEFRAME_EYE_CLEAR=1` clears); and the releaser ends a
  finished frame no earlier than 16 ms after its wake-up, or, in a room whose frames finish near SteamVR's
  on-time limit, 22 ms after it so that every frame is shown a period later (`PRIMEFRAME_CYCLE_END`).
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
another display time than the one they were ended with (must be 0; poses located past it on purpose by
`PRIMEFRAME_PREDICT_EXTRA_MS` count as located for it), `extra_avg_ms` and `extra_frames` how far past their
display time the frames' poses were located (average over all ended frames) and how many were.

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
`predictedDisplayPeriod`, `ex` how far past the display time its poses were located. `empty` marks an end
without layers. XrTime and the steady clock are taken to be
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

## Prediction for the real display time (`PRIMEFRAME_PREDICT_EXTRA_MS`, off by default)

### What the headset session of 2026-10-14 (cycle, framesToThrottle 1, 120 Hz) showed

- SteamVR's XrTime is not the steady clock: `lead` in the log reads about 3990 ms. It runs with
  `CLOCK_MONOTONIC_RAW`: the difference to `CLOCK_MONOTONIC` was 3975 ms at the time of the check and grows by
  32 us a second (NTP's rate correction), and the logged leads grew by the same 0.03 ms a second. So SteamVR's
  real lead (its wake-up to the predicted display time) was about 36.5 ms at framesToThrottle 1, with a
  `predictedDisplayPeriod` of 16.667 ms.
- How late each `xrWaitFrame` call returned after SteamVR's wake-up (`x`: the largest lead of the last 20 s
  minus the call's lead) plus `r` (its return to `xrEndFrame`, about 16.5 ms: the game draws all frame long)
  matches SteamVR's own "poses to frame ready" second by second within about 0.5 ms. `x` is a sawtooth: the
  game's 59.94 Hz against SteamVR's 60 slots moves it by about 1 ms a second, from 0 to a whole slot in 16 s.
- Frames ready up to about 19.5 ms after the wake-up were shown on the predicted vsync (vsyncs to photons 2,
  one mispresent per frame: the repeat); frames ready later were shown a whole throttled period (16.7 ms,
  vsyncs 4, two mispresents) after it, a few two periods. Over the launch about half the frames were late
  (average shortfall 8.0 ms from the vsync counts), in its second half nearly all.
- So the head and controller poses were predicted 0 or 16.7 ms short of the refresh that showed them; the
  controller poses are located in the same `xrWaitFrame` (`UpdateInputActions`), right before the views.

### What the switch does

`PRIMEFRAME_PREDICT_EXTRA_MS=<ms>` locates the views and the controller poses (aim and grip spaces, which
PrimedGun uses for the arm cannon, the laser and aiming) of every frame that much past the display time its
`xrWaitFrame` returned; `auto` adds one display period for every period the frame is expected to be ready
later than `PRIMEFRAME_PREDICT_ONTIME_MS` (19.5) after SteamVR's wake-up: expected ready = `x` (the largest
lead of two 10 s windows minus this call's) + the average of `r`, capped at three periods. Replayed on the
session's log, auto's late frames match SteamVR's late frames per second within 3 % (47 % against 46 %).
`xrEndFrame` keeps the frame's own display time and exactly the poses the frame was drawn with; nothing else
changes (frame order, input sync). Only cycle, drain and inline (the pipelined loop picks its refresh itself).
SteamVR's compositor still turns each layer from its poses to the head at display time, so for the world the
switch mostly changes how far it has to turn; for the arm cannon and the laser, drawn into the frame, it is
the whole difference.

### Mock (owner's settings: IR 3, eye 2160, sysmem, `--stamp --head 150`, cycle, `MOCK_XR_THROTTLE=1`)

The mock set to the Frame's timing: `MOCK_XR_LEAD=4` (33.3 ms), `MOCK_XR_LATCH_MS=13.8` (on time when
ready by 19.5 ms after the wake-up), `MOCK_XR_LAND=throttled` (a late frame lands a whole period later).
Shortfall: the refresh a new eye set lands on minus the time its poses were located for.

| Run (60 s) | Game fps steady | Head shortfall avg ms | Located for the landing refresh (+-1 ms) | One period short | Locate call to landing avg ms | Controllers shortfall avg ms | Stamps exact | Errors, unfinished, end times off |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| s01 off | 59.9 | 11.05 | 1613 of 4756 | 3141 | 38.3 | 11.05 | 9508 of 9512 (4 unstamped) | 0, 0, 0 |
| s01 auto | 59.9 | -0.01 | 4653 of 4758 | 48 (55 a period or two early) | 38.5 | -0.01 | 9516 of 9516 | 0, 0, 0 |
| s02 off | 47.5 | 17.73 | 118 of 3819 | 3342 (359 more) | 42.5 | 17.73 | 7638 of 7638 | 0, 0, 0 |
| s02 auto | 47.4 | 0.10 | 3405 of 3768 | 189 (172 early) | 42.9 | 0.10 | 7536 of 7536 | 0, 0, 0 |

The latency itself (the locate call to the landing, 38 to 43 ms, plus the game frame for the arm cannon,
below) does not change: the switch only aims the poses at the refresh that shows them.

Overshoot (`MOCK_XR_HEAD_PROFILE=stops`: the head turns at 150 deg/s for 0.3 s and stops dead, 60 stops a
minute; the mock predicts at constant speed from the call, as a runtime might), yaw of each new eye set
against the head at the refresh it lands on, s01:

| Run | avg abs error | while turning | overshoot max | sets more than 0.5 deg past the head | trail max |
| --- | --- | --- | --- | --- | --- |
| off | 0.52 deg | 1.61 deg | 2.50 deg | 79 | 2.50 deg |
| auto | 0.20 deg | 0.37 deg | 5.00 deg | 141 | 2.50 deg |

At a dead stop a prediction made while turning swings past by the turning speed times the part of its
horizon after the stop: up to one more period (2.5 deg at 150 deg/s) with auto, for two or three frames
per stop. A real head decelerates, so less; SteamVR's compositor turns the world back at display either way.

### Not known

How SteamVR's own prediction behaves 53 ms ahead (its extrapolation, overshoot at stops); whether the
19.5 ms window holds in heavy rooms and at framesToThrottle 0; the arm cannon: PrimedGun writes the aim
pose into the game in `OnFrameEnd` on the CPU thread from the last `xrWaitFrame`'s poses, so the cannon is
drawn in the next game frame, about one more game frame (16.7 ms) after the pose's display time; the
switch does not cover that frame (it predicts for the frame begun with the pose).

## The heavy room: overlap, GPU kicks and the end time

Mock set to the Frame (`MOCK_XR_THROTTLE=1 MOCK_XR_LEAD=4 MOCK_XR_LATCH_MS=13.8 MOCK_XR_LAND=throttled`:
a frame finished up to 19.5 ms after SteamVR's wake-up for it is shown on its display time, a later one a
whole period later), owner's settings (IR 3, eye 2160, sysmem, VI at exactly 60 Hz, `--stamp --head 150`,
`PRIMEFRAME_PREDICT_EXTRA_MS=auto`), s02 in the player view next to the ship (`--state 02 --warmup 100`).

### Where the time went (cycle as it was)

Per frame, from the `XRC` line (below), 25 s of the player view: 45.7 frames a second.

| From the frame's `xrWaitFrame` return | ms |
| --- | --- |
| to the submit at the present (the video thread draws the frame) | 13.1 (p90 16.1) |
| the video thread's CPU time per frame | 13.3: it is busy the whole time, not waiting for the game |
| to the fence (GPU done) | 21.4: 8.3 ms after the submit, all of the frame's GPU work (Dolphin hands the GPU the whole frame at the present when nothing reads the EFB back) |
| to `xrEndFrame` | 21.5, ended late every frame |
| the video thread waiting at the next frame's first draw for this frame's end | 5.8 per frame |
| frame to frame | 21.9 |

So the frame rate went to the GPU tail: the next frame could not begin before this one ended, and it ended
8.3 ms after the video thread was done with it. The emulation (game CPU thread) ran at full speed
(game_vps 59.8); the game dropped frames because its GPU (the video thread) was late.

### What changed

| s02 player view, 20 s | Game fps | submit after the wake | GPU tail after the submit | wait for the last end | frame to frame |
| --- | --- | --- | --- | --- | --- |
| cycle as it was | 45.7 | 13.1 | 8.3 | 5.8 | 21.9 |
| no eye clear, GPU kicks every 200 draws | 58.2 | 13.0 | 3.3 | 1.6 | 17.2 |
| kicks every 100 | 58.6 | 13.0 | 3.0 | 1.2 | 17.1 |
| kicks every 50 | 58.6 | 13.3 | 2.8 | 1.2 | 17.1 |
| kicks every 100, overlap | 60.0 | 14.2 | 2.4 | 0 | 16.67 |

1. **No eye clear.** The blit covers the whole eye image (both eyes; the cinema screen blits one full image
   too), so the clear before it only cost GPU time: gpu_ms 9.1 to 9.3 with it, 8.4 to 8.7 without.
   `PRIMEFRAME_EYE_CLEAR=1` brings it back.
2. **GPU kicks** (`PRIMEFRAME_KICK_DRAWS`, default 100, in OpenXR only; `VertexManagerBase::OnDraw`): every
   100 draws the video thread hands the command buffer to the GPU (Dolphin's own background submit), so
   the GPU works while the game still draws and the tail after the present shrinks from 8.3 to about
   2.5 ms (the last draws, the XFB copy, the eye blits). 50, 100 and 200 draws differ by tenths of a ms.
   With 13 kicks a frame Dolphin's ring of 8 command buffers wraps within a frame; the video thread's
   waits for its own command buffers (`XRC fw`) stayed at 0.1 ms a frame even with the GPU at 12 ms
   (IR 4).
3. **Overlap** (`PRIMEFRAME_CYCLE_OVERLAP`, default on): the next frame's `xrWaitFrame` (at its first draw,
   on the video thread) no longer waits for the last frame's end; only its `xrBeginFrame` does: the releaser
   thread calls it right after the last frame's `xrEndFrame`, or the video thread right after the
   `xrWaitFrame` when nothing is left to end. The order SteamVR sees: wait N, begin N, ..., wait N+1,
   end N, begin N+1, ..., end N+1. Every frame still ends in its own `xrWaitFrame` cycle with that
   cycle's display time, the poses it was drawn with, released after its fence; the eye images of frame
   N+1 are still acquired only after frame N released them (one eye set).

That holds 60 in the heavy room. What it does not fix is the phase:

### The phase, and the end time

The game runs at exactly 60 Hz against SteamVR's 60 Hz slots, so every frame starts at the same phase `x`
after the wake-up of the slot that is running (the game decides when its first draw comes; a late
`xrWaitFrame` returns at once with that slot) and finishes `x` + about 16.5 ms after it (the frame's
draws take most of a period). `x` is whatever the launch gives (0 to 16.7 ms). Two s02 runs of the same
build: `x` 11.3 ms, every frame finished about 27.5 ms after its wake-up, shown a period late, all of
them (steady); `x` 1.3 ms, the frames finished 17.4 ms after the wake-up (p90 19.9), 12 % past the 19.5 ms
limit: on time and a period late mixed, an image held 4 refreshes and the next never shown (the headset
showed frames ended near that limit as doubled images, H2). A light room finishes early: s01 frames finished 8 ms after their wake-up (the
video thread waits in `xrWaitFrame`, then draws the queued frame in 3 to 4 ms).

Tried and dropped: moving the game's phase by holding the video thread back before its `xrWaitFrame`
(skip to the next wake-up, or hold to a later phase). The game snaps back to its own phase, so the
holds repeat and cost frames (54 to 59 fps, the mode flipping every few seconds).

What does it instead (`PRIMEFRAME_CYCLE_END=auto|asap|late`, with the overlap, releaser thread,
`CycleEndHold`): the releaser holds a finished frame (release and end, after its fence) until a fixed time
after SteamVR's wake-up for its display time (display time minus the lead of the last `xrWaitFrame` that
blocked):

- `asap`: no earlier than `PRIMEFRAME_CYCLE_EARLIEST_MS` (16) after the wake-up. The frames the headset
  showed clean ended 16 to 19.5 ms after it; the pipelined frames it doubled ended within 12 ms. The light
  room's frames now end at 16 ms instead of 8: the same display time, no added lag.
- `late`: 2.5 ms past the limit (22 ms). Every frame is shown a period after its display time, all of them;
  `PRIMEFRAME_PREDICT_EXTRA_MS=auto` sees the end come later than the limit and locates the poses for that
  refresh (mock: the landing refresh minus the time the poses were located for, 0.08 ms on average).
- `auto` (default): asap while 95 % of the last 120 frames finished 0.5 ms inside the limit, else late; back
  to asap when 99 % of the last 600 finished 1.5 ms inside it, at the earliest 10 s after the switch (twice
  that after each return to asap that lasted under 30 s, up to 160 s). One log line per switch
  (`PrimeFrame cycle end:`).

### Results

Frame-like mock, owner's settings, `--stamp --head 150`, the default build (cycle, overlap, kicks every 100,
no eye clear, end auto, prediction auto). Holds: refreshes each new eye set stayed on screen, whole session
(the s02 sessions include the 95 s opening and the load). Late: layer stacks the mock landed after their
display time; in the late mode every frame lands a period after it on purpose. Finished: XRF `rg`, the
`xrEndFrame` after SteamVR's wake-up for the frame (the hold included), player view.

| Run | Game fps | Late (mock) | Holds 2 / 4 / 5+ | End mode | Finished p50 / p90 ms | Poses' locate to landing avg ms | Landing minus pose time avg ms | Stamps exact | End times right | Errors, unfinished |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| s02 90 s, cycle as it was | 41.1 | 7979 of 7988 | 4621 / 3363 / 3 | (none) | 31.7 avg | 45.5 | -0.04 | 15976 of 15976 | 7990 of 7990 | 0, 0 |
| s02 90 s | 60.0 (steady min 59.2) | 11357 of 11357 | 11341 / 11 / 2 | late | 24.2 / 26.1 | 44.0 | 0.04 | 22714 of 22714 | 11359 of 11359 | 0, 0 |
| s01 90 s | 60.0 (steady min 59.3) | 8 of 6561 | 6553 / 3 / 1 | asap | 16.2 / 16.3 | 33.2 | -0.03 | 13122 of 13122 | 6563 of 6563 | 0, 0 |
| s02 soak 300 s | 60.0 (every second 59.4 or more) | 23952 of 23952 | 23926 / 16 / 3 | late | 22.3 / 24.8 | 45.8 | 0.05 | 47904 of 47904 | 23954 of 23954 | 0, 0 |
| s02 IR 4, 25 s (GPU 12 ms) | 60.0 | 7749 of 7750 | 7733 / 13 / 2 | late | 22.3 / 24.2 | | | | | 0, 0 |
| s02 30 s, `--nowait` | | | | | | | | | | 5909 of 5910 releases changed after release: FAIL, as it must |

- s02 holds 60 with the GPU at 8.6 to 9.4 ms a frame (IR 3) and at 12 ms (IR 4). The two s02 runs had
  different phases (the frames finished 24 and 21 ms after the wake-up before any hold); in both every
  frame landed a period after its display time, with the poses located for that refresh (prediction auto).
- s01 is as before (lane J's run: late 716 of 5362, holds 2 5355): every frame on its display time, now
  ended 16.2 ms after the wake-up (held 7.8 ms) instead of 8 to 11; the poses' locate to the landing is
  the same 33.2 ms as without the hold.
- Soak: no stall, no fatal line; GPU at most 72.8 C, CPU 89.2 C.
- Compared with the start of this work, the heavy room's latency stays where it was (44 to 46 ms from the
  locate to the landing; its frames landed a period late before too), the light room's too (33 ms).

### The `XRC` line

One per cycle frame, from the releaser thread (with `PRIMEFRAME_FRAME_LOG`, as `XRF`), ms from the
frame's `xrWaitFrame` return:

```
XRC n=7062 sub=14.15 gpu=17.26 end=17.42 vw=0.00 vcpu=13.79 vwall=16.55 q=0.02 hold=0.00 fw=0.08
```

`sub` the submit at the present, `gpu` the fence seen signalled, `end` the `xrEndFrame`; `vw` the video
thread's wait in this frame for the last frame's end (or, with the overlap, for its begin), `vcpu` and
`vwall` the video thread's CPU and wall time since the last submit, `q` the releaser picking the frame up
after the submit, `hold` how long the end was held (`CycleEndHold`), `fw` Dolphin's waits for its own
command buffers since the last submit.

### Not known until the headset

Whether SteamVR minds the new order (the next `xrWaitFrame` before the last `xrEndFrame`, the
`xrBeginFrame` from the releaser thread); whether the late mode looks as steady as the drain's late frames
did (it ends frames 22 ms after the wake-up, the drain 17 to 29); whether the 16 ms minimum matters (the
light room's frames ended 8 ms after the wake-up in the mock); the heavy room's CPU and GPU times with
SteamVR's compositor on the same GPU and CPUs; and the real phase between the game and SteamVR's slots,
which may also drift if SteamVR's refresh is not exactly 120 Hz (auto then switches as the finish times
move).

## Default

`cycle` with the overlap, the GPU kicks every 100 draws, no eye clear and `PRIMEFRAME_CYCLE_END=auto`,
`PRIMEFRAME_PREDICT_EXTRA_MS=auto` (the wrapper's default), framesToThrottle 1. The headset settled `cycle`
against `pipelined` on 2026-10-14 (cycle clean, every pipelined variant doubled).

## Next headset session: the heavy room (two launches, under a minute of play each)

Install this build (`install.sh --dev`), same SteamVR settings as the last session (120 Hz,
framesToThrottle 1 by the wrapper, motion smoothing off, eye 2160). `session.env`:
`PRIMEFRAME_STATE=2`; `session.env.next`: `PRIMEFRAME_STATE=1`.

1. Heavy room (s02). The state starts in the ship's flight to the frigate; after about 95 s Samus stands
   next to the ship with the HUD up: wait for that (no way to skip it without a new save state; once
   there, a save state in a free slot would make the next test start there). Then turn the head steadily
   left and right looking at an edge for ten seconds, then quick turns and stops: smooth at 60, or doubled
   edges, or a stutter now and then? Does it lag more than the light room?
2. Light room (s01): the same turns. Clean as in the last session, no new doubling, no more lag?

The logs must show, launch 1 in the player view: XRSTATS `game_fps` 59 to 60, `gpu_ms` about 9; one line
`PrimeFrame cycle end: late (held)` (or none if the frames fit: then `asap`); XRF `rg` steady at 22 to 25
(late) or 16 to 19.5 (asap), `ex` 16.67 in late; XRC `vw` 0, `fw` under 0.5, `gpu` minus `sub` 2 to 4;
vrtiming: 60 frames a second, presents >1 equal to frames, about one mispresent per frame, vsyncs to
photons the same every second (4 in late, 2 in asap). Launch 2: no `late` line after the load, XRF `rg`
about 16.2, vrtiming vsyncs 2 every second.

How it decides: both clean -> done. Launch 1 doubled or stuttering with vrtiming showing frames landing
on two different vsync counts -> the end mode against the real limit (`PRIMEFRAME_PREDICT_ONTIME_MS`, the
19.5 ms window, may differ in this room); launch 1 doubled with a steady vsync count -> the overlap's call
order (`PRIMEFRAME_CYCLE_OVERLAP=0`, which brings the heavy room back to about 46 fps). Launch 2 doubled ->
`PRIMEFRAME_CYCLE_EARLIEST_MS=0` (frames ended as soon as they are done), then `PRIMEFRAME_CYCLE_OVERLAP=0`.

## Earlier session plan: prediction (two launches, s01, done 2026-10-14)

1. `PRIMEFRAME_PREDICT_EXTRA_MS=auto`, 2. `PRIMEFRAME_PREDICT_EXTRA_MS=0` (today's default), both cycle
with `PRIMEFRAME_THROTTLE=1`, same SteamVR settings. In each: quick head turns and stops looking at a door
frame (does the world trail the head, or swing past and settle back?), then move the arm cannon quickly
and stop (does it follow the hand closely, trail, or swing past?); then which launch lagged less.

The logs must show, launch 1: XRSTATS `pose_off` 0, `extra_frames` about the frames vrtiming shows late in
the same second (two mispresents per frame, vsyncs 4) and 0 in seconds at vsyncs 2, `extra_avg_ms` 0 to
16.7; XRF `ex` 16.67 on frames with `r` plus their lateness above 19.5 ms. vrtiming as launch 2 (the
switch does not change when frames end). Launch 2: `extra_frames` 0.

How it decides: launch 1 lags less without swinging past -> auto becomes the default. Swings past at stops
-> a smaller fixed extra (`PRIMEFRAME_PREDICT_EXTRA_MS=8`) or off. No difference -> off stays (the
compositor's turn already hides it); the arm cannon then needs the extra game frame looked at.

## Earlier session plan (pipelined against cycle, done 2026-10-14)

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
