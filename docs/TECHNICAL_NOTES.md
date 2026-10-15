# PrimedFrame: technical notes

PrimedFrame is a fork of Nobbie248's [PrimedGun](https://github.com/Nobbie248/PrimedGun), the Dolphin based build that plays Metroid Prime in VR through OpenXR. PrimedFrame builds it as a native Linux Arm64 NoGUI program (`dolphin-nogui`, renamed `primedframe` on install) that runs in the Valve Steam Frame's own SteamVR. This document is for version 1.0.0.

Install, settings, troubleshooting and how to report problems are in the [README](../README.md). This one says how the build and the wrapper work and what I learned about SteamVR on the Frame. The developer notes with the build flags, the headless test setup and the statistics line are in [frame/DEV.md](../frame/DEV.md).

Not affiliated with Valve, Nintendo, the Dolphin team or the PrimedGun author.

## AI disclosure

I did not write this code by hand. The build, the wrapper, the scripts and these notes were written with Anthropic's Claude (Fable 5.1, Opus 5.5 and Sonnet 5.5, through Claude Code), running on my Steam Frame. I chose what to attempt, reviewed the code and the logic of the changes and the scripts as they were written, and tested each build in the headset. PrimedGun's own README says Codex was used to assist its development. Neither Nobbie nor the Dolphin team has reviewed this port. Read the code before trusting it.

## Status

I played this on one Steam Frame in the headset. Smoothness, the frame rate in heavy rooms, the absence of doubled images, starting from the title screen, the memory card save, the controls under SteamVR's automatic binding and the in-VR menu are confirmed; [frame/UNTESTED.md](../frame/UNTESTED.md) lists what is built but not tested.

## How it works

### Build

`frame/build.sh` is the single source of the CMake flags: Ninja, clang, Release, Vulkan and VR on, Qt off, NoGUI on, analytics, Discord, achievements, LLVM and auto update off, and `LINUX_LOCAL_DEV=ON`. That last flag gives a relocatable layout: `Sys/` and `assets/` are looked up next to the binary, so an install is a folder copy. The program also takes `-u <dir>` for the user folder, which the wrapper uses to keep everything under `~/.local/share/PrimedFrame/user`. The OpenXR loader, SDL3 and the other bundled libraries are linked in; the rest are stock SteamOS packages. Vulkan is loaded at run time.

### Launch

The Steam shortcut's program is `~/Games/PrimedFrame/app/primedframe` and its launch options are `"<app>/primedframe.sh" %command%`, so Steam passes the program's path to the wrapper. The wrapper:

1. makes the session folder `~/.local/share/PrimedFrame/sessions/<date-time>/` and points `sessions/latest` at it;
2. sources `session.env` once if it exists (one launch of test settings), then renames it to `session.env.used`;
3. reads the disc image path from `~/.local/share/PrimedFrame/rom` and refuses to start with a clear line in `launch.log` if the file is missing (there is no window to ask in);
4. sets the SteamVR per-app settings for the shortcut's app id with `vrcmd` and reads them back from `steamvr.vrsettings`;
5. starts `vrtiming.py`, which logs SteamVR's compositor timing once a second;
6. runs `primedframe -u <user> -p x11 -v Vulkan -e <disc image>` and stays alive to stop the logger, copy `dolphin.log` and a stall file if there is one, and write `report.txt`.

The platform is `x11`. Dolphin's headless platform never presents VR frames, because the Vulkan presenter returns early without a swapchain; on the Frame the X11 window lives in Steam's own session.

### Settings

The seed in `frame/seed/user` is what the installer puts in the user folder where files are missing. The values are the ones I run: Vulkan, 3x internal resolution, background shader compilation, the `[VR]` section of `GFX.ini`, the video timing at exactly 60 Hz (`VIOverclock`), and `EnableGPUTextureDecoding = False`. With GPU texture decoding on, the Frame's GPU (Adreno, Turnip driver) reads freed memory and the kernel logs a GPU fault that takes SteamVR down. `Logger.ini` turns on the log file for the OpenXR and Video categories at info level, so the once a second statistics line (`XRSTATS key=value ...`) reaches `dolphin.log`.

### Session report

`tools/session-report.py` reads the statistics lines key by key, so a new field in the line needs no change in the script, and puts the numbers from SteamVR's compositor beside them. Several sessions make several columns of one table.

## What I learned about SteamVR on the Frame

- SteamVR shows a swapchain image the moment it is released, finished or not, for Vulkan too. An image has to be released only after a fence or timeline wait shows that the GPU finished it. This costs no latency.
- Only one waited, unreleased image per swapchain exists. Pipelining two frames needs two sets of eye swapchains.
- SteamVR allocates only mip level 0 of a Vulkan swapchain image, so swapchains have a single level.
- The head should be located once per refresh and both eyes rendered from exactly those poses; releasing and ending the frame with the same poses. A right eye rendered a refresh later got a random placement error.
- The Vulkan instance must be 1.0 to 1.2, with newer features as extensions, and there is one queue; the SteamVR client submits on it inside the frame calls in our process.
- `XR_EXT_frame_synthesis` and `XR_FB_space_warp` are accepted and ignored, as the GalaxyFrame probe showed on the same runtime; I have not tested depth submission in this game.
- SteamVR drops per-app settings now and then, so the launcher sets them at every launch.
- SteamVR takes focus for a few milliseconds now and then. That is not a pause.
- Valve's implicit layer `XR_APILAYER_VALVE_fdm_injection` sits in the Vulkan chain. `PRIMEDFRAME_FDM_LAYER=0` turns it off for an A/B comparison.

## Pacing

The full record, with the headset sessions and the numbers, is [frame/PACING.md](../frame/PACING.md). The short version:

### The designs I tried

- **The first loop** (a drain): after every frame the CPU waited for the GPU, then released the eye images. It fixed the ghosting in the right eye but serialised the CPU, the GPU and the compositor, which cost frame rate.
- **A pipelined loop** with two sets of eye swapchains and a separate pacing thread that ended a frame on every refresh. At 60 game frames on 120 Hz it repeated the last layer on the refresh between new frames, with poses located for the refresh before. The headset showed doubled images when turning the head, in every variant I tried.
- **The frame-per-cycle loop** won. Each game frame runs in its own `xrWaitFrame` cycle: wait, begin, locate the views once, draw, release after a fence, end the frame with the same display time and poses, and let SteamVR show the frame twice itself. A releaser thread waits for the fence so the emulation thread keeps going. The pipelined and drain loops stay as switches (`PRIMEDFRAME_PACING`).

### The landing plan

The only thing that matters for the cadence is which refresh a frame is shown on. Each frame is planned at its `xrWaitFrame` to land on the refresh after the last frame's, whatever refresh SteamVR handed it. A frame handed an earlier refresh is held by the releaser to just past the on-time limit, and its poses are located for the refresh it lands on. A frame lands a refresh later only when it cannot make it. That gives one landing per room: the light room settles on its shortest landing, the heavy room lands every frame a period late, constant.

### Prediction

The game predicts head and controller poses for the refresh the frame really appears on, not for the display time `xrWaitFrame` returned (`PRIMEDFRAME_PREDICT_EXTRA_MS=auto`, the launcher's default). Less lag in the headset.

### Exactly 60 Hz

Dolphin's video timing is 59.94 Hz. With `VIOverclock = 1.001001` the game runs at exactly 60, 0.1 % faster, so frames stop drifting against SteamVR's slots.

### Heavy rooms

Measured headless on the Frame, the GPU time per frame in the heaviest room fell from 19 ms to 6 ms at the old settings when I set `TU_DEBUG=sysmem`. Turnip's default tiled mode runs a draw once per tile, and PrimedGun's stereo geometry shader writes both eyes, so every draw ran once per tile. Direct rendering paid for 3x and 2160. The other changes for heavy rooms: the next frame's `xrWaitFrame` overlaps the last frame's end, the GPU gets the drawing every 100 draws instead of all at the end of the frame, and the eye image is not cleared before the blit.

### The shader cache

The no-window build never created its shader cache folder, so every launch recompiled every shader. With the folder created, only the first launch stutters.

### The HUD

The helmet and the 2D HUD are drawn head-locked into the projection layer, and SteamVR turns that whole layer from the poses it was submitted with to the head at display time. Any prediction error therefore moves the HUD. With the pacing and the prediction fixed, a very light jitter is left on fast head turns. I built a test ring on a composition layer of its own in view space, which SteamVR does not turn, and it looked almost still. The HUD as its own layer is not built into the game yet.

## Build from source

```
frame/build.sh . dolphin-nogui
frame/install.sh --dev build-linux
```

The second line installs the build as a separate "PrimedFrame (dev)" shortcut in `~/Games/PrimedFrame/app-dev`. Run it again after a rebuild.

## Release

`frame/release.sh` packs the binary, `Sys/`, `assets/`, the wrapper, the install, uninstall and log scripts, the two run time tools, the seed, the README and the licences into `primedframe-1.0.0-linux-arm64.tar.gz`, and runs the privacy check on the tarball. The ROM, saves, states and anything from the user folder are never packed.
