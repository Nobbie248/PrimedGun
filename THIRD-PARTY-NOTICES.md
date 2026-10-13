# Third-party notices

PrimeFrame is a fork of [PrimedGun](https://github.com/Nobbie248/PrimedGun) by Nobbie248, which is built on Dolphin ReduX and the Dolphin Emulator. That code, and the third-party code bundled in `Externals/`, keeps its own licences: see `COPYING`, the SPDX headers in the source files, and the texts in `LICENSES/`. This file covers what PrimeFrame adds (`frame/`, `docs/` and the top level notices).

## PrimeFrame's own files

The seed settings in `frame/seed` and the documents in `docs/` and the top level that I wrote for PrimeFrame are licensed GPL-2.0-or-later, as the code they belong to.

## Files taken from my other Steam Frame ports

These are small tools that I wrote for, or adapted between, my own projects. Each keeps the licence its header names. The scripts are separate programs that run beside the game; none is linked into it. The text of the GPLv3 is in `frame/LICENSE-GPLv3.txt` and in the release tarball as `licenses/GPL-3.0.txt`.

| File | Origin | Licence |
|---|---|---|
| `frame/tools/vrtiming.py` | `tools/vrtiming.py` of Moonlight XR Native (simplysanatori), via GalaxyFrame | GPLv3 |
| `frame/tools/privacy-check.sh` | adapted from `tools/privacy-check.sh` of Moonlight XR Native | GPLv3 |
| `frame/tools/session-report.py`, `frame/primeframe.sh`, `frame/install.sh`, `frame/uninstall.sh`, `frame/collect-logs.sh`, `frame/test-installer.sh`, `frame/release.sh` | adapted from the KartFrame equivalents | GPLv3, as the KartFrame originals |
| `frame/steam-cef.py` | copied from KartFrame (one added option) | GPLv3, as KartFrame |
| `frame/tools/xrmock/` | the mock OpenXR runtime of my other ports, extended here | development tool, not in the release tarball |

The release tarball carries only `vrtiming.py` and `session-report.py` from the tools above, plus the install scripts.

## Not redistributed

The Metroid Prime disc and its data, save files and states are never part of the repository or a release. Valve's SteamVR files are used by the program at run time from the user's own install and are not copied.
