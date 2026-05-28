# PrimedGun

PrimedGun is a Dolphin ReduX-based build focused on improving Metroid Prime's VR experience.

## Build

Use a Visual Studio x64 developer environment, then build the fused PrimeGun/Dolphin executable:

```bat
git clone --recurse-submodules https://github.com/Nobbie248/PrimedGun.git
cd PrimedGun
cmake -S . -B build\Release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build\Release --target PrimedGun.exe
```

The built app is written to `Binary\x64\PrimedGun.exe`.

## Runtime Files

For distribution, use the contents of `Binary\x64`. The important runtime pieces are:

- `PrimedGun.exe`
- `assets/`
- `Sys/`
- `QtPlugins/`
- Qt DLLs copied by the build
- `qt.conf`

`build_info.txt` and the `Languages/` folder are not needed for the PrimeGun release build.

## Features

- Full directional movement.
- Offhand controller yaw controls directional strafing.
- Visor hand gesture input: the app reads the control stick as visor D-pad input when the controller is near your head.
- Improved gun-based lock/scan targeting.
- VR arm cannon tracking through Dolphin-side OpenXR.
- One-click height calibration.
- Position, rotation, scale calibration.
- In-headset settings menu.

## Credits

- Created by Nobbie.
- Thank you to the Metroid Prime modding community for the resources and research that helped make this possible.
- Huge thank you to iChris4 for Dolphin ReduX development.
- Thank you to the early testers: GeekyGami, Lucaspec72, TorchRing, detective_yoshi, PHA3ESH1FTGAMES, retrovideogamer, Samevi, Mochu, VideoGameEsoterica and VRified Games.
- For further enhancements to your VR experience, join the Dolphin VR Discord: https://discord.gg/GdmffzCTrh
