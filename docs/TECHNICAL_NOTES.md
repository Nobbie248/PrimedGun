# PrimeFrame: technical notes

PrimeFrame is a fork of Nobbie248's [PrimedGun](https://github.com/Nobbie248/PrimedGun), the Dolphin based build that plays Metroid Prime in VR through OpenXR. PrimeFrame builds it as a native Linux Arm64 NoGUI program (`dolphin-nogui`, renamed `primeframe` on install) that runs in the Valve Steam Frame's own SteamVR. This document is for version 1.0.0.

Install, settings, troubleshooting and how to report problems are in the [README](../README.md). This one says how the build and the wrapper work and what I learned about SteamVR on the Frame. The developer notes with the build flags, the headless test setup and the statistics line are in [frame/DEV.md](../frame/DEV.md).

Not affiliated with Valve, Nintendo, the Dolphin team or the PrimedGun author.

## AI disclosure

I did not write this code by hand. The build, the wrapper, the scripts and these notes were written with Anthropic's Claude (Fable 5.1, Opus 5.5 and Sonnet 5.5, through Claude Code), running on my Steam Frame. I chose what to attempt and reviewed the scripts as they were written. PrimedGun's own README says Codex was used to assist its development. Neither Nobbie nor the Dolphin team has reviewed this port. Read the code before trusting it.

## Status

Nothing is confirmed in the headset yet. What exists: the program builds on the Frame itself, starts headless against a mock OpenXR runtime, and the installer and the wrapper pass their dry run tests (`frame/test-installer.sh`). Everything that needs the headset is marked [to confirm in the headset], and [frame/UNTESTED.md](../frame/UNTESTED.md) lists it.

## How it works

### Build

`frame/build.sh` is the single source of the CMake flags: Ninja, clang, Release, Vulkan and VR on, Qt off, NoGUI on, analytics, Discord, achievements, LLVM and auto update off, and `LINUX_LOCAL_DEV=ON`. That last flag gives a relocatable layout: `Sys/` and `assets/` are looked up next to the binary, so an install is a folder copy. The program also takes `-u <dir>` for the user folder, which the wrapper uses to keep everything under `~/.local/share/PrimeFrame/user`. The OpenXR loader, SDL3 and the other bundled libraries are linked in; the rest are stock SteamOS packages. Vulkan is loaded at run time.

### Launch

The Steam shortcut's program is `~/Games/PrimeFrame/app/primeframe` and its launch options are `"<app>/primeframe.sh" %command%`, so Steam passes the program's path to the wrapper. The wrapper:

1. makes the session folder `~/.local/share/PrimeFrame/sessions/<date-time>/` and points `sessions/latest` at it;
2. sources `session.env` once if it exists (one launch of test settings), then renames it to `session.env.used`;
3. reads the disc image path from `~/.local/share/PrimeFrame/rom` and refuses to start with a clear line in `launch.log` if the file is missing (there is no window to ask in);
4. sets the SteamVR per-app settings for the shortcut's app id with `vrcmd` and reads them back from `steamvr.vrsettings`;
5. starts `vrtiming.py`, which logs SteamVR's compositor timing once a second;
6. runs `primeframe -u <user> -p x11 -v Vulkan -e <disc image>` and stays alive to stop the logger, copy `dolphin.log` and a stall file if there is one, and write `report.txt`.

The platform is `x11`. Dolphin's headless platform never presents VR frames, because the Vulkan presenter returns early without a swapchain; on the Frame the X11 window lives in Steam's own session.

### Settings

The seed in `frame/seed/user` is what the installer puts in the user folder where files are missing. The values are the ones I run: Vulkan, 2x internal resolution, background shader compilation, the `[VR]` section of `GFX.ini`, and `EnableGPUTextureDecoding = False`. With GPU texture decoding on, the Frame's GPU (Adreno, Turnip driver) reads freed memory and the kernel logs a GPU fault that takes SteamVR down. `Logger.ini` turns on the log file for the OpenXR and Video categories at info level, so the once a second statistics line (`XRSTATS key=value ...`) reaches `dolphin.log`.

### Session report

`tools/session-report.py` reads the statistics lines key by key, so a new field in the line needs no change in the script, and puts the numbers from SteamVR's compositor beside them. Several sessions make several columns of one table.

## What I learned about SteamVR on the Frame

- SteamVR shows a swapchain image the moment it is released, finished or not, for Vulkan too. An image has to be released only after a fence or timeline wait shows that the GPU finished it. This costs no latency.
- Only one waited, unreleased image per swapchain exists. Pipelining two frames needs two sets of eye swapchains.
- SteamVR allocates only mip level 0 of a Vulkan swapchain image, so swapchains have a single level.
- The head should be located once per refresh and both eyes rendered from exactly those poses; releasing and ending the frame with the same poses. A right eye rendered a refresh later got a random placement error.
- The Vulkan instance must be 1.0 to 1.2, with newer features as extensions, and there is one queue; the SteamVR client submits on it inside the frame calls in our process.
- `XR_EXT_frame_synthesis` and `XR_FB_space_warp` are accepted and ignored. Depth submission works.
- SteamVR drops per-app settings now and then, so the wrapper sets them at every launch.
- SteamVR takes focus for a few milliseconds now and then. That is not a pause.
- Valve's implicit layer `XR_APILAYER_VALVE_fdm_injection` sits in the Vulkan chain. `PRIMEFRAME_FDM_LAYER=0` turns it off for an A/B comparison.

## Pacing

How the build paces frames against SteamVR is described in [frame/DEV.md](../frame/DEV.md). How it behaves in the headset [to confirm in the headset].

## Build from source

```
frame/build.sh . dolphin-nogui
frame/install.sh --dev build-linux
```

The second line installs the build as a separate "PrimeFrame (dev)" shortcut in `~/Games/PrimeFrame/app-dev`. Run it again after a rebuild.

## Release

`frame/release.sh` packs the binary, `Sys/`, `assets/`, the wrapper, the install, uninstall and log scripts, the two run time tools, the seed, the README and the licences into `primeframe-1.0.0-linux-arm64.tar.gz`, and runs the privacy check on the tarball. The ROM, saves, states and anything from the user folder are never packed.
