# PrimeFrame developer notes

PrimeFrame is the native Linux arm64 build of PrimedGun (the Dolphin fork with OpenXR VR for
Metroid Prime) for the Steam Frame. It is a NoGUI build (`dolphin-nogui`, no Qt, no desktop
window of its own), run in the Frame's own SteamVR. This file is how to build it, how to run it
headless against the mock OpenXR runtime, and what the tools print.

## Layout

- `frame/build.sh <worktree> [targets]` serialised build (one at a time, shared ccache). It is the
  single source of truth for the CMake flags.
- `frame/tools/mockrun.sh` the only way the game is run during development: mock OpenXR runtime,
  private Xvfb, no sound, scratch user dir.
- `frame/tools/xrmock/` the mock OpenXR runtime (see its README).
- `frame/tools/privacy-check.sh` looks for personal data in the tree, the history or a tarball.
- `frame/tools/vrtiming.py` SteamVR compositor timing, a line per second (for headset sessions).
- `frame/BASELINE.md` the headless numbers (drain on and off) the pacing work is compared with.

## Build

```
frame/build.sh . dolphin-nogui          # binary: build-linux/Binaries/dolphin-nogui
```

It takes `~/Games/PrimeFrame/.build.lock` (waits for another build or mock run), configures on
first use and runs `nice ninja -j4`. Environment: `BUILD` (default `build-linux`), `JOBS`,
`PF_CMAKE_ARGS` (extra CMake arguments), `PF_CONFIGURE_ONLY=1` (configure only, no lock).

CMake flags (host: SteamOS arm64, clang 19, cmake 3.29, ninja, ccache):

```
-G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
-DUSE_SYSTEM_FMT=OFF -DUSE_AUDIOTRACK=OFF -DLINUX_LOCAL_DEV=ON
-DENABLE_NOGUI=ON -DENABLE_QT=OFF -DENABLE_VULKAN=ON -DENABLE_VR=ON
-DENABLE_TESTS=OFF -DENABLE_ANALYTICS=OFF -DUSE_DISCORD_PRESENCE=OFF
-DUSE_RETRO_ACHIEVEMENTS=OFF -DENABLE_LLVM=OFF -DENABLE_AUTOUPDATE=OFF
```

What the host needed (everything else configured and compiled as it came):

- `USE_SYSTEM_FMT=OFF`: the image has fmt 10.2, the tree includes `fmt/base.h` (fmt 11). Use the
  bundled one. (If a build directory was configured before this flag, also clear the cached
  `FMT_*` and `pkgcfg_lib_FMT*` entries once: `PF_CMAKE_ARGS="-UFMT_* -Upkgcfg_lib_FMT*"`.)
- `USE_AUDIOTRACK=OFF`: the image ships `/usr/include/android/log.h`, so the bundled cubeb
  configures its Android backend and the link fails with `cannot find -llog`.
- `LINUX_LOCAL_DEV=ON`: relocatable layout, `Sys/` and `assets/` are looked up next to the binary
  (the build links both into `build-linux/Binaries/`; an install copies them). In this mode the
  stock code ignored `-u` and forced a `user/` folder next to the binary; `UICommon.cpp` now lets
  `-u <dir>` win, so the user dir can live in `PRIMEFRAME_HOME`.
- The CMake target keeps the name `dolphin-nogui` and now also produces a binary of that name
  (upstream's output name is `dolphin-emu-nogui`).

Numbers from the first build (5 CPUs, `-j4`, cold ccache): 1165 steps, about 7 minutes to the
first failure (fmt) and about 9 more after the fixes, so roughly 16 minutes cold; a relink after
a source change is about 1 minute; with a warm ccache a full rebuild is about 1 minute. Binary:
21.8 MB (Release, not stripped, no LTO).

### Shared libraries

`readelf -d` lists 30 NEEDED libraries, every one from a stock SteamOS package (checked with
`pacman -Qo`): libXrandr, libX11, libXi (libxrandr, libx11, libxi), alsa-lib, libpulse, libevdev,
lzo, lz4, systemd-libs (libudev), speexdsp, bluez-libs, hidapi, bzip2, xz (liblzma), zstd,
xxhash, curl, libglvnd (libEGL, libOpenGL, libGLX), glu, libusb, zlib, openssl (libssl, libcrypto),
gcc-libs (libstdc++, libgcc_s), glibc (libc, libm, ld.so). None is outside the image. Linked in
statically (bundled): SDL3, cubeb, fmt, mbedtls, enet, SFML network, spng, glslang, imgui, the
OpenXR loader (so no `libopenxr_loader` is needed; the runtime comes from SteamVR's
`active_runtime.json` or `XR_RUNTIME_JSON`). Vulkan is not linked: the backend loads
`libvulkan.so.1` with dlopen. Run `ldd` with `LD_LIBRARY_PATH` unset: with Steam's runtime path set,
a few audio libraries (libogg, libvorbis) resolve from `steamrtarm64`, which is not what the
installed binary should use.

## Running (headless, under the mock)

```
frame/tools/mockrun.sh --state 01 --seconds 120            # StateSaves/GM8E01.s01, 120 s window
frame/tools/mockrun.sh --state 02 --seconds 120 --drain    # with PRIMEDGUN_XR_WAIT_GPU=1
frame/tools/mockrun.sh --state 01 --seconds 60 --validate  # Vulkan validation layer
```

The script header lists all options. What it does: takes the lock, refuses to run next to a game
(the pgrep check of the lane rules, ignoring podman's API service and command lines of shells),
builds the mock into `~/.cache/pf-xrmock/<checkout hash>` when its sources changed, seeds
`~/.cache/pf-mockrun/user` by copying `Config/`, `GameSettings/`, `GC/`, `StateSaves/` from the
existing user dir (read only; the analytics ID line is removed), writes `Config/Logger.ini` (file
log, OpenXR, Video and Core categories, INFO level), starts a private Xvfb, runs

```
dolphin-nogui -u <scratch user> -p x11 -v Vulkan -C 'Dolphin.DSP.Backend=No Audio Output' \
              -e <Metroid Prime RVZ> -s <state file>
```

with `XR_RUNTIME_JSON` pointing at the mock (hard error if empty or missing), `MOCK_XR_HZ=120`,
eye size 1728x1728, `MOCK_XR_BUSY_MS=4` (the exact "image released before the GPU finished"
check), and stops the game (SIGTERM, a second one, SIGKILL after 15 s) shortly after the mock
ends the session. Each run's logs are kept in `~/.cache/pf-mockrun/runs/<time>-<tag>/`
(`dolphin.log`, `stdout.log` with the mock's summary, `mock.trace`, `result.txt`,
`PrimedGunFrameStall.txt` if the watchdog wrote one). It prints the mock summary lines, the
session states the game saw, the statistics, the stall file, fatal lines, and exits non-zero on a
fatal line, no FOCUSED, mock errors or no submitted frames.

Things learned while getting it to run:

- Platform `headless` never presents VR frames: `Presenter::Present` returns early when the
  Vulkan backend has no swapchain. The game needs a window, so the platform is `x11`, in a private
  Xvfb.
- On a plain Xvfb Turnip cannot present ("Failed to find an acceptable present queue", no DRI3).
  `MESA_VK_WSI_DEBUG=sw` makes Mesa present with ordinary X requests; the script sets it.
- The desktop session enables Valve's gamescope WSI layer, which needs Xwayland and breaks the
  swapchain on Xvfb: the script unsets `ENABLE_GAMESCOPE_WSI` and sets `DISABLE_GAMESCOPE_WSI=1`.
- Valve's implicit OpenXR layer `XR_APILAYER_VALVE_fdm_injection` loads in front of the mock
  (it prints "fdm_injection: added swapchain size 1728"); that is expected.
- After the mock ends the session the game goes on rendering and every acquire fails; the log is
  full of `xrAcquireSwapchainImage failed` errors from then on. mockrun stops the game two seconds
  after `endSession`, but the error-level line count it prints still includes that tail (a few
  hundred lines); judge a run by the mock's counters and the statistics, not by that count.
- The process takes up to 15 seconds to go away after SIGTERM (a video thread keeps running while
  the others are already in `do_exit`); mockrun ends it with SIGKILL after that.
- ptrace is not available in the development sandbox (no gdb, no eu-stack); `--stacks N` writes a
  `ps -L` thread list (CPU, state, wait channel) after N seconds for hang hunting.

## The mock

`frame/tools/xrmock` is the mock OpenXR runtime of the sibling projects, with these additions for
PrimedGun:

- `XR_KHR_vulkan_enable` (v1): `xrGetVulkanGraphicsRequirementsKHR` (instances of Vulkan 1.0 to 1.2,
  as SteamVR), `xrGetVulkanInstanceExtensionsKHR`, `xrGetVulkanDeviceExtensionsKHR`,
  `xrGetVulkanGraphicsDeviceKHR`. With v1 the app creates the VkInstance and VkDevice itself and
  passes them in the session's graphics binding; the mock takes `vkGetInstanceProcAddr` from the
  loader already in the process (dlsym) and makes its swapchain images on the app's device. The two
  extension lists are fixed strings (override with `MOCK_XR_VK_INSTANCE_EXTS` and
  `MOCK_XR_VK_DEVICE_EXTS`); the defaults are the usual SteamVR set (external memory, external
  memory fd, dedicated allocation, get memory requirements 2, get physical device properties 2),
  not read from a Frame.
- Swapchain images get `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` when the app asks for
  `XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT` (it views the sRGB image as its UNORM sibling), and
  images without colour-attachment usage (the sampled quad swapchains of the in-VR overlay) start in
  a shader-read layout instead of an attachment layout.
- `xrCreateInstance` refuses an extension the mock does not offer
  (`XR_ERROR_EXTENSION_NOT_PRESENT`, with a line on stderr).
- Kept: one waited image per swapchain, single mip, the released-unfinished check
  (`MOCK_XR_BUSY_MS`), both eyes of a layer from one `xrLocateViews`.

What PrimedGun asks of OpenXR (read from `VulkanOpenXR.cpp` and `VideoCommon/VR/`): required
`XR_KHR_vulkan_enable`; optional `XR_FB_display_refresh_rate` (the mock offers it),
`XR_KHR_vulkan_swapchain_format_list` (not offered, as on the Frame's SteamVR as far as known: the
game then simply omits the format list; its mutable-format usage flag stays),
`XR_META_touch_controller_plus` (not offered), `XR_MND_headless` for a utility session (not offered;
skipped when missing). Android-only extensions are not compiled in. Swapchains: two eye swapchains
(format sRGB RGBA8, usage colour attachment, transfer dst, mutable format, mip 1, array 1, the
recommended size), plus small sampled/transfer-dst RGBA8 swapchains for the overlay and laser
quads, composition layers projection and quad. Input: action set with boolean/float actions,
action spaces, suggested bindings for the simple, touch, touch plus, index, motion and vive
profiles (no Frame profile yet), haptics.

## Statistics line

`VulkanOpenXR.cpp` logs one line a second at INFO level (category OpenXR):

```
XRSTATS mode= submitted= layers= shown= late= gpu_waits= gpu_wait_max_ms= drain_avg_ms= drain_max_ms= gpu_ms= xrwait_avg_ms= xrwait_max_ms= set_waits= discards= end_avg_ms= end_max_ms= end_late= age_avg_ms= age_max_ms= lead_avg_ms= pose_off= eye= game_fps= game_vps=
```

frame/PACING.md explains every field and the per-frame `XRF` line. `gpu_ms` is GPU busy time per eye
set from the kernel's DRM fdinfo. `frame/tools/session-report.py` reads the fields generically.

## Install, wrapper and release

- `frame/primeframe.sh` is the launch wrapper; its header lists every `PRIMEFRAME_*` switch. The
  Steam shortcut runs it as `"<app>/primeframe.sh" %command%`. Per launch it sets the five SteamVR
  per-app keys with `vrcmd` for `$SteamAppId` and reads them back, makes
  `~/.local/share/PrimeFrame/sessions/<date-time>/` (`launch.log`, `stdout.log`, `dolphin.log`,
  stall file, `vrtiming.log`, `report.txt`), and runs
  `primeframe -u <home>/user -p x11 -v Vulkan -e <rom> "$@"`. `session.env` (VAR=value lines) is
  sourced for one launch and renamed `session.env.used`: this is how a test setting such as
  `PRIMEDGUN_XR_WAIT_GPU=1` or `PRIMEFRAME_PACING=...` reaches the game from the Steam library.
  `TU_DEBUG=sysmem` is set by default (a third of the GPU time in the heavy room, frame/perf/RESULTS.md); `PRIMEFRAME_SYSMEM=0` leaves Turnip tiled. `PRIMEFRAME_EYE` (default 2160) is the eye image size asked of SteamVR.
- `frame/install.sh --dev build-linux` copies `Binaries/dolphin-nogui` as `primeframe`, plus
  `Sys/` and `assets/` (links followed) into `~/Games/PrimeFrame/app-dev` and adds the shortcut
  "PrimeFrame (dev)". Without `--dev` it takes a release tarball or an unpacked release.
  `PRIMEFRAME_STEAM_DIR=<copy of a Steam folder>` edits the copy's `shortcuts.vdf` instead of the live
  Steam; `PRIMEFRAME_SKIP_STEAM=1` skips Steam. The user folder is seeded from `frame/seed/user`
  only where files are missing.
- `frame/test-installer.sh` runs all of it in a scratch HOME (`~/.cache/pf-laneD/test-installer`)
  with a fake `vrcmd`, fake game and fake build tree; it must print "all passed".
- `frame/release.sh` makes `primeframe-1.0.0-linux-arm64.tar.gz` (flat layout; `install.sh`,
  `primeframe.sh`, `tools/`, `seed/` sit beside the binary, `Sys/` and `assets/`) and runs the
  privacy check on it. The tarball check skips upstream's `Sys/` by path.
- `frame/tools/privacy-check.sh`: default scope is our own additions (changed against `main`,
  which is upstream v1.1.7, plus `frame/` and our three docs); `--history` covers the commits after
  `main`; a DIR or tarball argument covers everything but `Sys/`. Personal values go in a local
  pattern list outside the repository (`PF_PRIVACY_PATTERNS`).

### Importing from the old Podman setup (owner only, not in the README)

```
frame/install.sh --dev build-linux --import-podman      # source: ~/PrimedGun-podman/src/build-podman/Binaries/user
```

Copies (never moves, never writes into the source) `GC/MemoryCardA.USA.raw`, `GC/SRAM.raw`,
`StateSaves/`, and only the `[VR]` section of `Config/GFX.ini`, `[Video_Settings]` of
`GameSettings/GM8E01.ini` and `[VR]` of `Config/Dolphin.ini` into
`~/.local/share/PrimeFrame/user`. An existing destination file is kept (with a message) unless
`--force`, which first copies it to `~/.local/share/PrimeFrame/backup-<date-time>/`. An ini file you have
edited since the seed is also kept unless `--force`. The Analytics ID and the keyboard controller
mapping (`GCPadNew.ini`) are not carried over: PrimedGun applies its OpenXR bindings itself.

## Pacing

frame/PACING.md has the whole account (the loops, the mock's cadence model, the tables, the headset
sessions). In short:

- `PRIMEFRAME_PACING=cycle` (default) | `pipelined` | `drain` | `inline`; `PRIMEDGUN_XR_WAIT_GPU=1` alone
  still means `drain`. Cycle: each game frame in its own `xrWaitFrame` cycle (the drain's order as SteamVR
  sees it), begun at the frame's first draw that reads the head pose, released after its fence and ended
  by a releaser thread, no CPU drain. Pipelined: two eye swapchain sets; a pacing thread ends each set
  right after the `xrWaitFrame` of the slot its poses were located for (blocking for it when it is the
  next slot, i.e. with SteamVR throttling the app to 60), sets are freed for the game as soon as their
  fence signalled. Keep `ImmediateXFBEnable = True`: with it off the mock's stamp check shows images drawn
  a game frame before their pose.
- An `XRF` line per ended frame and the per-second XRSTATS fields `end_*`, `age_*`, `lead_avg_ms`,
  `pose_off`, `discards` give the timing against SteamVR's wake-up (PACING.md "Frame log").
- While the OpenXR session runs the desktop window is neither drawn nor presented
  (`PRIMEFRAME_WINDOW_PRESENT=1` keeps it). Under the mock the software WSI waited for the GPU inside that
  present, which hid every early release and cost the heavy room about 40 % of its frame rate.
- `PRIMEFRAME_NO_GPU_WAIT=1` releases with no wait at all: the negative test, which the mock's release
  check must fail (`mockrun.sh --nowait`).
- mockrun: `--pacing M`, `--nowait`, `--window`, `--stamp`, `--head DPS`; it sets `MOCK_XR_SKIP=late` (a
  late xrWaitFrame returns at once, as SteamVR does), passes `MOCK_XR_THROTTLE` (SteamVR's
  framesToThrottle) through, waits while `~/.cache/headset-in-use` names a live process and marks a run
  INVALID (exit 3) when a headset session starts during it. The mock adds the `xrmock: cadence` and
  `xrmock: latency` summary lines.
