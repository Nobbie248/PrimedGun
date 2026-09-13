# Quest descriptor and texture-hash investigation — 2026-09-13

The sampler descriptor cache reduces CPU work, but does not resolve the heavy
area's slow simulation. Two exact-result texture hashing experiments did not
produce a useful general improvement and were not added to production code.
See [the threading design](Quest-Renderer-Threading.md) for the larger next option.

## Descriptor change

`StateTracker` now keeps a bounded 1,024-entry cache of immutable GX sampler
descriptor sets. The key includes all 16 sampler bindings: sampler handle, image
view and image layout. Every field is compared after indexing, so collisions
produce misses. Eviction removes the lookup entry without modifying or freeing
the descriptor set used by already-recorded commands.

The cache is cleared when recording state is invalidated, on texture unbind/view
destruction, and before the sampler-object cache is cleared. It therefore does
not retain a set across descriptor-pool recycling. Utility and compute descriptor
sets remain separate. Texture unbind now also marks affected bindings dirty;
the fallback storage-image descriptor uses `GENERAL`, matching the dummy image's
actual layout. The latter corrects a pre-existing inconsistency exposed by
explicitly dirtying that binding.

`VKPERF` includes sampler-cache hits/misses per frame. The cache adds no heap
allocations during lookup, has fixed memory use, and does not change which
textures, samplers, draws, Prime overrides or culling decisions are selected.

Later the same day the per-draw `VKPERF` timers were put behind `[VR] PerfCounters`
(off by default) because the timer reads themselves cost about 3% of the video
thread; enable it before repeating the counter sampling below. See
[Quest-Video-Thread-Overhead.md](Quest-Video-Thread-Overhead.md).

## Measurements

The user saved `GM8E01.s01` at the heavy-area spot. The file was backed up before
installation. Baseline PID 23780/video TID 23951 ran the preceding draw-cache
build. The new cache was profiled in PID 10758/video TID 10961 after the user
reloaded the saved scene in a later session.

Both CPU profiles used 500 Hz CPU-clock sampling with DWARF call chains, lasted
approximately 25 seconds, and lost no samples. These are inclusive percentages
of video-thread CPU samples; parent and child percentages overlap.

| Function | Before descriptor cache | With descriptor cache |
| --- | ---: | ---: |
| `StateTracker::UpdateGXDescriptorSet` | 12.04% | 8.36% |
| Driver `vkUpdateDescriptorSets` | 6.36% | 2.73% |
| Driver `vkAllocateDescriptorSets` | 3.09% | 1.32% |
| Texture-memory ARM CRC hash | 9.96% | 10.51% |
| `TextureCacheBase::LoadImpl` | 13.19% | 13.69% |

The new lookup itself accounted for 1.59% of video-thread samples, plus 0.34%
for insertion. The unchanged hashing path occupies a larger relative share
after descriptor work is removed; this does not itself indicate a hash regression.

Read-only live counters after profiling, normalized over intervals without a
counter reset:

| Measurement | With descriptor cache |
| --- | ---: |
| Rendered FPS | 37.8–38.7 |
| Emulation speed | Usually 63–65%, one 66% sample |
| Draws per frame | About 2,489 |
| Sampler-cache hits per frame | About 1,205 |
| Sampler-cache misses per frame | About 549 |
| Cache hit fraction | 68.7% |
| Draw/bind CPU time | 4.12 ms/frame |
| Uniform upload CPU time | 2.04 ms/frame |
| Fence wait | 0.010 ms/frame |
| Queue submissions | 1.00/frame |
| Newly compiled pipelines | Zero in the counter intervals |

The preceding draw-cache-only capture measured 5.44 ms/frame in draw/bind calls.
The approximately 1.3 ms difference is useful but too small to bridge the full
gap to a 16.7 ms frame budget. The new video-thread profile sampled 24.66 CPU
seconds over 24.97 wall seconds: approximately 99% of one core.

Nearby baseline telemetry was about 37 FPS, with CPU 2208 MHz and GPU 545 MHz.
The profiled cache session used those same reported clocks. The final build's
later warmed-up telemetry reached about 39 FPS with GPU 599 MHz, so that final
FPS value is not a controlled cache-only speedup. Saved game state improves
repeatability, but head pose, dynamic clocks, background load and scene evolution
still need controlling for precise comparisons.

## Texture hashing findings

The TMEM fast path and ARM CRC implementation already match the sibling
DolphinXR source. Current settings use `SafeTextureCacheColorSamples=512`;
forced-safe copies still use full hashing. RAM content, palettes and EFB copies
can change independently of a frame number, so a per-frame hash memo cannot
replace content validation safely.

Two standalone Quest experiments preserved the existing hash outputs:

1. A contiguous CRC loop unroll passed 458,864 comparisons covering alignment,
   lengths, tails and sampled/full modes. Timing showed no useful improvement.
2. Caching sampled bytes and verifying them with scalar/NEON comparisons passed
   422,708 equivalence cases, including sampled and unsampled mutations. A single
   hot 4 KiB texture improved, but a 512-texture working set was slower even when
   unchanged: 495 ns versus 284 ns for 4 KiB textures, 2,134 versus 1,734 ns for
   32 KiB textures, and 4,586 versus 4,031 ns for 256 KiB textures (samples=512).
   Full hashing also lost on larger working sets. These tests already assumed
   the cache slot was known; actual lookup/invalidation would add overhead.

Neither experiment changed production hashing or settings. Smaller possibilities
remain: reuse a just-computed failed validation hash when the subsequent load
uses identical pointer/size/layout/sampling, or cache palette hashes against
renderer-owned TMEM write generations. Both need call-frequency measurements
before implementation; neither is expected to remove the main frame-time deficit.

## Validation and reproduction

- Three focused cache tests passed on Quest: every key field matters, forced
  collisions cannot return the wrong set, and clearing prevents lifetime reuse.
  They are integrated into the Vulkan-enabled native test build; the full native
  suite was not run. The tests also compiled standalone against bundled GoogleTest
  and Vulkan headers with the installed Android NDK.
- Quest APK builds passed, including the final storage-image layout correction.
  The final APK was installed with `adb install -r` and the saved scene loaded.
- The user reported no texture problems after loading the cache build, and also
  confirmed that the heavy scene still lags. A compositor image showed both eyes,
  world geometry and HUD. No fatal signal or Vulkan error was found in the
  checked gameplay logs. Startup did contain Android OpenXR worker-registration
  warnings; those are separate from descriptor validity.
- A second read-only review found no stale-cache path across pool reset, sampler
  replacement, texture destruction, save-state load, or GX/utility transitions.
- Changed regions were formatted with clang-format 19.1.5; `git diff --check`
  passed. Desktop and non-VR builds were not separately compiled.

To reload the same file through ADB-driven Android UI, enable
`[Core] EnableSaveStates=True`, open the in-game Android menu, select Load State,
then select the entry whose timestamp corresponds to `GM8E01.s01`. The current
Android menu labels this **Slot 2**, because it passes a zero-based menu index to
`State::Load`; the VR menu's slot 1 refers to `.s01`. The saved file was not
renamed or overwritten. Close the submenu and main menu after loading. The
Android state-menu option was left enabled; other game/graphics settings were
retained and the original persisted Logger.ini was restored.

Artifacts are under the ignored `Source/Android/app/build/descriptor-hash-profile/`
directory, including profiles, counter CSV/rates, state/config backups, and both
standalone hash experiments. Build logs are
`Source/Android/app/build/quest-descriptor-cache-build.log` and
`Source/Android/app/build/quest-descriptor-cache-final-build.log`.
