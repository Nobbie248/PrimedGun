# Quest open-area performance analysis — 2026-09-12

The renderer is the bottleneck in the severe-slowdown scene. Two independent
costs stand out: redundant Android-window/OpenXR submission with premature frame
resource recycling, and substantial CPU work preparing approximately 2,430 Vulkan
draws per rendered frame. No application code or gameplay settings were changed
during this analysis. The current scene was preserved.

## Measurements from the severe-slowdown scene

The user returned to the heavy area after the first capture. A new app process
and updated binary were detected; addresses and symbols were refreshed. The
measurements below are from this second capture (PID 23037, approximately
21:15–21:18 device time), against the build whose local HEAD was `33351e2f`.

| Measurement | Observed |
| --- | --- |
| Rendered FPS | 17.1–17.4 |
| Emulated video interrupts per second | 17.1–17.4 |
| Emulation speed | 28.8–29.1% |
| Vulkan draw calls per rendered frame | About 2,433 |
| Vulkan queue submissions per rendered frame | About 2.00 |
| CPU wall time waiting for Vulkan fences | About 23.2 ms/frame |
| CPU time in instrumented draw/bind calls | About 6.1 ms/frame |
| CPU time uploading uniforms | About 2.1 ms/frame |
| CPU time committing vertices | About 0.21 ms/frame |
| CPU time in queue submission | About 0.31 ms/frame |
| Newly created Vulkan pipelines | Zero in the counter samples |
| GPU utilization | Approximately 46–48%, at 492 MHz |
| Android thermal status | 0 |

CPU/off-CPU stack sampling found the emulation thread spending approximately
81.6% of its wall time in `CoreTiming::GlobalIdle` / `Common::BlockingLoop::Wait`.
The video thread spent approximately 40.0% waiting in `vkWaitForFences`, reached
through `VulkanOpenXR::SubmitFrame` and `CommandBufferManager` resource recycling.
Another approximately 14% was attributed to the GPU FIFO event wait. These are
sampled inclusive call-chain percentages, not values to add to the counters above.

An independent CPU-only profile sampled 10.766 seconds of video-thread execution
over 19.97 wall seconds. At roughly 17.3 FPS this is about 31 ms of host CPU work
per rendered frame, already above the 16.7 ms budget for 60 FPS.

Notable inclusive shares of that video-thread CPU profile:

- `VertexManagerBase::Flush`: 76.2% (encompasses most work below).
- `VKGfx::DrawIndexed`: 19.0%.
- `StateTracker::UpdateGXDescriptorSet`: 12.3%.
- Texture-cache loading: 12.2%; ARM texture hashing alone accounts for 9.5%.
- Vertex loading/conversion: 8.6%.
- Uniform uploads: 7.0%.
- Stable draw-submatch identification: 4.2%.
- `ShaderHunter::RegisterShader`: 3.4%.

These percentages overlap through parent/child calls. They establish CPU hotspots;
they do not imply that all associated work can be removed safely.

## Concrete submission issue

`vr_android_direct_to_hmd` is loaded into VideoConfig, but no presenter/backend
code consumes it in this fork. `Presenter::Present` unconditionally binds and
presents the Android backbuffer before blitting/submitting OpenXR eyes.

`VKGfx::PresentBackbuffer` advances the frame-resource ring during its submission.
The Android path in `VulkanOpenXR::SubmitFrame` advances it a second time. There
are two frames in that ring. The second advance therefore revisits the resources
used by the Android submission from the same emulated frame, triggering fence
waiting before resource reuse. This prevents the intended overlap between
preparing the next frame and executing the current one.

The two submissions/frame, 23.2 ms/frame of fence wait, and the sampled call chain
all agree with this source-code finding. DolphinXR's reference presenter already
has a direct-to-HMD branch that skips the Android backbuffer path.

The first change to validate is porting that branch while retaining PrimedGun's
cinematic/overlay routing, pending-frame handoff, and the multiview acquisition
wait fix. Resource recycling should advance once per intended frame in direct
mode. This analysis does not remove waits: they protect resources still in use.
The size of the resulting performance gain needs an A/B test in this same scene.
The remaining CPU draw workload means this change alone cannot be assumed to
deliver 60 FPS.

## Culling and visibility

Read-only inspection of the running PrimedGun settings found HMD frustum culling
enabled with a 115-degree cone, and the blanket disable-frustum patch flag off.
The on-disk Dolphin graphics config has `DisableCPUCull=True` and `CPUCull=False`.
These are different culling mechanisms; the scene is not simply using every
available culling-disable option.

Approximately 2,430 Vulkan draws/frame is a substantial workload, but no
visibility-breaking culling A/B was performed, so the fraction attributable to
the visibility patches is not established. Keep the existing visibility intact.
Further optimization should preserve a conservative HMD-visible volume, including
both eyes and head movement, rather than restoring the original game-camera FOV.

After fixing submission, profile again before changing draw matching. Candidate
work includes avoiding redundant texture hashes, descriptor/uniform updates, and
repeated stable draw signatures while preserving the identifiers used by Prime's
HUD, visor, and world overrides. A 3x-to-2x internal-resolution A/B would distinguish
remaining pixel/bandwidth cost from CPU draw cost; reducing OpenXR eye resolution
alone does not reduce the cost of rendering the game's 3x EFB.

## Capture details and limitations

All artifacts are under the ignored directory
`Source/Android/app/build/performance-open-area/`. Relevant files:

- `heavy.data`, `heavy-video.txt`, `heavy-cpu.txt`: 25-second CPU/off-CPU profile.
- `heavy-active.data`, `heavy-video-active.txt`: 20-second CPU-only profile.
- `live-heavy-scene.csv`, `counter-summary.json`: read-only live-counter samples.
- `logcat-heavy.txt`, `thermal-heavy.txt`: runtime and thermal corroboration.
- `sample-live.ps1`, `read-counters.c`, symbol-layout dumps: capture implementation.

The helper reads only selected app memory using `pread`; it never writes process
memory. It was removed from the app after capture. No game restart, installation,
setting change, or culling toggle was performed by this analysis. The app process
and installed binary changed between the initial and heavy-scene captures.

Counters reset periodically. Per-frame figures use only adjacent samples without
a reset (13.746 seconds total), dividing counter deltas by the sampled FPS integrated
over those intervals. They are estimates, not exact frame-boundary measurements.
GPU busy percentage includes headset-wide GPU activity and is not proof that the
app's GPU work is cheap: the fence stalls demonstrate its effect on the critical path.
No GPU pass-level timestamps or PC comparison capture were collected.

The earlier `cpu.data` / `offcpu.data` capture was a different, less severe state:
the emulator clock was near 100% while rendered FPS was low. Do not combine those
figures with the 29%-speed heavy scene or treat them as a controlled A/B comparison.
