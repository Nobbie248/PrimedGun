# Vulkan recording worker — 2026-09-13

This is the first implemented stage of the split proposed in
[Quest-Renderer-Threading.md](Quest-Renderer-Threading.md): a helper thread that owns command
recording into the current draw command buffer, fed by the video thread through a bounded
queue. It is Vulkan-only, controlled by `[Settings] VulkanRecordingWorker` in GFX.ini (default
on for Android builds, off elsewhere), and read once when the backend starts.

## What moves and what stays

| Runs on the recording worker | Stays on the video thread (drains first) |
| --- | --- |
| Pipeline, texture, sampler, viewport and scissor binds | Command buffer submission, advancement and fence waits |
| Uniform, vertex, index and texel buffer *bindings* (offsets) | Stream buffer reservation and the uniform/vertex copies into them |
| Descriptor set allocation and updates, render pass begin/end | Texture uploads, EFB copies, readbacks, staging flushes |
| Framebuffer binds with discard/clear, `vkCmdClearAttachments` fast paths | Object creation and destruction (textures, framebuffers, pipelines, shaders) |
| Draws, compute dispatches, render-target-to-shader-read transitions | Presentation, swapchain handling, every OpenXR call, config changes |

The rule that keeps this safe is ownership: from the first queued command after a drain until
the next drain, only the worker may touch the state tracker, the current command buffers or the
frame's descriptor pools. `DrainRecordingWorkerForDirectAccess()` waits for the queue to empty
and hands ownership back. Every direct path listed on the right calls it first; the command
buffer getters and descriptor allocation assert the calling thread matches the owner and log
`Vulkan recording ownership violation` (counted in `VKPERF` as `owner_violations`) if a path was
missed. Because the worker never submits or advances command buffers, the stream buffers' fence
bookkeeping on the video thread stays exact: a reservation is stamped with the same fence
counter the queued draw will record into.

Per-draw command volume matters, so the video thread keeps mirrors of state the tracker would
ignore anyway (unchanged pipeline, viewport, scissor, vertex/index buffer) and skips those
commands, and the up-to-four GX uniform bindings of a draw are collected into one command that
is recorded before the next command that uses them. `SetTexture` is not deduplicated: its layout
transition depends on the texture's state when the command is recorded, which earlier queued
commands may change. `FinishedRendering` is queued for the same reason (an early prototype
drained there and lost most of the overlap), but is skipped entirely for textures that are
neither render targets nor compute images and already read as shader-read, since only the video
thread can move those out of that layout; the layout field is atomic for that read. The result is
about 9,900 commands per frame for roughly 2,500 draws.

## Queue

`RecordingQueue` (`VKRecordingQueue.h`) is a 32,768-slot ring of 64-byte trivially copyable
commands with free-running u32 indices. Both sides wait with `std::atomic::wait` on the index
the other side advances, so wake-ups always coincide with a value change; shutdown pushes a
no-op command to wake a sleeping consumer. The producer caches the consumer's index and only
re-reads it when the ring might be full, prefetches the slot it will write a few pushes ahead
(the consumer pulled that line into its own cache one lap earlier), and accumulates statistics
privately until the next drain. The consumer spins with a pause hint long enough to bridge the
gap between one draw's commands and the next before it sleeps, so during a frame the producer
never pays a futex wake per command; that spin is why the worker thread shows 70-85% CPU while
doing about 4.5 ms of recording per frame.

The ring is deliberately about three frames deep. A 4,096-slot version was tried and caused the
game to overrun its GX FIFO: the worker shares core 3 with the pacing, submission and a few
system threads and is held off for milliseconds at a time (`max_depth` regularly reaches 2,000
to 3,000 commands), and while the video thread waited on the full ring the emulated CPU kept
writing. The assert "FIFO is overflowed by GatherPipe! CPU thread is too fast!" then raises a
`PanicAlert`, which on the Quest is an Android dialog nobody can see in VR, and the emulated CPU
waits for it forever; that is what an apparent freeze was. See the behavior notes for how to
recover. `Drain` now waits in two-second slices and logs the worker's last command type and
count if it ever fails to make progress, and the first full-ring wait after a drain is logged.

The worker thread is pinned like the other emulator threads when `[VR] PinEmulationCores` is
on. The Quest app cpuset only allows cores 3 to 5, with the CPU thread on 5 and the video thread
on 4, so the request for a fourth fast core fails and it falls back to core 3.

## Measured result

Same saved state (`GM8E01.s01`), same capture method as the earlier documents: 500 Hz
cpu-clock, DWARF call chains, 25 s, per-draw `[VR] PerfCounters` timers off, the user standing at
the heavy save-station spot. FPS is the compositor's VrApi telemetry.

| Build (all 2026-09-13) | Video thread CPU per emulated frame | Compositor FPS | GPU busy |
| --- | ---: | ---: | ---: |
| Descriptor cache only (morning) | ~25.8 ms | 37.8–38.7 | 79–84% at 599 MHz |
| + cheap producer-side wins | ~23.2 ms | 41–44, mean 42.6 | 84% at 640 MHz |
| + recording worker, first tuning (timers on) | ~20.2 ms | 46–50, mean 48.6 | 90% at 640 MHz |
| + fewer commands, prefetching push (final) | ~19.1 ms | 49–51, mean 51 over 4 min | 95% at 640 MHz |

In the final capture the video thread still ran 97.6% of a core (12,199 samples), the worker
84% (mostly the idle spin), and the emulated CPU 61%. Two drains per frame (both at present),
drain waits below 0.01 ms, no full-ring waits, no ownership violations. The GPU is now the
limiter: at 95% busy the scene cannot pass about 53 FPS at 640 MHz and 3x internal resolution,
so the next step for the frame rate is the resolution scale, not the video thread.

Video-thread shares after the worker: `GetHash64_ARMv8_CRC32` 13.4%, `RunVertices` 12.3%,
`UploadUniforms` 9.4% (the constant copies and stream buffer reservation), `ResolveDraw` 7.6%,
`memmove` 5.7%, the queue push 5.7% (4.0% self, about 45 ns per command), `DrawIndexed` and
`SetTexture` pushes 2.6% and 2.2%. The `VKGfx::DrawIndexed` share that was 14.9% before the
worker is gone from this thread.

Artifacts are in the ignored `Source/Android/app/build/recording-worker-profile/`:
`worker-final.data` with its video-thread reports, the first-tuning capture
(`worker-round1-*`), the drain-heavy prototype capture (`worker-prototype-drains-*`, which
shows 10.8% of the video thread in `Drain` before `FinishedRendering` was queued), the freeze
log with the FIFO overflow assert, the build log, the scene screenshot and the arm64 queue test
binary (4 tests, run 30 times in a loop on the headset).

## Behavior notes

- If the game appears frozen at a few FPS, check `adb logcat` for `E Dolphin` lines first. A
  `PanicAlert` dialog blocks the emulated CPU behind the VR view; `adb shell uiautomator dump`
  shows its buttons ("Cancel", "Ignore for this session", "OK") and `adb shell input tap` on the
  button's bounds resumes emulation. `[Interface] SuppressCPUThreadWarnings = True` in
  Dolphin.ini silences the FIFO overflow assert specifically.
- `VKPERF` prints its count-only fields (draws, sampler cache, worker commands, drains, full
  waits, maximum depth, ownership violations) whenever video warning logs are enabled
  (Logger.ini `Verbosity = 3`, `Video = True`); the `*_us` timings still need
  `[VR] PerfCounters = True`. `draw_us` now measures the worker's time in the draw commands.
- Forcing a pixel shader drains and unbinds the previous forced pipeline before replacing it,
  because the replacement can land at the same address.
- Disabling the worker (`VulkanRecordingWorker = False`) runs every command through the same
  `ExecuteRecordCommand` switch on the video thread, so the two modes share one code path.
- Save-state loads, texture uploads, EFB copies and readbacks all drain; in the heavy scene that
  is two drains per frame, so those paths were left as they are. If a scene ever shows many
  drains per frame in `VKPERF`, the copy paths are the next candidates to queue.
