# PrimedFrame

Metroid Prime in VR, running natively on the Valve Steam Frame.

PrimedFrame is a fork of Nobbie248's [PrimedGun](https://github.com/Nobbie248/PrimedGun), the Dolphin based build that plays Metroid Prime in VR. PrimedGun has no Linux Arm64 build, so before this I ran its PC build on the Frame inside a Linux container with the Frame's graphics driver mounted in. PrimedFrame is the same game code built as a plain Linux Arm64 program with no window of its own, tuned to run in the Frame's own SteamVR. You do not need a container, Lepton, an APK or a PC.

You need your own Metroid Prime disc image. No game data is in this repository or its releases.

What it does:

- Plays Metroid Prime with PrimedGun's VR controls, visor and arm cannon, launched from the Steam library into the Frame's SteamVR.
- Renders at 3x internal resolution (1920x1584 per eye) into eye images of 2160 pixels per eye, the size of the Frame's panel.
- Runs the 120 Hz display with SteamVR pacing the game at its own 60 frames a second, two refreshes for each game frame.
- Holds 60 frames a second in heavy rooms.
- Keeps your settings, the in-VR menu's settings, your memory card and save states in one folder under your home.

## What this adds to PrimedGun

PrimedGun runs on Windows and on the Meta Quest. PrimedFrame changes the platform side and the frame pacing, and leaves the game side as it is.

Against the container setup I ran before:

| | Container setup | PrimedFrame |
|---|---|---|
| How it runs | PC build in a Linux container with the Frame's graphics driver mounted in | native Linux Arm64 build, started from the Steam library |
| Internal resolution | 2x (1280x1056 per eye) | 3x (1920x1584 per eye) |
| Eye images | 1728 per eye | 2160 per eye, the panel's native size |
| Display | 90 Hz with SteamVR's defaults | 120 Hz, SteamVR paces the game at 60 |
| Big rooms | 20 to 30 frames a second | 60 frames a second |
| Right eye | ghosted unless the GPU was drained every frame, which cost more frame rate | no doubled images |
| GPU texture decoding | off, after Adreno GPU faults | off, kept |
| In-VR menu settings | saved by the desktop window | saved by the game itself, there is no window |

The heaviest room I know averaged 59.6 frames a second in the headset session logs. The container setup ran it at 20 to 30.

Everything that was changed or added for the Frame, by area:

**The build**

- A native Linux Arm64 build of Dolphin's NoGUI program with PrimedGun's VR code: no Qt, no desktop window, Vulkan on SteamVR's own device, the OpenXR loader built in. The rest are stock SteamOS libraries.
- A relocatable layout: `Sys/` and `assets/` are looked up next to the program, the user folder is given with `-u`, so an install is a folder copy and nothing is written outside the folders under [Files](#files).
- The desktop window is not drawn or presented while the VR session runs. Under the X11 platform Steam gives the game, presenting it cost GPU time and hid pacing errors.
- The shader cache works. The no-window build never created its cache folder, so every launch recompiled every shader. Now only the first launch stutters.
- The in-VR menu's settings are saved. The no-window build had no way to keep them (PrimedGun keeps them through the desktop window's settings), so every launch reset the menu. They now live in `Config/PrimedGun.ini` and are written when the menu closes, when you press Save in it, and at exit. The Save and Exit Game buttons work.

**The GPU**

- Direct rendering on the Frame's GPU. The launcher sets `TU_DEBUG=sysmem` for the Turnip driver. In the driver's default tiled mode every draw runs once per tile, and PrimedGun's stereo geometry shader draws both eyes in one pass, so the heaviest room's 1250-draw pass ran nine times. Rendering straight to memory cut the GPU time per frame in that room from 19 ms to 6 ms at the old settings (measured headless on the Frame). That is what paid for 3x and 2160.
- 3x internal resolution (1920x1584 per eye) and 2160 pixel eye images as the defaults, chosen from a resolution table measured headless (see [Performance](#performance)).
- The eye images are no longer cleared before the game's image is blitted into them. The blit covers the whole image; the clear was 0.6 ms per frame at 1728 and more at 2160.
- The GPU gets the game's drawing every 100 draws instead of all at the end of the frame, so it works while the game is still emulating the frame instead of starting at the present.
- GPU texture decoding stays off. With it on, the Frame's GPU reads freed memory and the kernel logs a GPU fault that takes SteamVR down.
- Hi-res textures, background shader compilation, immediate XFB and the rest of the graphics settings are in the shipped ini files, with the values I run.

**The pacing**

- No GPU drain. Eye images go to SteamVR only after a fence shows the GPU has finished them. SteamVR shows a Vulkan image the moment it is released, finished or not, which is where the container setup's ghosting in the right eye came from; the container setup fixed it by draining the GPU every frame, which serialised the CPU, the GPU and the compositor. A releaser thread waits for the fence, so the emulation thread keeps going.
- One SteamVR frame cycle per game frame. The game runs at 60 frames a second and the display at 120. Each game frame runs in its own `xrWaitFrame` cycle: wait, begin, locate the head once, draw both eyes from exactly those poses, release after the fence, end the frame with the same display time and poses. SteamVR shows the frame on two refreshes itself. The variant with a pacing thread that ended a frame on every refresh, repeating the last pair, showed doubled images in the headset in every form I tried and is kept as a switch only.
- One landing per room. The only thing that decides whether motion is even is which refresh each frame is shown on. Each frame is planned at its `xrWaitFrame` to land on the refresh after the previous frame's, whatever refresh SteamVR hands it. A frame handed an earlier refresh is held just past the on-time limit, with its poses located for the refresh it lands on; a frame lands later only when it cannot make it. The light rooms settle on their shortest landing, the heaviest rooms land every frame one period late, constant, instead of flipping between the two.
- Prediction for the refresh the frame really appears on. Head and controller poses are located for that refresh, not for the display time `xrWaitFrame` first returned. In the headset this was less lag than without, in an A/B of two launches.
- The next frame's `xrWaitFrame` overlaps the last frame's end, so the game does not wait for SteamVR's hand-out of the next refresh before it starts emulating.
- SteamVR's own wake-up timing is measured from its blocking `xrWaitFrame` calls only, on `CLOCK_MONOTONIC_RAW`, which is the clock SteamVR's OpenXR times are on. An estimate that included the early hand-outs was a period too long and shook the HUD.
- Exactly 60 Hz. The GameCube's video timing is 59.94 Hz. With `VIOverclock = 1.001001` the game runs at exactly 60, 0.1 % faster than the original, so frames stop drifting against SteamVR's 60 Hz slots, which cost a late frame every 16 seconds.
- SteamVR's per-app settings for the game, set at every launch because SteamVR drops them now and then: 120 Hz, one frame to throttle (SteamVR paces the game at 60 and shows each frame twice), no extra prediction, motion smoothing off, 2160 pixel eye images.

**The launcher, the installer and the tools**

- A launcher for the Frame. The Steam entry runs the game through it. It keeps one folder per session with the launch log, the game's log, SteamVR's compositor timing and a report, applies the per-app settings and reads them back, refuses to start with a clear line in the log when the disc image is missing (there is no window to ask in), and takes one launch of test settings from `session.env`.
- An installer that unpacks the release, seeds the settings where files are missing, finds your disc image, adds the Steam entry with the VR flag and the library art (capsule, cover, hero, logo and icon), or updates the entry if it is there.
- A statistics line once a second in the game's log (`XRSTATS`, with the frame rate, GPU time, late frames, the landing plan's state and more), a per-frame line if asked, a logger for SteamVR's compositor counters, and a script that puts both into one table per session.
- Tools to collect masked logs for a bug report and to remove the program again.
- A privacy check that runs on my additions, on the history after upstream and on the release tarball before anything is published.
- A headless test setup: a mock OpenXR runtime with SteamVR's hand-out behaviour, a stamp check that proves each frame's poses match its display time, and a positive control that fails when an image is released before the GPU finished it. Everything in the pacing was proven there before a headset session.

## Status

Version 1.0.0 is the first release. It has been played on one Steam Frame (SteamOS 0.4.5, build 20261007.6125817) with the US disc.

Seen working in the headset: starting from the Steam library into the title screen, loading and saving the memory card save, PrimedGun's controls under SteamVR's automatic binding, the in-VR menu and its settings kept between launches, 120 Hz with the game at 60, 3x internal resolution with 2160 pixel eye images, heavy rooms at 60 frames a second, no doubled images, a steady helmet HUD. [frame/UNTESTED.md](frame/UNTESTED.md) lists what is built but not tested.

Known:

- The game's world, enemies and the arm cannon move at 60 frames a second. On the 120 Hz display an object your eye follows can look doubled along its movement. Head movement is corrected at 120.
- The helmet HUD has a very light jitter left on fast head turns. The HUD is drawn head-locked into the same image as the world, and SteamVR turns that whole image to your head at display time; any prediction error moves the HUD. A HUD on a layer of its own would fix it; only a test ring exists.
- In the heaviest rooms the picture is one refresh period behind the light rooms, by the landing plan. It is constant, not a flicker.
- The first launch stutters while the shaders compile.
- There is a 75 ms hitch at the step from the ship flight intro into play. It is the game's own.

## Requirements

- A Steam Frame with SteamOS and SteamVR, and a few hundred MB free.
- Your own Metroid Prime disc image, the US release (game ID GM8E01, revision 0, the version PrimedGun supports), as `.rvz`, `.iso` or `.gcm`. No disc image is provided; [Your disc image](#your-disc-image) shows how to make one.
- A terminal on the Frame: SSH, or Konsole from the desktop (SteamVR dashboard, Launch a program, Desktop).
- Steam running on the Frame while you install, so the installer can add the library entry.

### Your disc image

1. **Dump your own disc.** Follow Dolphin's guide, [Ripping Games](https://dolphin-emu.org/docs/guides/ripping-games/). A Wii with CleanRip reads GameCube discs. A GameCube-compatible disc drive on a PC does not read them.
2. **Shrink it (optional).** Open the image in Dolphin on a PC, right click the game and choose Convert File to make an `.rvz`. It is about 1 GB instead of 1.4.
3. **Check it is the right one.** In Dolphin, the game's properties show the ID and the revision. They must read GM8E01 and revision 0. A different region or revision does not work with PrimedGun.
4. **Put it on the Frame.** Copy it with a USB drive opened from the desktop, or with `scp` over SSH from a PC, into `~/Documents/Roms/gc`. The installer searches that folder. Anywhere else works too: give the path with `--rom`.

## Install

1. Download `primedframe-1.0.0-linux-arm64.tar.gz` and `SHA256SUMS` from the [Releases](https://github.com/simplysanatori/PrimedFrame/releases) page, onto the Frame.
2. In a terminal, in the folder you downloaded them to:

   ```sh
   sha256sum -c --ignore-missing SHA256SUMS
   mkdir -p ~/Downloads/primedframe && tar xf primedframe-1.0.0-linux-arm64.tar.gz -C ~/Downloads/primedframe
   ~/Downloads/primedframe/install.sh --rom "/path/to/Metroid Prime (USA).rvz"
   ```

   Leave out `--rom` when the image is in `~/Documents/Roms/gc`.

3. Put the headset on and launch **PrimedFrame** from your Steam library. The first launch stutters while the shaders compile; later launches do not.

The installer copies the program to `~/Games/PrimedFrame/app`, puts default settings in `~/.local/share/PrimedFrame` where files are missing, remembers the path of your disc image, adds **PrimedFrame** to your Steam library as a VR shortcut with its library art, or updates the entry if it is there, and sets SteamVR's per-app settings for it. It talks to Steam through its local debugging port, which SteamOS has on by default. It changes nothing outside the folders under [Files](#files), the shortcut and those settings. It never writes into a SteamVR file; the launcher sets the per-app settings through SteamVR's own `vrcmd`.

To update, download the new tarball and run `install.sh` again. Your settings, memory card and save states stay where they are; the installer only adds files that are missing. To change the disc image later, run `install.sh --rom PATH` again, or edit the one line in `~/.local/share/PrimedFrame/rom`.

To bring over an existing save, copy your memory card and save states into `~/.local/share/PrimedFrame/user` (`GC/` and `StateSaves/`) from a PC Dolphin or PrimedGun install, before the first launch. PrimedGun advises against carrying save states from one build to another. To bring over the in-VR menu settings of a PrimedGun desktop build, paste the `[primedgun]` section of its `Qt.ini` into `user/Config/PrimedGun.ini`.

To remove it, run `~/Games/PrimedFrame/app/tools/uninstall.sh`. It removes the Steam entry and the program; your data folder stays unless you add `--purge`.

## Running it on the Frame

Start SteamVR (put the headset on), open the Steam library and launch **PrimedFrame**. The entry has the VR flag, so SteamVR starts it as a VR app. There is no desktop window: the game starts straight into the headset, on the title screen, after a few seconds of black while SteamVR hands it the display.

At every launch the launcher does this, and `sessions/latest/launch.log` shows each step:

1. Makes a session folder under `~/.local/share/PrimedFrame/sessions/` and points `sessions/latest` at it.
2. Reads `session.env` if it exists, for one launch of test settings, and renames it `session.env.used`.
3. Reads the path of your disc image from the `rom` file and refuses to start, with `REFUSING TO START` in the log and exit status 66, if the file is not there.
4. Sets SteamVR's per-app settings for the Steam entry through `vrcmd` and reads them back from SteamVR's settings file:

   | SteamVR setting | Value | Why |
   |---|---|---|
   | Refresh rate | 120 Hz | The game runs at 60 frames a second. On 120 Hz each frame is shown exactly twice. On 90 Hz, 60 frames a second cannot be shown evenly. |
   | Frames to throttle | 1 | SteamVR paces the game at half the refresh, 60, and shows each frame on two refreshes. Without it SteamVR expects a new frame every refresh and the in-between refresh is mishandled. |
   | Additional frames to predict | 0 | The game predicts the poses for the right refresh itself. |
   | Motion smoothing | off | SteamVR did not apply it to the game in my sessions even when it was allowed; off keeps the cadence in the game's hands. |
   | Resolution | 2160 pixels per eye | The panel's native size. SteamVR's 100 % on the Frame is 1728. |

5. Starts a logger for SteamVR's compositor timing, once a second.
6. Sets `TU_DEBUG=sysmem` for the Turnip driver (direct rendering) and runs the game with the user folder, the X11 platform, Vulkan and your disc image.
7. When the game exits, stops the logger, copies the game's log and a stall file if the game wrote one, writes `report.txt`, and keeps the last 30 session folders.

In the game, PrimedGun's own notes apply: click the right stick to set your height, click the left thumbstick to open or close the in-headset settings menu, stay in the centre of your play space and face forward, and use Exit Game at the top right of the menu (press it twice) to leave the game cleanly. Exit Game writes the shader caches and the menu's settings. Quitting from the SteamVR dashboard also works; the launcher still writes the session report.

Save in the game as usual. The save goes to the memory card in `~/.local/share/PrimedFrame/user/GC`.

### One launch of test settings

Put `NAME=value` lines in `~/.local/share/PrimedFrame/session.env`. The launcher reads it for the next launch only and renames it afterwards; `session.env.next`, if present, becomes the launch after's `session.env`, for two-launch comparisons. These are the switches, with the defaults the game ships with:

| Switch | Default | What it does |
|---|---|---|
| `PRIMEDFRAME_STATE` | unset | Start from a save state: a slot number 1 to 10, or a file path. Useful to test a given room. |
| `PRIMEDFRAME_EYE` | `2160` | Eye image size asked of SteamVR. `1728` is SteamVR's 100 % on the Frame. |
| `PRIMEDFRAME_REFRESH_HZ` | `120` | Refresh rate asked of SteamVR, 90 or 120. |
| `PRIMEDFRAME_THROTTLE` | `1` | SteamVR's frames to throttle for the game. |
| `PRIMEDFRAME_MOTION_SMOOTHING` | `2` | SteamVR's motion smoothing for the game: 0 global setting, 1 on, 2 off, 3 always on. |
| `PRIMEDFRAME_SYSMEM` | `1` | `0` leaves the Turnip driver in its tiled mode (slow here). |
| `PRIMEDFRAME_PACING` | `cycle` | The frame loop: `cycle` (one SteamVR cycle per game frame), `pipelined` (a pacing thread; doubled images in the headset), `drain` (the GPU drained every frame, the container setup's loop), `inline` (as drain, fence waited for before the release). |
| `PRIMEDFRAME_CYCLE_END` | `plan` | `plan`: one landing per room. `asap`: each frame ends as soon as its GPU work is done, for an A/B. |
| `PRIMEDFRAME_CYCLE_OVERLAP` | `1` | `0`: the next frame's `xrWaitFrame` waits for the last frame's end. |
| `PRIMEDFRAME_PREDICT_EXTRA_MS` | `auto` | How far past SteamVR's display time the poses are predicted: `auto` is one period for each period the frame lands late, a number is fixed milliseconds, `0` is off. |
| `PRIMEDFRAME_PREDICT_ONTIME_MS` | `19.5` | With `auto`, how long after SteamVR's wake-up a frame still counts as on time. |
| `PRIMEDFRAME_KICK_DRAWS` | `100` | The GPU gets the frame's drawing every this many draws; `0` only at the present. |
| `PRIMEDFRAME_EYE_CLEAR` | `0` | `1` clears the eye images before the blit, as upstream. |
| `PRIMEDFRAME_XR_CLOCK` | `raw` | The clock SteamVR's times are read on, `raw` or `steady`. |
| `PRIMEDFRAME_FDM_LAYER` | `1` | `0` turns off Valve's `fdm_injection` Vulkan layer for an A/B. |
| `PRIMEDFRAME_HUD_TEST` | `0` | `1` shows a white test ring on a composition layer of its own, head-locked, to compare with the HUD. |
| `PRIMEDFRAME_FRAME_LOG` | `1` | `0` turns off the per-frame line in the game's log (about 20 MB an hour). |
| `PRIMEDFRAME_VRTIMING` | `1` | `0`: no compositor timing log. |
| `PRIMEDFRAME_KEEP_SESSIONS` | `30` | Session folders kept. |
| `PRIMEDFRAME_WINDOW_PRESENT` | `0` | `1` presents the desktop window during the VR session, as upstream. |

PrimedGun's own `PRIMEDGUN_*` switches pass through too. The switches other than the defaults are built but not confirmed in the headset; [frame/UNTESTED.md](frame/UNTESTED.md) says which.

## Controls

The controls are PrimedGun's, unchanged. It reads the controllers through OpenXR and SteamVR binds its actions to the Frame controllers automatically; I have not added a binding of my own. The buttons land on the Frame controllers as PrimedGun's layout picture shows: it is `assets/controller layout.png`, drawn for Touch controllers. Click the right stick to set your height; click the left thumbstick for the in-VR menu.

## Settings

Settings are the ini files in `~/.local/share/PrimedFrame/user`, and PrimedGun's in-VR menu (click the left thumbstick):

- `Config/GFX.ini`: graphics, and the `[VR]` section with the PrimedGun options. The values that matter on the Frame: `ImmediateXFBEnable = True` (off, the eye images are a frame older than their poses), `LockHeadPosePerFrame = False` (PrimedGun's default for Vulkan), `DisableCPUCull = True`, `OpcodeReplay = 0`, `UnitsPerMeter = 1.5`, hi-res textures on.
- `GameSettings/GM8E01.ini`: overrides for this game: `InternalResolution = 3`, background shader compilation, and `EnableGPUTextureDecoding = False`. Leave that last one off: with it on, the Frame's GPU reads bad memory and takes SteamVR down.
- `Config/Dolphin.ini`: the Vulkan backend, the DSP thread and the 60 Hz video timing (`VIOverclock = 1.001001`).
- `Config/Logger.ini`: turns on the log file for the OpenXR and video categories, which carries the once a second statistics line.
- `Config/PrimedGun.ini`: the in-VR menu's settings, in a `[primedgun]` section. They are saved when the menu closes, when you press Save in it, and at exit. It ships with my HUD placement for the Frame (distance 1.15, size 1.0, 0.08 to the right and 0.02 up); everything else is PrimedGun's default until you change it in the menu.

## Performance

Numbers from my Frame, from the game's own log and SteamVR's compositor counters, at 120 Hz with 3x internal resolution and 2160 pixel eye images, unless stated. "Headless" means the mock OpenXR runtime on the Frame itself, with SteamVR idle, which gives the GPU and the cadence without a headset session.

| | PrimedFrame |
|---|---|
| Game frames a second, the heaviest room I know (the landing site with the HUD up) | 59.6 on average over a launch |
| Game frames a second, a light room | 59.2 on average over a launch |
| Refreshes per game frame | 2, constant: the compositor reused 93 to 97 % of its refreshes, which is each frame shown twice |
| Frames SteamVR had to reproject | 0 |
| Frames ended with poses other than the ones drawn | 0 |
| GPU per game frame, averaged over a session | about 5 ms, against the 16.7 ms a game frame has |
| GPU per game frame, the heaviest room, headless | 9 ms at 3x and 2160; 6 ms at 2x and 1728; 12 ms at 4x and 2160 |
| The same room, headless, in the driver's tiled mode at 2x and 1728 | 19 ms, 52 frames a second |
| Compositor GPU per refresh | about 1 ms |
| Head pose to display, light room, headless | 41 to 48 ms, depending on where in the period the frame starts |
| Head pose to display, the heaviest room, headless | about 50 ms, one period later than a light room |
| Late frames after the 60 Hz timing, headless | 0.04 a second, from 21.7 |
| First launch | stutters while the shaders compile; later launches start from the cache |

The resolution table (headless, the heaviest room, direct rendering): 2x and 1728 cost 6 ms of GPU per frame, 3x and 2160 cost 9 ms with the GPU at 77 C flat over six minutes, 4x and 2160 cost 12 ms with the GPU at 87 C and rising. 3x and 2160 is the default; 4x is about the panel's own resolution and works headless, but I have not run it in the headset for long.

Where the time went, and what was done about it:

- **The GPU.** The whole frame was one render pass of about 1250 draws, and Turnip's tiled mode ran it once per tile, nine times, because PrimedGun's stereo geometry shader puts both eyes into one pass. Direct rendering (`TU_DEBUG=sysmem`) runs it once: 19 ms became 6 ms at 2x and 1728. The eye clears were pure waste and are gone, and the GPU gets the frame's draws in slices of 100 instead of all at the present, so it is busy while the CPU still emulates. The heavy-room GPU tail that used to stall the next frame is overlapped with it.
- **The pacing.** SteamVR shows an image the moment it is released. The container setup drained the GPU every frame to keep the right eye clean, which serialised everything; a fence does the same without the stall. Then the cadence: a loop that ended a frame on every refresh, repeating the last pair for the in-between refresh, doubled the picture in the headset, because SteamVR took each repeat as a new frame and the repeats carried poses a refresh old. One SteamVR cycle per game frame, with SteamVR told to pace the game at 60, is clean. The last piece was the landing plan: left to itself, SteamVR's hand-out of refreshes depended on how long the game held each frame, and the cadence flipped between on-time and a period late several times a launch, which was felt as lag spikes. Planning each frame's landing refresh from the last one's gives one steady cadence per room.
- **Prediction.** With each frame landing on a known refresh, the head and controller poses are predicted for that refresh. Before, they were predicted for the display time SteamVR first offered, which for a frame landing a period later was a period short, felt as lag, and when the estimate was wrong by a period, as a shaking HUD.
- **60 Hz.** The GameCube's 59.94 Hz drifted a frame against SteamVR's 60 Hz slots every 16 seconds, a late frame each time. The video timing at exactly 60 removed them.
- **Shaders.** The no-window build never created its shader cache folder. With it created, the first launch compiles and later launches load.

What did not help, so you know: Valve's `fdm_injection` Vulkan layer is in the chain either way and was not the difference in any comparison; SteamVR's motion smoothing was never applied to the game in my sessions, allowed or not; PrimedGun's opcode replay (drawing each refresh again from a fresh pose) is not wired in this tree and does nothing here.

Each session writes `sessions/latest/report.txt`, a table of the game's once a second statistics line and SteamVR's compositor counters, and the per-frame log holds every frame's timing. If your Frame is too slow for a heavy room, lower `InternalResolution` in `GameSettings/GM8E01.ini` to 2.

## Files

| What | Where |
|---|---|
| Program, launcher, tools, the seed settings, the library art | `~/Games/PrimedFrame/app` |
| Settings, memory card, save states, the in-VR menu's settings, shader caches | `~/.local/share/PrimedFrame/user` (`Config/`, `GameSettings/`, `GC/`, `StateSaves/`, `Cache/`, `Logs/`) |
| Path of your disc image | `~/.local/share/PrimedFrame/rom` (a text file, one line) |
| Logs | `~/.local/share/PrimedFrame/sessions/latest/` (`launch.log`, `dolphin.log`, `vrtiming.log`, `report.txt`, `stdout.log`) |
| One launch of test settings | `~/.local/share/PrimedFrame/session.env` and `session.env.next`, `NAME=value` lines |

Your memory card is `user/GC/MemoryCardA.USA.raw`. Save normally in the game.

## Troubleshooting

- **`install.sh` says Steam's debugging port is not reachable.** Start Steam and run it again. With `PRIMEDFRAME_SKIP_STEAM=1` it installs without the library entry.
- **The game closes at once.** Read `sessions/latest/launch.log`. A missing disc image gives `REFUSING TO START` and exit status 66. Fix it with `install.sh --rom PATH`.
- **The game starts flat, on the desktop, not in the headset.** The Steam entry lost its VR flag. Run `install.sh` again; it sets the flag on the existing entry.
- **Motion is uneven, or the picture ghosts when you turn your head.** Check `launch.log` for the read-back line: the per-app SteamVR settings should be as asked (120 Hz, one frame to throttle, motion smoothing off). `report.txt` shows what the game and SteamVR's compositor did; `late` and `plan_breaks` in it should be near 0 a second.
- **Doubled images or uneven timing in a room.** Put `PRIMEDFRAME_PACING=drain` in `~/.local/share/PrimedFrame/session.env` for one launch. That is the older loop that drains the GPU every frame; it is slower but a useful comparison. `PRIMEDFRAME_CYCLE_END=asap` is the other comparison.
- **Slow or glitchy on direct rendering.** Put `PRIMEDFRAME_SYSMEM=0` in `session.env` for one launch to leave the driver tiled.
- **Too slow in a heavy room.** `InternalResolution = 2` in `GameSettings/GM8E01.ini`, or `PRIMEDFRAME_EYE=1728` in `session.env` for one launch.
- **The HUD shakes.** `wake_lead_ms` in `report.txt` should sit around 36 ms and `extra_frames` should be low in a light room. If not, send me the report.
- **The game froze.** If it wrote `PrimedGunFrameStall.txt`, the session folder has a copy.
- **The Steam overlay does not appear in the game.** Steam's overlay does not inject into this program. The `LD_PRELOAD` lines in `launch.log` come from that and are harmless.
- **A bad mapping on the Frame controllers.** Tell me which button and what it did; see below.

## Reporting a problem

Run `~/Games/PrimedFrame/app/tools/collect-logs.sh`. It writes an archive of the latest session with addresses, user names and IDs masked. Read it before you attach it to an [issue](https://github.com/simplysanatori/PrimedFrame/issues) of the PrimedFrame repository, together with what you did, what you expected and what happened. Masking is best effort.

## Build from source

See the [technical notes](docs/TECHNICAL_NOTES.md) and [frame/DEV.md](frame/DEV.md). `frame/build.sh` builds on the Frame itself with the tools SteamOS has, in about 16 minutes the first time. `frame/install.sh --dev` installs a build as a separate **PrimedFrame (dev)** entry. `frame/tools/mockrun.sh` runs the game headless under the mock runtime. `frame/tools/privacy-check.sh` checks my own additions, the history after upstream and a release tarball for personal data before publishing.

## Support

PrimedFrame exists because of the people who made PrimedGun and the VR builds of Dolphin it grew from. If you want to support someone, support them first:

- Nobbie made [PrimedGun](https://github.com/Nobbie248/PrimedGun). I could not find a donation page, so star and follow the project, report what you find there, and send improvements upstream.
- iChris4 made Dolphin ReduX, the VR build of Dolphin that PrimedGun is based on, and ported PrimedGun to the Quest. You can [sponsor iChris4 on GitHub](https://github.com/sponsors/iChris4).
- The Dolphin team builds the emulator all of this runs on. Their site, [dolphin-emu.org](https://dolphin-emu.org), is the place to follow and help them.

The credits below name everyone else.

If you would like more Steam Frame ports, you can also [buy me a coffee](https://ko-fi.com/sana748653). That link is only for my Steam Frame work. It pays for the Claude API credits these ports are built with, spent as carefully as I can.

## Credits

- Built by [simplysanatori](https://github.com/simplysanatori).
- [PrimedGun](https://github.com/Nobbie248/PrimedGun) by Nobbie, and everyone PrimedGun thanks: iChris4, who made Dolphin ReduX for PC and Quest and ported PrimedGun to the Quest, the Metroid Prime modding community, the Metroid Prime decompilation project, Metaforce and PrimeHack, and the early testers. Its own README, kept as [README.upstream.md](README.upstream.md), has the full list.
- iChris4's Dolphin ReduX and the old Dolphin Hydra build, whose Metroid Prime behaviour PrimedGun carries forward.
- Carl Kenner and the contributors of Dolphin VR, the first VR build of Dolphin, which started the line of VR work that later builds grew from.
- josethevrtech and [PrimedSteam](https://github.com/josethevrtech/PrimedSteam), who contributed PrimedGun's SteamOS build workflow.
- The Dolphin team, whose emulator all of this is built on.
- Valve, for the Steam Frame, SteamVR and the OpenXR runtime in it.

[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) lists what my additions use and where it came from.

Metroid Prime and its logos are trademarks of Nintendo. This is an unofficial fan project. It is not affiliated with or endorsed by Nintendo, Valve, the Dolphin team or the PrimedGun author.

## Licence

Dolphin, PrimedGun and my changes are free software under the GNU General Public License version 2 or later, as Dolphin is: see `COPYING` and the licence texts in `LICENSES/`. Files that say otherwise in their header keep their own licence. No Nintendo game data is included.

## How this was made

I wanted PrimedGun on my Frame without a container, so I took Nobbie's code and had it built natively for SteamVR. The container setup worked, but big rooms ran at 20 to 30 frames a second and the right eye ghosted. The work after the native build was pacing written for how SteamVR actually behaves, and finding that the Frame's GPU driver was doing the stereo drawing once per tile. It started as a project for my own headset. It ended up good enough to share.

This is vibe coded, with manual review throughout. I wrote it with Claude Code, using Anthropic's Fable 5.1, Opus 5.5 and Sonnet 5.5 models running on the Frame itself. I chose what to build, reviewed the code and the logic of the changes and the scripts as they were written, tested each build in the headset and reported what I saw; Claude read the logs and changed the code. PrimedGun's README says Codex was used to assist its development, so the code underneath is AI assisted too. Neither Nobbie nor the Dolphin team has reviewed this port. If that is not for you, no worries. Read the code before trusting it.

Developer notes and what was learned about SteamVR on the Frame are in [frame/DEV.md](frame/DEV.md), [frame/PACING.md](frame/PACING.md) and the [technical notes](docs/TECHNICAL_NOTES.md).
