# PrimedGun

![PrimedGun gameplay](assets/readme/primedgun-hero.jfif)

PrimedGun is a Dolphin ReduX-based build focused on improving Metroid Prime's VR experience.

## Build - Windows

Open a Visual Studio x64 Native Tools Command Prompt, then run:

```bat
git clone --recurse-submodules https://github.com/Nobbie248/PrimedGun.git
cd PrimedGun
git submodule update --init --recursive
cmake -S . -B build\Release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build\Release --parallel
```

The built app is written to `Binary\x64\PrimedGun.exe`.

## Build - Linux

Requires GCC 12+ or Clang 15+.

```bash
git clone --recurse-submodules https://github.com/Nobbie248/PrimedGun.git
cd PrimedGun
git submodule update --init --recursive
cmake -S . -B build -G Ninja \
  -DLINUX_LOCAL_DEV=ON
cmake --build build --parallel
ln -sfn ../../Data/Sys build/Binaries/Sys
```

## Runtime Files

For Windows distribution, use the contents of `Binary\x64`. The important runtime pieces are:

- `PrimedGun.exe`
- `assets/`
- `Licenses/`
- `Sys/`
- `User/`
- `QtPlugins/`
- `COPYING`
- `qt.conf`
- `Qt6Core.dll`
- `Qt6Gui.dll`
- `Qt6Svg.dll`
- `Qt6Widgets.dll`

## Features

- Full directional movement.
- Modern VR control scheme.
- Visor head tracking with hand gesture input.
- Improved gun-based targeting and grapple.
- 6DOF arm cannon tracking.
- One-click height calibration.
- Cannon position, rotation calibration.
- Easy cannon texture swapping tool.
- In-headset settings menu.

## Setup Notes

- HMD refresh rate set to 120 Hz is recommended.
- Meta's own OpenXR environment is not recommended; try SteamVR or VD instead.
- Select your Metroid Prime GameCube game file.
- Transfer your memory card into `User\GC` if you want existing saves.
- Once in game, click the right stick to set your height.
- Click the left thumbstick to open or close the in-headset settings menu.
- Try to stay in the centre of your play space and face forward for the best interaction.
- Use Save Settings after changing PrimedGun options to apply them.

## Credits

- Created by Nobbie.
- Thank you to the Metroid Prime modding community for the resources and research that helped make this possible.
- Huge thank you to iChris4 for Dolphin ReduX development, and to the Dolphin team.
- Thank you to the early testers: GeekyGami, Lucaspec72, TorchRing, detective_yoshi, PHA3ESH1FTGAMES, retrovideogamer, Samevi, Mochu, VideoGameEsoterica and VRified Games.
- For further enhancements to your VR experience, join the Dolphin VR Discord: https://discord.gg/GdmffzCTrh
