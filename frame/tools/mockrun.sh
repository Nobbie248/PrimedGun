#!/bin/bash
# Runs the game against the mock OpenXR runtime (xrmock), headless: no window, no sound.
#
#   frame/tools/mockrun.sh [--state N] [--seconds N] [--warmup N] [--drain] [--validate]
#                          [--exe PATH] [--platform x11|headless] [--tag NAME]
#
#   --state N     load StateSaves/GM8E01.sNN (01 or 02) at boot (default: none, the game's title)
#   --seconds N   length of the measured window: the first N one-second XRSTATS lines, counted
#                 from the first submitted frame (default 60)
#   --warmup N    extra seconds of session before the window can start (boot, state load);
#                 the mock ends the session after N + warmup seconds (default 30)
#   --drain       PRIMEDGUN_XR_WAIT_GPU=1 (the GPU drain before the eye images are released)
#   --pacing M    PRIMEFRAME_PACING=pipelined|inline|drain (default: the game's default, pipelined)
#   --nowait      PRIMEFRAME_NO_GPU_WAIT=1: release with no GPU wait at all (the negative test:
#                 the mock's release check must fail)
#   --window      PRIMEFRAME_WINDOW_PRESENT=1: keep the desktop window present while in VR
#   --validate    VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation (needs the layer installed for the user)
#   --exe PATH    the dolphin-nogui binary (default <repo>/build-linux/Binaries/dolphin-nogui)
#   --platform P  dolphin-nogui window platform (default x11, in the private Xvfb; headless never presents VR frames)
#   --tag NAME    suffix of the run directory
#   --stacks N    debugging: list the game threads (ps -L: cpu, state, wait channel; ptrace is not available) in threads.txt after N s
#   --eye N       MOCK_XR_EYE=NxN (the eye image size the mock recommends; default 1728)
#   --sysmem      TU_DEBUG=sysmem (Turnip renders straight to memory instead of tiling through GMEM)
#   --tudebug V   TU_DEBUG=V (any Turnip debug flags, e.g. gmem); overrides --sysmem
#   --set F:S.K=V edit the scratch user dir after seeding (repeatable): F is GFX, Dolphin or a game
#                 ini (GM8E01), S the section, K the key, V the value; the key is replaced or added.
#                 A game ini value wins over GFX.ini (InternalResolution, ShaderCompilationMode live there)
#   --clocklog    log the GPU clock (devfreq), its thermal cap, GPU and CPU temperatures and CPU clocks
#                 once a second to clock.log in the run dir, and summarise them over the window
#   --cool C      before the game starts, wait (up to 10 min) until every GPU thermal zone is below C
#                 degrees Celsius, so runs start from the same temperature
#
# Environment: MOCK_XR_* settings of xrmock (MOCK_XR_HZ default 120, MOCK_XR_EYE default 1728x1728,
# MOCK_XR_BUSY_MS default 4: the exact released-unfinished check, MOCK_XR_SKIP default late: a late
# xrWaitFrame returns at once with the running refresh, as SteamVR on the Frame), PF_MOCKRUN_HOME (scratch,
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
STATE=""; SECS=60; WARMUP=30; DRAIN=0; VALIDATE=0; PLATFORM=x11; TAG=""; STACKS=""; PACING=""; NOWAIT=0; WINDOW=0
EYE=""; TUDEBUG=""; SETS=(); CLOCKLOG=0; COOL=""
EXE=${PF_BUILD:-$REPO/build-linux}/Binaries/dolphin-nogui
while [ $# -gt 0 ]; do
  case $1 in
    --state) STATE=$2; shift ;;
    --seconds) SECS=$2; shift ;;
    --warmup) WARMUP=$2; shift ;;
    --drain) DRAIN=1 ;;
    --pacing) PACING=$2; shift ;;
    --nowait) NOWAIT=1 ;;
    --window) WINDOW=1 ;;
    --validate) VALIDATE=1 ;;
    --exe) EXE=$2; shift ;;
    --platform) PLATFORM=$2; shift ;;
    --tag) TAG=$2; shift ;;
    --stacks) STACKS=$2; shift ;;
    --eye) EYE=$2; shift ;;
    --sysmem) TUDEBUG=${TUDEBUG:-sysmem} ;;
    --tudebug) TUDEBUG=$2; shift ;;
    --set) SETS+=("$2"); shift ;;
    --clocklog) CLOCKLOG=1 ;;
    --cool) COOL=$2; shift ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
  shift
done
die() { echo "mockrun: $*" >&2; exit 1; }
# The owner plays other games on this Frame: every game wrapper writes ~/.cache/headset-in-use
# ("<name> <pid>") while it runs. Never measure next to that.
INUSE=$HOME/.cache/headset-in-use
headset_in_use() { [ -f "$INUSE" ] && kill -0 "$(awk '{print $2}' "$INUSE" 2>/dev/null)" 2>/dev/null; }
while headset_in_use; do echo "mockrun: headset in use ($(cat "$INUSE")), waiting" >&2; sleep 60; done
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
# --set: per-run ini edits (GFX, Dolphin -> Config/<F>.ini, anything else -> GameSettings/<F>.ini).
for SET in ${SETS[@]+"${SETS[@]}"}; do
  F=${SET%%:*}; KV=${SET#*:}
  case $F in GFX|Dolphin) INI=$USERDIR/Config/$F.ini ;; *) INI=$USERDIR/GameSettings/$F.ini ;; esac
  python3 -I - "$INI" "$KV" <<'PY2' || die "bad --set $SET"
import os, re, sys
path, kv = sys.argv[1], sys.argv[2]
sk, val = kv.split("=", 1); sec, key = sk.split(".", 1)
lines = open(path).read().splitlines() if os.path.exists(path) else []
out, cur, done, seen = [], None, False, False
for l in lines:
    m = re.match(r"\s*\[(.+)\]\s*$", l)
    if m:
        if cur == sec and not done: out.append(f"{key} = {val}"); done = True
        cur = m.group(1); seen = seen or cur == sec
    elif cur == sec and re.match(r"\s*" + re.escape(key) + r"\s*=", l):
        if not done: out.append(f"{key} = {val}"); done = True
        continue
    out.append(l)
if not done:
    if not seen: out.append(f"[{sec}]")
    out.append(f"{key} = {val}")
open(path, "w").write("\n".join(out) + "\n")
PY2
  echo "set: $SET" >> "$RUN/sets.txt"
done
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

# Headless: a private virtual X server, nothing audible. The game window (platform x11) lives in it:
# Dolphin's own headless platform never reaches the VR present path (Presenter::Present returns early).
unset WAYLAND_DISPLAY
XVFB_DISPLAY_FILE=$RUN/xvfb.display; : > "$XVFB_DISPLAY_FILE"
Xvfb -displayfd 3 -screen 0 1280x720x24 -nolisten tcp 3>"$XVFB_DISPLAY_FILE" >"$RUN/xvfb.log" 2>&1 &
XVFB_PID=$!
trap 'kill $XVFB_PID 2>/dev/null' EXIT
for _ in $(seq 50); do [ -s "$XVFB_DISPLAY_FILE" ] && break; sleep 0.2; done
[ -s "$XVFB_DISPLAY_FILE" ] || die "Xvfb did not start (see $RUN/xvfb.log)"
export DISPLAY=:$(head -1 "$XVFB_DISPLAY_FILE")
# Valve's gamescope WSI layer (implicit, enabled by the desktop session) needs Xwayland and breaks
# swapchains on a plain Xvfb.
unset ENABLE_GAMESCOPE_WSI GAMESCOPE_WAYLAND_DISPLAY
export DISABLE_GAMESCOPE_WSI=1
# Xvfb has no DRI3: Mesa presents through plain X requests (host copy) with this.
export MESA_VK_WSI_DEBUG=${MESA_VK_WSI_DEBUG:-sw}
export SDL_AUDIODRIVER=dummy SDL_AUDIO_DRIVER=dummy
# The loader looks for the validation layer under XDG_DATA_HOME.
if [ $VALIDATE = 1 ]; then
  export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
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
[ -n "$EYE" ] && export MOCK_XR_EYE=${EYE}x${EYE}
[ -n "$TUDEBUG" ] && export TU_DEBUG=$TUDEBUG
# --cool: the GPU clock comes down when it is hot; start every run from the same temperature.
GPU_ZONES=$(for z in /sys/class/thermal/thermal_zone*; do grep -q '^gpuss' "$z/type" 2>/dev/null && echo "$z/temp"; done)
CPU_ZONES=$(for z in /sys/class/thermal/thermal_zone*; do grep -q '^cpu[0-9]' "$z/type" 2>/dev/null && echo "$z/temp"; done)
maxtemp() { cat $1 2>/dev/null | sort -n | tail -1; }
if [ -n "$COOL" ] && [ -n "$GPU_ZONES" ]; then
  for _ in $(seq 60); do
    T=$(maxtemp "$GPU_ZONES"); [ "$T" -lt $((COOL * 1000)) ] && break
    echo "mockrun: GPU at $((T / 1000)) C, cooling to $COOL C" >&2; sleep 10
  done
fi
export MOCK_XR_SECONDS=${MOCK_XR_SECONDS:-$TOTAL}
export MOCK_XR_HZ=${MOCK_XR_HZ:-120}
export MOCK_XR_BUSY_MS=${MOCK_XR_BUSY_MS:-4}
export MOCK_XR_TRACE=${MOCK_XR_TRACE:-$RUN/mock.trace}
export MOCK_XR_SKIP=${MOCK_XR_SKIP:-late}
[ $DRAIN = 1 ] && export PRIMEDGUN_XR_WAIT_GPU=1 || unset PRIMEDGUN_XR_WAIT_GPU
[ -n "$PACING" ] && export PRIMEFRAME_PACING=$PACING || unset PRIMEFRAME_PACING
[ $NOWAIT = 1 ] && export PRIMEFRAME_NO_GPU_WAIT=1 || unset PRIMEFRAME_NO_GPU_WAIT
[ $WINDOW = 1 ] && export PRIMEFRAME_WINDOW_PRESENT=1 || unset PRIMEFRAME_WINDOW_PRESENT

CMD=("$EXE" -u "$USERDIR" -p "$PLATFORM" -v Vulkan -C 'Dolphin.DSP.Backend=No Audio Output' -e "$ROM" "${STATE_ARGS[@]}")
{
  echo "exe: $EXE"; echo "state: ${STATE:-none}  window: ${SECS}s  warmup: ${WARMUP}s  drain: $DRAIN  pacing: ${PACING:-default}  nowait: $NOWAIT  window_present: $WINDOW  validate: $VALIDATE  platform: $PLATFORM"
  echo "mock: $XR_RUNTIME_JSON  hz: $MOCK_XR_HZ  eye: ${MOCK_XR_EYE:-1728x1728}  busy_ms: $MOCK_XR_BUSY_MS  skip: $MOCK_XR_SKIP  session seconds: $MOCK_XR_SECONDS"
  echo "TU_DEBUG: ${TU_DEBUG:-unset}${GPU_ZONES:+  gpu temp at start: $(( $(maxtemp "$GPU_ZONES") / 1000 )) C}"
  [ -s "$RUN/sets.txt" ] && cat "$RUN/sets.txt"
} | tee "$RUN/info.txt"
echo "mockrun: run dir $RUN"
set +e
LOG=$RUN/stdout.log
"${CMD[@]}" > "$LOG" 2>&1 &
PID=$!
# --clocklog: one line a second, Dolphin's log clock (MM:SS:mmm) first so it can be joined with XRSTATS.
CLOCKPID=""
if [ $CLOCKLOG = 1 ]; then
  GDF=$(ls -d /sys/class/devfreq/*.gpu 2>/dev/null | head -1)
  GCD=$(grep -l "devfreq-$(basename "${GDF:-none}")" /sys/class/thermal/cooling_device*/type 2>/dev/null | head -1)
  ( while kill -0 $PID 2>/dev/null; do
      printf '%s gpu_mhz=%d gpu_cap_mhz=%d gpu_cdev=%s gpu_c=%.1f cpu_c=%.1f cpu_mhz=%s\n' "$(date +%M:%S:%3N)" \
        $(( $(cat "$GDF/cur_freq" 2>/dev/null || echo 0) / 1000000 )) $(( $(cat "$GDF/max_freq" 2>/dev/null || echo 0) / 1000000 )) \
        "$(cat "$(dirname "${GCD:-/x}")/cur_state" 2>/dev/null || echo -)" \
        "$(maxtemp "$GPU_ZONES")e-3" "$(maxtemp "$CPU_ZONES")e-3" \
        "$(for p in /sys/devices/system/cpu/cpufreq/policy*; do printf '%d,' $(( $(cat $p/scaling_cur_freq) / 1000 )); done)"
      sleep 1
    done ) > "$RUN/clock.log" 2>/dev/null &
  CLOCKPID=$!
fi
# The mock ends the XR session after MOCK_XR_SECONDS; the game is stopped shortly after.
( sleep $((MOCK_XR_SECONDS + 40)); kill -TERM $PID 2>/dev/null; sleep 15; kill -KILL $PID 2>/dev/null ) &
WATCH=$!
# (thread list for hang hunting)
T0=$SECONDS; ENDED=""; TERMED=""; TERM2=""; INVALID=""
while kill -0 $PID 2>/dev/null; do
  if headset_in_use; then
    INVALID=1; kill -KILL $PID 2>/dev/null; break
  fi
  if [ -n "$STACKS" ] && [ ! -e "$RUN/threads.txt" ] && [ $((SECONDS - T0)) -ge "$STACKS" ]; then
    { ps -L -o tid,comm,pcpu,stat,wchan:32 -p $PID; sleep 2; ps -L -o tid,comm,pcpu,stat,wchan:32 -p $PID; } > "$RUN/threads.txt" 2>&1
  fi
  # The mock ended the XR session: stop the game soon after (it would only fail to acquire images).
  if [ -z "$ENDED" ] && grep -aq 'endSession' "$RUN/mock.trace" 2>/dev/null; then ENDED=$SECONDS; fi
  if [ -n "$ENDED" ] && [ -z "$TERMED" ] && [ $((SECONDS - ENDED)) -ge 2 ]; then
    kill -TERM $PID 2>/dev/null; TERMED=$SECONDS
  fi
  # A second SIGTERM makes Dolphin stop at once (its first one asks nicely); the game has not always
  # exited on the first (it hung after "OpenXR Vulkan: Shut down").
  if [ -n "$TERMED" ] && [ $((SECONDS - TERMED)) -ge 5 ] && [ -z "$TERM2" ]; then kill -TERM $PID 2>/dev/null; TERM2=1; fi
  if [ -n "$TERMED" ] && [ $((SECONDS - TERMED)) -ge 15 ]; then kill -KILL $PID 2>/dev/null; fi
  sleep 0.5
done
wait $PID; RC=$?
kill $WATCH 2>/dev/null
[ -n "$CLOCKPID" ] && kill $CLOCKPID 2>/dev/null
if [ -n "$INVALID" ]; then
  echo "mockrun: INVALID run, headset in use ($(cat "$INUSE" 2>/dev/null)); the game was stopped" | tee "$RUN/result.txt" >&2
  exit 3
fi
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
python3 -I - "$DL" "$SECS" "$RUN/clock.log" <<'PY'
import os, re, statistics, sys
rows, stamps = [], []
for line in open(sys.argv[1], errors="replace"):
    if "XRSTATS" in line:
        rows.append({k: v for k, v in re.findall(r"(\w+)=(\S+)", line)})
        stamps.append(line.split()[0])
rows = rows[:int(sys.argv[2])]
if not rows:
    print("no XRSTATS lines"); sys.exit()
f = lambda k, r=rows: [float(x.get(k, 0)) for x in r]
sub, fps, vps = f("submitted"), f("game_fps"), f("game_vps")
med = statistics.median(sub)
slow = [x for x in sub if x < 0.9 * med]
steady = rows[10:] or rows
avg = lambda k, r=rows: statistics.mean(f(k, r))
print("mode=%s seconds=%d submitted_per_s avg=%.1f min=%d median=%.0f | game_fps avg=%.1f min=%.1f | game_vps avg=%.1f | eye=%s"
      % (rows[0].get("mode", "?"), len(rows), statistics.mean(sub), min(sub), med, statistics.mean(fps), min(fps), statistics.mean(vps), rows[0]["eye"]))
print("layers_per_s avg=%.1f | shown_per_s avg=%.1f | late total=%d (%.2f/s) | gpu_waits total=%d max_ms=%.2f | drain_ms avg=%.2f max=%.2f | gpu_ms avg=%.2f max=%.2f | xrwait_ms avg=%.2f max=%.2f | set_waits total=%d"
      % (avg("layers"), avg("shown"), sum(f("late")), avg("late"), sum(f("gpu_waits")), max(f("gpu_wait_max_ms")), avg("drain_avg_ms"), max(f("drain_max_ms")),
         avg("gpu_ms"), max(f("gpu_ms")), avg("xrwait_avg_ms"), max(f("xrwait_max_ms")), sum(f("set_waits"))))
print("steady (after 10 s): submitted_per_s avg=%.1f min=%d | game_fps avg=%.1f min=%.1f | gpu_ms avg=%.2f | late/s %.2f"
      % (statistics.mean(f("submitted", steady)), min(f("submitted", steady)), statistics.mean(f("game_fps", steady)),
         min(f("game_fps", steady)), avg("gpu_ms", steady), avg("late", steady)))
print("slow seconds (submitted < 90%% of the median): %d, worst %d submitted" % (len(slow), min(sub)))
# --clocklog: the samples inside the measured window (Dolphin's MM:SS:mmm clock, wraps each hour).
if os.path.exists(sys.argv[3]) and stamps:
    sec = lambda t: int(t.split(":")[0]) * 60 + int(t.split(":")[1])
    a, b = sec(stamps[0]), sec(stamps[len(rows) - 1])
    inwin = lambda t: (a <= t <= b) if a <= b else (t >= a or t <= b)
    cl = [dict(re.findall(r"(\w+)=(\S+)", l)) for l in open(sys.argv[3]) if l[:2].isdigit() and inwin(sec(l.split()[0]))]
    g = lambda k: [float(x[k]) for x in cl if k in x]
    if cl:
        print("clock (window, %d samples): gpu_mhz avg=%.0f min=%.0f | gpu_cap_mhz min=%.0f | gpu_c max=%.1f | cpu_c max=%.1f"
              % (len(cl), statistics.mean(g("gpu_mhz")), min(g("gpu_mhz")), min(g("gpu_cap_mhz")), max(g("gpu_c")), max(g("cpu_c"))))
PY
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
