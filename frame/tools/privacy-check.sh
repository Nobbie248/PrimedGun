#!/bin/bash
# Looks for personal data before anything is published. GPLv3 (adapted from
# Moonlight XR Native's tools/privacy-check.sh).
#   tools/privacy-check.sh              the tracked files (git ls-files), third_party/ excluded
#   tools/privacy-check.sh --history    the lines every commit on the current branch added, and
#                                       the commits' authors, committers and messages
#   tools/privacy-check.sh DIR|TAR      every file under DIR, or inside a release tarball
# Prints each finding (never the matched text itself) and exits 1 when there is one.
#
# Built-in checks (generic, safe to keep in the repository): access tokens and
# API keys, private keys and certificates, e-mail addresses, MAC addresses,
# IPv4 addresses (LAN ranges included), Steam user ids in paths, home
# directories other than a generic one, and commit identities
# other than a GitHub no-reply address.  Placeholders used by docs and tests
# are allowed: 127.x, 0.0.0.0, the RFC 5737 ranges, multicast, locally
# administered and all-zero MACs.
#
# Personal values (the owner's real name and e-mail, the home network's SSID,
# LAN addresses, host names, other user names) must not be written into the
# repository, not even as search patterns. Put them, one case-insensitive
# extended regular expression per line, in a file outside it:
#   PF_PRIVACY_PATTERNS=FILE
# The default is ~/.config/moonlight-xr-privacy-patterns.txt, the list the
# Moonlight XR Native checker reads, so one list serves both projects
# (MLXR_PRIVACY_PATTERNS names another).  Without a list only the generic
# checks run, and the script says so.
set -uo pipefail
cd "$(dirname "$0")/../.."
PAT="${PF_PRIVACY_PATTERNS:-${MLXR_PRIVACY_PATTERNS:-$HOME/.config/moonlight-xr-privacy-patterns.txt}}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
MODE=files
if [ $# -eq 0 ]; then
    git ls-files -z | grep -zv -e '^third_party/' > "$TMP/list"
    BASE=.
elif [ "$1" = "--history" ]; then
    MODE=history; BASE=.
    git log --format='@@@%H%n%an <%ae>%n%cn <%ce>%n%B%n@@@END' > "$TMP/meta"
    git log -p -U0 --no-color --no-ext-diff --format='@@@COMMIT %H' > "$TMP/diffs"
    : > "$TMP/list"
elif [ -f "$1" ]; then
    mkdir "$TMP/x" && tar -xf "$1" -C "$TMP/x" || { echo "cannot unpack $1"; exit 2; }
    BASE="$TMP/x"; ( cd "$BASE" && find . -type f -print0 ) > "$TMP/list"
else
    BASE="$1"; ( cd "$BASE" && find . -type f -not -path './.git/*' -print0 ) > "$TMP/list"
fi
python3 -I - "$BASE" "$TMP/list" "$PAT" "$MODE" "$TMP" <<'PY'
import ipaddress, os, re, sys
base, listp, patp, mode, tmp = sys.argv[1:6]
files = [f for f in open(listp, "rb").read().split(b"\0") if f]
nets_ok = [ipaddress.IPv4Network(n) for n in ("127.0.0.0/8", "0.0.0.0/32", "192.0.2.0/24", "198.51.100.0/24",
           "203.0.113.0/24", "224.0.0.0/4", "255.255.255.255/32")]
generic = [
    ("token", re.compile(r"\b(ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9]{20,}|\bgithub_pat_[A-Za-z0-9_]{20,}|\bsk-[A-Za-z0-9_-]{20,}|\bAKIA[0-9A-Z]{16}\b|\bxox[abps]-[A-Za-z0-9-]{10,}|\bAIza[0-9A-Za-z_-]{30,}")),
    ("key", re.compile(r"-----BEGIN (RSA |EC |OPENSSH |ENCRYPTED )?PRIVATE KEY-----\s*\n?\s*[A-Za-z0-9+/]{20,}")),
    ("email", re.compile(r"\b[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}\b")),
    ("mac", re.compile(r"(?<![0-9A-Fa-f:])([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}(?![0-9A-Fa-f:])")),
    ("ipv4", re.compile(r"(?<![0-9A-Za-z.])\d{1,3}(\.\d{1,3}){3}(?![0-9A-Za-z]|\.\d)")),
    ("steam user id", re.compile(r"userdata/\d{5,}")),
    ("home directory", re.compile(r"/home/(?!steamos\b|runner\b|user\b|someone\b|<)[a-z_][a-z0-9_-]+/")),
]
email_ok = re.compile(r"@(example\.(com|org|net|invalid)|users\.noreply\.github\.com|anthropic\.com)$|^noreply@", re.I)
def mac_ok(s):
    b = [int(x, 16) for x in s.split(":")]
    return b[0] & 2 or all(x == 0 for x in b) or s.upper().startswith("AA:BB:CC:")
def ip_ok(s, line):
    try: a = ipaddress.IPv4Address(s)
    except ValueError: return True            # 7.1.431.0 and other version numbers
    if any(a in n for n in nets_ok): return True
    # SDK and library version strings in licence texts and headers
    return bool(re.search(r"(?i)(version|ver|v|sdk|release|rev)[\s=:\"'<>/A-Za-z_]{0,14}" + re.escape(s), line))
personal = []
if os.path.isfile(patp):
    for l in open(patp, encoding="utf-8"):
        l = l.strip()
        if l and not l.startswith("#"): personal.append(re.compile(l, re.I))
print("privacy-check: %d personal patterns from %s" % (len(personal),
      "the local list" if personal else "nowhere (no local list; generic checks only)"))
found = 0
def scan(text, where, checks):
    """where: a label for the report; text: one line.  Counts findings."""
    hits = []
    for name, rx in checks:
        for m in rx.finditer(text):
            s = m.group(0)
            if name == "email" and email_ok.search(s): continue
            if name == "mac" and mac_ok(s): continue
            if name == "ipv4" and ip_ok(s, text): continue
            hits.append(name)
    hits += ["personal pattern %d" % (i + 1) for i, rx in enumerate(personal) if rx.search(text)]
    return hits
def report(where, hits):
    global found
    for h in hits:
        found += 1
        print("  %s: %s" % (where, h))   # never the matched value itself
SKIP = ("third_party/",)
allow = []
ap = os.path.join("frame", "tools", "privacy-allow.txt")   # the script runs from the repository root
if os.path.isfile(ap):
    for l in open(ap, encoding="utf-8"):
        if l.startswith("#") or "\t" not in l: continue
        a, b = l.rstrip("\n").split("\t", 1)
        allow.append((re.compile(a), re.compile(b)))
def clean(path, line):
    for pr, tr in allow:
        if pr.search(path): line = tr.sub(" ", line)
    return line
if mode == "history":
    # identities and messages
    meta = open(os.path.join(tmp, "meta"), encoding="utf-8", errors="replace").read()
    n = 0
    for block in meta.split("@@@")[1:]:
        lines = block.split("\n")
        if lines[0] == "END": continue
        n += 1
        h = lines[0][:10]
        for who in lines[1:3]:
            m = re.search(r"<([^>]*)>", who)
            if not m or not re.search(r"@users\.noreply\.github\.com$", m.group(1)):
                report("commit %s identity" % h, ["identity is not a GitHub no-reply address"])
        for ln, line in enumerate(lines[3:], 1):
            report("commit %s message line %d" % (h, ln), scan(line, "", generic[:5] + generic[6:]))
    # added lines
    commit = path = None; skip = False; added = 0
    for line in open(os.path.join(tmp, "diffs"), encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        if line.startswith("@@@COMMIT "): commit = line[10:20]; continue
        if line.startswith("+++ "):
            path = line[6:] if line.startswith("+++ b/") else line[4:]
            skip = path.startswith(SKIP) or path == "/dev/null"; continue
        if skip or line.startswith("+++") or not line.startswith("+"): continue
        added += 1
        report("commit %s %s" % (commit, path), scan(clean(path, line[1:]), "", generic))
    print("privacy-check: %d commits, %d added lines" % (n, added))
else:
    nfiles = 0
    for f in files:
        rel = f.decode("utf-8", "replace")
        if rel.startswith("./"): rel = rel[2:]
        p = os.path.join(base, rel)
        try: data = open(p, "rb").read()
        except OSError: continue
        nfiles += 1
        if b"\0" in data[:4096]:
            text = data.decode("latin-1"); checks = [c for c in generic if c[0] == "token"]   # binaries: tokens and personal patterns only
        else:
            text = data.decode("utf-8", "replace"); checks = generic
        for n, line in enumerate(text.splitlines(), 1):
            report("%s:%d" % (rel, n), scan(clean(rel, line), "", checks))
    print("privacy-check: %d files" % nfiles)
print("privacy-check: %d finding(s)" % found)
sys.exit(1 if found else 0)
PY
