#!/bin/bash
# PrimeFrame launch wrapper. The Steam shortcut runs it as `"<app>/primeframe.sh" %command%`
# (the shortcut's program is <app>/primeframe), or you can run it directly. There is no screen
# for messages in VR: everything goes to the session log, and a refusal is a non-zero exit.
#
# Every launch: re-applies SteamVR's per-app settings (it drops them now and then), starts the
# compositor timing logger, runs the game, then writes a session folder
# $PRIMEFRAME_HOME/sessions/<date-time>/ with launch.log, stdout.log, dolphin.log,
# PrimedGunFrameStall.txt (if the game wrote one), vrtiming.log and report.txt.
# $PRIMEFRAME_HOME/sessions/latest points at the newest one.
#
# Environment (all optional; session.env below is the way to set them from the Steam library):
#   PRIMEFRAME_HOME         data folder, default ~/.local/share/PrimeFrame (user/, rom, sessions/)
#   PRIMEFRAME_REFRESH_HZ   refresh rate asked of SteamVR, 90 or 120 (default 120)
#   PRIMEFRAME_EYE          eye image size asked of SteamVR (resolutionOverride), default 2160 (the panel;
#                           1728 is SteamVR's 100 % here)
#   PRIMEFRAME_FDM_LAYER=0  sets DISABLE_VULKAN_FDM_INJECTION_LAYER=1 (A/B: Valve's FDM layer off)
#   PRIMEFRAME_SYSMEM=0     leaves TU_DEBUG unset (Turnip's tiled rendering); by default the wrapper sets
#                           TU_DEBUG=sysmem (renders straight to memory: a third of the GPU time here)
#   PRIMEFRAME_PACING       passed through to the game, which has its own default when unset
#   PRIMEFRAME_PLATFORM     Dolphin NoGUI platform, default x11 (headless never presents VR)
#   PRIMEFRAME_VRTIMING=0   no compositor timing log
#   PRIMEFRAME_STATE        start from a save state: a slot number 1 to 10 (user/StateSaves/GM8E01.s01..s10)
#                           or a file path (useful in session.env for a test in a given room)
#   PRIMEFRAME_KEEP_SESSIONS number of session folders kept (default 30)
# session.env: if $PRIMEFRAME_HOME/session.env exists it is sourced for ONE launch (VAR=value
# lines, any of the above or the game's own PRIMEDGUN_* switches), then renamed session.env.used.
# Test hooks: PRIMEFRAME_VRCMD, PRIMEFRAME_STEAMVR_DIR, PRIMEFRAME_VRSETTINGS, PRIMEFRAME_VRTIMING_PY,
# PRIMEFRAME_REPORT_PY, PRIMEFRAME_EXE.
HERE=$(dirname "$(readlink -f "$0")")
PFHOME=${PRIMEFRAME_HOME:-$HOME/.local/share/PrimeFrame}
STAMP=$(date +%Y%m%d-%H%M%S)
SESSION=$PFHOME/sessions/$STAMP
mkdir -p "$SESSION" || exit 1
ln -sfn "$STAMP" "$PFHOME/sessions/latest"
exec > "$SESSION/launch.log" 2>&1
log() { echo "$(date +%T) $*"; }
log "PrimeFrame wrapper, session $STAMP, steam appid '${SteamAppId:-unset}'"

# One launch's test environment.
if [ -f "$PFHOME/session.env" ]; then
    log "session.env:"; cat "$PFHOME/session.env"; cp "$PFHOME/session.env" "$SESSION/session.env"
    set -a; . "$PFHOME/session.env"; set +a
    mv -f "$PFHOME/session.env" "$PFHOME/session.env.used"
    log "session.env renamed to session.env.used"
fi
# The game must use the -u folder, never a user path from the environment.
unset DOLPHIN_EMU_USERPATH
[ "$PRIMEFRAME_FDM_LAYER" = 0 ] && { export DISABLE_VULKAN_FDM_INJECTION_LAYER=1; log "A/B: Valve FDM layer disabled"; }
# Direct rendering by default: the stereo geometry shader made each draw run once per GMEM tile.
[ "${PRIMEFRAME_SYSMEM:-1}" = 1 ] && { export TU_DEBUG=${TU_DEBUG:+$TU_DEBUG,}sysmem; log "TU_DEBUG=$TU_DEBUG"; }
[ -n "$PRIMEFRAME_PACING" ] && { export PRIMEFRAME_PACING; log "PRIMEFRAME_PACING=$PRIMEFRAME_PACING"; }

# The game: Steam's %command% (the binary the shortcut names), else the installed one.
EXE=${PRIMEFRAME_EXE:-$HERE/primeframe}
if [ -z "$PRIMEFRAME_EXE" ] && [ $# -gt 0 ] && [ -x "$1" ] && [ -f "$1" ]; then EXE=$1; shift; fi
[ -x "$EXE" ] || { log "REFUSING TO START: no game binary at $EXE (run install.sh)"; exit 1; }

# The disc image: the path in $PFHOME/rom, chosen at install.
USERDIR=$PFHOME/user
mkdir -p "$USERDIR"
ROM=
[ -f "$PFHOME/rom" ] && ROM=$(head -n 1 "$PFHOME/rom")
if [ -z "$ROM" ]; then
    log "REFUSING TO START: $PFHOME/rom is missing or empty. Write the path of your Metroid Prime (USA) disc image into it, or run install.sh --rom PATH."
    exit 66
fi
if [ ! -f "$ROM" ]; then
    log "REFUSING TO START: the disc image named in $PFHOME/rom does not exist: $ROM"
    exit 66
fi
log "disc image: $ROM"

# SteamVR's per-app settings, every launch. SteamAppId is set by Steam for a library launch.
HZ=${PRIMEFRAME_REFRESH_HZ:-120}
EYE=${PRIMEFRAME_EYE:-2160}
case "$EYE" in 1[0-9][0-9][0-9] | 2[0-9][0-9][0-9] | 3[0-9][0-9][0-9]) ;; *) log "PRIMEFRAME_EYE=$EYE is not a pixel size, using 2160"; EYE=2160 ;; esac
case "$HZ" in 90 | 120) ;; *) log "PRIMEFRAME_REFRESH_HZ=$HZ is not 90 or 120, using 120"; HZ=120 ;; esac
VRCMD=${PRIMEFRAME_VRCMD:-$(command -v vrcmd || echo /opt/steamvr/bin/linuxarm64/vrcmd)}
VRSETTINGS=${PRIMEFRAME_VRSETTINGS:-$HOME/.config/openvr/config/steamvr.vrsettings}
STEAMVR_DIR=${PRIMEFRAME_STEAMVR_DIR:-/opt/steamvr/bin/linuxarm64}
SETTINGS="preferredRefreshRate=$HZ framesToThrottle=0 additionalFramesToPredict=0 motionSmoothingOverride=2 resolutionOverride=$EYE"
if [ -z "$SteamAppId" ]; then
    log "no SteamAppId: SteamVR per-app settings NOT applied (not a Steam library launch)"
elif [ ! -x "$VRCMD" ]; then
    log "no vrcmd at $VRCMD: SteamVR per-app settings NOT applied"
else
    for kv in $SETTINGS; do
        "$VRCMD" --set-settings-int "steam.app.$SteamAppId.${kv%=*}" "${kv#*=}" > /dev/null 2>&1 ||
            log "WARNING: vrcmd could not set ${kv%=*} (is SteamVR running?)"
    done
    sleep 0.3   # SteamVR writes its settings file shortly after a change
    python3 -I - "$VRSETTINGS" "steam.app.$SteamAppId" $SETTINGS << 'PY' || log "settings read-back failed"
import json, sys, time
path, section, pairs = sys.argv[1], sys.argv[2], sys.argv[3:]
try:
    got = json.load(open(path)).get(section, {})
except (OSError, ValueError) as e:
    print(f"{time.strftime('%T')} read-back: cannot read {path} ({e.__class__.__name__}); settings unverified")
    sys.exit(0)
bad = 0
for p in pairs:
    k, v = p.split("=")
    if str(got.get(k)) != v:
        bad += 1
        print(f"{time.strftime('%T')} MISMATCH {section}.{k}: wanted {v}, SteamVR has {got.get(k)}")
print(f"{time.strftime('%T')} read-back: {len(pairs) - bad} of {len(pairs)} per-app settings as asked")
PY
fi

# Compositor timing for the whole session; it cannot stop the game from starting.
VRT_PID=
VRT=${PRIMEFRAME_VRTIMING_PY:-$HERE/tools/vrtiming.py}
if [ "${PRIMEFRAME_VRTIMING:-1}" != 0 ] && [ -f "$VRT" ] && [ -f "$STEAMVR_DIR/libopenvr_api.so" ]; then
    env -u LD_LIBRARY_PATH python3 -I "$VRT" 86400 "$SESSION/vrtiming.log" > /dev/null 2>&1 &
    VRT_PID=$!
else
    log "no vrtiming.log (disabled, or vrtiming.py / libopenvr_api.so missing)"
fi

# Not exec: the wrapper stays to stop the logger and write the report. A signal for the wrapper
# (Steam stopping the app) goes on to the game.
PLATFORM=${PRIMEFRAME_PLATFORM:-x11}
if [ -n "$PRIMEFRAME_STATE" ]; then
    case "$PRIMEFRAME_STATE" in
        [1-9] | 10) STATE=$USERDIR/StateSaves/GM8E01.s$(printf %02d "$PRIMEFRAME_STATE") ;;
        *) STATE=$PRIMEFRAME_STATE ;;
    esac
    if [ -f "$STATE" ]; then set -- -s "$STATE" "$@"; log "starting from the save state $STATE"
    else log "PRIMEFRAME_STATE=$PRIMEFRAME_STATE: no file $STATE, starting normally"; fi
fi
log "starting $EXE -u $USERDIR -p $PLATFORM -v Vulkan -e <disc image> $*"
touch "$SESSION/.start"
"$EXE" -u "$USERDIR" -p "$PLATFORM" -v Vulkan -e "$ROM" "$@" > "$SESSION/stdout.log" 2>&1 &
GAME_PID=$!
# The headset is in use: development builds and headless GPU runs on this machine (this project's and
# the sibling projects') wait while this file exists and names a live process.
HEADSET_MARK=${XDG_CACHE_HOME:-$HOME/.cache}/headset-in-use
echo "primeframe $GAME_PID" > "$HEADSET_MARK" 2> /dev/null
trap 'kill -TERM "$GAME_PID" 2> /dev/null' TERM INT HUP
rc=0
while kill -0 "$GAME_PID" 2> /dev/null; do
    wait "$GAME_PID"; rc=$?
done
trap - TERM INT HUP
[ "$(awk '{print $2}' "$HEADSET_MARK" 2> /dev/null)" = "$GAME_PID" ] && rm -f "$HEADSET_MARK"
log "game exited with status $rc"
[ -n "$VRT_PID" ] && { kill "$VRT_PID" 2> /dev/null; wait "$VRT_PID" 2> /dev/null; }

# The game's own log and stall file, when this run wrote them.
if [ -f "$USERDIR/Logs/dolphin.log" ] && [ "$USERDIR/Logs/dolphin.log" -nt "$SESSION/.start" ]; then
    cp "$USERDIR/Logs/dolphin.log" "$SESSION/dolphin.log"
else
    log "no dolphin.log from this run; the report reads stdout.log"; cp "$SESSION/stdout.log" "$SESSION/dolphin.log"
fi
[ -f "$USERDIR/Logs/PrimedGunFrameStall.txt" ] && [ "$USERDIR/Logs/PrimedGunFrameStall.txt" -nt "$SESSION/.start" ] &&
    { cp "$USERDIR/Logs/PrimedGunFrameStall.txt" "$SESSION/"; log "PrimedGunFrameStall.txt copied: the game stalled"; }
rm -f "$SESSION/.start"
REPORT=${PRIMEFRAME_REPORT_PY:-$HERE/tools/session-report.py}
if [ -f "$REPORT" ]; then
    python3 -I "$REPORT" "$SESSION" > "$SESSION/report.txt" 2>&1 || log "session-report exited non-zero"
else
    log "no session-report.py at $REPORT"
fi
# Keep the newest sessions only.
KEEP=${PRIMEFRAME_KEEP_SESSIONS:-30}
ls -1d "$PFHOME"/sessions/2* 2> /dev/null | head -n -"$KEEP" | xargs -r rm -rf
exit $rc
