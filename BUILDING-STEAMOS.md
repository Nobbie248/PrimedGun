# Building PrimedGun on SteamOS

SteamOS support and the original build workflow were contributed by
[josethevrtech](https://github.com/josethevrtech) through
[PrimedSteam](https://github.com/josethevrtech/PrimedSteam).

## Original verification environment

The contributed workflow was originally verified with:

- SteamOS 3.9, build `20260617.1001`
- Linux `6.18.33-valve2-1-neptune-618-g29c9aecca098`
- AMD Ryzen 9 PRO 8945HS with Radeon 780M Graphics
- AMD Radeon RX 7900 GRE
- 32 GiB RAM

## Requirements

The original workflow was tested on SteamOS 3.9 with an AMD GPU. In Desktop Mode, temporarily make
the system partition writable and install the required build dependencies:

```bash
sudo steamos-readonly disable
sudo pacman -S --needed git base-devel cmake ninja pkgconf clang libglvnd \
  qt6-base qt6-svg qt6-tools qt6-wayland \
  mesa vulkan-headers vulkan-radeon \
  libx11 libxi libxrandr libxext libxrender libxfixes \
  libevdev systemd-libs alsa-lib libpulse bluez-libs \
  openal ffmpeg openxr sdl3 libxcb xcb-proto xorgproto \
  glibc linux-api-headers
sudo steamos-readonly enable
```

SteamOS updates can remove development packages. If a later build reports missing headers or
tools, reinstall the dependencies before changing PrimedGun source files.

`vulkan-radeon` is appropriate for AMD GPUs. Install the Vulkan driver that matches the system if
using different graphics hardware.

If `pacman` reports keyring or signature errors on a fresh installation or after changing SteamOS
update channels, rebuild the package keyring before installing the dependencies:

```bash
sudo install -d -m 755 /etc/pacman.d/gnupg
sudo chown -R root:root /etc/pacman.d/gnupg
sudo pacman-key --init

for keyring in /usr/share/pacman/keyrings/*.gpg; do
  sudo pacman-key --populate "$(basename "$keyring" .gpg)"
done

sudo pacman -Syy
```

## Build

Clone PrimedGun with its submodules, then run the SteamOS build script:

```bash
git clone --recurse-submodules https://github.com/Nobbie248/PrimedGun.git
cd PrimedGun
./scripts/build-steamos.sh
```

The finished application is written to:

```text
build-steamos/Binaries/PrimedGun
```

Launch it with:

```bash
./build-steamos/Binaries/PrimedGun
```

Check that all runtime libraries resolve before launching:

```bash
ldd build-steamos/Binaries/PrimedGun | grep "not found" || echo "runtime libs OK"
```

The script configures a portable Release build with VR and Vulkan enabled. Automatic updates and
analytics are disabled for this SteamOS build. Runtime assets and the `Sys` link are produced by
the existing PrimedGun CMake targets.

To use a different build directory, pass it as the first argument:

```bash
./scripts/build-steamos.sh "$HOME/primedgun-build"
```

Do not patch files inside `Externals/OpenXR`. PrimedGun configures the bundled OpenXR loader from
the root CMake project so the OpenXR submodule remains clean.

## Update

```bash
cd PrimedGun
git pull --ff-only
git submodule update --init --recursive
./scripts/build-steamos.sh
```

If CMake reports stale cached paths after a major toolchain or Qt update, remove only the dedicated
`build-steamos` directory and run the script again.

If configuration reports that `EGL/egl.h` is missing, reinstall `libglvnd`.
