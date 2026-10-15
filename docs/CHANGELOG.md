# Changelog

## 1.0.0 (October 2026)

The first release of PrimedFrame, Metroid Prime in VR running natively on the Valve Steam Frame. It is a fork of Nobbie248's PrimedGun. It replaces running PrimedGun's PC build in a Linux container on the Frame.

### The game on the Frame

- A native Linux Arm64 NoGUI build that runs in the Frame's SteamVR, launched from the Steam library. No container, no PC.
- PrimedGun's game code, controls and in-VR menu as they are.
- Settings, memory card and save states in plain folders under `~/.local/share/PrimedFrame`.

### Resolution and frame rate

- Internal resolution 3x (1920x1584 per eye) and 2160 pixel eye images.
- The 120 Hz display, with SteamVR pacing the game at 60 frames a second.
- Heavy rooms hold 60 frames a second (59.5 on average in the heaviest room I know). No doubled images.

### Rendering and pacing

- Direct rendering on the Frame's GPU (`TU_DEBUG=sysmem`): the GPU time per frame in the heaviest room went from 19 ms to 6 ms at the old settings (measured headless).
- Eye images released to SteamVR after a fence shows the GPU finished them, with no GPU drain.
- Each game frame in its own SteamVR frame cycle, with one landing per room.
- Poses predicted for the refresh the frame appears on.
- The game's video timing at exactly 60 Hz instead of 59.94.
- The GPU handed the game's drawing every 100 draws; the eye image is not cleared before it is overwritten.
- The shader cache works; only the first launch stutters.
- GPU texture decoding off by default, because the Frame's GPU faults with it on.

### Install

- An installer that unpacks the release, seeds the settings, finds your disc image and adds the Steam shortcut with the VR flag.
- A launcher that sets SteamVR's per-app settings at every launch and keeps session logs.
- Tools to collect logs for a bug report and to remove the program again.
- A privacy check for my own additions, the history after upstream and the release tarball.
