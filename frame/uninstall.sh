#!/bin/bash
# Removes PrimeFrame's Steam shortcut and program folder. The data folder (user/ with your saves,
# states and settings, the rom file, sessions) stays unless --purge.
#
#   frame/uninstall.sh [--dev] [--name NAME] [--purge]
#
# Environment: PRIMEFRAME_APP, PRIMEFRAME_HOME, PRIMEFRAME_SKIP_STEAM, PRIMEFRAME_STEAM_DIR as in install.sh.
# --purge also removes $PRIMEFRAME_HOME (saves, states, settings, logs); there is no undo.
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
    NAME=${NAME:-PrimeFrame (dev)}; APP=${PRIMEFRAME_APP:-$HOME/Games/PrimeFrame/app-dev}
else
    NAME=${NAME:-PrimeFrame}; APP=${PRIMEFRAME_APP:-$HOME/Games/PrimeFrame/app}
fi
PFHOME=${PRIMEFRAME_HOME:-$HOME/.local/share/PrimeFrame}
STEAM=${PRIMEFRAME_STEAM_DIR:-$HOME/.local/share/Steam}
if pgrep -f "^$APP/primeframe( |\$)" > /dev/null 2>&1; then echo "the game is running; quit it first" >&2; exit 1; fi

if [ -n "$PRIMEFRAME_SKIP_STEAM" ]; then
    echo "PRIMEFRAME_SKIP_STEAM set: Steam left alone"
elif [ -n "$PRIMEFRAME_STEAM_DIR" ]; then
    for CFG in "$STEAM"/userdata/*/config; do
        [ -d "$CFG" ] || continue
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
        rm -f "$HOME"/.local/share/Steam/userdata/*/config/grid/"$ID"{,p,_hero,_logo,_icon}.png
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
