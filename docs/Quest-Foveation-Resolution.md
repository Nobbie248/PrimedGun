# Quest foveation and OpenXR resolution scaling

Graphics settings in the Quest build now expose Vulkan multiview, OpenXR resolution
scale, foveation level (off/low/medium/high), dynamic foveation, and experimental EFB
foveation. These controls require restarting the game. Multiview retains its native
default of off; it is enabled in the test Quest's existing configuration.

`GFX.ini` `[VR]` settings:

```ini
UseVulkanMultiview = True
ResolutionScale = 0.85
FoveationLevel = 2
DynamicFoveation = True
FoveateEFB = False
```

ResolutionScale defaults to 0.85 on Android and 1.0 elsewhere. It scales the OpenXR
runtime's recommended eye dimensions once during system initialization, separately
from Dolphin's internal EFB resolution (still 3x on the test Quest). Values are
clamped to 0.5–2.0, scaled dimensions rounded to multiples of four and limited to
the runtime maximum. Non-finite input falls back to 1.0. At scale 1.0, recommended
dimensions are preserved exactly. The native per-game VR override loader handles
these settings too.

Fixed foveation is deliberately limited to the final layered stereo swapchain.
The Vulkan backend enables the available OpenXR foveation extensions, obtains the
runtime-owned density-map images, attaches their views to the multiview render
passes, and applies the selected profile. Per-eye swapchains used for cinematic
quads and PrimedGun's separate overlay swapchains remain unfoveated. Dynamic mode
lets the runtime adjust the strength; it does not track the user's eyes.

Unsupported devices/configurations use ordinary rendering. Failed foveated setup
retries a regular layered swapchain. Resource setup completes before density-map
transitions are recorded; teardown drains recorded GPU work and destroys dependent
framebuffers/views before OpenXR destroys the runtime images. The previous layered
image wait and pacing-thread submission fixes are retained.

The existing EFB foveation implementation is exposed as an experimental opt-in and
remains off by default. It requires multiview, no MSAA, and a nonzero foveation
level. It can hurt performance in games with frequent EFB copies and was not
enabled during this validation.

Validation on Quest 3, 2026-09-12:

- `Source/Android/build-quest.ps1`: both implementation and final UI builds passed.
- Installed the final Quest debug APK and launched Metroid Prime.
- Default run: both eyes changed from 1680×1760 to 1428×1496; logs confirm a
  three-image layered foveated swapchain and a successfully applied level-2 dynamic
  profile. EFB foveation was disabled. Captured OpenXR pacing samples had zero errors.
- Off/1.0 run: both eyes used 1680×1760 and the layered swapchain was unfoveated;
  no profile was applied. Captured pacing samples again had zero errors.
- Restored the pre-test GFX.ini (multiview on, scale/foveation use new defaults)
  and original Logger.ini, then relaunched the final APK.
- Subjective gameplay clarity and head-motion smoothness with foveation are awaiting
  user feedback. No performance improvement is claimed from these launch checks.
- Desktop backends, EFB opt-in, and forced resource-allocation failures were not
  separately runtime-tested.

Logs, screenshots and configuration backups are in the ignored directory
`Source/Android/app/build/multiview-validation/`.
