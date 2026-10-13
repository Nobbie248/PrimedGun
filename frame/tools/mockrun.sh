#!/bin/bash
# Runs the game against the mock OpenXR runtime (xrmock), headless: no window, no sound.
#
#   frame/tools/mockrun.sh [--state N] [--seconds N] [--warmup N] [--drain] [--validate]
#                          [--exe PATH] [--platform headless|x11] [--tag NAME]
#
#   --state N     load StateSaves/GM8E01.sNN (01 or 02) at boot (default: none, the game's title)
#   --seconds N   length of the measured window: the first N one-second XRSTATS lines, counted
#                 from the first submitted frame (default 60)
#   --warmup N    extra seconds of session before the window can start (boot, state load);
#                 the mock ends the session after N + warmup seconds (default 30)
#   --drain       PRIMEDGUN_XR_WAIT_GPU=1 (the GPU drain before the eye images are released)
#   --validate    VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation (needs the layer installed for the user)
#   --exe PATH    the dolphin-nogui binary (default <repo>/build-linux/Binaries/dolphin-nogui)
#   --platform P  dolphin-nogui window platform (default headless; x11 draws into the private Xvfb)
#   --tag NAME    suffix of the run directory
#
# Environment: MOCK_XR_* settings of xrmock (MOCK_XR_HZ default 120, MOCK_XR_EYE default 1728x1728,
# MOCK_XR_BUSY_MS default 4: the exact released-unfinished check), PF_MOCKRUN_HOME (scratch,
# default ~/.cache/pf-mockrun), PF_BUILD (build dir), PF_XRMOCK_DIR (where the mock is built;
# default one per checkout under ~/.cache/pf-xrmock), PF_ROM (the Metroid Prime image).
# The scratch user dir (PF_MOCKRUN_HOME/user) is seeded by copying Config/, GameSettings/, GC/
# and StateSaves/ from the owner's PrimedGun user dir (read only); Config and GameSettings are
# reseeded every run, the large GC and StateSaves once. Each run's logs go to
# PF_MOCKRUN_HOME/runs/<time>-<tag>/.
#
# The run holds ~/Games/PrimeFrame/.build.lock (waits for a build or another run).
# Safety: XR_RUNTIME_JSON must name an existing, non-empty mock runtime, else the script aborts
# (an empty one once fell through to the real SteamVR in the headset); it aborts when a game
# is running; the game gets SIGTERM, then SIGKILL, when the run is over.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
LOCK=$HOME/Games/PrimeFrame/.build.lock
if [ -z "${PF_MOCKRUN_LOCKED:-}" ]; then
  mkdir -p "$(dirname "$LOCK")"
  PF_MOCKRUN_LOCKED=1 exec flock "$LOCK" "$0" "$@"
fi
STATE=""; SECS=60; WARMUP=30; DRAIN=0; VALIDATE=0; PLATFORM=headless; TAG=""
EXE=${PF_BUILD:-$REPO/build-linux}/Binaries/dolphin-nogui
while [ $# -gt 0 ]; do
  case $1 in
    --state) STATE=$2; shift ;;
    --seconds) SECS=$2; shift ;;
    --warmup) WARMUP=$2; shift ;;
    --drain) DRAIN=1 ;;
    --validate) VALIDATE=1 ;;
    --exe) EXE=$2; shift ;;
    --platform) PLATFORM=$2; shift ;;
    --tag) TAG=$2; shift ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
  shift
done
die() { echo "mockrun: $*" >&2; exit 1; }
[ -x "$EXE" ] || die "no game binary at $EXE"
ROM=${PF_ROM:-$HOME/Documents/Roms/gc/Metroid Prime (USA).rvz}
[ -f "$ROM" ] || die "no game image at $ROM"
SRC_USER=$HOME/PrimedGun-podman/src/build-podman/Binaries/user
[ -d "$SRC_USER/Config" ] || die "no source user dir at $SRC_USER"

# Never next to a running game or the owner's session. Only real game-like processes count:
# the API service of podman, shells and git (a command line that merely mentions a name) do not.
RUNNING='SteamLaunch|dolphin|primedgun|primeframe|lepton|waydroid|podman'
others() {
  pgrep -af "$RUNNING" | grep -Ev 'pgrep|mockrun|podman .*system service|^[0-9]+ (/bin/|/usr/bin/)?(bash|sh|git|ssh|flock)( |$)|^[0-9]+ /usr/lib/git-core/' || true
}
if [ -n "$(others)" ]; then
  others >&2
  die "a game is running; wait for it"
fi

OUT=${PF_MOCKRUN_HOME:-$HOME/.cache/pf-mockrun}
RUN=$OUT/runs/$(date +%Y%m%d-%H%M%S)${TAG:+-$TAG}
USERDIR=$OUT/user
mkdir -p "$RUN" "$USERDIR"
# Seed the scratch user dir.
rm -rf "$USERDIR/Config" "$USERDIR/GameSettings" "$USERDIR/Logs"
cp -r "$SRC_USER/Config" "$SRC_USER/GameSettings" "$USERDIR/"
[ -d "$USERDIR/GC" ] || cp -r "$SRC_USER/GC" "$USERDIR/"
[ -d "$USERDIR/StateSaves" ] || cp -r "$SRC_USER/StateSaves" "$USERDIR/"
sed -i '/^ID *=/d' "$USERDIR/Config/Dolphin.ini"
# Dolphin's own log file: the OpenXR and Video categories at INFO level (the statistics line, the
# OpenXR setup), Core for the state load.
mkdir -p "$USERDIR/Logs"
cat > "$USERDIR/Config/Logger.ini" <<'INI'
[Logs]
OpenXR = True
Video = True
CORE = True
[Options]
WriteToFile = True
WriteToConsole = False
WriteToWindow = False
Verbosity = 4
INI
STATE_ARGS=()
if [ -n "$STATE" ]; then
  SF=$USERDIR/StateSaves/GM8E01.s$STATE
  [ -s "$SF" ] || die "no state file $SF"
  STATE_ARGS=(-s "$SF")
fi

# Headless: a private virtual X server (for libraries that want a display), nothing audible.
unset WAYLAND_DISPLAY
XVFB_DISPLAY_FILE=$RUN/xvfb.display; : > "$XVFB_DISPLAY_FILE"
Xvfb -displayfd 3 -screen 0 1280x720x24 -nolisten tcp 3>"$XVFB_DISPLAY_FILE" >"$RUN/xvfb.log" 2>&1 &
XVFB_PID=$!
trap 'kill $XVFB_PID 2>/dev/null' EXIT
for _ in $(seq 50); do [ -s "$XVFB_DISPLAY_FILE" ] && break; sleep 0.2; done
[ -s "$XVFB_DISPLAY_FILE" ] || die "Xvfb did not start (see $RUN/xvfb.log)"
export DISPLAY=:$(head -1 "$XVFB_DISPLAY_FILE")
export SDL_AUDIODRIVER=dummy SDL_AUDIO_DRIVER=dummy
# The loader looks for the validation layer under XDG_DATA_HOME.
if [ $VALIDATE = 1 ]; then
  export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
  export VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
fi

MOCK_DIR=${PF_XRMOCK_DIR:-$HOME/.cache/pf-xrmock/$(printf %s "$REPO" | md5sum | cut -c1-8)}
if [ ! -s "$MOCK_DIR/libxrmock.so" ] || [ -n "$(find "$HERE/xrmock" -newer "$MOCK_DIR/libxrmock.so" -name '*.[ch]' 2>/dev/null)" ]; then
  bash "$HERE/xrmock/build.sh" "$MOCK_DIR" >/dev/null || die "the mock runtime did not build"
fi
export XR_RUNTIME_JSON=$MOCK_DIR/runtime.json
[ -n "${XR_RUNTIME_JSON}" ] && [ -s "$XR_RUNTIME_JSON" ] || die "XR_RUNTIME_JSON is empty or missing: $XR_RUNTIME_JSON"
LIBPATH=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["runtime"]["library_path"])' "$XR_RUNTIME_JSON")
[ -s "$LIBPATH" ] || die "mock runtime library missing or empty: $LIBPATH"
TOTAL=$((SECS + WARMUP))
export MOCK_XR_SECONDS=${MOCK_XR_SECONDS:-$TOTAL}
export MOCK_XR_HZ=${MOCK_XR_HZ:-120}
export MOCK_XR_BUSY_MS=${MOCK_XR_BUSY_MS:-4}
export MOCK_XR_TRACE=${MOCK_XR_TRACE:-$RUN/mock.trace}
[ $DRAIN = 1 ] && export PRIMEDGUN_XR_WAIT_GPU=1 || unset PRIMEDGUN_XR_WAIT_GPU

CMD=("$EXE" -u "$USERDIR" -p "$PLATFORM" -v Vulkan -C 'Dolphin.DSP.Backend=No Audio Output' -e "$ROM" "${STATE_ARGS[@]}")
{
  echo "exe: $EXE"; echo "state: ${STATE:-none}  window: ${SECS}s  warmup: ${WARMUP}s  drain: $DRAIN  validate: $VALIDATE  platform: $PLATFORM"
  echo "mock: $XR_RUNTIME_JSON  hz: $MOCK_XR_HZ  eye: ${MOCK_XR_EYE:-1728x1728}  busy_ms: $MOCK_XR_BUSY_MS  session seconds: $MOCK_XR_SECONDS"
} | tee "$RUN/info.txt"
echo "mockrun: run dir $RUN"
set +e
LOG=$RUN/stdout.log
"${CMD[@]}" > "$LOG" 2>&1 &
PID=$!
# The mock ends the XR session after MOCK_XR_SECONDS; the game is stopped shortly after.
( sleep $((MOCK_XR_SECONDS + 40)); kill -TERM $PID 2>/dev/null; sleep 15; kill -KILL $PID 2>/dev/null ) &
WATCH=$!
wait $PID; RC=$?
kill $WATCH 2>/dev/null
echo "mockrun: game stopped (exit $RC)"
# Keep the game's own logs with the run.
cp "$USERDIR/Logs/dolphin.log" "$RUN/dolphin.log" 2>/dev/null
cp "$USERDIR/Logs/PrimedGunFrameStall.txt" "$RUN/" 2>/dev/null
DL=$RUN/dolphin.log; [ -f "$DL" ] || DL=/dev/null

{
echo "== mock summary"
grep -a 'xrmock:' "$LOG" || true
echo "== session states (mock events the game polled)"
NAMES=(0 IDLE READY SYNCHRONIZED VISIBLE FOCUSED STOPPING LOSS_PENDING EXITING)
for s in $(grep -a 'event sessionState=' "$RUN/mock.trace" | sed 's/.*sessionState=//' | tr '\n' ' '); do printf '%s ' "${NAMES[$s]:-$s}"; done; echo
echo "== statistics (XRSTATS, first $SECS seconds)"
grep -a 'XRSTATS' "$DL" | head -n "$SECS" | awk '
  { for (i=1;i<=NF;i++) { split($i,kv,"="); v[kv[1]]=kv[2] }
    n++; sub_s+=v["submitted"]; if (n==1||v["submitted"]<sub_min) sub_min=v["submitted"]
    fps_s+=v["game_fps"]; if (n==1||v["game_fps"]<fps_min) fps_min=v["game_fps"]
    vps_s+=v["game_vps"]; d_s+=v["drain_avg_ms"]; if (v["drain_max_ms"]>d_max) d_max=v["drain_max_ms"]; eye=v["eye"] }
  END { if (!n) { print "no XRSTATS lines"; exit }
        printf "seconds=%d submitted_per_s_avg=%.1f submitted_per_s_min=%d game_fps_avg=%.1f game_fps_min=%.1f game_vps_avg=%.1f drain_avg_ms=%.2f drain_max_ms=%.2f eye=%s\n",
               n, sub_s/n, sub_min, fps_s/n, fps_min, vps_s/n, d_s/n, d_max, eye }'
echo "== stall file: $([ -s "$RUN/PrimedGunFrameStall.txt" ] && echo "WRITTEN (the frame-completion watchdog paused the game)" || echo none)"
} | tee "$RUN/result.txt"

FATAL=$(grep -a -c 'PANIC\|Fatal\|fatal\|Signal 11\|Segmentation' "$LOG" "$DL" | awk -F: '{s+=$2} END {print s+0}')
echo "mockrun: fatal lines: $FATAL" | tee -a "$RUN/result.txt"
echo "mockrun: error-level lines in the game log: $(grep -a -c ' E\[' "$DL")" | tee -a "$RUN/result.txt"
if [ $VALIDATE = 1 ]; then
  echo "mockrun: validation messages: $(grep -a -c 'VUID-\|Validation Error' "$LOG" "$DL" | awk -F: '{s+=$2} END {print s+0}')" | tee -a "$RUN/result.txt"
fi
[ "$FATAL" = 0 ] || { echo "mockrun: FAIL, fatal lines" >&2; exit 1; }
grep -aq 'event sessionState=5' "$RUN/mock.trace" || { echo "mockrun: FAIL, the session never reached FOCUSED" >&2; exit 1; }
grep -aq 'xrmock: summary: 0 errors' "$LOG" || { echo "mockrun: FAIL, mock errors (see $RUN/mock.trace)" >&2; exit 1; }
grep -aq XRSTATS "$DL" || { echo "mockrun: FAIL, no frames submitted" >&2; exit 1; }
echo "mockrun: ok"
