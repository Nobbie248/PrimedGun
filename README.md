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
  -DLINUX_LOCAL_DEV=ON \
  -DUSE_SYSTEM_FMT=OFF
cmake --build build --parallel
ln -sfn ../../Data/Sys build/Binaries/Sys
```

If CMake has already configured the repo before, delete the old `build` folder before rebuilding so cached settings do not carry over. Do not patch files inside `Externals/OpenXR`; the repo configures the bundled OpenXR loader directly.

For SteamOS-specific builds, see [PrimedSteam](https://github.com/josethevrtech/PrimedSteam).

## Features

- Full directional movement.
- Modern VR control scheme.
- Visor head tracking with hand gesture input.
- Improved gun-based targeting and grapple.
- 6DOF arm cannon tracking.
- One-click height calibration.
- Cannon position and rotation calibration.
- Easy cannon texture swapping tool.
- In-headset settings menu.

## Setup Notes

- Setting the HMD refresh rate to 120 Hz is recommended; otherwise, frame pacing issues can occur.
- Meta's own OpenXR environment is not recommended; try SteamVR or Virtual Desktop instead.
- Run the app and select your Metroid Prime NTSC Revision 0 (1.0) game file.
- Check the Layout tab for controller bindings.
- To transfer your old saves, go to `User\GC`, copy your memory card, and place it into the new folder. Then go to Dolphin Settings, open the GameCube tab, and select the save there.
- Do not transfer save states across versions. Make sure to save normally before you transfer.
- Once in game, click the right stick to set your height.
- Click the left thumbstick to open or close the in-headset settings menu.
- Try to stay in the centre of your play space and face forward for the best interaction.
- Use Save Settings after changing PrimedGun options to apply them.

## Change Controller Bindings

![PrimedGun controller layout](assets/controller%20layout.png)

PrimedGun sets up the recommended controls automatically, but you can disable parts of that setup from the Controller tab. Turn off auto controller bindings, visor gesture input, or PrimedGun grip inputs there if you want to bind those controls manually.

By default, PrimedGun maps GameCube `Z` to the map, `Y` to missiles, and the D-Pad to visors.

To change bindings in Dolphin, open Dolphin Settings, go to Controllers, then choose Configure. Select `OpenXR Controller` at the top of the mapping window. Right-click any input you want to change, choose Clear, then assign the new input. When finished, name the profile and save it.

## Credits

- Created by Nobbie.
- Thank you to the Metroid Prime modding community for the resources and research that helped make this possible.
- Huge thank you to iChris4 for Dolphin ReduX development, and to the Dolphin team.
- Thank you to the early testers: GeekyGami, Lucaspec72, TorchRing, detective_yoshi, PHA3ESH1FTGAMES, retrovideogamer, Samevi, Mochu, VideoGameEsoterica and VRified Games.
- For further enhancements to your VR experience, join the Dolphin VR Discord: https://discord.gg/GdmffzCTrh
