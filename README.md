# PrimedGun

Dolphin OpenXR-side VR enhancement mod for Metroid Prime running in Dolphin.

## Requirements

- Windows 10/11
- CMake 3.16+
- A C++ compiler toolchain
- OpenXR SDK headers at `C:\OpenXR-SDK`
- Dolphin running Metroid Prime GCN NTSC Rev 0 (`GM8E01`)
(built and tested with Dolphin-OpenXR 2512-421 -dirty)

## Build

```bat
git clone --recurse-submodules https://github.com/Nobbie248/PrimedGun.git
cd PrimedGun
mkdir build
cd build
cmake .. -G Ninja
cmake --build . --config Release
```

## Runtime Files

For distribution/share, the useful files are:

- `PrimedGun.exe`
- `PrimedGun_DolphinHook.dll`
- `libgcc_s_seh-1.dll`
- `libstdc++-6.dll`
- `libwinpthread-1.dll`
- `primedgun_settings.ini`

## Features

- Full directional movement.
- Offhand controller yaw controls directional strafing.
- Visor hand gesture input: the app reads the control stick as visor D-pad input when the controller is near your head.
- Improved gun-based lock/scan targeting.
- VR arm cannon tracking through Dolphin-side OpenXR.
- Dolphin-side hook bridge for app-owned game patches and diagnostics.

## Usage

1. Launch `PrimedGun.exe`.
2. Load Metroid Prime in Dolphin.
3. PrimedGun will check then start.
4. While in game use y offset setting in PrimedGun to align cannon height,
click the right thumb stick and move your body to center view with arm cannon correctly.

## Notes

- Settings are saved to `primedgun_settings.ini`.
- PrimedGun currently expects Dolphin's user config at `C:\Users\<user>\Documents\Dolphin Emulator\...` for temporary controller/hotkey setup.
- The app reads controller tracking from Dolphin-side OpenXR and writes the cannon transform into Dolphin memory.
- PrimedGun should be running before Metroid Prime is loaded so the Dolphin-side hook is ready as soon as GM8E01 memory appears.
- For the best experience, try not to turn your body around. You can move in the game like this, but functionality is not ideal.
For further enhancements to your VR experience, join the Dolphin VR Discord.

- By Nobbie

Thank you to the Metroid Prime modding community for the resources and research that helped make this possible.
