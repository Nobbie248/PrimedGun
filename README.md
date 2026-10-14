# PrimeFrame

Metroid Prime in VR, running natively on the Valve Steam Frame.

PrimeFrame is a fork of Nobbie248's [PrimedGun](https://github.com/Nobbie248/PrimedGun), the Dolphin based build that plays Metroid Prime in VR. PrimedGun has no Linux Arm64 build, so before this I ran its PC build on the Frame inside a Linux container with the Frame's graphics driver mounted in. PrimeFrame is the same game code built as a plain Linux Arm64 program with no window of its own, tuned to run in the Frame's own SteamVR. You do not need a container, Lepton, an APK or a PC.

You need your own Metroid Prime disc image. No game data is in this repository or its releases.

What it does:

- Plays Metroid Prime with PrimedGun's VR controls, visor and arm cannon, launched from the Steam library into the Frame's SteamVR.
- Renders at 3x internal resolution (1920x1584 per eye) into eye images of 2160 pixels per eye, the size of the Frame's panel.
- Runs the 120 Hz display with SteamVR pacing the game at its own 60 frames a second, two refreshes for each game frame.
- Holds 60 frames a second in heavy rooms.
- Keeps your settings, memory card and save states in one folder under your home.

## What this adds to PrimedGun

PrimedGun runs on Windows and on the Meta Quest. PrimeFrame changes the platform side and the frame pacing, and leaves the game side as it is.

Against the container setup I ran before:

| | Container setup | PrimeFrame |
|---|---|---|
| How it runs | PC build in a Linux container with the Frame's graphics driver mounted in | native Linux Arm64 build, started from the Steam library |
| Internal resolution | 2x (1280x1056 per eye) | 3x (1920x1584 per eye) |
| Eye images | 1728 per eye | 2160 per eye, the panel's native size |
| Display | 90 Hz with SteamVR's defaults | 120 Hz, SteamVR paces the game at 60 |
| Big rooms | 20 to 30 frames a second | 60 frames a second |
| Right eye | ghosted unless the GPU was drained every frame, which cost more frame rate | no doubled images |
| GPU texture decoding | off, after Adreno GPU faults | off, kept |

The heaviest room I know averaged 59.5 frames a second in the headset session logs. The container setup ran it at 20 to 30. Performance is much better than it was.

The major changes:

- **Native Linux Arm64 build.** Dolphin's NoGUI program with PrimedGun's VR code: no Qt, no desktop window, Vulkan on SteamVR's own device, the OpenXR loader built in. Settings are the ini files and PrimedGun's own in-VR menu.
- **Direct rendering on the Frame's GPU.** The launcher sets `TU_DEBUG=sysmem` for the Turnip driver. PrimedGun's stereo geometry shader made every draw run once per tile in the driver's default tiled mode. Rendering straight to memory cut the GPU time per frame in the heaviest room from 19 ms to 6 ms at the old settings (measured headless). That is what paid for 3x and 2160.
- **No GPU drain.** Eye images go to SteamVR only after the GPU has finished them, waited for with a fence, instead of draining the GPU every frame. That keeps the right eye clean without serialising the CPU and the GPU.
- **One SteamVR frame cycle per game frame.** Each game frame is handed to SteamVR in its own cycle, with one planned landing per room. SteamVR shows it twice itself.
- **Prediction for the refresh the frame appears on.** Head and controller poses are predicted for that refresh, not for the one SteamVR first offered.
- **Exactly 60 Hz.** The game's video timing runs at 60 Hz instead of 59.94, 0.1 % faster, so frames stop drifting against SteamVR's slots.
- **Less GPU idle time.** The GPU gets the game's drawing every 100 draws instead of all at the end of the frame, and the eye image is no longer cleared before it is overwritten.
- **A working shader cache.** The no-window build never created its cache folder, so every launch recompiled every shader. Now only the first launch stutters.
- **A launcher for the Frame.** It re-applies SteamVR's per-app settings at every launch (120 Hz, SteamVR pacing the game at 60, no extra prediction, motion smoothing off, 2160 per eye) and keeps session logs.
- **An installer** that unpacks the release, seeds the settings, finds your disc image and adds the Steam shortcut with the VR flag and the library art.
- **Tools** to collect logs for a bug report and to remove the program again.

## Status

Version 1.0.0 is the first release. It has been played on one Steam Frame (SteamOS 0.4.5, build 20261007.6125817) with the US disc.

Seen working in the headset: starting from the Steam library into the title screen, loading and saving the memory card save, PrimedGun's controls under SteamVR's automatic binding, the in-VR menu, 120 Hz with the game at 60, 3x internal resolution with 2160 pixel eye images, heavy rooms at 60 frames a second, no doubled images, a steady helmet HUD. [frame/UNTESTED.md](frame/UNTESTED.md) lists what is built but not tested.

Known:

- The game's world, enemies and the arm cannon move at 60 frames a second. On the 120 Hz display an object your eye follows can look doubled along its movement. Head movement is corrected at 120.
- The helmet HUD has a very light jitter left on fast head turns.
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

1. Download `primeframe-1.0.0-linux-arm64.tar.gz` and `SHA256SUMS` from the [Releases](https://github.com/simplysanatori/PrimeFrame/releases) page, onto the Frame.
2. In a terminal, in the folder you downloaded them to:

   ```sh
   sha256sum -c --ignore-missing SHA256SUMS
   mkdir -p ~/Downloads/primeframe && tar xf primeframe-1.0.0-linux-arm64.tar.gz -C ~/Downloads/primeframe
   ~/Downloads/primeframe/install.sh --rom "/path/to/Metroid Prime (USA).rvz"
   ```

   Leave out `--rom` when the image is in `~/Documents/Roms/gc`.

3. Put the headset on and launch **PrimeFrame** from your Steam library. The first launch stutters while the shaders compile; later launches do not.

The installer copies the program to `~/Games/PrimeFrame/app`, puts default settings in `~/.local/share/PrimeFrame` where files are missing, remembers the path of your disc image, adds **PrimeFrame** to your Steam library as a VR shortcut with its library art (capsule, cover, hero, logo and icon), or updates the entry if it is there, and sets SteamVR's per-app settings for it. It talks to Steam through its local debugging port, which SteamOS has on by default. It changes nothing outside the folders under [Files](#files), the shortcut and those settings. It never writes into a SteamVR file; the launcher sets the per-app settings through SteamVR's own `vrcmd`.

To update, download the new tarball and run `install.sh` again. Your settings, memory card and save states stay where they are; the installer only adds files that are missing. To change the disc image later, run `install.sh --rom PATH` again, or edit the one line in `~/.local/share/PrimeFrame/rom`.

To bring over an existing save, copy your memory card and save states into `~/.local/share/PrimeFrame/user` (`GC/` and `StateSaves/`) from a PC Dolphin or PrimedGun install, before the first launch. PrimedGun advises against carrying save states from one build to another.

## First run

There is no desktop window. The game starts straight into the headset, on the title screen. If it does not start, open `~/.local/share/PrimeFrame/sessions/latest/launch.log`: it says what the launcher did and why it refused to start, for example when the disc image named in `~/.local/share/PrimeFrame/rom` does not exist.

PrimedGun's own notes say: click the right stick to set your height, click the left thumbstick to open or close the in-headset settings menu, stay in the centre of your play space and face forward, and use Exit Game at the top right of the menu (press it twice) to leave the game cleanly, which also writes the shader caches.

Save in the game as usual. The save goes to the memory card in `~/.local/share/PrimeFrame/user/GC`.

## Controls

The controls are PrimedGun's, unchanged. It reads the controllers through OpenXR and SteamVR binds its actions to the Frame controllers automatically; I have not added a binding of my own. The buttons land on the Frame controllers as PrimedGun's layout picture shows: it is `assets/controller layout.png`, drawn for Touch controllers.

## Settings

Settings are the ini files in `~/.local/share/PrimeFrame/user`, and PrimedGun's in-VR menu (click the left thumbstick):

- `Config/GFX.ini`: graphics, and the `[VR]` section with the PrimedGun options.
- `GameSettings/GM8E01.ini`: overrides for this game: `InternalResolution = 3`, background shader compilation, and `EnableGPUTextureDecoding = False`. Leave that last one off: with it on, the Frame's GPU reads bad memory and takes SteamVR down.
- `Config/Dolphin.ini`: backend, sound and the 60 Hz video timing.
- `Config/Logger.ini`: turns on the log file that carries the once a second statistics line.
- `Config/PrimedGun.ini`: the in-VR menu's settings, in a `[primedgun]` section. They are saved when the menu closes, when you press Save in it, and at exit. It ships with my HUD placement for the Frame. To use the settings of a PrimedGun desktop build, paste the `[primedgun]` section of its `Qt.ini` into this file.

## Performance

Everything the pacing does is in the list above; the account of how I got there is in the [technical notes](docs/TECHNICAL_NOTES.md). A few things that decide the result:

- 3x internal resolution and 2160 pixel eye images are the defaults because direct rendering freed the GPU time for them.
- The game runs at 60 frames a second. SteamVR shows each frame twice on the 120 Hz display.
- Each session writes `sessions/latest/report.txt`, a table of the game's once a second statistics line and SteamVR's compositor counters.
- If your Frame is too slow for a heavy room, lower `InternalResolution` in `GameSettings/GM8E01.ini` to 2.

## Files

| What | Where |
|---|---|
| Program | `~/Games/PrimeFrame/app` |
| Settings, memory card, save states | `~/.local/share/PrimeFrame/user` (`Config/`, `GameSettings/`, `GC/`, `StateSaves/`) |
| Path of your disc image | `~/.local/share/PrimeFrame/rom` (a text file, one line) |
| Logs | `~/.local/share/PrimeFrame/sessions/latest/` (`launch.log`, `dolphin.log`, `vrtiming.log`, `report.txt`, `stdout.log`) |
| One launch of test settings | `~/.local/share/PrimeFrame/session.env`, `NAME=value` lines, used for the next launch only |

Your memory card is `user/GC/MemoryCardA.USA.raw`. Save normally in the game.

## Troubleshooting

- **`install.sh` says Steam's debugging port is not reachable.** Start Steam and run it again. With `PRIMEFRAME_SKIP_STEAM=1` it installs without the library entry.
- **The game closes at once.** Read `sessions/latest/launch.log`. A missing disc image gives `REFUSING TO START` and exit status 66. Fix it with `install.sh --rom PATH`.
- **Motion is uneven, or the picture ghosts when you turn your head.** Check `launch.log` for the read-back line: the per-app SteamVR settings should be as asked. The report shows what the game and SteamVR's compositor did.
- **Doubled images or uneven timing in a room.** Put `PRIMEFRAME_PACING=drain` in `~/.local/share/PrimeFrame/session.env` for one launch. That is the older loop that drains the GPU every frame; it is slower but a useful comparison.
- **Slow or glitchy on direct rendering.** Put `PRIMEFRAME_SYSMEM=0` in `session.env` for one launch to leave the driver tiled.
- **Resolution.** `PRIMEFRAME_EYE=1728` in `session.env` asks SteamVR for 1728 pixels per eye for one launch, and `InternalResolution` in `GameSettings/GM8E01.ini` sets the internal resolution.
- **`PRIMEFRAME_THROTTLE`** sets SteamVR's frames to throttle for the game; the default makes SteamVR pace the game at 60.
- **The game froze.** If it wrote `PrimedGunFrameStall.txt`, the session folder has a copy.
- **A bad mapping on the Frame controllers.** Tell me which button and what it did; see below.

## Reporting a problem

Run `~/Games/PrimeFrame/app/tools/collect-logs.sh`. It writes an archive of the latest session with addresses, user names and IDs masked. Read it before you attach it to an [issue](https://github.com/simplysanatori/PrimeFrame/issues) of the PrimeFrame repository, together with what you did, what you expected and what happened. Masking is best effort.

## Build from source

See the [technical notes](docs/TECHNICAL_NOTES.md) and [frame/DEV.md](frame/DEV.md). `frame/build.sh` builds on the Frame itself with the tools SteamOS has. `frame/tools/privacy-check.sh` checks my own additions, the history after upstream and a release tarball for personal data before publishing.

## Support

PrimeFrame exists because of the people who made PrimedGun and the VR builds of Dolphin it grew from. If you want to support someone, support them first:

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

This is vibe coded, with manual review of the scripts as they were written. I wrote it with Claude Code, using Anthropic's Fable 5.1, Opus 5.5 and Sonnet 5.5 models running on the Frame itself. I chose what to build, tested each build in the headset and reported what I saw; Claude read the logs and changed the code. PrimedGun's README says Codex was used to assist its development, so the code underneath is AI assisted too. Neither Nobbie nor the Dolphin team has reviewed this port. If that is not for you, no worries. Read the code before trusting it.

Developer notes and what was learned about SteamVR on the Frame are in [frame/DEV.md](frame/DEV.md), [frame/PACING.md](frame/PACING.md) and the [technical notes](docs/TECHNICAL_NOTES.md).
