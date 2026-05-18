# PrimedGun

PrimedGun is a Dolphin OpenXR-side VR enhancement app for Metroid Prime.

## Requirements

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
- `assets\gecko\GM8E01_PrimedGun.ini`
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
- In-headset settings menu anchored to the left controller.
- Dolphin-side hook bridge for app-owned game patches, OpenXR tracking, overlays, and diagnostics.
- Automatic Dolphin XR setup for the GM8E01 VR settings PrimedGun expects.
- Pre-configured shader profile.

## Dolphin Setup

PrimedGun configures the Dolphin pieces it needs at startup where possible:

- Sets the GM8E01 Dolphin XR camera forward offset to `0`.
- Sets the GM8E01 Dolphin XR world scale / units-per-meter value used by PrimedGun.
- Can map Dolphin's Port 1 controls and reset-view hotkey to OpenXR when the setting is enabled.

PrimedGun picks the active GM8E01 profile it can find, then writes its Dolphin defaults there:

- `%APPDATA%\Dolphin Emulator\GameSettingsVR\GM8E01.ini`
- `%APPDATA%\Dolphin Emulator\GameSettings\GM8E01.ini`
- Portable Dolphin profiles beside `Dolphin.exe`, such as `User\GameSettingsVR\GM8E01.ini` and `User\GameSettings\GM8E01.ini`

When Dolphin is running, portable profiles near the running `Dolphin.exe` are preferred. Otherwise PrimedGun uses Dolphin's normal roaming profile folder under `%APPDATA%\Dolphin Emulator`. If no existing GM8E01 profile is found, PrimedGun creates the best matching profile for that Dolphin setup.

The VR profile is where PrimedGun saves the GM8E01 VR settings and pre-configured shader overrides. The normal game profile is used to disable unmanaged AR/Gecko enabled-code lists so PrimedGun's app-owned patch set is the one in control.

## App-Owned Patches

The hook loads PrimedGun patches from:

```text
assets\gecko\GM8E01_PrimedGun.ini
```

These patches are owned by PrimedGun and are applied through the hook, not through Dolphin's normal user-managed AR/Gecko enabled-code flow. Current patch areas include culling, VR rotation restore, lock/scan targeting, visor input support, and arm cannon idle fidget disable.

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
- Temporary controller and hotkey setup is written to Dolphin's normal roaming config files under `%APPDATA%\Dolphin Emulator\Config\...`, or to the portable `User\Config\...` folder beside `Dolphin.exe` when portable Dolphin is in use.
- The app reads controller tracking from Dolphin-side OpenXR and writes the cannon transform into Dolphin memory.
- PrimedGun should be running before Metroid Prime is loaded so the Dolphin-side hook is ready as soon as GM8E01 memory appears.
- For the best experience, try not to turn your body around. You can move in the game like this, but functionality is not ideal.

For further enhancements to your VR experience, join the Dolphin VR Discord.

- By Nobbie

Thank you to the Metroid Prime modding community for the resources and research that helped make this possible.
Huge thank you to iChris4 for Dolphin ReduX development.
