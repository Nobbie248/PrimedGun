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
  eye images are not cleared before the blit (`PRIMEFRAME_EYE_CLEAR=1` clears). Since section "One
  landing": every frame is planned at its `xrWaitFrame` to land on the refresh after the last frame's,
  whatever refresh SteamVR handed it; a frame planned after its display time is held past the on-time
  limit (at most until the next frame's present) and its poses are located for the landing refresh, a
  frame planned on its display time is kept pending until the next frame's `xrWaitFrame` returned
  (`PRIMEFRAME_CYCLE_END=plan`, the default; `asap` is section "The wake-up"'s asap for an A/B).
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
display time the frames' poses were located (average over all ended frames) and how many were. Cycle
(section "The wake-up"): `early` frames whose `xrWaitFrame` returned more than 0.5 ms before SteamVR's
wake-up for their display time, `wake_lead_ms` SteamVR's lead (wake-up to display time; `?` until a call
blocked), `slot_ms` the spacing of the refreshes it hands out, `end_mode` plan or asap. Landing plan (section
"One landing"): `plan` frames planned to land 0 / 1 / 2 or more refreshes after their display time,
`plan_breaks` frames that had to land a refresh later than the one after the last frame's (one image shown
twice; a game frame late), `plan_catchups` frames moved a refresh earlier after 5 stable seconds and
`plan_drops` frames landed on the last frame's refresh because holding them would have reached the next
frame's present (the last frame is not shown in both), `hold_cuts` holds ended early because the video
thread already waited for the eye images.

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
`predictedDisplayPeriod`, `ex` how far past the display time its poses were located, `w` its `xrWaitFrame`
return minus SteamVR's wake-up for its display time (negative: handed out early), `pl` the refreshes it was
planned to land after its display time (cycle end late). `empty` marks an end without layers. `lead` is
display time minus the steady clock: SteamVR's XrTime is CLOCK_MONOTONIC_RAW (section "Prediction for the
real display time"), so it reads about 4 s; `rg` and `w` convert (section "The wake-up").

## The mock

`MOCK_XR_SKIP=late` (mockrun's default now): a late `xrWaitFrame` returns at once with the refresh that is
running, display times stay in the future (as SteamVR on the Frame). A layer stack lands on the first refresh
at least one period (`MOCK_XR_LATCH_MS`) after its `xrEndFrame`, never before its display time; the summary
line `xrmock: cadence` gives the late layers and how many refreshes each new eye set stayed on screen.
`MOCK_XR_THROTTLE=1` hands out every second refresh (SteamVR's framesToThrottle). The `xrmock: latency` line:
for new eye sets, the poses' `xrLocateViews` call to the refresh they land on, and the `xrEndFrame` after
the wake-up of their display time (with how many came later than `MOCK_XR_ONTIME_MS`, 2.5: the headset
showed such frames in a 1-3 cadence, see "Headset sessions H2"). The mock does not model that cadence.
`MOCK_XR_EARLY=3` (mockrun `--frame`) is SteamVR's hand-out as session M showed it and `MOCK_XR_PHASE_MS`
(mockrun `--phase`) puts the game's `xrWaitFrame` calls at a given time after a wake-up (section "One
landing"); an `xrmock: window` line every 10 s gives the holds, the frames never shown, the landings on
and after the display time and the hand-outs of those seconds.

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

## The wake-up: what the heavy-room session showed, and asap against late

### The session (2026-10-14, the heavy room build)

Two launches, cycle with the overlap, kicks, end auto, prediction auto, 120 Hz, framesToThrottle 1, eye
2160, IR 3. The owner: "performance much better! However the HUD was shaky and jittery. There were lag
spikes occasionally." No doubling. Heavy room (s02) 59.4 fps on average, 9 seconds under 57 (lowest 50);
the end mode switched 7 times; extra prediction on about 56 frames a second. Light room (s01): late (held)
from the load to the end, extra prediction on about 59 frames a second.

### Our wake-up was wrong, SteamVR's own timing says so

The cycle end and the prediction both measure a frame against SteamVR's wake-up for its display time,
which the heavy room build took as the display time minus the largest lead (display time minus
`xrWaitFrame` return) seen. Joined second by second with SteamVR's vrtiming.log:

| Launch, mode | Our finish after the wake-up (XRF rg) | Lead (log units) | SteamVR poses to frame ready | SteamVR vsyncs to photons | Our extra |
| --- | --- | --- | --- | --- | --- |
| light, late (most of it) | 22.3 ms | 46.5 to 47.5 | 6.3 to 7.4 ms | 2.00 (on time) | 22.2 ms |
| light, late (44:48 to 44:55) | 22.4 ms | 31.3 to 31.7 | 20.6 to 20.8 ms | 4.00 (a period late) | 22.2 ms |
| heavy, late | 22.2 to 22.4 ms | 30 to 39 | 12 to 15.7 ms | 2.00 (on time) | 22.2 ms |
| heavy, asap (42:36 to 42:41) | 16.3 ms | 18.4 to 18.6 | 24.5 to 25.6 ms | 3.8 to 4.0 (a period late) | 0 |

Everything was inverted: frames SteamVR showed on time had their poses located 22.2 ms past their display
time, frames it showed a period late had none. Replayed frame by frame with the right wake-up, 97 % (heavy)
and 94 % (light) of the frames had the wrong extra. That is the HUD shake: SteamVR turns the whole
projection layer, the head-locked HUD included, from the submitted poses to the head at display, so every
frame predicted a period wrong moves the HUD by the head's turning speed times 16.7 to 22.2 ms (2.5 to 3.3
degrees at 150 degrees a second), and the frames switched between right and wrong with the mode and with
SteamVR's hand-out.

Why:

1. **SteamVR hands out the next refresh early.** With the overlap the next `xrWaitFrame` comes while the
   last frame is still pending (begun, not ended). SteamVR then returns at once, often with a refresh whose
   wake-up is still 6 to 15 ms ahead: the lead looks one period longer than its real lead minus how late the
   call came (light room 46.7 against 31.9, heavy 39.1 against 29.1). Such frames are on time for that
   refresh (SteamVR: poses to ready 6 to 15 ms, vsyncs 2). The largest lead took those leads for the wake-up,
   so it saw every such frame finish 22 ms after it: the light room stuck late, and the extra added a period
   to frames already aimed at the right refresh.
2. **SteamVR's lead is exact only from calls that block.** `xrWaitFrame` blocked (over 2 ms) 14 times in
   the heavy launch and 3 times in the light one; each gave the same lead, and the vrtiming poses to ready
   matches our finish measured from that lead within about 1.5 ms in every regime.
3. **SteamVR's XrTime is CLOCK_MONOTONIC_RAW** (section "Prediction for the real display time"): against the
   steady clock the lead grows by about 33 us a second, 2 ms a minute.
4. **predictedDisplayPeriod is not the refresh spacing.** SteamVR returned 16.667, 22.222, 33.333, 44.444
   and 50 ms (22.222 in 92 % of the frames); the display times step by 16.667 ms (throttled 120 Hz). The
   extra used the 22.222.
5. **Frames for the same refresh.** 184 frames in 180 s (heavy) and 193 in 75 s (light) were ended for the
   same display time as the one before (`dd=0`), in bursts of up to 30 a second, nearly all where our end
   came 5 to 7 ms after the next call (the late hold) or the video thread waited for the last frame's begin.

The real lead (wake-up to display, CLOCK_MONOTONIC_RAW) at framesToThrottle 1, 120 Hz: about 37.5 ms.

### The lag spikes

- Mode switches: every switch to late moved SteamVR to early hand-out within a second, every switch to
  asap back; each cost a few frames ended for the same display time and 1 to 3 seconds of SteamVR
  mispresents. They came from the wrong wake-up (the light room never left late).
- The heavy room's dips (40:34 50 fps, 40:37, 41:25 to 41:43, 42:01 to 42:07 51 to 57) are the video thread
  itself: CPU time over 16 ms in 7 to 26 frames of those seconds, and Dolphin waiting for its own command
  buffers (XRC `fw`) 50 to 166 ms in those seconds against under 1 ms in clean ones; the GPU did 5.4 to
  7.0 ms a frame then (4.9 median), never full. The launch started with an empty shader cache (`Loaded 0
  cached shaders`): the first pass through a room compiles shaders on the CPUs the video thread needs. The
  cache is filled now (3.5 MB of pixel shaders after these launches). Not pacing; any end mode has them.
- The GPU tail depends on the phase: work submitted 0 to 4 ms after SteamVR's wake-up finished 6.2 ms after
  the submit, at 7 to 16 ms 1.4 to 2.3 ms. Something SteamVR does at its wake-up (probably the compositor)
  holds our queue about 5 ms; early hand-out puts our submit right there.

### What changed

- **The wake-up** (`OpenXRManager::UpdateWakeGrid`): the XrTime clock is chosen at the first frame (the one
  whose lead is plausible; CLOCK_MONOTONIC_RAW when both are; `PRIMEFRAME_XR_CLOCK=raw|steady`); the lead is
  kept in XrTime and converted with the offset of the moment. It is the largest lead of `xrWaitFrame` calls
  that blocked over 2 ms with no frame pending (a blocked call returns at the wake-up or a little after it;
  one made while a frame is pending may return early); a shorter one is taken only when every such call of
  a 10 s window (at least three) was over 1 ms shorter. Until a call blocked, the largest lead seen; a lead
  calibration at the first cycle frames gets a blocking call (it ends what is pending and, if the call still
  returns at once, spends one empty frame; every 2 s, five times at most). Early: a frame whose call
  returned more than 0.5 ms before the wake-up (XRSTATS `early`, XRF `w` negative).
- **The refresh step** (`slot_ms`): the smallest recurring step between display times, not
  predictedDisplayPeriod. The extra prediction counts these steps.
- **asap waits for the last end.** In asap the next frame's `xrWaitFrame` waits for the last frame's end
  (`OpenXRManager::StartCycleFrame`; `PRIMEFRAME_CYCLE_ASAP_WAIT=0` keeps the overlap), so SteamVR blocks it
  until its wake-up and the poses are located then: in the light room the overlap only made SteamVR hand
  out the refresh early, the same refresh with the poses located 7 to 15 ms earlier (a longer prediction,
  more HUD shake). A room that needs the overlap is late. The asap minimum
  (`PRIMEFRAME_CYCLE_EARLIEST_MS`) is 0: frames end as soon as their GPU work is done.
- **late keeps the overlap and plans every frame's landing** at its `xrWaitFrame`
  (`OpenXRManager::WaitFrame`; XRF `pl`): expected ready = its return after the wake-up (negative when
  handed out early) plus the average return-to-fence time of frames of its kind (early or not). A frame
  that came after the wake-up and is expected within 2 ms of the limit, or an early one expected past it,
  is planned to land a refresh late: its poses are located for that refresh and the releaser holds it to
  2.5 ms past the limit, so that no such frame straddles the limit. A frame handed the refresh the last
  frame lands on is planned late too (else it replaces that frame, which is then never shown): a late run.
  Once 15 frames in a row were handed out at least 2 ms early, the run ends (that late frame is not shown;
  at most once a second; log line `PrimeFrame cycle: frames handed out early end a late run`). A frame
  expected on time is kept pending until the next frame's `xrWaitFrame` returned (at most until 1 ms inside
  the limit): ended before it, the next call would block for the wake-up and start that frame too late
  for a room whose frames take most of a period; pending, SteamVR hands the next frame its refresh early
  and it is on time too.
- **The switch** (`PRIMEFRAME_CYCLE_END=auto`, `CycleEndHold`): asap -> late when the 95th percentile of
  2 s of finish times (fence after the wake-up) comes within 0.5 ms of the limit; late -> asap after 5 s
  and the dwell when the video thread's CPU time per frame plus 2.5 ms is 3.5 ms inside a refresh and the
  expected finish of a blocked start (the later of the frame's submit after the wake-up and its video
  thread CPU time, plus 2.5 ms) 2.5 ms inside the limit, both 95th percentiles of 5 s. Dwell 4 s, doubled
  after each return to asap that lasted under 30 s, up to 160 s. One log line per switch.
- xrmock: `MOCK_XR_CLOCK=raw` (XrTime on CLOCK_MONOTONIC_RAW), `MOCK_XR_EARLY=1` (a call made while a
  frame is pending before the next refresh's wake-up returns at once with it), `MOCK_XR_EARLY=2` (also one
  made after that wake-up gets the refresh after it: what SteamVR seems to do, "often with the refresh after
  the running one"; which one SteamVR does is not known); summary line `waits`. mockrun `--frame` sets the
  whole Frame-like mock (`MOCK_XR_EARLY=1`).

### What latency each room can have

Locate to display = SteamVR's lead (33.3 in the mock, about 37.5 on the Frame) plus how early the poses are
located before the wake-up. asap: located at the wake-up, so the lead alone. late: the frames take most of a
period from their first draw (the video thread is busy all frame), so the game's phase decides: a frame
whose first draw comes x ms after a wake-up is shown on the next refresh, located 16.7 - x ms before its
wake-up, whether SteamVR hands it out early or we plan it late; both give lead + 16.7 - x. Only a first
draw within about 2 ms after a wake-up could make the earlier refresh (the frames take 17 to 21 ms from
it), and such frames would straddle the limit; the late plan gives them the next refresh too. So in a heavy
room the prediction is between the lead and the lead plus a period, set by the game's phase, and the
switch to asap is what shortens it wherever the room allows.

### Results

Frame-like mock (`mockrun.sh --frame`: throttled, lead 4, latch 13.8, late frames a period later, XrTime on
CLOCK_MONOTONIC_RAW, `MOCK_XR_EARLY=1`), owner's settings (IR 3, eye 2160, sysmem, VI at 60, prediction
auto, `--stamp --head 150`). SteamVR was idling on the machine through all of these runs (about one core)
and the CPU ran at about 80 C: the video thread's CPU time per frame in s02 was 14.6 to 16.1 ms against
12.3 in the heavy room section's runs, so the frame rates below are lower for every build (the heavy room
build run under the same conditions: 59.2 fps). Shortfall: the refresh a new eye set lands on minus the
time its poses were located for.

| Run | Game fps (lowest second) | End mode, switches | Extra / early frames a second | Locate to landing avg ms | Shortfall within 1 ms | One period short | Holds 2 / 4 / 5+, never shown | Stamps exact, end times right, errors |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| s01 60 s, the wake-up commit only | 60.0 (60.0) | asap, 0 | 0 / 47.6 | 40.1 (3440 of 4758 calls handed out early) | 4735 of 4755 | 1 | 4748 / 2 / 1, 3 | 9510 of 9510, 4758 of 4758, 0 |
| s01 90 s | 60.0 (59.1) | asap, 0 | 0 / 0 (6231 of 6559 calls blocked) | 33.1 | 6541 of 6555 | 0 | 6549 / 2 / 1, 2 | 13110 of 13110, 6558 of 6558, 0 |
| s02 60 s, the heavy room build | 59.2 (57.9) | late | 59.7 / (not counted) | 41.0 | 253 of 9464 (9208 located 1 to 2 periods too far) | 0 | 9187 / 181 / 4, 91 | all, all, 0 |
| s02 90 s, the wake-up commit only | 58.8 (56.2) | late | 45.7 / 14.0 | 45.5 | 10380 of 11198 | 779 | 9689 / 827 / 3, 678 | all, all, 0 |
| s02 90 s | 58.7 (57.3) | late, 1 (at the start) | 59.1 / 9.8 | 46.6 | 10879 of 11202 | 302 | 10702 / 327 / 2, 170 | 22404 of 22404, 11205 of 11205, 0 |
| s02 60 s, `MOCK_XR_EARLY=2` | 59.5 (58.2) | late, 1 | 3.7 / 59.3 | 51.0 | 9335 of 9485 | 144 | 9243 / 157 / 2, 82 | all, all, 0 |
| s02 soak 300 s | 57.5 (54.5) | late, 1 | 56.5 / 6.6 | 43.8 | 22220 of 23139 | 912 | 21646 / 1154 / 2, 336 | 46278 of 46278, 23142 of 23142, 0 |
| s02 30 s, `--nowait` | | | | | | | | 15305 of 15309 releases changed after release: FAIL, as it must |
| s01 30 s, `PRIMEFRAME_HUD_TEST=1` | | asap | | | 2952 of 2954 | | | the VIEW-space quad on all 2957 frames, 0 errors |

(Holds and never shown are for the whole session, the s02 ones with the 95 s opening and the load.)

- Light room: asap from the load to the end, every `xrWaitFrame` blocking for the wake-up, no extra, the
  poses located 33.1 ms before the landing instead of 40.1 (the mock's lead is 33.3; on the Frame the
  session's light room was handed out about 15 ms early, so there the difference should be larger).
- Heavy room: late from the start and no switch after it. The game's first draw came a few ms after the
  wake-up (XRF `w` 1.7 median), so most frames were late calls planned a refresh late (`pl=1`, extra
  16.67); 97 % landed on the refresh their poses were located for, against 3 % with the heavy room build
  in the same mock. With `MOCK_XR_EARLY=2` (SteamVR's hand-out as the session suggests) nearly every frame
  was handed out early and on time, with no extra (3.7 frames a second). Late runs ended 10 times in the
  s02 run, 9 of them in the opening flight; 30 in the soak's 400 s.
- Dips: in every s02 run the seconds under 59 fps are the video thread's own CPU time (84 to 93 % of the
  frames over 17.5 ms had over 15.5 ms of CPU; its wait for the last frame, XRC `vw`, was 0); in the soak
  that CPU time rose from 14.7 to 16.1 ms a frame over the five minutes (heat), and the rate with it.

### Not known until the headset

Which hand-out SteamVR does for a call made while a frame is pending after the next refresh's wake-up
(`MOCK_XR_EARLY=1` or `2`; the session suggests 2, the late plan handles both); whether the light room's
calls block for the wake-up on the Frame as in the mock (XRF `blk` several ms, `w` near 0) and how much
steadier that makes the HUD; how often late runs start and end in the heavy room and whether a run's end
(one frame not shown) is visible; the heavy room's phase on the Frame (it sets the prediction between
37.5 and 54 ms).

## The HUD: why it shakes, and a layer of its own (design; only a test quad built)

The helmet and the 2D HUD are drawn head-locked into the projection layer (geometry shader paths
`cstereo.w` -2: 2D elements on a head-locked screen, -3: the game's perspective HUD models placed in head
space; both use the unrotated per-eye projection). SteamVR turns the whole projection layer from the poses
it was submitted with to the head's pose at display. The world is meant to be turned; the HUD is not: any
difference between the predicted and the real head pose moves the HUD by that difference. In the session
the prediction was a period wrong on nearly every frame (section "The wake-up") and switched between right
and wrong, which is the shake the owner saw; with the wake-up fixed the HUD moves only by SteamVR's own
prediction error over the lead (about 37 ms in asap, up to 54 ms in late), which a longer prediction makes
larger: asap's shorter prediction in light rooms matters for the HUD too.

A HUD that does not move at all needs its own composition layer in `XR_REFERENCE_SPACE_TYPE_VIEW`, which
SteamVR composes head-locked without turning it:

- **Render target.** A second EFB-sized two-layer target (RGBA8 color, cleared to transparent black each
  frame, and its own depth), the same formats as the EFB so Dolphin's Vulkan pipelines stay compatible. In
  `VertexManagerBase::Flush`, a draw whose `cstereo.w` is -2 or -3 (known from GeometryShaderManager before
  the draw) binds the HUD target, any other draw the EFB. Prime draws the HUD after the world, so a frame
  has one or two switches (render pass breaks; cheap on Turnip in sysmem).
- **Blending.** The layer must be premultiplied alpha. Alpha blending (source alpha, inverse source alpha)
  and additive draws map directly (the alpha channel written as coverage: a separate alpha blend
  1, 1 - source alpha, and 0, 1 for additive); multiplicative or subtractive HUD draws, and any that
  read destination alpha, cannot be represented over a transparent target and would differ (the EFB has
  no usable alpha to start from). Each HUD draw needs a pipeline variant with the alpha blend changed:
  more pipelines to compile once.
- **Depth.** The -3 models depth-test among themselves against their own depth (the game clears depth for
  the HUD); nothing from the world reaches them. World-locked things drawn after the HUD (lock-on reticles,
  scan markers if they are not head-locked) would end up under the HUD layer; most of the HUD is
  transparent, so that would show only where they overlap.
- **Visor effects.** Effects that copy the EFB (thermal, X-ray, the scan visor's darkening, a pause
  screen background taken from the frame) would not see the HUD; the XFB copy that the eye blit reads
  would not contain it either, so the HUD target is blitted into its own swapchain at the present, with
  the gamma and brightness of the XFB copy applied by hand.
- **Submission.** Two more single-level swapchains (or one two-layer one) acquired, waited, released and
  fenced with the eye set (the cycle's release rules apply to them too), and a second
  `XrCompositionLayerProjection` with `space` = a VIEW reference space, each view's pose the eye's pose
  relative to the head (from `xrLocateViews` in the VIEW space) and the frame's field of view, layer flags
  `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`. xrmock needs VIEW-space projection layers and its
  stamp and release checks for the second layer.
- **Cost.** GPU: a 2160 x 2160 x 2 clear and blit (about 0.5 to 1 ms) and SteamVR composing one more full
  layer (about 0.3 to 0.5 ms), against 5 to 7 ms a frame in the heavy room on the Frame. CPU: small.
- **Risks.** How SteamVR treats a VIEW-space projection layer is not known: if it turns it anyway (it
  could place it with the predicted head pose and reproject), nothing is won; a quad layer in VIEW space
  is the fallback but is mono (no HUD depth). The blend modes above, and every visor and menu screen, need
  checking in the headset.
- **Estimate.** Three to five days: the target and the redirection (VertexManagerBase, FramebufferManager:
  shared headers, a wide rebuild), the blend variants, the swapchains and the layer in the cycle and the
  other loops, xrmock, then headset rounds for SteamVR's behaviour and each visor. Not built: it is
  neither small nor clean (shared headers, pipeline variants, visor effects), and the first thing to learn
  is whether SteamVR keeps a VIEW-space layer still.
- **The test that is built.** `PRIMEFRAME_HUD_TEST=1` (cycle): a white ring with a cross, 8 cm wide, 1 m
  ahead and 12 cm below the centre of view, in a quad layer in a VIEW reference space
  (`VulkanOpenXR::AppendHudTestLayer`, `OpenXRManager::GetViewSpace`). In the headset: if it stays
  perfectly still while the game's HUD moves when the head turns, the layer is worth building.

## One landing: what sessions L and M showed, and the landing plan

### The sessions (2026-10-14, lane L's build)

Two pairs of launches, heavy room (s02) then light room (s01), 120 Hz, framesToThrottle 1, eye 2160, IR 3.
Session L (lane L's defaults, asap with its wait): "maybe felt less smooth and occasional spikes"; heavy
56.3 fps (56 s under 57), light 57.6 (28 s under 57). Session M (`PRIMEFRAME_CYCLE_ASAP_WAIT=0`): "HUDs
good, but more lag spikes, especially the second launch"; game fps fine (heavy 59.5, light 59.6). The HUD
was steady in both (lane L's wake-up). The spikes in M are changes of SteamVR's cadence, second by second
in vrtiming.log; frame by frame in the `XRF`/`XRC` lines of the last launch in each dolphin.log.

### How SteamVR hands out refreshes

From every frame of session M (about 11800), the refresh `xrWaitFrame` returned against what was going on
at the call. Pending: the last frame begun and not yet ended (the overlap; the releaser holds it).

| At the call | SteamVR returned | Frames |
| --- | --- | --- |
| a frame pending for the running refresh, its GPU work not done | the next refresh at once, early | 99 % of 4900 |
| a frame pending for the running refresh, its GPU work done (held) | that same refresh again (`dd=0`) | 75 % of 448 (light); 32 % of 184 (heavy) |
| a frame pending for a refresh still ahead | that same refresh again | all 400 (light) |
| a frame pending for the refresh before the running one | the running one (heavy) or the next one (light) | 2178 of 2203 (heavy); 114 of 136 (light) |
| none pending, the last frame ended after its on-time limit | the refresh after the one it lands on, at once, early (`dd=2`) | 98 % of 861 (light), 64 % of 430 (heavy; the rest mostly the running one) |
| none pending, the last frame on time | blocks until the next wake-up (session L) | |

So the refresh a frame gets depends on our own holds and on whether its GPU work is done, not only on the
time of the call; and the game called at nearly the same time after a wake-up all launch long: light room
13 to 15 ms (2 to 3 ms before the next wake-up), heavy room 0 to 1 ms (4900 of 7730 frames) or 9 to 14 ms.

### What went wrong

1. **Light room, the 1-3 stretches.** With the calls 2.7 ms before a wake-up, the frames alternated: one
   handed the next refresh early (`w` -2.9) and on time (finished 11.7 ms after its wake-up), the next
   handed that same refresh again 13.5 ms after its wake-up (the first one pending, its GPU work done) and
   planned a refresh late. Clean (vrtiming 33:00 to 33:03: vsyncs 2.00, 60 reused, 60 mispresented), but
   the display times stepped by 0 and 33.3 ms only, so after 64 such steps the refresh step (`slot_ms`)
   read 33.33: every frame's extra prediction became 33.3 ms and the late-run rule planned both frames of
   a pair onto one refresh. The two 1-3 stretches (33:04 to 33:07 and 33:22 to 33:27: 30 reused, 90
   mispresented, vsyncs 1.00) are exactly the seconds with `slot_ms=33.33`; each ended with "frames handed
   out early end a late run" (the run end resets the alternation, the step goes back to 16.67).
2. **Light room, never back to asap.** The return condition took the frame's submit after its wake-up as
   what a blocked start would give; frames handed the running refresh 13.5 ms after its wake-up counted as
   finishing 27 ms after it. Its 95th percentile, per 10 s from the load: 19.5, 19.5, 26.2, 29.6, 30.8,
   29.4 ms against the 17.0 needed. With the asap wait off nothing else could bring the frames back.
3. **Heavy room, v2 and v4.** Calls 0 to 1 ms after a wake-up give two states that both last: a frame
   handed the running refresh, planned a refresh late and held, so that the next call finds it pending and
   gets the running refresh again (vsyncs 4); or a frame handed the next refresh 16 ms early and on time,
   kept pending until the next call, which then gets the next refresh early again (vsyncs 2). Both land on
   the same refresh. The changes between them cost frames: 9 late-run ends (each replaces a frame) and late
   starts (an image shown twice) every few seconds from 30:00 to 31:31.
4. **Heavy room, the drops 31:18 to 31:31** (52 to 57 fps): Dolphin waiting for its own command buffers
   (`XRC fw` 50 to 100 ms a second against under 1 in clean seconds), video thread CPU time normal: not
   pacing (as in section "The wake-up").

The mock never showed any of it: with `MOCK_XR_EARLY=1` a pending frame always got the next refresh, and
its random phase did not put the calls next to a wake-up.

### The mock now

`MOCK_XR_EARLY=3` hands out refreshes by the table above (the GPU work of the pending frame is "done" when
its first eye image's memory has changed since the app waited for it; the app blits the eyes last; a frame
handed out more than 4 ms after its wake-up counts as handed out late) and `MOCK_XR_PHASE_MS=<ms>@<s>` moves
the mock's refresh grid once, at that session second, so that the game's call comes that long after a
wake-up (14 for the light room, 0.5 for the heavy room, from the session). mockrun `--frame` uses
`MOCK_XR_EARLY=3`, `--phase` sets the phase. The head moves on the session clock, so a grid move does not
change the poses. With lane L's build and session M's switch (`PRIMEFRAME_CYCLE_ASAP_WAIT=0`):

| Run | What it showed |
| --- | --- |
| s01, phase 14 ms, 90 s | `slot_ms` 33.33 from 7 s on, end mode late from 7 s and never back, extra 33.3 on every frame; every second new image never shown (2475 of 5350, held 4 refreshes 2476): 30 new images a second, the session's 1-3 at its worst |
| s02, phase 0.5 ms, 160 s | 8 late-run ends; per 10 s after the phase move 259 to 300 frames landed a period late and the rest on time (v2 and v4 mixed), 6 to 43 new images never shown |

### The landing plan (`PRIMEFRAME_CYCLE_END=plan`, the default)

The refresh a frame is shown on is all that matters for the cadence; which refresh SteamVR handed it, and
whether SteamVR counts it as on time, is bookkeeping. So the cycle loop keeps one rule
(`OpenXRManager::WaitFrame`, at each frame's `xrWaitFrame`, with the overlap always on):

- **Continuity.** The frame lands on the refresh after the last frame's landing, whatever SteamVR handed
  it: the next refresh early, the running one, the same one again, one further. A frame handed a refresh
  before its landing is held by the releaser to 2.5 ms past the on-time limit of the refresh before its
  landing (`CycleEndHold`), and its views and controller poses are located for the landing refresh (with
  `PRIMEFRAME_PREDICT_EXTRA_MS=auto`, the wrapper's default). A frame landing on its own display time is
  kept pending until the next frame's `xrWaitFrame` returned, as before.
- **Later only when it must.** When the frame cannot make that refresh (its call after the wake-up of its
  display time plus the 95th percentile of the last 120 frames' return-to-finish time, later than 1 ms
  inside the 19.5 ms limit), it lands a refresh later: the last image is shown twice (log `PrimeFrame cycle
  landing: 1 refresh later`, XRSTATS `plan_breaks`). In the mock these came with the game's own late frames.
- **Earlier only while settling.** Within 40 s of the start or a load (a call over 100 ms after the
  last), when every frame of the last 5 s had 3 ms to spare a refresh before its landing, one frame lands
  on the last frame's refresh (it is not shown; log `one refresh earlier`, `plan_catchups`); a catch-up
  followed by a break within 60 s doubles the 5 s. No change of landing in steady play: a game whose
  phase drifts against SteamVR's refreshes (the mock's drifted 75 us a second) may end up to a refresh
  later than it could until the next load.
- **No long hold into the next frame.** With one eye set the next frame's present waits for this frame's
  release, so a hold reaching it stalls the game (in the mock a room fell to 32 fps that way). The plan
  lets a hold reach the next frame's expected present (a refresh after this call plus the 10th percentile of
  the call-to-submit time) by 3 ms at most; beyond that it lands the frame on the last frame's refresh
  instead (that frame is not shown; `plan_drops`). When the video thread already waits for the eye images,
  the releaser ends the hold at the earliest time that still lands right (0.5 ms past the limit) if that is
  at most 4 ms away, else at once (`hold_cuts`; around loads).
- **The refresh step** (`UpdateWakeGrid`): the smallest value recurring among the steps between display
  times and SteamVR's `predictedDisplayPeriod` values (mostly 22.2 ms, but 16.67 recurs: 2311 of 4400 frames
  in the light launch, 847 of 8000 in the heavy one), never replaced by a larger one: dup and skip hand-outs
  (steps of 0 and 33.3 ms only) and a loading game (steps of two refreshes) cannot make it 33.3 ms again.
- The asap wait, the late mode, the auto switch and the late-run ends are gone. `PRIMEFRAME_CYCLE_END=asap`
  is section "The wake-up"'s asap (each `xrWaitFrame` after the last end) for an A/B.

What this gives per room: the light room settles on its shortest landing within seconds of the load, the
alternation of the session included (one frame on time, the next handed the same refresh again and a
refresh late, every image shown for exactly two refreshes); the heavy room lands every frame a period late,
held a few ms past the limit, constant. In vrtiming the light room should read vsyncs 2.00 (as 33:00 to 33:03
did), the heavy room 4.00 with now and then an on-time frame among them (same refresh, SteamVR's count).

### Results

Frame-like mock (`mockrun.sh --frame`: `MOCK_XR_EARLY=3`, throttled, lead 4, latch 13.8, late frames a
period later, XrTime on CLOCK_MONOTONIC_RAW), owner's settings (IR 3, eye 2160, sysmem, VI at 60,
prediction auto, `--stamp --head 150`), the game's phase set at 10 s (s01) or at 100 s, the end of the
flight (s02). Holds: refreshes each new image stayed on screen, summed over the mock's 10 s windows after
settling (2 is the cadence; 4 an image shown twice; never shown: a frame replaced). Events: frames that
moved the landing (`plan_breaks`, `plan_catchups`, `plan_drops`) after settling.

| Run | Game fps avg (lowest second) | Settled | After settling: held 2 / held 4 / never shown | Events after settling | Locate to landing avg ms | Stamps exact, end times right, errors, releases unfinished |
| --- | --- | --- | --- | --- | --- | --- |
| lane L's build, `ASAP_WAIT=0`, s01 phase 14 ms, 90 s | 59.2 | never | 181 / 2309 / 2309 (10 to 90 s) | end mode late from 7 s, `slot_ms` 33.33 | | 10704 of 10706 (2 from the grid move, since fixed), all, 0, 0 |
| lane L's build, `ASAP_WAIT=0`, s02 phase 0.5 ms, 60 s after the flight | 59.9 | never | 3332 / 134 / 131 | 8 late-run ends in the session | 42.7 | all, all, 0, 0 |
| s01 phase 14 ms, 180 s | 60.0 (59.0) | 28 s (1 break, 2 catch-ups) | 10801 / 0 / 0 (30 to 210 s) | none | 41.3 | 25124 of 25124, 12564 of 12564, 0, 0 of 25127 |
| s02 phase 0.5 ms, 180 s after the flight | 59.9 (53.8) | at the end of the flight | 10792 / 3 / 1 (the last 100 s: 6000 / 0 / 0) | 5, all in seconds the game ran under 60 fps (53.8 to 59.4) | 52.5 | 33488 of 33488, 16746 of 16746, 0, 0 of 33491 |
| s02 phase 8 ms, 60 s after the flight | 60.0 (56.5) | at the end of the flight | 3596 / 1 / 1 | 2, in the 56.5 fps second | 49.6 | 19106 of 19106, 9555 of 9555, 0, 0 of 19109 |
| s01 phase 3 ms, 60 s | 60.0 (59.0) | 25 s | 3600 / 0 / 0 | none | 47.9 | 10726 of 10726, 5365 of 5365, 0, 0 of 10729 |
| s01 phase 8 ms, 60 s (the build before the last catch-up rule) | 60.0 (59.1) | 30 s | 3600 / 0 / 0 | none | 43.8 | all, all, 0, 0 |
| s02 soak, 300 s after the flight | 59.2 (47.2) | at the end of the flight | 17929 / 34 / 1 | 52: 48 in 12 s when the game fell to 47 to 57 fps (another project's mock run on the machine), 4 at 59 to 60 fps 22 s apart (the game a little under 60 frames a second: an image has to repeat) | 49.1 | 47490 of 47490, 23747 of 23747, 0, 0 of 47493 |
| s01 `PRIMEFRAME_CYCLE_END=asap` (A/B, the build before the hold changes), 60 s | 59.9 (57.5) | | 4196 / 0 / 0 | | 33.2 | all, all, 0, 0 |
| s02 `--nowait`, 30 s | | | | | | 7126 of 7126 releases changed after release: FAIL, as it must |

- The light room settles on its shortest landing for the game's phase within 30 s of the load and stays
  there (one frame on time, the next handed the same refresh again and landing a refresh later, or all on
  time): every image is shown 33.3 ms plus the time from the call to the next wake-up after its poses were
  located (about 40 ms at the phase the mock drifted to). asap (each call blocked until the wake-up) gives
  33.2 ms in the mock, but its waits cost frames on the Frame (session L).
- The heavy room lands every frame a period late (about 50 ms), constant; what moves its landing is the
  game's own late frames, one image shown twice each.
- No 1-3, no alternation between landings, no mode switches, no catch-up after settling; `slot_ms` 16.67 in
  every second of every run. The mock's game phase drifted (from 14 to about 9.5 ms in a minute of s01);
  on the Frame it held 13 to 15 ms for the whole light launch of session M.


### Not known until the headset

Whether SteamVR's hand-out on the Frame with the plan is the table's (the plan takes whatever is handed
out, so a different hand-out changes only the bookkeeping); whether frames held a few ms past the limit
show anything with SteamVR's compositor on the same queue; how often the heavy room's own late frames break
the cadence there (the mock's dips are this machine's); the light room's settling on the Frame (catch-ups
expected within about 10 s of the load); the heavy room's latency (one period late: about 50 ms from the
locate to the landing in the mock, about 54 on the Frame).

## Default

`cycle` with the GPU kicks every 100 draws, no eye clear, the overlap and the landing plan
(`PRIMEFRAME_CYCLE_END=plan`: every frame lands on the refresh after the last one's, section "One landing"),
`PRIMEFRAME_PREDICT_EXTRA_MS=auto` (the wrapper's default), framesToThrottle 1. A/B switches:
`PRIMEFRAME_CYCLE_END=asap` (each `xrWaitFrame` after the last end, section "The wake-up";
`PRIMEFRAME_CYCLE_ASAP_WAIT=0` keeps the overlap there), `PRIMEFRAME_CYCLE_OVERLAP=0`,
`PRIMEFRAME_XR_CLOCK=raw|steady`, `PRIMEFRAME_HUD_TEST=1` (section "The HUD"). The headset settled `cycle`
against `pipelined` on 2026-10-14 (cycle clean, every pipelined variant doubled).

## Next headset session: one landing (two launches, about a minute of play each)

Install this build (`install.sh --dev`), same SteamVR settings as the last sessions (120 Hz, framesToThrottle
1 by the wrapper, motion smoothing off, eye 2160). `session.env`: `PRIMEFRAME_STATE=2`; `session.env.next`:
`PRIMEFRAME_STATE=1`. Nothing else: the landing plan is the default.

1. Heavy room (s02). Wait the 95 s of flight until Samus stands next to the ship. Turn the head steadily
   left and right for ten seconds, then quick turns and stops. Any lag spike (a jump, a hitch)? Smooth all
   the time? The HUD as steady as last time?
2. Light room (s01). The same turns. Any spike, in the first seconds after the load or later? Smooth? HUD?
   Lag compared with launch 1 (it should feel shorter: the light room lands a refresh earlier)?

The logs must show, launch 1 in the player view: XRSTATS `end_mode=plan`, `slot_ms` 16.67 every second,
`plan_catchups` 0 after the first 30 s, `plan_drops` 0, `hold_cuts` 0, `plan_breaks` only in seconds whose
`game_fps` is under 60 (the game's own late frames), `pose_off` 0; vrtiming: 60 frames, 60 reused, vsyncs
to photons the same value every second after the flight (4.00, about two mispresents per frame; an on-time
frame among them may make a second read 3.9), no second at 30 reused and 90 mispresented. Launch 2: at most
two `PrimeFrame cycle landing: one refresh earlier` lines, within 30 s of the load, then no landing line;
XRSTATS `plan` about 30/30/0 (one frame on time, the next handed the same refresh again) or 60/0/0;
vrtiming vsyncs 2.00 every second after that, 60 reused, about 60 mispresented, no 1-3 second.

How it decides: no spikes, smooth, HUD steady -> done. Spikes in launch 1 in seconds with `plan_breaks` and
`game_fps` under 60 -> the game's frames (XRC `vcpu` over 16 or `fw` over 2 ms), not the pacing. Spikes with
vrtiming's vsyncs changing while `plan_breaks`, `plan_catchups` and `plan_drops` are 0 -> SteamVR lands
frames elsewhere than planned (its on-time limit in that room is not 19.5 ms): XRF `rg` and `pl` against
vrtiming's vsyncs per second. A 1-3 second in launch 2 -> `slot_ms` (must be 16.67) and `plan` that second.

## Earlier session plan: HUD and lag spikes (two launches, done 2026-10-14)

Install this build (`install.sh --dev`), same SteamVR settings as the last session (120 Hz,
framesToThrottle 1 by the wrapper, motion smoothing off, eye 2160). `session.env`: `PRIMEFRAME_STATE=2`;
`session.env.next`: `PRIMEFRAME_STATE=1` and `PRIMEFRAME_HUD_TEST=1`. The shader cache is warm now, so
the first pass through a room no longer compiles on the video thread's CPUs.

1. Heavy room (s02). Wait the 95 s of flight until Samus stands next to the ship. Turn the head steadily
   left and right for ten seconds, then quick turns and stops. Watch the helmet and the HUD while turning:
   still against the view, or shaking? Any lag spikes (a hitch, a jump) in about a minute? Smooth?
2. Light room (s01), with a small white ring and cross below the centre of view (the HUD test: a
   head-locked layer of its own). The same turns. Is the HUD steadier than in launch 1, and than in the
   last session? Does the ring stay perfectly still while the game's HUD moves, or does it move with it?
   Lag compared with launch 1?

The logs must show, launch 1 in the player view: one `PrimeFrame cycle end: late` line after the start
(a heavy room keeps the overlap), XRSTATS `end_mode=late`, `wake_lead_ms` about 37.5 with no `?`,
`slot_ms` 16.67, `pose_off` 0, and either `early` near 60 with `extra_frames` near 0 (SteamVR hands each
frame its refresh early) or `early` near 0 with `extra_frames` near 60 (late calls planned a refresh
late): one of the two steadily, not alternating; `late run` lines rare (under one in 10 s); `game_fps`
59 to 60. vrtiming: 60 frames a second, vsyncs to photons the same every second, about one mispresent per
frame. Launch 2: `end_mode=asap` within a few seconds of the load and no switch after it, `early` 0,
`extra_frames` 0, XRF `blk` several ms (each `xrWaitFrame` blocks for SteamVR's wake-up), `w` 0 to 0.3,
`rg` about 7 to 12; vrtiming vsyncs 2 every second, poses to frame ready about 7 to 12 ms; no
`no VIEW reference space` warning.

How it decides: HUD steady and no spikes -> done. Launch 2's HUD steadier than launch 1's -> the
prediction length is what moves it (late rooms predict up to 16 ms longer). The ring still while the HUD
moves -> a HUD layer of its own is worth building (section "The HUD"); the ring moving too -> SteamVR
turns VIEW layers as well and the layer would not help. Lag spikes in launch 1 with `late run` lines at
the same seconds -> the run ends cost frames (more evidence needed before ending runs); spikes with XRC
`vcpu` over 16 or `fw` over 2 ms -> the video thread's CPU or Dolphin's command buffers, not the pacing.
Launch 2 not in asap -> `PRIMEFRAME_CYCLE_END=asap` for a launch and compare.

## Earlier session plan: the heavy room (two launches, done 2026-10-14)

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
