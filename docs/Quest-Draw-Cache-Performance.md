# Quest draw-cache performance — 2026-09-13

After the direct-to-HMD change, the user confirmed a substantial improvement and
left Prime running in the area with the most slowdown. The remaining bottleneck
in this capture is CPU work on the video thread.

## Baseline with direct mode

Local HEAD: `f1c773e5`, app PID 20504, video thread 20656. A 500 Hz CPU-clock
simpleperf capture with DWARF call chains lasted 24.976 seconds. It sampled
24.502 seconds of execution on the video thread: approximately 98% of one core.
Nearby runtime telemetry reported 27–30 FPS at 72 Hz, GPU utilization 73–81% at
492 MHz, and OpenXR resolution scale 0.85. These FPS readings are runtime
telemetry, not a separate measurement of emulation speed. The previous 17 FPS
capture used another session/viewpoint, so it is not a controlled before/after.

Selected inclusive percentages of video-thread CPU samples (parents overlap):

| Function | CPU samples |
| --- | ---: |
| `StateTracker::UpdateGXDescriptorSet` | 11.45% |
| Driver `vkUpdateDescriptorSets` | 6.24% |
| Driver `vkAllocateDescriptorSets` | 3.22% |
| `TextureCacheBase::LoadImpl` | 11.34% |
| `Common::GetHash64_ARMv8_CRC32` | 8.82% |
| `Common::ComputeCRC32` | 6.37% |
| `ElementsGroupManager::GetStableSubMatchSignatureLocked` | 5.13% |
| `ShaderHunter::RegisterShader` | 3.06% |
| `TextureCacheBase::GetBoundTextureHash` | 2.87% |

## Cache audit and changes

- Shader override CRC32 values are now computed lazily and invalidated individually
  when the current vertex, pixel, or geometry UID changes. The existing UID
  equality comparison covers exactly the complete bytes used by CRC32. No
  shader-ID or persisted override hash format changes. Vertex/geometry family
  signatures reuse that same CRC32 instead of computing it again; pixel-family
  classification remains unchanged.
- Texture dump-name hashes are parsed when the texture name is assigned. The
  name is private and all mutations use a setter, keeping the cached value
  synchronized. Empty/unparseable/zero filename hashes still fall back to the
  current internal hash, including after palette/EFB content changes. Texture
  memory validation and its ARM hash function are untouched.
- Stable submatch queries previously returned a signature by value, including
  its texture vector, although all seven callers discarded that return value.
  They now only ensure the per-draw context exists. Building a new context also
  reuses the base signature instead of constructing it twice. The same base key,
  occurrence increment, draw-sequence cache, frame reset, and mutex protection
  remain in place.
- The active Prime 1 GC profile flag is cached when overrides are loaded and
  reset when they are cleared, replacing a per-draw mutex/vector search.
- `IsQuestOrVirtualDesktopRuntime` still constructs a lower-case runtime name.
  Its only external caller is in the non-Android branch of Vulkan submission;
  porting the sibling DolphinXR cache there would not address this Quest profile.
  Override loaders already compare the loaded game ID before doing file work.
  The native Prime runtime's game gate reads six emulated ID bytes once per frame.

Visibility, culling, Prime matching rules, draw order, direct presentation,
multiview, foveation and resolution settings are retained.

## Validation

- Quest debug build passed (`Source/Android/build-quest.ps1`, 51 seconds).
- Installed with `adb install -r` and launched Metroid Prime, PID 22146.
- Startup telemetry reached approximately 60 FPS in the title/menu workload;
  the captured log contained no fatal signal, Java fatal exception, Vulkan error,
  or OpenXR error. This is a startup check, not the heavy-area performance result.
- Changed C++ regions formatted with clang-format 19.1.5; `git diff --check` passed.
- The user reports that the new VR menu cannot save states. They returned to
  the slow spot manually after installation. This save-state issue is not fixed
  by the cache changes.
  Later in the session the user fixed saving and supplied slot 1 for repeatable
  profiling; see `Quest-Descriptor-Hash-Performance.md` for the follow-up.
- A compositor screenshot in that area shows world geometry, visor/HUD and the
  arm cannon in both eyes. Subjective headset comfort and the full set of Prime
  effects still require player feedback.
- Desktop/non-VR builds and the native unit-test suite were not run.

## Updated heavy-area capture

PID 22146, video thread 22226, 500 Hz CPU-clock/DWARF profile lasting 24.975
seconds, with no lost samples. Video-thread execution was 24.556 seconds,
approximately 98% of a core. Runtime telemetry during the capture showed
36–37 FPS, GPU utilization about 83–85% at 545 MHz, and CPU frequency 2208 MHz.

| Function | Baseline CPU share | Updated CPU share |
| --- | ---: | ---: |
| `Common::ComputeCRC32` | 6.37% | 0.58% |
| Stable submatch context preparation | 5.13% | 3.52% |
| `ShaderHunter::RegisterShader` | 3.06% | 1.93% |
| `StateTracker::UpdateGXDescriptorSet` | 11.45% | 12.71% |
| `Common::GetHash64_ARMv8_CRC32` | 8.82% | 10.25% |

These are inclusive sampled shares, not an additive accounting. The unchanged
functions can occupy a larger share after other work is removed. Texture-name
parsing no longer occurs in the per-draw hash getter.

A separate read-only sample of the emulator's live counters, after simpleperf
stopped, reported:

- Rendered FPS: 36.1–37.5; emulation speed: 60.5–62.6%.
- Approximately 2,497 Vulkan draws and 1.00 submission per rendered frame.
- Fence wait: approximately 0.009 ms/frame.
- Draw/bind CPU time: approximately 5.44 ms/frame; uniforms: 1.94 ms/frame.
- Zero newly created pipelines in the counter intervals.

Counter rates use adjacent intervals without a counter reset and integrate
reported FPS over each interval. The read-only helper reads `/proc/PID/mem`
through `run-as`; it does not write application memory or settings.

The decrease in checksum/signature work is directly visible in the CPU profile.
The 27–30 versus 36–37 FPS observation is encouraging but is not a controlled
speedup measurement: the GPU clock also increased from 492 to 545 MHz and the
viewpoint was restored manually. The game still runs substantially below full
simulation speed, and the video thread remains saturated. No runtime fatal or
Vulkan/OpenXR error was found in the post-change gameplay log.

## Remaining targets

At the time of this capture, descriptor setters already avoided dirtying unchanged bindings. Changed sampler
bindings still allocate a fresh descriptor set and rewrite the sampler array.
A future descriptor cache must respect command-buffer/pool retirement and
texture/sampler lifetime, and should be checked with allocation/update counts.
The sampler update path is similar in the sibling DolphinXR source; this is
not an obvious missing cache to copy wholesale.

The subsequent bounded sampler-set cache and its measured results are documented
in `Quest-Descriptor-Hash-Performance.md`. A staged renderer-thread split and
its synchronization requirements are documented in `Quest-Renderer-Threading.md`.

Actual texture-memory hashing remains a measurable cost. Reducing that work
requires correct invalidation for CPU writes, palettes, overlapping textures and
EFB copies; a filename-hash cache is independent of those validity checks.

Raw captures and reports are in the ignored
`Source/Android/app/build/post-direct-profile/` directory. The build log is
`Source/Android/app/build/quest-draw-cache-build.log`.
