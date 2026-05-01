# PrimedGun - By Nobbie

OpenVR-based VR enhancement mod for Metroid Prime running in Dolphin.

## Requirements

- Windows 10/11
- SteamVR
- CMake 3.16+
- A C++ compiler toolchain
- OpenVR SDK at `C:\openvr`
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
- `openvr_api.dll`
- `libgcc_s_seh-1.dll`
- `libstdc++-6.dll`
- `libwinpthread-1.dll`
- `primedgun_settings.ini`
- `PrimedGun Setup Guide.pdf`

## Features

- Full directional movement.
- Offhand controller yaw controls directional strafing.
- Visor hand gesture input: the app reads the control stick as visor D-pad input when the controller is near your head.
- Improved gun-based lock/scan targeting.
- VR arm cannon tracking through OpenVR.

## Setup Guide

See [PrimedGun Setup Guide.pdf](<PrimedGun Setup Guide.pdf>) for screenshot help.

1. Add Dolphin to Steam as a non-Steam game.
2. In SteamVR video settings for Dolphin, set throttling behavior to `Fixed`, lock application framerate to `60 FPS`, and use about `16.67 ms` additional prediction.
3. In Dolphin hotkey settings, assign `Reset VR Position` to a controller button so recentering is easy in-game.
4. In Dolphin controller mapping, right-click the button selection, then `Clear`, then `Add Input`.
5. In Dolphin VR settings, adjust world scale as shown in the setup guide.

## Usage

1. Start SteamVR.
2. Start Dolphin and load Metroid Prime.
3. Launch `PrimedGun.exe`.
4. Confirm both `Dolphin` and `OpenVR` show as connected.
5. Click `INACTIVE` to start writing transforms.
6. While moving your head, use Dolphin's reset-view toggle to line the arm cannon up with your controller position.
   It's best to assign the reset-view toggle to your controller for this.
7. If playing seated or standing, adjust the Y axis in `Offset Tuning` as needed.

## Notes

- Settings are saved to `primedgun_settings.ini`.
- The app reads controller tracking from OpenVR and writes the cannon transform into Dolphin memory.
- The project is OpenVR-only. Old OpenXR experiments have been removed.
- For the best experience, try not to turn your body around. You can move in the game like this, but functionality is not ideal.

## Dolphin AR Codes

Use these with Metroid Prime GCN NTSC Rev 0 (`GM8E01`).

```ini
PrimedGun Disable Culling Outside Camera View
04337A24 38600001
04337A28 4E800020

PrimedGun No Arm Cannon Bob / Idle Sway
040EA15C 38810044
0400E538 60000000
04014820 4E800020
0400E73C 60000000
0400F810 48000244

PrimedGun Restore VR rotation after 80040FE8 Hook
04040FEC 4BFC0854
04001840 807C0740
04001844 3D60817F
04001848 616BE000
0400184C 914B0030
04001850 918B0034
04001854 818B0038
04001858 2C0C0000
0400185C 41820058
04001860 7C1C6040
04001864 40820050
04001868 395C04A8
0400186C 818B0000
04001870 918A0000
04001874 818B0004
04001878 918A0004
0400187C 818B0008
04001880 918A0008
04001884 818B000C
04001888 918A0010
0400188C 818B0010
04001890 918A0014
04001894 818B0014
04001898 918A0018
0400189C 818B0018
040018A0 918A0020
040018A4 818B001C
040018A8 918A0024
040018AC 818B0020
040018B0 918A0028
040018B4 814B0030
040018B8 818B0034
040018BC 4803F734

PrimedGun Gun Ray Lock/Scan Target Hook
0417CC70 4BE84C90
04001900 3D00817F
04001904 6108E400
04001908 81280000
0400190C 7C092040
04001910 40820018
04001914 A1280004
04001918 2809FFFF
0400191C 4182000C
04001920 B1230000
04001924 4E800020
04001928 9421FFF0
0400192C 4817B348

PrimedGun Visor Patch
041133D8 48000040
04113A98 4E800020
042BA4B4 4E800020
042BA7E0 4E800020
0419697C 4BE6AFC4
04001940 806300A4
04001944 2C030000
04001948 4D820020
0400194C 38800000
04001950 38A00001
04001954 482C8D94
04194D34 4BE6CC2C
04001960 80630078
04001964 2C030000
04001968 4D820020
0400196C 38800000
04001970 38A00001
04001974 482C8D74
0411437C C02295D0
04114380 4E800020
041143C4 C02295D0
041143C8 4E800020

```
For further enhancements to your VR experience, join the Dolphin VR Discord.

Thank you to the Metroid Prime modding community for the resources and research that helped make this possible.
