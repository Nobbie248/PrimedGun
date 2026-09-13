# Quest video-thread overhead reductions — 2026-09-13

These changes remove fixed per-draw overhead from the video thread without touching draw
order, Prime matching decisions, texture validation or the OpenXR frame contract. They were
chosen from the post-descriptor-cache profile in
[Quest-Descriptor-Hash-Performance.md](Quest-Descriptor-Hash-Performance.md)
(`steady-video.txt`, 12,330 samples, about 2,490 draws per frame at 38 FPS, roughly 26 ms of
video-thread CPU per emulated frame).

All of the work below sits on the producer side of the split proposed in
[Quest-Renderer-Threading.md](Quest-Renderer-Threading.md); a backend worker could not have
removed any of it.

## What changed

| Item | Profile share before | Change |
| --- | ---: | --- |
| `Common::Timer::NowUs` from the VKPERF per-draw timers | 3.38% (2.90% in `__kernel_clock_gettime`) | Six timer reads per draw (commit, uniforms, draw) now run only while `[VR] PerfCounters = True`. Counts still accumulate unconditionally. |
| `std::mutex` lock/unlock on the draw path | about 4% (`pthread_mutex_lock` 1.61%, `pthread_mutex_unlock` 2.15%, plus the LSE atomics behind them) | `ElementsGroupManager::ResolveDraw` takes the elements mutex once per draw instead of five to seven times; `ShaderHunter::RegisterDrawShaders` registers all three shaders under one lock instead of three. |
| `unordered_map<u64,int>` emplace for stable-submatch occurrence counters | 2.38% self | Replaced by `VideoCommon::GenerationCounterTable`, an open-addressing table cleared by bumping a generation. Steady-state frames allocate nothing. |
| `SamplerDescriptorSetCache::Find` | 1.59% self, plus 0.34% insert | `StateTracker` keeps one hash contribution per sampler slot and XORs them into a running hash on `SetTexture`/`SetSampler`; a lookup no longer hashes all 16 bindings. |
| `OpenXRManager::GetEyeProjectionRows` and `tanf` | 1.16% and 1.06% | `GeometryShaderManager` caches eye/head projection rows per units-per-meter (four entries), dropped at the XFB boundary and, with the head-pose lock off, whenever `LocateViews` publishes new views. Draws alternating between HUD and world scales, and the per-layer HUD hack path, reuse entries. |

Summed, these were about 10% of the sampled video-thread time, roughly 2.5 ms per frame at the
profiled rate. The measured result below matches that estimate.

## Measured result

Same saved state (`GM8E01.s01`), same capture parameters as the earlier profiles (500 Hz
cpu-clock, DWARF call chains, 25 s, no lost samples), recorded through the NDK
`app_profiler.py` while the user stood at the heavy spot. The build is the one described in
this document; the baseline column is the post-descriptor-cache capture from
`Quest-Descriptor-Hash-Performance.md`.

| Measurement | Descriptor-cache build | This build |
| --- | ---: | ---: |
| Video-thread samples / CPU seconds in 25 s | 12,330 / 24.66 s | 12,359 / 24.72 s |
| Video-thread share of one core | 99% | 99% |
| Compositor FPS during the window (VrApi) | 37.8–38.7 | 41–44, mean 42.6 |
| Video-thread CPU per emulated frame | ~25.8 ms | ~23.2 ms |
| kgsl `gpubusy` / clock | 79–84% at 599 MHz | 84% at 640 MHz |
| Emulated CPU thread | ~0.45 core | ~0.51 core |

Per-frame video-thread cost fell about 10% and rendered FPS rose about 11%; the thread is
still saturated, so the scene is still CPU-bound on the video thread, but the GPU is now close
behind it. The GPU clock stepped from 599 to 640 MHz to keep up; at 599 MHz the same load would
be about 90% busy. Further video-thread savings will show diminishing FPS returns until GPU cost
per frame drops (see the resolution-scale note in `Quest-Renderer-Threading.md`).

Inclusive shares of video-thread samples, before and after:

| Function | Before | After |
| --- | ---: | ---: |
| `Common::Timer::NowUs` / `clock_gettime` | 3.38% / 3.07% | absent (<0.25%) |
| `SamplerDescriptorSetCache::Find` | 1.59% | absent |
| `OpenXRManager::GetEyeProjectionRows` / `tanf` | 1.16% / 1.06% | absent |
| `GeometryShaderManager::SetConstants` | 3.70% | 1.81% |
| `pthread_mutex_lock` / `pthread_mutex_unlock` (self) | 1.61% / 2.15% | 1.12% / 1.18% |
| `unordered_map<u64,int>` emplace for occurrence counters | 2.38% | replaced: `GenerationCounterTable::Increment` 1.17% |
| `VKGfx::DrawIndexed` | 16.24% | 14.91% |
| `VertexManager::UploadUniforms` | 8.89% | 7.60% |
| `TextureCacheBase::Load` (unchanged code) | 13.73% | 15.42% (same absolute cost, larger share) |
| `GetHash64_ARMv8_CRC32` (unchanged code) | 10.51% | 11.83% (same absolute cost) |

Remaining mutex traffic (about 2.3% self in the pthread functions plus the LSE atomics behind
them) comes from the one elements lock and one hunter lock per draw that remain, and from
per-draw `unordered_map` updates in `ShaderHunter::AdvanceOverrideDrawCounters` and
`RegisterShaderLocked` (the two remaining `__emplace_unique_key_args` lines, 0.96% and 0.70%).
`Common::VR::OpenXRInputState::GetPrimedGunOverlay()` also copies its state under a mutex twice
per draw (in `Flush` and in `SetConstants`); that was not verified in this capture and is a
candidate for a per-frame snapshot. `EnsureStableSubMatchSignatureLocked` still costs about 4%
because `MakeStableSubMatchSignature` allocates a texture-hash vector per draw before the key
is hashed.

Artifacts are in the ignored `Source/Android/app/build/cheap-wins-profile/` directory:
`after.data`, `after-video.txt` (video-thread report), `after-scene.png` (compositor
screenshot of both eyes at the spot), `after-startup.log`, the on-device thread sampler, the
build log and the arm64 test binary.

## Behavior notes

- `[VR] PerfCounters` (GFX.ini) defaults to **off**. While off, `draw_us`, `uniform_us`,
  `vtx_us` and `fence_us` in the `VulkanContext::PerfCounters` struct stay at zero and the
  `VKPERF:` log line is not printed. `draws`, `submits`, `pipelines` and the sampler cache
  hit/miss counts still update, so the `/proc/PID/mem` probe scripts keep working for those.
  Set it to `True` before profiling with the counters; it takes effect at the next presented
  frame without a restart.
- `ResolveDraw` evaluates skip, handling and layer/depth/units-per-meter under one lock and
  returns them together. The decisions are consumed under the same conditions as before, so
  results are unchanged. One visible difference: with ShaderHunter debug logging enabled,
  `match(handling)` lines can now appear for draws that the shader hunter or texture manager
  went on to skip, because the query is no longer short-circuited by those later checks.
- With the head-pose lock **on** (`LockHeadPosePerFrame = True`, not the Quest default), the
  old code re-fetched the current eye views every time the units-per-meter value changed
  between draws; the cache now keeps the pose captured at the first draw of the frame for every
  scale, which is what the lock intends. With the lock off (the Quest default) the output is
  identical to before for every draw.
- The per-layer HUD projection path used to compute from the live eye views on every matching
  draw; it now reads the same per-scale entries. With the lock off these are refreshed together
  with the main path in the same `SetConstants` call.
- Override entries with `condition=` flags would still take the shader hunter mutex once per
  entry per query inside `DoesEntryMatch`. The shipped `GM8E01` overrides use none, so this was
  not changed.

## Validation

- `GenerationCounterTable` and the sampler cache hash have gtest coverage in
  `Source/UnitTests/VideoCommon/`. Both files were also compiled standalone with the NDK
  clang (`--target=aarch64-linux-android29`) against the bundled GoogleTest and Vulkan headers,
  pushed to the Quest and run: 8 tests, all passing. The table test cross-checks against
  `std::unordered_map` across growth and repeated clears; the cache test checks that the
  incremental per-slot hash equals the full hash after updates and that slot order matters.
- Quest debug APK built with `Source/Android/build-quest.ps1` (`app:assembleQuestDebug`),
  installed with `adb install -r`, and Metroid Prime launched through `EmulationActivity`
  with the VR intent category. The startup logcat contained no fatal signal, Java exception,
  Vulkan or OpenXR error. The user loaded the heavy-area save state; the compositor screenshot
  shows world geometry, visor frame, HUD, arm cannon and minimap in both eyes.
  The desktop Windows and non-VR builds were not compiled in this session; the changes to
  `GeometryShaderManager` are under `ENABLE_VR`, and the Vulkan changes are backend-local.
- Changed lines were formatted with `git clang-format` using clang-format 19.1.5;
  `git diff --check` passes. The full `tools/lint.sh` still reports pre-existing drift in
  untouched regions of the same files.

## What is still on the video thread

From the same profile, the largest remaining producer-side costs are texture-memory
validation (`GetHash64_ARMv8_CRC32` 10.5%, inside `TextureCacheBase::Load` at 13.7%), vertex
loading and FIFO decode, the Prime signature/matching work that remains after the lock
consolidation, and the roughly 4 KB `VertexShaderConstants` copy per dirty draw (part of the
5.4% in `memmove`). None of these move to a backend worker either.
