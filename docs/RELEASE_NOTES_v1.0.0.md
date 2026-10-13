# PrimeFrame 1.0.0

Metroid Prime in VR, running natively on the Valve Steam Frame. PrimeFrame is a fork of Nobbie248's PrimedGun with a native Linux Arm64 NoGUI build, Vulkan on SteamVR's own device and a launch wrapper for the Frame. You need your own Metroid Prime disc image; no game data is included.

Nothing in this release has been confirmed in the headset yet. Every claim that needs it is marked [to confirm in the headset].

### The game on the Frame

- A native Linux Arm64 build that runs in the Frame's SteamVR. No container, Lepton, APK or PC.
- PrimedGun's VR game code as it is: the visor, the arm cannon, the in-headset settings menu. [to confirm in the headset]
- No desktop window. Settings are the ini files and PrimedGun's own menu.
- Settings, memory card and save states in plain folders under `~/.local/share/PrimeFrame`.

### Rendering

- Vulkan on SteamVR's own device, with the OpenXR loader built in.
- 1728 pixels per eye, SteamVR's own 100 % on the Frame, and 2x internal resolution as the starting point. [to confirm in the headset]
- Eye images handed to SteamVR only after the GPU has finished them. [to confirm in the headset]
- GPU texture decoding off by default: with it on, the Frame's GPU reads bad memory and takes SteamVR down.
- Smooth head motion and no ghosting in either eye. [to confirm in the headset]

### Controls

- PrimedGun's own controls through OpenXR, remapped to the Frame controllers by SteamVR. [to confirm in the headset]

### Install

- An installer that unpacks the program, creates the settings folder where files are missing, finds your disc image, and adds the Steam library entry.
- A launch wrapper that sets SteamVR's per-app settings (120 Hz, no throttling, no extra prediction, motion smoothing off, 1728 resolution) at every launch and writes a session folder with the logs and a report.
- Tools to collect logs for a bug report and to remove the program again.

## Download

- `primeframe-1.0.0-linux-arm64.tar.gz`
- `primeframe-1.0.0-linux-arm64.tar.gz.sha256`

## Install

Clone the repository, then run `frame/install.sh --rom <your disc image>` with the tarball as its argument. The [README](https://github.com/simplysanatori/PrimeFrame/blob/native/README.md) has the full steps, the settings and the troubleshooting.

## Known

[to confirm in the headset]

## Reporting problems

Open an issue with what you did, what you expected and what happened, and the archive from `~/Games/PrimeFrame/app/tools/collect-logs.sh`.
