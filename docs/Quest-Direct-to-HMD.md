# Quest direct-to-HMD presentation

The Android OpenXR presenter now honors `[VR] AndroidDirectToHMD = True`.
While an OpenXR session and swapchain are active, it skips binding, drawing, and
presenting the Android backbuffer. The existing eye-blit helper still handles
multiview stereo, per-eye fallback, and PrimedGun's cinematic screen. Eye blits and
submission remain inside the existing pose/content handoff. Inline OpenXR also
blits its eyes here because it no longer passes through `RenderXFBToScreen`.

Frame resources advance once per presented frame:

- Direct mode: the OpenXR submission advances the resource ring.
- Mirror mode: `PresentBackbuffer` advances it; the subsequent OpenXR submission
  does not advance it again.

Direct mode submits recorded game work even if no eye image was acquired, so
transient GPU resources can retire while OpenXR temporarily skips rendering.
Duplicate XFB publications retain the existing pacing-thread skip. Multiview's
pending-finalization wait and successful image-wait requirement are unchanged.

The performance log now counts direct OpenXR frame submissions as well as window
presents. This keeps `VKPERF` draw/submission/fence statistics available when the
Android backbuffer is skipped.

The shortcut is Android-only and requires an active OpenXR session/swapchain.
Before the session is running, or with the setting off, the usual window path is
used. The change retains the Android surface and normal renderer initialization;
it does not introduce rendering without an initial Android surface.

See `Quest-Open-Area-Performance.md` for the pre-change profile and explanation of
the redundant submissions/resource-ring advances.

Validation on Quest 3, 2026-09-12:

- `Source/Android/build-quest.ps1` completed successfully and the debug APK was
  installed after the user confirmed their progress was saved.
- With `AndroidDirectToHMD=False`, Metroid Prime launched and the submission log
  alternated window `advance=1,present=1` and XR `advance=0,present=0`. Steady
  title/menu samples reported two submissions per frame, with one ring advance.
- With the original config restored (`AndroidDirectToHMD=True`), the log showed
  XR worker submissions with `advance=1,present=0`. Steady title/menu samples
  reported one submission per frame and approximately 0.01 ms/frame of fence wait.
  Both eyes were visible in the captured compositor image.
- Neither launch capture contained a fatal signal, Java fatal exception, or
  Video/OpenXR error-level log. Internal resolution (3x), multiview, foveation,
  and culling settings were retained.
- `git diff --check` passed. Changed C++ regions were formatted with clang-format
  19.1.5. Desktop and non-VR builds were not separately compiled; the direct-mode
  predicate compiles to false outside Android VR.
- The heavy-area gameplay comparison and subjective HUD/visor/head-motion check
  are awaiting user feedback; title/menu timings are not comparable to the earlier
  heavy-area performance measurements.

Artifacts are in the ignored `Source/Android/app/build/direct-hmd-validation/`
directory (`build-1.log`, `mirror.log`, `direct.log`, `direct.png`, config backups).
The original GFX.ini and persisted Logger.ini were restored. The current running
process retains its diagnostic logging until its next restart.
