# PrimedGun

PrimedGun is a Dolphin OpenXR-side VR enhancement app for Metroid Prime.

## Requirements To Build

- Windows 10/11
- CMake 3.16+
- A C++ compiler toolchain
- OpenXR SDK headers at `C:\OpenXR-SDK`
- Dolphin running Metroid Prime GCN NTSC Rev 0 (`GM8E01`)
- Built and tested with Dolphin-OpenXR 2512-421 -dirty

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
- `assets/`
- `PrimeGun configs/`
- `D3DCompiler_47.dll`
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
- One-click height calibration.
- Position, rotation, scale, and render-only model offset calibration.
- In-headset settings menu.
- Dolphin-side hook bridge for app-owned game patches, OpenXR tracking, and overlays.
- Automatic Dolphin OpenXR controller binding setup.

## Dolphin Setup

PrimedGun only applies Dolphin's Port 1 controller bindings automatically on startup.

- Maps Dolphin's Port 1 controls to OpenXR.

Set Dolphin VR/OpenXR, world scale, camera forward, and other emulator settings manually in Dolphin.
If PrimedGun cannot find Dolphin's active controller config location, it shows a warning in the app.

## App-Owned Patches

Patches are applied automatically through the PrimedGun app.
Each app-owned AR/Gecko code can be enabled or disabled from the `AR Codes` tab.

PrimedGun disables unmanaged AR/Gecko enabled-code lists so PrimedGun's app-owned patch set is the one in control.

## Usage

1. Launch `PrimedGun.exe`.
2. Load Metroid Prime GCN NTSC Rev 0 (`GM8E01`) in Dolphin.
3. PrimedGun detects the game when GM8E01 is loaded into memory.
4. Click the right stick to set height.

## VR Settings Menu

- Click the left thumbstick to open or close the in-headset settings menu.
- The menu is attached to the left controller.
- Use A to change the pointed-at setting.
- Use `Save Settings` to write `primedgun_settings.ini`.

## Notes

- Settings are saved to `primedgun_settings.ini`.
- Dolphin controller bindings are applied automatically on startup.
- The app reads controller tracking from Dolphin-side OpenXR and writes the cannon transform into Dolphin memory.
- PrimedGun should be running before Metroid Prime is loaded so the Dolphin-side hook is ready as soon as GM8E01 memory appears.
- For the best experience, try not to turn your body around. You can move in the game like this, but functionality is not ideal.

## Credits

- Created by Nobbie.
- Thank you to the Metroid Prime modding community for the resources and research that helped make this possible.
- Huge thank you to iChris4 for Dolphin ReduX development.
- Thank you to the early testers: GeekyGami, Lucaspec72, TorchRing, detective_yoshi, PHA3ESH1FTGAMES, retrovideogamer, Samevi, and VRified Games.
- For further enhancements to your VR experience, join the Dolphin VR Discord.
