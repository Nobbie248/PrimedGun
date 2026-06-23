#!/usr/bin/env bash
# Build the PrimedGun Quest 3 APK (debug, sideloadable).
# Requires: jdk17-openjdk + Android SDK at ~/Android/Sdk (platform-36, build-tools-35,
# ndk;29.0.14206865, cmake;3.22.1). The libadrenotools dependency is populated
# automatically below, so this script can build from a complete-fresh checkout.
set -euo pipefail

export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-17-openjdk}"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$PATH"

REPO="${REPO:-$HOME/games/PrimedGun}"

# libadrenotools is required by the Android arm64 Vulkan build for custom-driver
# loading on Adreno (CMakeLists.txt: add_subdirectory(Externals/libadrenotools)).
# Unlike the other Externals it is not vendored into this repo, and it is not wired
# up as a real git submodule (git submodule status doesn't list it), so neither a
# plain checkout nor `git submodule update` populates it. Clone it (and its nested
# linkernsbypass submodule) on first build, pinned to a known-good commit so a fresh
# checkout is reproducible. The lib/linkernsbypass/CMakeLists.txt sentinel confirms
# both the outer tree and the nested submodule are present before we skip.
ADRENO_DIR="$REPO/Externals/libadrenotools"
# ADRENO_URL="https://github.com/bylaws/libadrenotools.git"
# ADRENO_COMMIT="8fae8ce254dfc1344527e05301e43f37dea2df80"
if [[ ! -f "$ADRENO_DIR/lib/linkernsbypass/CMakeLists.txt" ]]; then
  echo "[*] Populating submodules..."
  git -C "$REPO" submodule update --init --recursive
else
  echo "[*] libadrenotools already populated; skipping."
fi

# Bundle the latest Data/Sys (game INIs incl. GameSettingsVR/GM8.ini). The CMake build only
# copies Data/Sys -> assets on a *reconfigure* (file(COPY) at configure time), so an incremental
# build silently ships a stale GM8.ini and on-device VR overrides won't reflect Data/Sys edits.
# Mirror it here every build so a data-only change always lands in the APK.
echo "[*] Syncing Data/Sys -> app assets (keeps GameSettingsVR/GM8.ini etc. fresh)..."
cp -a "$REPO/Data/Sys/." "$REPO/Source/Android/app/src/main/assets/Sys/"

cd "$REPO/Source/Android"
echo "[*] Building :app:assembleDebug (NDK arm64 cross-compile of Dolphin — this takes a while)..."
./gradlew :app:assembleDebug --no-daemon "$@"

APK="${REPO}/Source/Android/app/build/outputs/apk/debug/app-debug.apk"
if [[ -f "$APK" ]]; then
  echo "[*] APK built: $APK"
  echo "[*] Install to a connected Quest:  adb install -r \"$APK\""
else
  echo "[!] Build finished but APK not found at expected path." >&2
  exit 1
fi
