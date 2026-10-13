#!/bin/bash
# Tests for install.sh, uninstall.sh, primeframe.sh, collect-logs.sh and session-report.py. Touches
# nothing real: HOME is a scratch folder, Steam is a scratch copy (a copy of the real shortcuts.vdf
# if there is one, read only), vrcmd and the game are fakes. Nothing is run that talks to SteamVR.
#   frame/test-installer.sh [SCRATCH_DIR]      (default ~/.cache/pf-laneD/test-installer, emptied first)
HERE=$(cd "$(dirname "$0")" && pwd)
export PYTHONDONTWRITEBYTECODE=1
T=${1:-$HOME/.cache/pf-laneD/test-installer}
REALHOME=$HOME
rm -rf "$T"; mkdir -p "$T"
fails=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
check() { local d=$1; shift; if "$@"; then ok "$d"; else bad "$d"; fi; }
not() { ! "$@"; }
has() { grep -q -- "$2" "$1"; }
export PRIMEFRAME_SKIP_STEAM= HOME=$T/home XDG_DATA_HOME=$T/home/xdg
mkdir -p "$HOME"

# ---- scratch Steam folder, with the owner's old shortcut and its art ----
STEAM=$T/steam; mkdir -p "$STEAM/userdata/1/config/grid"
REAL=$(ls "$REALHOME"/.local/share/Steam/userdata/*/config/shortcuts.vdf 2> /dev/null | head -1)   # read only, copied
[ -n "$REAL" ] && cp "$REAL" "$STEAM/userdata/1/config/shortcuts.vdf"
VDF=$STEAM/userdata/1/config/shortcuts.vdf
# The real list may already hold PrimeFrame entries (a real install): the copy starts without them.
[ -f "$VDF" ] && python3 -I -B -c 'import importlib.util as u, sys
s = u.spec_from_file_location("cef", sys.argv[1]); m = u.module_from_spec(s); s.loader.exec_module(m)
for n in ("PrimeFrame", "PrimeFrame (dev)"): m.vdf_remove(sys.argv[2], n)' "$HERE/steam-cef.py" "$VDF"
py() { python3 -I -B -c '
import importlib.util, sys
s = importlib.util.spec_from_file_location("c", sys.argv[1]); m = importlib.util.module_from_spec(s); s.loader.exec_module(m)
d = m.vdf_load(sys.argv[2])["shortcuts"]
'"$1" "$HERE/steam-cef.py" "$VDF" "${@:2}"; }
count() { py 'print(sum(1 for v in d.values() if v.get("AppName") == sys.argv[3]) if len(sys.argv) > 3 else len(d))' "$@"; }
field() { py 'print(next(v[sys.argv[4]] for v in d.values() if v.get("AppName") == sys.argv[3]))' "$@"; }
OLD="PrimedGun - Metroid Prime VR"
if [ "$(count "$OLD")" = 0 ]; then
    python3 -I -B -c '
import importlib.util, sys
s = importlib.util.spec_from_file_location("c", sys.argv[1]); m = importlib.util.module_from_spec(s); s.loader.exec_module(m)
m.vdf_shortcut(sys.argv[2], sys.argv[3], "\"/old/primedgun.sh\"", "\"/old/\"", "-e x")' "$HERE/steam-cef.py" "$VDF" "$OLD"
fi
OLDID=$(( $(field "$OLD" appid) & 0xffffffff ))
G=$STEAM/userdata/1/config/grid
for s in "" p _hero _logo; do echo "art$s" > "$G/$OLDID$s.png"; done; echo ico > "$G/${OLDID}_icon.ico"
BEFORE=$(count)

# ---- a fake build tree (what frame/build.sh makes) and fake old Podman user folder ----
BUILD=$T/build-linux; mkdir -p "$BUILD/Binaries" "$T/src-sys/Shaders" "$T/src-assets"
printf '#!/bin/bash\necho fake game\n' > "$BUILD/Binaries/dolphin-nogui"; chmod +x "$BUILD/Binaries/dolphin-nogui"
echo sys > "$T/src-sys/Shaders/a"; echo pic > "$T/src-assets/PrimedGun.png"
ln -s "$T/src-sys" "$BUILD/Binaries/Sys"; ln -s "$T/src-assets" "$BUILD/Binaries/assets"   # as the build links them
POD=$T/podman-user; mkdir -p "$POD/GC" "$POD/StateSaves" "$POD/Config" "$POD/GameSettings"
echo card > "$POD/GC/MemoryCardA.USA.raw"; echo sram > "$POD/GC/SRAM.raw"; echo st1 > "$POD/StateSaves/GM8E01.s01"
printf '[Settings]\nInternalResolution = 4\n[VR]\nUnitsPerMeter = 2.5\nEnableOpenXR = True\n' > "$POD/Config/GFX.ini"
printf '[Video_Settings]\nInternalResolution = 3\n' > "$POD/GameSettings/GM8E01.ini"
printf '[Analytics]\nID = 0123456789abcdef0123456789abcdef\n' > "$POD/Config/Dolphin.ini"
PODSUM=$(cd "$POD" && find . -type f -exec sha256sum {} + | sort | sha256sum)
mkdir -p "$HOME/Documents/Roms/gc"

export PRIMEFRAME_APP=$T/app-dev PRIMEFRAME_HOME=$T/pfhome PRIMEFRAME_STEAM_DIR=$STEAM
P=$PRIMEFRAME_HOME
"$HERE/install.sh" --dev "$BUILD" > "$T/install1.log" 2>&1; check "install.sh --dev exits 0" test $? -eq 0
A=$T/app-dev
check "binary, Sys and assets copied (links followed)" test -x "$A/primeframe" -a -f "$A/Sys/Shaders/a" -a -f "$A/assets/PrimedGun.png" -a ! -L "$A/Sys"
check "wrapper, tools, uninstall installed" test -x "$A/primeframe.sh" -a -f "$A/tools/vrtiming.py" -a -f "$A/tools/session-report.py" -a -x "$A/tools/uninstall.sh" -a -x "$A/tools/collect-logs.sh" -a ! -e "$A/dolphin-nogui"
check "user folder seeded" test -f "$P/user/Config/Dolphin.ini" -a -f "$P/user/Config/GFX.ini" -a -f "$P/user/GameSettings/GM8E01.ini" -a -f "$P/user/Config/Logger.ini"
check "seed keeps GPU texture decoding off" has "$P/user/GameSettings/GM8E01.ini" "^EnableGPUTextureDecoding = False"
check "seed has the VR section and no analytics id" has "$P/user/Config/GFX.ini" "^EnableOpenXR = True" && not grep -q "^ID = " "$P/user/Config/Dolphin.ini"
check "no rom yet: no rom file" test ! -s "$P/rom"
check "shortcut added once" test "$(count 'PrimeFrame (dev)')" = 1
check "shortcut count grew by one" test "$(count)" = $((BEFORE + 1))
check "launch options run the wrapper" test "$(field 'PrimeFrame (dev)' LaunchOptions)" = "\"$A/primeframe.sh\" %command%"
check "exe is the installed binary" test "$(field 'PrimeFrame (dev)' Exe)" = "\"$A/primeframe\""
NEWID=$(( $(field 'PrimeFrame (dev)' appid) & 0xffffffff ))
check "art of the old shortcut copied to the new appid" test -f "$G/${NEWID}p.png" -a -f "$G/${NEWID}_hero.png" -a -f "$G/${NEWID}_logo.png" -a -f "$G/$NEWID.png" -a -f "$G/${NEWID}_icon.ico"
check "old shortcut and its art untouched" test "$(count "$OLD")" = 1 -a -f "$G/${OLDID}p.png"
echo mine >> "$P/user/Config/GFX.ini"; echo m > "$P/user/GC.keep"
"$HERE/install.sh" --dev "$BUILD" > "$T/install2.log" 2>&1
check "second install: no duplicate, user edits kept" test "$(count 'PrimeFrame (dev)')" = 1 && has "$P/user/Config/GFX.ini" "^mine"

# disc image: one candidate, then several (no terminal: not chosen), then --rom
echo x > "$HOME/Documents/Roms/gc/Metroid Prime (USA).rvz"; echo x > "$HOME/Documents/Roms/gc/Metroid Prime 2 Echoes.rvz"
"$HERE/install.sh" --dev "$BUILD" > "$T/install3.log" 2>&1
check "one Metroid Prime image found: written to rom (Echoes ignored)" test "$(cat "$P/rom")" = "$HOME/Documents/Roms/gc/Metroid Prime (USA).rvz"
rm "$P/rom"; echo x > "$HOME/Documents/Roms/gc/Metroid Prime (USA) copy.iso"
"$HERE/install.sh" --dev "$BUILD" < /dev/null > "$T/install3b.log" 2>&1
check "several images, no terminal: nothing guessed" test ! -s "$P/rom" && has "$T/install3b.log" "run again with --rom"
mkdir -p "$T/disc"; echo x > "$T/disc/mp.rvz"
"$HERE/install.sh" --dev --rom "$T/disc/mp.rvz" "$BUILD" > "$T/install3c.log" 2>&1
check "--rom writes the path" test "$(cat "$P/rom")" = "$T/disc/mp.rvz"
"$HERE/install.sh" --dev "$BUILD" > "$T/install3d.log" 2>&1
check "an existing rom file is kept" test "$(cat "$P/rom")" = "$T/disc/mp.rvz"

# Podman import: copies, never touches the source, refuses to overwrite, backs up on --force
"$HERE/install.sh" --dev --import-podman "$POD" "$BUILD" > "$T/import1.log" 2>&1
check "import copies card, SRAM and states" test -f "$P/user/GC/MemoryCardA.USA.raw" -a -f "$P/user/GC/SRAM.raw" -a -f "$P/user/StateSaves/GM8E01.s01"
check "import leaves the source unchanged" test "$(cd "$POD" && find . -type f -exec sha256sum {} + | sort | sha256sum)" = "$PODSUM"
check "ini section import skipped for a user-edited GFX.ini" has "$T/import1.log" "GFX.ini was changed here"
check "unedited GM8E01.ini gets the old [Video_Settings]" has "$P/user/GameSettings/GM8E01.ini" "^InternalResolution = 3"
check "no analytics id imported" not grep -q "0123456789abcdef" -r "$P"
echo newcard > "$P/user/GC/MemoryCardA.USA.raw"
"$HERE/install.sh" --dev --import-podman "$POD" "$BUILD" > "$T/import2.log" 2>&1
check "second import keeps the existing card" test "$(cat "$P/user/GC/MemoryCardA.USA.raw")" = newcard && has "$T/import2.log" "already exists here, kept"
"$HERE/install.sh" --dev --import-podman "$POD" --force "$BUILD" > "$T/import3.log" 2>&1
check "--force replaces it after a backup" test "$(cat "$P/user/GC/MemoryCardA.USA.raw")" = card -a "$(cat "$P"/backup-*/GC/MemoryCardA.USA.raw)" = newcard
check "--force merged [VR] into GFX.ini, kept the rest" has "$P/user/Config/GFX.ini" "^UnitsPerMeter = 2.5"
check "the source still unchanged" test "$(cd "$POD" && find . -type f -exec sha256sum {} + | sort | sha256sum)" = "$PODSUM"

# release layout: a flat tarball with the binary, Sys/, assets/ and the scripts
R=$T/rel; mkdir -p "$R/tools"; cp "$BUILD/Binaries/dolphin-nogui" "$R/primeframe"; cp -rL "$T/src-sys" "$R/Sys"; cp -rL "$T/src-assets" "$R/assets"
cp "$HERE"/{primeframe.sh,install.sh,uninstall.sh,collect-logs.sh,steam-cef.py} "$R/"; cp "$HERE"/tools/{vrtiming.py,session-report.py} "$R/tools/"; cp -r "$HERE/seed" "$R/seed"
tar -czf "$T/rel.tar.gz" -C "$R" .
PRIMEFRAME_APP=$T/app "$HERE/install.sh" "$T/rel.tar.gz" > "$T/install5.log" 2>&1
check "tarball install into the app folder" test -x "$T/app/primeframe" -a -f "$T/app/Sys/Shaders/a" -a "$(count PrimeFrame)" = 1 -a "$(field PrimeFrame Exe)" = "\"$T/app/primeframe\""
PRIMEFRAME_APP=$T/app "$R/install.sh" > "$T/install6.log" 2>&1
check "install.sh run from an unpacked release" test $? -eq 0 -a "$(count PrimeFrame)" = 1
PRIMEFRAME_APP=$T/app PRIMEFRAME_STEAM_DIR= PRIMEFRAME_SKIP_STEAM=1 "$HERE/install.sh" "$T/rel.tar.gz" > "$T/install7.log" 2>&1
check "PRIMEFRAME_SKIP_STEAM=1 installs without Steam" test $? -eq 0 && has "$T/install7.log" "no Steam shortcut"
echo "mine" > "$P/user/StateSaves/keepme"; mkdir -p "$HOME/.local/share/other"; echo mine > "$HOME/.local/share/other/x"

# ---- uninstall ----
PRIMEFRAME_APP=$T/app "$HERE/uninstall.sh" > "$T/un1.log" 2>&1
check "uninstall removes the shortcut and app, keeps the dev one" test "$(count PrimeFrame)" = 0 -a ! -d "$T/app" -a "$(count 'PrimeFrame (dev)')" = 1
"$HERE/uninstall.sh" --dev > "$T/un2.log" 2>&1
check "uninstall --dev removes it; the user folder stays" test "$(count 'PrimeFrame (dev)')" = 0 -a ! -d "$A" -a -f "$P/user/StateSaves/keepme" -a "$(count)" = "$BEFORE"
check "the old shortcut is still there" test "$(count "$OLD")" = 1

# ---- primeframe.sh with a fake vrcmd, fake vrtiming and a fake game ----
unset PRIMEFRAME_STEAM_DIR
W=$T/w; mkdir -p "$W/bin" "$W/app/tools" "$W/svr" "$W/home/user/Logs"
export PRIMEFRAME_HOME=$W/home PRIMEFRAME_STEAMVR_DIR=$W/svr PRIMEFRAME_VRSETTINGS=$W/vrsettings.json
touch "$W/svr/libopenvr_api.so"
cp "$HERE/primeframe.sh" "$W/app/"; cp "$HERE/tools/session-report.py" "$W/app/tools/"
cat > "$W/app/tools/vrtiming.py" << 'PY'
import sys, time
open(sys.argv[2], "w").write("fake vrtiming\n"); time.sleep(60)
PY
cat > "$W/bin/vrcmd" << 'SH'
#!/bin/bash
echo "$@" >> "$FAKE_VRCMD_LOG"
[ -n "$FAKE_VRCMD_NOWRITE" ] && exit 0
python3 -I - "$2" "$3" "$FAKE_VRSETTINGS" << 'PY'
import json, os, sys
key, val, path = sys.argv[1:]
sec, name = key.rsplit(".", 1)
d = json.load(open(path)) if os.path.exists(path) else {}
d.setdefault(sec, {})[name] = int(val)
json.dump(d, open(path, "w"))
PY
SH
cat > "$W/app/primeframe" << 'SH'
#!/bin/bash
# a fake game: writes what it was given where the real one writes its log
U=; while [ $# -gt 0 ]; do [ "$1" = -u ] && U=$2; shift; done
set -- "$@"
mkdir -p "$U/Logs"
{ echo "env FDM=$DISABLE_VULKAN_FDM_INJECTION_LAYER TU=$TU_DEBUG PACING=$PRIMEFRAME_PACING USERPATH=${DOLPHIN_EMU_USERPATH:-unset}"
  echo "XRSTATS submitted=60 drain_avg_ms=0.10 drain_max_ms=1.0 eye=1728x1728 game_fps=59.9 game_vps=59.9"
  echo "XRSTATS submitted=59 drain_avg_ms=0.20 drain_max_ms=2.0 eye=1728x1728 game_fps=59.0 game_vps=59.9"; } > "$U/Logs/dolphin.log"
[ -n "$FAKE_STALL" ] && echo stalled > "$U/Logs/PrimedGunFrameStall.txt"
echo "args: $FAKE_ARGS_MARK"; echo "game stdout"; sleep 2
exit ${FAKE_RC:-0}
SH
# the fake game also records its own arguments
sed -i 's|^echo "args: \$FAKE_ARGS_MARK"; ||' "$W/app/primeframe"
sed -i '2i echo "$*" > "$(dirname "$0")/../args.txt"' "$W/app/primeframe"
chmod +x "$W/bin/vrcmd" "$W/app/primeframe" "$W/app/primeframe.sh"
export FAKE_VRCMD_LOG=$W/vrcmd.log FAKE_VRSETTINGS=$PRIMEFRAME_VRSETTINGS
run() { env PATH="$W/bin:$PATH" SteamAppId=4242 "$@" "$W/app/primeframe.sh" "$W/app/primeframe" --steamarg > "$W/out.txt" 2>&1; echo $? > "$W/rc"; }
run
S=$PRIMEFRAME_HOME/sessions/latest
check "no rom file: refuses to start (exit 66) with a clear line" test "$(cat "$W/rc")" = 66 && has "$S/launch.log" "REFUSING TO START"
sleep 1
echo "$W/nodisc.rvz" > "$PRIMEFRAME_HOME/rom"; run
check "rom path that does not exist: refuses (exit 66)" test "$(cat "$W/rc")" = 66 && has "$S/launch.log" "does not exist"
sleep 1
echo x > "$W/disc.rvz"; echo "$W/disc.rvz" > "$PRIMEFRAME_HOME/rom"
DOLPHIN_EMU_USERPATH=/elsewhere run
check "wrapper exits with the game's status" test "$(cat "$W/rc")" = 0
for k in preferredRefreshRate=120 framesToThrottle=0 additionalFramesToPredict=0 motionSmoothingOverride=2 resolutionOverride=1728; do
    check "vrcmd --set-settings-int steam.app.4242.${k%=*} ${k#*=}" has "$W/vrcmd.log" "^--set-settings-int steam.app.4242.${k%=*} ${k#*=}\$"
done
check "read-back logged as matching" has "$S/launch.log" "read-back: 5 of 5"
check "game started with -u -p x11 -v Vulkan -e <disc> and Steam's own arguments" has "$W/args.txt" "^-u $PRIMEFRAME_HOME/user -p x11 -v Vulkan -e $W/disc.rvz --steamarg\$"
check "the user path from the environment is dropped" has "$S/dolphin.log" "USERPATH=unset"
check "dolphin.log copied into the session" has "$S/dolphin.log" "XRSTATS submitted=60"
check "vrtiming.log written" has "$S/vrtiming.log" "fake vrtiming"
check "report.txt has the XRSTATS table" has "$S/report.txt" "^submitted"
check "stdout.log written" has "$S/stdout.log" "game stdout"
check "no stall file when the game wrote none" test ! -e "$S/PrimedGunFrameStall.txt"
check "vrtiming logger stopped at exit" test -z "$(pgrep -f "$W/app/tools/vrtiming.py")"
check "defaults: no sysmem, no switches" has "$S/dolphin.log" "env FDM= TU= PACING="
sleep 1
FAKE_STALL=1 FAKE_RC=3 run
check "the game's exit status is passed on" test "$(cat "$W/rc")" = 3
check "a stall file is copied into the session" has "$PRIMEFRAME_HOME/sessions/latest/PrimedGunFrameStall.txt" stalled
sleep 1
echo '{"steam.app.4242": {"preferredRefreshRate": 72, "framesToThrottle": 0}}' > "$PRIMEFRAME_VRSETTINGS"; run FAKE_VRCMD_NOWRITE=1
check "mismatch logged when SteamVR dropped the settings" has "$PRIMEFRAME_HOME/sessions/latest/launch.log" "MISMATCH steam.app.4242.preferredRefreshRate"
check "missing settings counted as mismatches" has "$PRIMEFRAME_HOME/sessions/latest/launch.log" "read-back: 1 of 5"
sleep 1
printf 'PRIMEFRAME_SYSMEM=1\n' > "$PRIMEFRAME_HOME/session.env"
run PRIMEFRAME_FDM_LAYER=0 PRIMEFRAME_PACING=pipelined PRIMEFRAME_REFRESH_HZ=90
S=$PRIMEFRAME_HOME/sessions/latest
check "switches and session.env reach the game" has "$S/dolphin.log" "env FDM=1 TU=sysmem PACING=pipelined"
check "90 Hz choice applied" has "$W/vrcmd.log" "preferredRefreshRate 90"
check "session.env renamed after one launch, copy in the session" test ! -f "$PRIMEFRAME_HOME/session.env" -a -f "$PRIMEFRAME_HOME/session.env.used" -a -f "$S/session.env"
sleep 1
run
check "next launch: session.env gone, sysmem off again" has "$PRIMEFRAME_HOME/sessions/latest/dolphin.log" "TU= PACING="
sleep 1
run SteamAppId=; check "no SteamAppId: settings skipped, game still runs" has "$PRIMEFRAME_HOME/sessions/latest/launch.log" "no SteamAppId"
sleep 1
PATH="$W/bin:$PATH" SteamAppId=4242 "$W/app/primeframe.sh" > "$W/out.txt" 2>&1
check "run directly (no %command%): the app's own binary" has "$PRIMEFRAME_HOME/sessions/latest/launch.log" "starting $W/app/primeframe"

# ---- collect-logs.sh masks what it should ----
mkdir -p "$W/logs"; export PRIMEFRAME_LOG_DIR=$W/logs
printf 'host user /home/someone/x 192.0.2.20 aa:bb:cc:dd:ee:01 me@example.org\n' >> "$PRIMEFRAME_HOME/sessions/latest/launch.log"
mkdir -p "$PRIMEFRAME_HOME/user/Config"; printf '[Analytics]\nID = 0123456789abcdef0123456789abcdef\n' > "$PRIMEFRAME_HOME/user/Config/Dolphin.ini"
"$HERE/collect-logs.sh" > "$W/collect.out" 2>&1
TB=$(ls "$W/logs"/primeframe-logs-*.tar.gz 2> /dev/null | head -1)
check "collect-logs.sh writes an archive" test -n "$TB"
mkdir -p "$W/cl"; [ -n "$TB" ] && tar -xzf "$TB" -C "$W/cl"
check "addresses, home user and analytics id masked" not grep -rqE '192\.168\.1\.20|/home/someone|0123456789abcdef|me@example' "$W/cl"
echo
[ $fails = 0 ] && echo "all passed" || { echo "$fails failed"; exit 1; }
