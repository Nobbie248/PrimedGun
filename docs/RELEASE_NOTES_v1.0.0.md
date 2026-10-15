# PrimedFrame 1.0.0

Metroid Prime in VR, running natively on the Valve Steam Frame. PrimedFrame is a fork of Nobbie248's PrimedGun built as a native Linux Arm64 program with frame pacing written for SteamVR. It replaces running PrimedGun's PC build in a Linux container with the Frame's graphics driver mounted in. You need your own Metroid Prime disc image; no game data is included.

### The game on the Frame

- A native Linux Arm64 build, launched from the Steam library into the Frame's own SteamVR. No container, no PC.
- PrimedGun's game code, controls and in-VR menu as they are. No desktop window; settings are the ini files and the menu.
- Settings, memory card and save states in plain folders under `~/.local/share/PrimedFrame`.

### Resolution and frame rate

- Internal resolution 3x (1920x1584 per eye) and 2160 pixel eye images, the panel's native size. The container setup ran 2x and 1728.
- The 120 Hz display, with SteamVR pacing the game at its own 60 frames a second.
- Heavy rooms hold 60 frames a second: 59.5 on average in the heaviest room I know, where the container setup ran 20 to 30.
- No doubled images, and a steady helmet HUD.

### What changed under the hood

- The Frame's GPU renders straight to memory instead of in tiles. The stereo geometry shader made every draw run once per tile; the GPU time per frame in the heaviest room went from 19 ms to 6 ms at the old settings (measured headless). That paid for 3x and 2160.
- Eye images are released to SteamVR only after the GPU has finished them, instead of draining the GPU every frame.
- Each game frame goes to SteamVR in its own frame cycle, with one landing per room.
- Head and controller poses are predicted for the refresh the frame really appears on.
- The game's video timing is exactly 60 Hz instead of 59.94, so frames stop drifting against SteamVR's slots.
- The GPU gets the game's drawing every 100 draws, and the eye image is no longer cleared before it is overwritten.
- The shader cache works now. The no-window build never created its folder, so every launch recompiled every shader; only the first launch stutters.
- GPU texture decoding stays off, because the Frame's GPU faults with it on.

### Install

- An installer that unpacks the release, seeds the settings, finds your disc image and adds the Steam shortcut with the VR flag and the library art.
- The in-VR menu's settings are saved (the no-window build lost them at exit; its Save and Exit Game buttons work now).
- A launcher that sets SteamVR's per-app settings at every launch (120 Hz, SteamVR pacing the game at 60, no extra prediction, motion smoothing off, 2160 per eye) and keeps session logs.
- Tools to collect logs for a bug report and to remove the program again.

## Download

- `primedframe-1.0.0-linux-arm64.tar.gz`
- `SHA256SUMS`

## Install

Unpack the tarball on the Frame and run its `install.sh --rom <your disc image>`. The [README](https://github.com/simplysanatori/PrimedFrame/blob/native/README.md) has the full steps, the settings and the troubleshooting.

## Known

- The game's world, enemies and the arm cannon move at 60 frames a second. On the 120 Hz display an object your eye follows can look doubled along its movement. Head movement is corrected at 120.
- A very light jitter is left on the helmet HUD on fast head turns.
- The first launch stutters while the shaders compile.
- A 75 ms hitch at the step from the ship flight intro into play. It is the game's own.
- Played on one Steam Frame with the US disc.

## Reporting problems

Open an issue with what you did, what you expected and what happened, and the archive from `~/Games/PrimedFrame/app/tools/collect-logs.sh`.

## Credits

Nobbie and [PrimedGun](https://github.com/Nobbie248/PrimedGun), iChris4 and Dolphin ReduX, Carl Kenner and the Dolphin VR contributors, josethevrtech and PrimedSteam, the Dolphin team, and Valve. The README has the links and the full list.
