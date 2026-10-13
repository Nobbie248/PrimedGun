#!/bin/bash
# Serialised build of one worktree: frame/build.sh <worktree> [ninja targets...]
# One build at a time on the Frame (RAM + 5 CPUs); ccache shared across worktrees.
# Env: BUILD (default build-linux), JOBS (4), PF_CMAKE_ARGS (extra CMake args),
#      PF_CONFIGURE_ONLY=1 (configure without taking the lock, then stop).
# Other heavy jobs (soaks, mock runs) take the same lock: flock ~/Games/PrimeFrame/.build.lock <cmd>
set -e
WT=$(cd "${1:?worktree}" && pwd); shift
BUILD=${BUILD:-build-linux}
PF=$HOME/Games/PrimeFrame
cd "$WT"
export CCACHE_BASEDIR=$PF CCACHE_DIR=${CCACHE_DIR:-$HOME/.cache/ccache-pf}
L=""; command -v ccache >/dev/null && L="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
# USE_SYSTEM_FMT=OFF: the system fmt is 10.2, the tree needs the bundled 11 (fmt/base.h).
# USE_AUDIOTRACK=OFF: SteamOS ships /usr/include/android/log.h, which makes cubeb pick its Android
# backend and link -llog.
# LINUX_LOCAL_DEV=ON: relocatable binary, Sys/ and assets/ are looked up next to it.
configure() {
  cmake -S . -B "$BUILD" -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Release $L \
    -DUSE_SYSTEM_FMT=OFF -DUSE_AUDIOTRACK=OFF -DLINUX_LOCAL_DEV=ON -DENABLE_NOGUI=ON -DENABLE_QT=OFF -DENABLE_VULKAN=ON -DENABLE_VR=ON \
    -DENABLE_TESTS=OFF -DENABLE_ANALYTICS=OFF -DUSE_DISCORD_PRESENCE=OFF \
    -DUSE_RETRO_ACHIEVEMENTS=OFF -DENABLE_LLVM=OFF -DENABLE_AUTOUPDATE=OFF \
    $PF_CMAKE_ARGS
}
if [ -n "$PF_CONFIGURE_ONLY" ]; then configure; exit; fi
# Never build while the owner is in the headset: a game's launch wrapper writes this file ("<name> <pid>")
# while it runs (PrimeFrame's and the sibling projects'); a stale file whose process is gone is ignored.
# A build that is already running is paused (SIGSTOP) while the file names a live process.
M=${XDG_CACHE_HOME:-$HOME/.cache}/headset-in-use
in_use() { [ -f "$M" ] && kill -0 "$(awk '{print $2}' "$M" 2>/dev/null)" 2>/dev/null; }
while in_use; do echo "pfbuild: headset in use, waiting"; sleep 60; done
exec 9>"$PF/.build.lock"
echo "pfbuild: waiting for lock ($WT)"; flock 9; echo "pfbuild: building $WT"
[ -f "$BUILD/build.ninja" ] || configure
setsid nice -n 5 ninja -j${JOBS:-4} -C "$BUILD" "$@" &
NINJA=$!
trap 'kill -CONT -$NINJA 2>/dev/null; kill -TERM -$NINJA 2>/dev/null' TERM INT HUP
PAUSED=0
while kill -0 $NINJA 2>/dev/null; do
  if in_use; then [ $PAUSED = 0 ] && { echo "pfbuild: headset in use, build paused"; kill -STOP -$NINJA; PAUSED=1; }
  elif [ $PAUSED = 1 ]; then echo "pfbuild: build resumed"; kill -CONT -$NINJA; PAUSED=0; fi
  sleep 5
done
wait $NINJA
