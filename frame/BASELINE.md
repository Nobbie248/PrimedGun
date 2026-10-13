# Baseline: native build under the mock OpenXR runtime

Measured headless on the Frame itself (Adreno 750, Turnip) with `frame/tools/mockrun.sh`. Mock at 120 Hz,
eye size 1728x1728, `InternalResolution = 2`, `EnableGPUTextureDecoding = False`, shader compilation
mode 3 (the copied `GM8E01.ini`), Vulkan backend, no audio, x11 window in a private Xvfb. Each run loads
one of the two existing save states at boot and measures the first 120 one-second statistics lines
(`XRSTATS`, counted from the first submitted frame; the session lasts 15 s longer so the window is
complete). The shader cache in the scratch user dir was filled by a few short runs before, so shader
compilation is mostly out of the numbers; the first second of every run is slow anyway (startup), which
is why the minimum column shows 37 to 39 on the light state. "Steady" below leaves out the first 10 s.

The mock's compositor is not SteamVR's: it takes no GPU time and never reprojects. The numbers say what
the game and the OpenXR path do on this hardware, not what the headset will show.

## Runs

| Run | Game fps (avg / min) | Eye sets submitted per s (avg / median / min) | Drain ms (avg / max) | Mock errors | Released unfinished (mock) | Session states reached | Slow seconds, worst | Stall file |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| s01, drain off | 59.6 / 28.2 (steady 59.9 / 58.1) | 59.9 / 60 / 37 (steady min 58) | 0 | 0 | 0 of 16089 | IDLE READY SYNCHRONIZED VISIBLE FOCUSED STOPPING EXITING | 1, worst 37 | none |
| s01, drain on | 59.7 / 30.4 (steady 59.9 / 59.0) | 59.9 / 60 / 39 (steady min 59) | 0.13 / 1.45 | 0 | 0 of 16095 | same | 1, worst 39 | none |
| s02, drain off | 35.9 / 20.7 (steady 35.6 / 20.9) | 36.5 / 32 / 21 | 0 | 0 | 0 of 9375 | same | 45, worst 21 | none |
| s02, drain on | 31.3 / 20.0 (steady 31.0 / 20.0) | 31.9 / 25 / 21 | 3.09 / 11.04 | 0 | 0 of 8398 | same | 28, worst 21 | none |

"Slow seconds" are seconds with fewer than 90 % of the run's median submitted eye sets. The vblank
rate (`game_vps`) was 59.4 to 59.8 in all four runs: the game itself keeps its 60 Hz clock, it is the
frame rate that drops. The mock ended 4201 to 8048 frames per run, always newly released eyes, none
repeated, both eyes from one locate for the display time (7 frames in every run were
located for an earlier display time).

Labels from the measured GPU load (the drain time is the time `SubmitFrame` waits for the GPU to finish
the frame's last command buffer):

- State `GM8E01.s01`: **lighter**. 60 fps with or without the drain, the drain waits 0.13 ms on average
  (1.45 ms at worst).
- State `GM8E01.s02`: **heavier**. 36 fps without the drain, 31 fps with it (the drain costs about 13 %
  of the frame rate), drain wait 3.1 ms on average and 11 ms at worst, the frame rate wanders between
  20 and 40 fps.

Neither state is GPU-bound at this resolution with the mock: even in the heavier one the GPU is only
about 3 ms behind the CPU at the end of a frame on average, so the drain's cost is the 13 % above and the
rest of the frame time is not GPU wait (CPU side: emulation, shaders, driver; not profiled). (The 15 to 30 ms fence waits seen in the
headset are therefore not reproduced here; SteamVR's compositor and the real display path are not in
this setup.)

## Validation layer (60 s, state s02, drain off)

`frame/tools/mockrun.sh --state 02 --seconds 60 --validate` (core validation, no synchronization
validation). The layer slows the game (22.8 fps here), so its frame rate is not comparable with the table.
Result: 2 messages, both `VUID-VkGraphicsPipelineCreateInfo-pStages-00738` (a geometry-shader pipeline
whose SPIR-V expects triangles was created with point-list topology), from Dolphin's pipeline
creation in the game code (not the OpenXR path, not our changes, not a Valve layer). No message from Valve's layers (the
gamescope WSI layer is switched off for the mock runs; `XR_APILAYER_VALVE_fdm_injection` is an OpenXR
layer and has no Vulkan messages), none about the OpenXR swapchain images or the eye blits, none from
our code. The mock's own leftovers at `vkDestroyDevice` (10 messages in the first validation run:
6 images, 4 fences) are fixed in the mock (it now frees its images at `xrDestroySwapchain` and its
fences at `xrDestroySession`).

## What did not work, or does not prove much

- The mock's exact release check reported 0 of 8398 to 16095 releases changed after release in every run,
  including the heavier state with the drain off, where the GPU is on average 3 ms behind at the release.
  There is no positive control for it on this app yet (a build that releases early on purpose), so
  "0 released unfinished" is a result, not yet proof; lane A should add the control (for example an
  environment switch that skips the fence wait, expected to fail the check).
- Dolphin's `headless` platform never presents VR frames (`Presenter::Present` returns when the
  Vulkan backend has no swapchain); the runs use the x11 platform in the Xvfb.
- `PrimedGunFrameStall.txt` (the watchdog's one-shot diagnostic when the game pauses) was never written
  in any run: no hitch of that kind. The per-second statistics are the hitch measure; the heavier state
  has 28 to 45 slow seconds out of 120 (frame rate between 21 and 40), the lighter state none after the
  first second.
- The process needs up to 15 s to exit after the session ends (SIGKILL fallback in the script); no fatal
  line in any run.
- The in-VR overlay and laser swapchains (small sampled quad layers) are not exercised by these states
  (no quad layer was ended: "0 a quad" in the mock summary); that path is untested here.
- No audio and no controller input: the mock reports no active actions.
- The mock does not offer `XR_KHR_vulkan_swapchain_format_list`, `XR_META_touch_controller_plus` or
  `XR_MND_headless`; the game treats all three as optional, so those paths are untested.
