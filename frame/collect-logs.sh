#!/bin/bash
# Collects the logs needed for a bug report into one archive, with personal data masked.
#
#   frame/collect-logs.sh [SESSION_DIR]
#
# Without an argument the newest session is used ($PRIMEDFRAME_HOME/sessions/latest). Collected: the
# session's launch.log, stdout.log, dolphin.log, the stall file, vrtiming.log, report.txt and
# session.env, the three ini files of the user folder, and a summary (SteamOS version, binary hash).
# Masked: IP and MAC addresses, UUIDs, hostnames, Wi-Fi network names, serial numbers, Steam IDs,
# email addresses, tokens, certificate and key blocks, and user names in /home paths.
# The masking is a best effort. Read the files before you post them.
set -euo pipefail
PFHOME=${PRIMEDFRAME_HOME:-$HOME/.local/share/PrimedFrame}
USERDIR=$PFHOME/user
SESSION=${1:-$PFHOME/sessions/latest}
[ -d "$SESSION" ] || { echo "no session folder at $SESSION (launch the game once, or pass a session folder)"; exit 1; }
SESSION=$(readlink -f "$SESSION")

STAMP=$(date +%Y%m%d-%H%M%S)
WORK=$(mktemp -d)
RAW=$WORK/raw; OUT=$WORK/primedframe-logs-$STAMP
mkdir -p "$RAW" "$OUT"
grab() { [ -f "$1" ] && cp "$1" "$RAW/$2" || true; }

for f in launch.log stdout.log dolphin.log PrimedGunFrameStall.txt vrtiming.log report.txt session.env; do grab "$SESSION/$f" "$f"; done
grab "$USERDIR/Config/Dolphin.ini" Dolphin.ini; grab "$USERDIR/Config/GFX.ini" GFX.ini; grab "$USERDIR/GameSettings/GM8E01.ini" GM8E01.ini

{
    echo "collected: $(date -u +%Y-%m-%dT%H:%MZ)"
    echo "session: $(basename "$SESSION")"
    grep -E '^(VERSION_ID|BUILD_ID)=' /etc/os-release 2>/dev/null | sed 's/^/steamos /' || true
    APPBIN=${PRIMEDFRAME_APP:-$HOME/Games/PrimedFrame/app}/primedframe
    [ -x "$APPBIN" ] && echo "binary sha256: $(sha256sum "$APPBIN" | cut -d' ' -f1)"
} > "$RAW/summary.txt"

python3 -I - "$RAW" "$OUT" "$(hostname 2>/dev/null || true)" <<'EOF'
import os, re, sys
src, dst, host = sys.argv[1], sys.argv[2], sys.argv[3]
I = re.I
DEFAULT_HOSTS = {'steamdeck', 'steamos', 'steamframe', 'frame', 'localhost'}
rules = [
    (re.compile(r'-----BEGIN [A-Z ]+-----.*?-----END [A-Z ]+-----', re.S), '<pem removed>'),
    (re.compile(r'\b(?:(?:ghp|gho|ghu|ghs|github_pat|xox[bp])_|sk-)[A-Za-z0-9_-]{16,}'), '<token>'),
    (re.compile(r'[\w.+-]+@[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)*\.(?!(?:service|socket|mount|slice|target|scope|so|rc|xml|bin|conf|json|log)\b)[A-Za-z]{2,}\b'), '<email>'),
    (re.compile(r'\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b', I), '<uuid>'),
    (re.compile(r'\b[0-9a-f]{2}([:-])[0-9a-f]{2}(\1[0-9a-f]{2}){4}\b', I), '<mac>'),
    (re.compile(r'\b(?:[0-9a-f]{1,4}:){4,7}[0-9a-f]{1,4}\b|\b(?:[0-9a-f]{1,4}:){1,6}:(?:[0-9a-f]{1,4}:){0,5}[0-9a-f]{1,4}\b', I), '<ipv6>'),
    (re.compile(r'\b(?!0\.0\.0\.0\b)(?!127\.)(?:\d{1,3}\.){3}\d{1,3}\b'), '<ip>'),
    (re.compile(r'\b(\w*(?:_sn|serial_?no|serial_?number|android_?id|device_?id|imei)\b["\']?\s*[:=]\s*)\S+', I), r'\1<serial>'),
    (re.compile(r'\b(b?ssid\b["\']?\s*[:=]?\s*)("[^"]*"|\'[^\']*\'|\S+)', I), r'\1<ssid>'),
    (re.compile(r'(Hostname set to\s*)<*[^\s<>]+>*', I), r'\1<host>'),
    (re.compile(r'\b7656119\d{10}\b'), '<steamid>'),
    (re.compile(r'(userdata/)\d+'), r'\1<id>'),
    (re.compile(r'^(ID\s*=\s*)\S+', re.M), r'\1<id>'),
    (re.compile(r'/home/[^/\s]+'), '/home/<user>'),
]
for name in sorted(os.listdir(src)):
    text = open(os.path.join(src, name), errors='replace').read()
    if len(host) >= 3 and host.lower() not in DEFAULT_HOSTS:
        text = re.sub(r'\b' + re.escape(host) + r'\b', '<host>', text)
    for pattern, repl in rules:
        text = pattern.sub(repl, text)
    open(os.path.join(dst, name), 'w').write(text)
EOF

DEST=${PRIMEDFRAME_LOG_DIR:-$HOME}/primedframe-logs-$STAMP.tar.gz
tar -czf "$DEST" -C "$WORK" "$(basename "$OUT")"
rm -rf "$WORK"
echo "wrote $DEST"
echo "Open it and read the files before you attach it anywhere. The masking can miss things."
