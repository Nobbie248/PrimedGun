# Quest multiview port — 2026-09-12

The specialized and uber vertex shaders now select the per-eye projection with
`gl_ViewIndex` when Vulkan multiview is active. They bind the existing geometry
uniform block and share `GenerateVRProjection` with the geometry shader path.
This keeps PrimedGun's world projection, curved and perspective HUD, cinematic
overrides, and per-draw depth ordering consistent between rendering modes.
Point/line expansion offsets are retained after reprojection. Vertex shader UID
versions invalidate the previously cached flat multiview shaders.

The first device test confirmed stereo and correct HUD/visor placement, but head
movement jumped between frames. Turning multiview off in the same APK restored
smooth movement. The layered swapchain acquisition now waits for pending Vulkan
frame finalization and uses `XR_INFINITE_DURATION`, replacing the 5 ms timeout
that attempted to release an image before a successful wait. With these changes
and multiview re-enabled, the tester confirmed smooth head movement.

Validation: `Source/Android/build-quest.ps1` built `app:assembleQuestDebug`
successfully. The resulting APK was installed on Quest 3 and tested in Metroid
Prime. The first multiview run created specialized VS/PS caches with host key
`FC6DFF44` and an empty GS cache; no bad-shader dumps were present. The user
confirmed stereo, HUD, visor, and then smooth head movement with the wait fix.
Exclusive ubershader mode and desktop backends were not separately device-tested.

Multiview is enabled in the connected Quest's GFX.ini; application defaults remain
unchanged. This change does not enable EFB foveation or finish swapchain foveation.

Local test logs and the original GFX/Logger configuration backups are in the
gitignored `Source/Android/app/build/multiview-validation/` directory. The Quest
disconnected after the final user test. `Logger-before.ini` was restored when the
Quest reconnected, before starting the foveation/resolution-scale work.
