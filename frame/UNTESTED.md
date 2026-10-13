# Built but not confirmed in the headset

One line per claim. The documents carry `[to confirm in the headset]` at each of these. When I have tested one, I delete its line here and complete the text in README.md, docs/RELEASE_NOTES_v1.0.0.md, docs/CHANGELOG.md and docs/TECHNICAL_NOTES.md.

Nothing below has a verdict yet.

## Status and performance

- The overall verdict: PrimeFrame starts from the Steam library and plays Metroid Prime through SteamVR.
- Smooth head motion, no ghosting in either eye on turns.
- Performance in the heavy rooms, and any number: frame rate, GPU time per eye set, late and repeated sets, hitches.
- 1728 pixels per eye with 2x internal resolution looking sharp enough, and 120 Hz being reached.
- Eye images released to SteamVR only after the GPU has finished them (the pacing work in frame/DEV.md).
- Shader compilation in the background causing no visible hitch in play.
- The x11 platform under Steam's session on the Frame (the headless runs used a private X server).
- Valve's foveation layer on or off (`PRIMEFRAME_FDM_LAYER`) making no visible difference.
- `PRIMEFRAME_SYSMEM=1` (Turnip rendering straight to memory) as a speed gain without a visible problem.

## Controls

- The whole layout under SteamVR's automatic remap to the Frame controllers: aiming, the visor, the arm cannon, jump, scan, missiles, map.
- Setting the height with the right stick and opening the settings menu with the left thumbstick, as PrimedGun describes.
- Exit Game from the in-VR menu leaving cleanly and writing the shader caches.

## Install and first run

- `install.sh` adding the Steam shortcut through Steam's debugging port, and updating it, with the library entry showing up and launching.
- The shortcut's OpenVR flag: the installer creates it through Steam's client API and I have not checked what that sets.
- Copying the art of the existing "PrimedGun - Metroid Prime VR" shortcut to the new app id, and the icon.
- The per-app SteamVR settings (120 Hz, no throttling, no extra prediction, motion smoothing off, 1728 resolution) taking effect from a library launch, and the read back in `launch.log`.
- The sessions folder, `report.txt` and `vrtiming.log` from a real session, with the real statistics line fields.
- A launch without a disc image refusing cleanly.
- The memory card in `user/GC` being used and kept between launches.
- Importing from the old Podman setup (`--import-podman`, in frame/DEV.md): the save and the states loading in the new build.
- `collect-logs.sh` and `uninstall.sh` on the real install.
- Installing from the release tarball on a clean Frame.
