#!/bin/bash
# Installs PrimeFrame on the Steam Frame: the program into ~/Games/PrimeFrame/app, the launch
# wrapper and tools beside it, a default user folder in PRIMEFRAME_HOME, and a non-Steam shortcut
# in Steam's library (through Steam's own client API, so Steam must be running).
#
#   install.sh [options] [primeframe-*-linux-arm64.tar.gz | UNPACKED_RELEASE_DIR]
#   install.sh --dev [BUILD_DIR]     install a build tree (default <repo>/build-linux) as
#                                    "PrimeFrame (dev)" in app-dev; run again after a rebuild
#
#   --name NAME        shortcut name (default "PrimeFrame", with --dev "PrimeFrame (dev)")
#   --rom PATH         your Metroid Prime (USA) disc image (.rvz .iso .gcm); written to PRIMEFRAME_HOME/rom.
#                      Without it, an existing rom file is kept, else ~/Documents/Roms/gc is searched
#   --import-podman [DIR]  owner only: copy the memory card, SRAM, save states, the VR / game
#                      overrides and the [primedgun] settings (Qt.ini) from the old Podman user folder (default
#                      ~/PrimedGun-podman/src/build-podman/Binaries/user). Copies, never moves.
#   --force            with --import-podman: replace saves that already exist (a backup copy is made first)
#   -h, --help
#
# Environment:
#   PRIMEFRAME_APP=DIR       where the program goes (default ~/Games/PrimeFrame/app; dev: app-dev)
#   PRIMEFRAME_HOME=DIR      data folder (default ~/.local/share/PrimeFrame)
#   PRIMEFRAME_SKIP_STEAM=1  no Steam shortcut at all (Steam need not be running)
#   PRIMEFRAME_STEAM_DIR=DIR edit the shortcuts.vdf files of this (copied) Steam folder offline
#                            instead of the live one: DIR/userdata/*/config/shortcuts.vdf, grid art
#                            beside it. Used for dry runs; nothing of the real Steam is touched.
# Your user folder is seeded from seed/ only where files are missing; nothing there is overwritten.
# The library art (art/) is always written for the shortcut's appid into Steam's grid folder.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
export PYTHONDONTWRITEBYTECODE=1
DEV=0; NAME=; ROM_ARG=; IMPORT=0; PODMAN_USER=; FORCE=0; SRC=
while [ $# -gt 0 ]; do
    case $1 in
        --dev) DEV=1 ;;
        --name) NAME=$2; shift ;;
        --rom) ROM_ARG=$2; shift ;;
        --import-podman) IMPORT=1; case ${2:-} in -* | "") ;; *) PODMAN_USER=$2; shift ;; esac ;;
        --force) FORCE=1 ;;
        -h | --help) sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) SRC=$1 ;;
    esac
    shift
done
[ -z "$ROM_ARG" ] || [ -f "$ROM_ARG" ] || { echo "no such disc image: $ROM_ARG" >&2; exit 1; }
PFHOME=${PRIMEFRAME_HOME:-$HOME/.local/share/PrimeFrame}
STEAM=${PRIMEFRAME_STEAM_DIR:-$HOME/.local/share/Steam}
SCRATCH=
cleanup() { [ -n "$SCRATCH" ] && rm -rf "$SCRATCH"; return 0; }
trap cleanup EXIT

# ---- where the files come from: ROOT holds primeframe.sh, tools/, seed/; BIN_DIR holds the binary, Sys/, assets/
if [ "$DEV" = 1 ]; then
    NAME=${NAME:-PrimeFrame (dev)}
    APP=${PRIMEFRAME_APP:-$HOME/Games/PrimeFrame/app-dev}
    BUILD=$(cd "${SRC:-$HERE/../build-linux}" 2> /dev/null && pwd) || { echo "no build tree at ${SRC:-$HERE/../build-linux}" >&2; exit 1; }
    BIN_DIR=$BUILD; [ -x "$BUILD/Binaries/dolphin-nogui" ] && BIN_DIR=$BUILD/Binaries
    [ -x "$BIN_DIR/dolphin-nogui" ] || { echo "no dolphin-nogui in $BUILD (build with frame/build.sh first)" >&2; exit 1; }
    BIN=$BIN_DIR/dolphin-nogui
    ROOT=$HERE
else
    NAME=${NAME:-PrimeFrame}
    APP=${PRIMEFRAME_APP:-$HOME/Games/PrimeFrame/app}
    if [ -f "$SRC" ]; then
        mkdir -p "${TMPDIR:-$HOME/.cache}"
        SCRATCH=$(mktemp -d "${TMPDIR:-$HOME/.cache}/pf-install.XXXXXX")
        tar -xzf "$SRC" -C "$SCRATCH"
        ROOT=$SCRATCH
        [ -f "$ROOT/primeframe.sh" ] || ROOT=$(dirname "$(find "$SCRATCH" -maxdepth 2 -name primeframe.sh | head -1)")
    elif [ -d "$SRC" ]; then
        ROOT=$(cd "$SRC" && pwd)
    elif [ -z "$SRC" ] && [ -x "$HERE/primeframe" ]; then
        ROOT=$HERE      # run from inside an unpacked release
    else
        echo "usage: $0 [--dev [BUILD_DIR]] | primeframe-*-linux-arm64.tar.gz | UNPACKED_RELEASE_DIR" >&2; exit 1
    fi
    BIN_DIR=$ROOT; BIN=$ROOT/primeframe
    [ -x "$BIN" ] && [ -f "$ROOT/primeframe.sh" ] && [ -d "$ROOT/Sys" ] || { echo "$ROOT is not a PrimeFrame release (primeframe, primeframe.sh, Sys/ expected)" >&2; exit 1; }
fi
[ -f "$ROOT/primeframe.sh" ] && [ -d "$ROOT/seed/user" ] || { echo "primeframe.sh or seed/ missing in $ROOT" >&2; exit 1; }
[ -d "$BIN_DIR/Sys" ] || { echo "no Sys/ beside the binary in $BIN_DIR" >&2; exit 1; }
case $NAME in *\'* | *\\*) echo "the shortcut name cannot hold a quote or backslash" >&2; exit 2 ;; esac
if [ -z "$PRIMEFRAME_SKIP_STEAM" ] && [ -z "$PRIMEFRAME_STEAM_DIR" ]; then
    curl -s --max-time 3 127.0.0.1:8080/json > /dev/null || {
        echo "Steam's debugging port is not reachable. Make sure Steam is running (on SteamOS it is on by default)," >&2
        echo "or set PRIMEFRAME_SKIP_STEAM=1 to install without the library entry." >&2
        exit 1
    }
fi
if pgrep -f "^$APP/primeframe( |\$)" > /dev/null 2>&1; then echo "the game is running from $APP; quit it first" >&2; exit 1; fi

# ---- the program: binary, Sys/ and assets/ beside it (the build looks them up next to the binary)
echo "installing into $APP"
mkdir -p "$APP/tools" "$PFHOME/user"
cp "$BIN" "$APP/primeframe.new" && mv "$APP/primeframe.new" "$APP/primeframe"
chmod +x "$APP/primeframe"
for d in Sys assets; do
    [ -d "$BIN_DIR/$d" ] || continue
    rm -rf "${APP:?}/$d"; cp -rL "$BIN_DIR/$d" "$APP/$d"
done
cp "$ROOT/primeframe.sh" "$APP/primeframe.sh"
for t in vrtiming.py session-report.py; do cp "$ROOT/tools/$t" "$APP/tools/$t"; done
cp "$ROOT/steam-cef.py" "$APP/tools/steam-cef.py"
chmod +x "$APP/primeframe.sh" "$APP/tools/steam-cef.py"
for f in install.sh uninstall.sh collect-logs.sh; do [ -f "$ROOT/$f" ] && cp "$ROOT/$f" "$APP/tools/$f" && chmod +x "$APP/tools/$f"; done
for f in README.md COPYING THIRD-PARTY-NOTICES.md; do [ -f "$ROOT/$f" ] && cp "$ROOT/$f" "$APP/$f"; done
rm -rf "$APP/seed"; cp -r "$ROOT/seed" "$APP/seed"

# ---- the user folder: seeded only where a file is missing
SEEDED=0
while IFS= read -r -d '' f; do
    rel=${f#"$ROOT/seed/user/"}
    if [ ! -e "$PFHOME/user/$rel" ]; then
        mkdir -p "$(dirname "$PFHOME/user/$rel")"; cp "$f" "$PFHOME/user/$rel"; SEEDED=$((SEEDED + 1))
    fi
done < <(find "$ROOT/seed/user" -type f -print0)
echo "user folder $PFHOME/user: $SEEDED default file(s) added, existing files kept"

# ---- the disc image
ROMFILE=$PFHOME/rom
if [ -n "$ROM_ARG" ]; then
    [ -f "$ROM_ARG" ] || { echo "no such disc image: $ROM_ARG" >&2; exit 1; }
    printf '%s\n' "$(readlink -f "$ROM_ARG")" > "$ROMFILE"
elif [ -s "$ROMFILE" ]; then
    echo "disc image kept: $(head -n 1 "$ROMFILE")"
else
    mapfile -t FOUND < <(find "$HOME/Documents/Roms/gc" -maxdepth 2 -type f \( -iname '*.rvz' -o -iname '*.iso' -o -iname '*.gcm' \) -iname '*metroid*prime*' 2> /dev/null |
        grep -v -i -E 'echoes|corruption|hunters|pinball|trilogy|prime ?[23]' | sort)
    CHOSEN=
    if [ ${#FOUND[@]} = 1 ]; then CHOSEN=${FOUND[0]}
    elif [ ${#FOUND[@]} -gt 1 ]; then
        if [ -t 0 ]; then
            echo "Several Metroid Prime disc images:"
            for i in "${!FOUND[@]}"; do echo "  $((i + 1))) ${FOUND[$i]}"; done
            read -r -p "Which one (number)? " n
            case $n in '' | *[!0-9]*) ;; *) CHOSEN=${FOUND[$((n - 1))]:-} ;; esac
        else
            echo "Several Metroid Prime disc images found; run again with --rom PATH." >&2
        fi
    fi
    if [ -n "$CHOSEN" ]; then printf '%s\n' "$CHOSEN" > "$ROMFILE"; else echo "No disc image chosen: run install.sh --rom PATH before you play."; fi
fi
[ -s "$ROMFILE" ] && echo "disc image: $(head -n 1 "$ROMFILE")"

# ---- owner only: copy from the old Podman user folder (copy, never move; nothing is written there)
if [ "$IMPORT" = 1 ]; then
    P=${PODMAN_USER:-$HOME/PrimedGun-podman/src/build-podman/Binaries/user}
    [ -d "$P" ] || { echo "no old user folder at $P" >&2; exit 1; }
    U=$PFHOME/user
    STAMP=$(date +%Y%m%d-%H%M%S)
    BK=$PFHOME/backup-$STAMP
    backup() {  # copy an existing destination to the backup folder before it is replaced
        [ -e "$U/$1" ] || return 0
        mkdir -p "$BK/$(dirname "$1")"; cp -a "$U/$1" "$BK/$1"
    }
    copy_item() {  # REL: file or folder
        local rel=$1
        [ -e "$P/$rel" ] || { echo "import: $rel not in the old folder, skipped"; return 0; }
        if [ -e "$U/$rel" ] && [ "$FORCE" != 1 ]; then
            echo "import: $rel already exists here, kept (--force replaces it, after a backup)"; return 0
        fi
        backup "$rel"
        mkdir -p "$U/$(dirname "$rel")"
        if [ -d "$P/$rel" ]; then rm -rf "${U:?}/$rel"; cp -a "$P/$rel" "$U/$rel"; else cp -a "$P/$rel" "$U/$rel"; fi
        echo "import: $rel copied"
    }
    copy_item GC/MemoryCardA.USA.raw
    copy_item GC/SRAM.raw
    copy_item StateSaves
    # ini sections: [VR] of GFX.ini, [Video_Settings] of GameSettings/GM8E01.ini, [VR] of Dolphin.ini,
    # [primedgun] of Qt.ini (the in-VR menu settings, into Config/PrimedGun.ini)
    # (only those sections; the rest of the file and everything personal stay out)
    for spec in "Config/GFX.ini:VR:Config/GFX.ini" "GameSettings/GM8E01.ini:Video_Settings:GameSettings/GM8E01.ini" \
        "Config/Dolphin.ini:VR:Config/Dolphin.ini" "Config/Qt.ini:primedgun:Config/PrimedGun.ini"; do
        IFS=: read -r src sec f <<< "$spec"
        [ -f "$P/$src" ] || continue
        grep -q "^\[$sec\]" "$P/$src" || continue
        if [ -f "$U/$f" ] && ! cmp -s "$U/$f" "$ROOT/seed/user/$f" && [ "$FORCE" != 1 ]; then
            echo "import: $f was changed here, [$sec] kept (--force replaces it, after a backup)"; continue
        fi
        backup "$f"
        python3 -I - "$P/$src" "$U/$f" "$sec" << 'PY'
import re, sys
src, dst, sec = sys.argv[1:]
def sections(text):
    out, cur = [], None
    for line in text.splitlines():
        m = re.match(r"\[(.+)\]\s*$", line)
        if m: cur = [m.group(1), []]; out.append(cur)
        elif cur is not None: cur[1].append(line)
        else: out.append(["", [line]])
    return out
new = next(s for s in sections(open(src, errors="replace").read()) if s[0] == sec)
try: cur = sections(open(dst, errors="replace").read())
except OSError: cur = []
hit = False
for s in cur:
    if s[0] == sec: s[1] = new[1]; hit = True
if not hit: cur.append(new)
open(dst, "w").write("".join(("[%s]\n" % n if n else "") + "".join(l + "\n" for l in ls) for n, ls in cur))
PY
        echo "import: [$sec] of $src copied to $f"
    done
    [ -d "$BK" ] && echo "import: replaced files were saved first in $BK"
fi

# ---- Steam shortcut
EXE=$APP/primeframe
LAUNCH="\"$APP/primeframe.sh\" %command%"
APPID=none; GRID=
if [ -n "$PRIMEFRAME_SKIP_STEAM" ]; then
    echo "PRIMEFRAME_SKIP_STEAM set: no Steam shortcut"
elif [ -n "$PRIMEFRAME_STEAM_DIR" ]; then
    for CFG in "$STEAM"/userdata/*/config; do
        [ -d "$CFG" ] || continue
        APPID=$(python3 "$HERE/steam-cef.py" --vdf "$CFG/shortcuts.vdf" --name "$NAME" --exe "\"$EXE\"" \
            --startdir "\"$APP\"" --opts "$LAUNCH")
        echo "shortcut '$NAME' written to $CFG/shortcuts.vdf (appid $APPID)"
        GRID=$CFG/grid
    done
    [ "$APPID" != none ] || echo "warning: no userdata/*/config in $STEAM; no shortcut written"
else
    CEF="python3 $HERE/steam-cef.py"
    EXISTING=$($CEF "(() => { for (const a of (appStore.allApps || [])) { if (a.display_name === '$NAME' && a.app_type === 1073741824) return a.appid; } return 0; })()" 2> /dev/null || echo 0)
    if [ "$EXISTING" != "0" ] && [ -n "$EXISTING" ]; then
        APPID=$EXISTING
        echo "Steam already has a '$NAME' shortcut (appid $APPID): updating it"
        $CEF "(() => { SteamClient.Apps.SetShortcutExe($APPID, '\"$EXE\"'); SteamClient.Apps.SetShortcutStartDir($APPID, '\"$APP\"'); return 1; })()" > /dev/null
    else
        APPID=$($CEF "(async () => { const id = await SteamClient.Apps.AddShortcut('$NAME', '$EXE', '$APP', ''); SteamClient.Apps.SetShortcutName(id, '$NAME'); return id; })()")
        echo "added the '$NAME' shortcut (appid $APPID)"
    fi
    $CEF "(() => { SteamClient.Apps.SetShortcutLaunchOptions($APPID, '$LAUNCH'); SteamClient.Apps.SetShortcutIsVR($APPID, true); return 1; })()" > /dev/null
    GRID=$(ls -d "$STEAM"/userdata/*/config 2> /dev/null | head -1)/grid
fi

# ---- library art: always written for the shortcut's appid (earlier copies of it are overwritten)
if [ "$APPID" != none ] && [ -n "$GRID" ]; then
    if [ -d "$ROOT/art" ]; then
        NEWID=$(( APPID & 0xffffffff ))
        mkdir -p "$GRID"
        for pair in grid.png:.png grid_p.png:p.png hero.png:_hero.png logo.png:_logo.png icon.ico:_icon.ico; do
            [ -f "$ROOT/art/${pair%%:*}" ] || continue
            cp "$ROOT/art/${pair%%:*}" "$GRID/$NEWID${pair##*:}"
        done
        echo "art: library images written to $GRID ($NEWID*)"
        # offline copies only: the vdf icon field (a live Steam rewrites shortcuts.vdf itself)
        if [ -n "$PRIMEFRAME_STEAM_DIR" ] && [ -f "$GRID/${NEWID}_icon.ico" ]; then
            for CFG in "$STEAM"/userdata/*/config; do
                [ -d "$CFG" ] && python3 "$HERE/steam-cef.py" --vdf "$CFG/shortcuts.vdf" --name "$NAME" --exe "\"$EXE\"" \
                    --startdir "\"$APP\"" --opts "$LAUNCH" --icon "$CFG/grid/${NEWID}_icon.ico" > /dev/null
            done
        fi
    else
        echo "warning: no art/ folder in $ROOT; the library entry has no images"
    fi
fi

cat << MSG

Installed in $APP.
$([ "$APPID" != none ] && echo "Steam library entry \"$NAME\" (appid $APPID) runs $EXE through $APP/primeframe.sh.")
$([ -s "$ROMFILE" ] || echo "Before you play: install.sh --rom <your Metroid Prime (USA) disc image>")
Launch it from the library with SteamVR running.
Logs: $PFHOME/sessions/latest/ (launch.log, dolphin.log, vrtiming.log, report.txt)
MSG
