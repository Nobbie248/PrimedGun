# PrimeFrame

Metroid Prime in VR, running natively on the Valve Steam Frame.

PrimeFrame is a fork of Nobbie248's [PrimedGun](https://github.com/Nobbie248/PrimedGun), the Dolphin based build that plays Metroid Prime in VR. PrimedGun has no Linux Arm64 build, so on the Frame it needed a container with a PC style graphics stack inside. PrimeFrame builds the same code as a plain Linux Arm64 program with no window of its own, and runs it in the Frame's SteamVR. You do not need a container, Lepton, an APK or a PC.

You need your own Metroid Prime disc image. No game data is in this repository or its releases.

What it is meant to do:

- Play Metroid Prime with PrimedGun's VR controls, visor and arm cannon, in the Frame's SteamVR. [to confirm in the headset]
- Run from the Steam library with one click, at 120 Hz, with SteamVR's per-app settings set for it at every launch. [to confirm in the headset]
- Render each eye at 1728 pixels, SteamVR's own 100 % size on the Frame, and hand it to SteamVR only after the GPU has finished it. [to confirm in the headset]
- Keep your settings, memory card and save states in one folder under your home.

## What this adds to PrimedGun

PrimedGun runs on Windows and on the Meta Quest. PrimeFrame changes the platform side and leaves the game side as it is:

- A Linux Arm64 build of Dolphin's NoGUI program with PrimedGun's VR code: no Qt, no desktop window. Settings are the ini files and PrimedGun's own in-VR menu.
- Vulkan on SteamVR's own device, with the OpenXR loader built in.
- A launch wrapper that sets SteamVR's per-app settings for the Steam shortcut at every launch (120 Hz, no throttling, no extra prediction, motion smoothing off, 1728 resolution) and writes a session folder with the logs and a report.
- Default settings for the Frame: Vulkan, 2x internal resolution, shaders compiled in the background, and GPU texture decoding off. The Frame's GPU reads bad memory with it on and takes SteamVR down.
- An installer that unpacks the program, creates the Steam library entry and the settings folder, and finds your disc image.
- Tools to collect logs for a bug report and to remove the program again.

## Status

This is a first build for the Frame and nothing in it has been confirmed in the headset yet. Every claim that needs the headset says so: smoothness [to confirm in the headset], no ghosting in either eye [to confirm in the headset], performance in heavy rooms [to confirm in the headset], and the controls [to confirm in the headset]. [frame/UNTESTED.md](frame/UNTESTED.md) lists everything that is built but not tested. I will update this page when I have played it.

## Requirements

- A Steam Frame with SteamOS and SteamVR, and a few hundred MB free.
- Your Metroid Prime disc as an RVZ, ISO or GCM image. The US disc, NTSC Revision 0 (game ID GM8E01), is the one PrimedGun supports.
- A terminal on the Frame: SSH, or Konsole from the desktop.
- Steam running on the Frame while you install, so the installer can add the library entry.

## Install

1. Get the repository:

   ```sh
   git clone https://github.com/simplysanatori/PrimeFrame.git
   cd PrimeFrame
   ```

2. Download `primeframe-1.0.0-linux-arm64.tar.gz` and its `.sha256` file from the [Releases](https://github.com/simplysanatori/PrimeFrame/releases) page. Check the download and install it with the path of your disc image:

   ```sh
   sha256sum -c primeframe-1.0.0-linux-arm64.tar.gz.sha256
   frame/install.sh --rom /path/to/MetroidPrime.rvz ~/Downloads/primeframe-1.0.0-linux-arm64.tar.gz
   ```

   The installer copies the program to `~/Games/PrimeFrame/app`, puts default settings in `~/.local/share/PrimeFrame/user` where files are missing, and adds **PrimeFrame** to your Steam library as a non-Steam shortcut, or updates the entry if it is there. It talks to Steam through its local debugging port, which SteamOS has on by default. Without `--rom` it looks for a Metroid Prime image under `~/Documents/Roms/gc`.

3. Put the headset on and launch **PrimeFrame** from your Steam library.

To update, download the new tarball and run `install.sh` again. Your settings, memory card and save states stay where they are; the installer only adds files that are missing.

The installer changes nothing outside the folders under [Files](#files) and the Steam shortcut. It never writes into a SteamVR file; the wrapper sets SteamVR's per-app settings through SteamVR's own `vrcmd`.

## First run

There is no desktop window. The game starts straight into the headset. If it does not, open `~/.local/share/PrimeFrame/sessions/latest/launch.log`: it says what the wrapper did and why it refused to start, for example when the disc image named in `~/.local/share/PrimeFrame/rom` does not exist.

PrimedGun's own notes say: click the right stick to set your height, click the left thumbstick to open or close the in-headset settings menu, stay in the centre of your play space and face forward, and use Exit Game at the top right of the menu (press it twice) to leave the game cleanly, which also writes the shader caches. [to confirm in the headset]

## Controls

PrimedGun sets up its recommended controls itself and reads the controllers through OpenXR. On the Frame, SteamVR remaps them to the Frame's controllers on its own, and I have not changed that. How the buttons land on the Frame controllers [to confirm in the headset]. PrimedGun's layout picture is `assets/controller layout.png`, drawn for Touch controllers. If a mapping is wrong, I will bind the Frame's own controller profile in code, and this page will then show the table.

## Settings

Settings are the ini files in `~/.local/share/PrimeFrame/user`:

- `Config/GFX.ini`: graphics, and the `[VR]` section with the PrimedGun options.
- `GameSettings/GM8E01.ini`: overrides for this game: internal resolution 2x, background shader compilation, and `EnableGPUTextureDecoding = False`. Leave that last one off.
- `Config/Dolphin.ini`: backend and sound.
- `Config/Logger.ini`: turns on the log file that carries the once a second statistics line.

Resolution is the main thing. 1728 pixels per eye is SteamVR's 100 % on the Frame and I treat it as the lowest I will ship. Internal resolution 2x is the setting I start from. How these look and run [to confirm in the headset].

## Performance

[to confirm in the headset] I have no headset numbers yet. Each session writes `sessions/latest/report.txt`, a table of the game's once a second statistics line and SteamVR's compositor counters, so the numbers will come from there.

## Files

| What | Where |
|---|---|
| Program | `~/Games/PrimeFrame/app` |
| Settings, memory card, save states | `~/.local/share/PrimeFrame/user` (`Config/`, `GameSettings/`, `GC/`, `StateSaves/`) |
| Path of your disc image | `~/.local/share/PrimeFrame/rom` (a text file, one line) |
| Logs | `~/.local/share/PrimeFrame/sessions/latest/` (`launch.log`, `dolphin.log`, `vrtiming.log`, `report.txt`, `stdout.log`) |
| One launch of test settings | `~/.local/share/PrimeFrame/session.env`, `NAME=value` lines, used for the next launch only |

Your memory card is `user/GC/MemoryCardA.USA.raw`. Save normally in the game; PrimedGun advises against carrying save states from one build to another.

## Troubleshooting

- **`install.sh` says Steam's debugging port is not reachable.** Start Steam and run it again. With `PRIMEFRAME_SKIP_STEAM=1` it installs without the library entry.
- **The game closes at once.** Read `sessions/latest/launch.log`. A missing disc image gives `REFUSING TO START` and exit status 66. Fix it with `install.sh --rom PATH`.
- **Motion is uneven, or the picture ghosts when you turn your head.** Check `launch.log` for the read-back line: all five per-app SteamVR settings should be as asked. The report shows what the game and SteamVR's compositor did.
- **The game froze.** If it wrote `PrimedGunFrameStall.txt`, the session folder has a copy.
- **A bad mapping on the Frame controllers.** Tell me which button and what it did; see below.

## Reporting a problem

Run `~/Games/PrimeFrame/app/tools/collect-logs.sh`. It writes an archive of the latest session with addresses, user names and IDs masked. Read it before you attach it to an [issue](https://github.com/simplysanatori/PrimeFrame/issues), together with what you did, what you expected and what happened. Masking is best effort.

## Build from source

See the [technical notes](docs/TECHNICAL_NOTES.md) and [frame/DEV.md](frame/DEV.md). The build runs on the Frame itself with the tools SteamOS has. `frame/tools/privacy-check.sh` checks my own additions, the history after upstream and a release tarball for personal data before publishing.

## Support

PrimeFrame exists because of Nobbie's PrimedGun. If you want to support someone, support Nobbie first: star and follow [PrimedGun](https://github.com/Nobbie248/PrimedGun), report what you find there, and send improvements upstream.

If you would like more Steam Frame ports, you can also [buy me a coffee](https://ko-fi.com/sana748653). It pays for the Claude API credits these ports are built with, spent as carefully as I can.

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

I wanted PrimedGun on my Frame without a container, so I took Nobbie's code and had it built natively for SteamVR, with a wrapper and an installer around it. It started as a project for my own headset.

This is vibe coded, with manual review of the scripts as they were written. I wrote it with Claude Code, using Anthropic's Claude models (Fable 5.1, Opus 5.5 and Sonnet 5.5) running on the Frame itself. I chose what to build; Claude read the logs and changed the code. PrimedGun's README says Codex was used to assist its development, so the code underneath is AI assisted too. Neither Nobbie nor the Dolphin team has reviewed this port. Nothing here has been tested in the headset yet, and I say so wherever it matters. If that is not for you, no worries. Read the code before trusting it.

Developer notes are in [frame/DEV.md](frame/DEV.md) and the [technical notes](docs/TECHNICAL_NOTES.md).
