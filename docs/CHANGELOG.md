# Changelog

## 1.0.0 (October 2026)

The first release of PrimeFrame, Metroid Prime in VR running natively on the Valve Steam Frame. It is a fork of Nobbie248's PrimedGun. Nothing here has been confirmed in the headset yet; claims that need it are marked [to confirm in the headset].

### The game on the Frame

- A native Linux Arm64 NoGUI build that runs in the Frame's SteamVR. No container, Lepton, APK or PC.
- PrimedGun's VR game code as it is. [to confirm in the headset]
- Settings, memory card and save states in plain folders under `~/.local/share/PrimeFrame`.

### Rendering

- Vulkan on SteamVR's own device, with the OpenXR loader built in.
- 1728 pixels per eye and 2x internal resolution as the starting point. [to confirm in the headset]
- Eye images handed to SteamVR only after the GPU has finished them. [to confirm in the headset]
- GPU texture decoding off by default, because the Frame's GPU reads bad memory with it on.

### Controls

- PrimedGun's controls through OpenXR, remapped to the Frame controllers by SteamVR. [to confirm in the headset]

### Install

- An installer that unpacks the program, creates the settings folder, finds your disc image and adds the Steam library entry.
- A launch wrapper that sets SteamVR's per-app settings at every launch and writes a session folder with the logs and a report.
- Tools to collect logs for a bug report and to remove the program again.
- A privacy check for my own additions, the history after upstream and the release tarball.
