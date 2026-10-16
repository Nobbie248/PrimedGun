#!/bin/bash
# Removes PrimedFrame's Steam shortcut and program folder. The data folder (user/ with your saves,
# states and settings, the rom file, sessions) stays unless --purge.
#
#   frame/uninstall.sh [--dev] [--name NAME] [--purge]
#
# Environment: PRIMEDFRAME_APP, PRIMEDFRAME_HOME, PRIMEDFRAME_SKIP_STEAM, PRIMEDFRAME_STEAM_DIR as in install.sh.
# --purge also removes $PRIMEDFRAME_HOME (saves, states, settings, logs); there is no undo.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
export PYTHONDONTWRITEBYTECODE=1
DEV=0; NAME=; PURGE=0
while [ $# -gt 0 ]; do
    case $1 in
        --dev) DEV=1 ;;
        --name) NAME=$2; shift ;;
        --purge) PURGE=1 ;;
        -h | --help) sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
if [ "$DEV" = 1 ]; then
    NAME=${NAME:-PrimedFrame (dev)}; APP=${PRIMEDFRAME_APP:-$HOME/Games/PrimedFrame/app-dev}
else
    NAME=${NAME:-PrimedFrame}; APP=${PRIMEDFRAME_APP:-$HOME/Games/PrimedFrame/app}
fi
PFHOME=${PRIMEDFRAME_HOME:-$HOME/.local/share/PrimedFrame}
STEAM=${PRIMEDFRAME_STEAM_DIR:-$HOME/.local/share/Steam}
if pgrep -f "^$APP/primedframe( |\$)" > /dev/null 2>&1; then echo "the game is running; quit it first" >&2; exit 1; fi

if [ -n "$PRIMEDFRAME_SKIP_STEAM" ]; then
    echo "PRIMEDFRAME_SKIP_STEAM set: Steam left alone"
elif [ -n "$PRIMEDFRAME_STEAM_DIR" ]; then
    for CFG in "$STEAM"/userdata/*/config; do
        [ -d "$CFG" ] || continue
        ID=$(python3 -I -B - "$HERE/steam-cef.py" "$CFG/shortcuts.vdf" "$NAME" << 'PY'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("c", sys.argv[1]); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
for v in m.vdf_load(sys.argv[2])["shortcuts"].values():
    if v.get("AppName") == sys.argv[3]:
        print(v["appid"] & 0xffffffff); break
PY
) || ID=
        [ -z "$ID" ] || rm -f "$CFG/grid/$ID".png "$CFG/grid/$ID"{p,_hero,_logo}.png "$CFG/grid/${ID}_icon.ico"
        N=$(python3 "$HERE/steam-cef.py" --vdf "$CFG/shortcuts.vdf" --name "$NAME" --remove)
        echo "shortcut '$NAME' in $CFG/shortcuts.vdf: removed=$N"
    done
else
    curl -s --max-time 3 127.0.0.1:8080/json > /dev/null || { echo "Steam is not reachable; start it, or the shortcut stays" >&2; exit 1; }
    CEF="python3 $HERE/steam-cef.py"
    ID=$($CEF "(() => { for (const a of (appStore.allApps || [])) { if (a.display_name === '$NAME' && a.app_type === 1073741824) return a.appid; } return 0; })()" 2> /dev/null || echo 0)
    if [ "$ID" != 0 ] && [ -n "$ID" ]; then
        $CEF "(() => { SteamClient.Apps.RemoveShortcut($ID); return 1; })()" > /dev/null
        echo "removed the '$NAME' shortcut (appid $ID)"
        rm -f "$STEAM"/userdata/*/config/grid/"$ID".png "$STEAM"/userdata/*/config/grid/"$ID"{p,_hero,_logo}.png "$STEAM"/userdata/*/config/grid/"${ID}_icon.ico"
    else
        echo "Steam has no '$NAME' shortcut"
    fi
fi

[ -d "$APP" ] && { rm -rf "$APP"; echo "removed $APP"; }
if [ "$PURGE" = 1 ]; then
    echo "--purge: removing $PFHOME (saves, states, settings, logs)"
    rm -rf "$PFHOME"
else
    echo "kept your data in $PFHOME; --purge removes it"
fi
