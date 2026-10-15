#!/usr/bin/env bash
# Builds the PrimedFrame release tarball from a finished build. It does not build anything.
#
#   release.sh [--build DIR] [--out DIR]
#
#   --build DIR   the CMake build directory (default <repo>/build-linux): Binaries/dolphin-nogui,
#                 Binaries/Sys and Binaries/assets (links are followed)
#   --out DIR     where the tarball, its .sha256 and SHA256SUMS (the same line, the name the README uses) go
#                 (default <repo>/release)
#
# The tarball has no top level folder: primedframe (the binary), Sys/, assets/, primedframe.sh,
# install.sh, uninstall.sh, collect-logs.sh, steam-cef.py, tools/{vrtiming.py,session-report.py},
# seed/, art/ (the Steam library images), README.md, README.upstream.md, THIRD-PARTY-NOTICES.md, COPYING, LICENSES/,
# licenses/GPL-3.0.txt, docs/. install.sh unpacks it into the app folder. Nothing from the user folder,
# no disc image, no save, no state. Then frame/tools/privacy-check.sh runs on the tarball (upstream's
# Sys/ is skipped by path) and the sha256 is written beside it. Any failure removes the tarball again.
set -euo pipefail

VERSION=1.0.0
NAME=primedframe-$VERSION-linux-arm64
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/.." && pwd)
build=$repo/build-linux out=$repo/release

fail() { echo "release.sh: error: $*" >&2; exit 1; }
while [[ $# -gt 0 ]]; do
    case $1 in
        --build) build=${2:?}; shift ;;
        --out) out=${2:?}; shift ;;
        -h | --help) sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        *) fail "unknown argument $1" ;;
    esac
    shift
done
bin=$build/Binaries
[[ -x $bin/dolphin-nogui ]] || fail "no $bin/dolphin-nogui (build with frame/build.sh first)"
[[ -d $bin/Sys ]] || fail "no $bin/Sys"
for f in README.md README.upstream.md COPYING THIRD-PARTY-NOTICES.md docs/RELEASE_NOTES_v1.0.0.md docs/CHANGELOG.md docs/TECHNICAL_NOTES.md \
         frame/primedframe.sh frame/install.sh frame/uninstall.sh frame/collect-logs.sh frame/steam-cef.py frame/LICENSE-GPLv3.txt \
         frame/tools/vrtiming.py frame/tools/session-report.py frame/tools/privacy-check.sh frame/seed/user/Config/GFX.ini \
         frame/seed/user/Config/PrimedGun.ini frame/art/grid.png frame/art/grid_p.png frame/art/hero.png frame/art/logo.png frame/art/icon.ico; do
    [[ -f $repo/$f ]] || fail "missing $f"
done
[[ -d $repo/LICENSES ]] || fail "missing LICENSES/"

mkdir -p "$out" "$HOME/.cache"
out=$(cd "$out" && pwd)
stage=$(mktemp -d "${TMPDIR:-$HOME/.cache}/pf-release.XXXXXX")
tarball=$out/$NAME.tar.gz
rm -f "$tarball" "$tarball.sha256"
trap 'rm -f "$tarball" "$tarball.sha256"; rm -rf "$stage"' ERR
trap 'rm -rf "$stage"' EXIT

echo "release: staging"
install -m 0755 "$bin/dolphin-nogui" "$stage/primedframe"
cp -rL "$bin/Sys" "$stage/Sys"
if [[ -d $bin/assets ]]; then
    cp -rL "$bin/assets" "$stage/assets"
    rm -rf "$stage/assets/readme"        # upstream's README pictures, not used at run time
fi
mkdir -p "$stage/tools" "$stage/docs" "$stage/licenses"
for f in primedframe.sh install.sh uninstall.sh collect-logs.sh steam-cef.py; do install -m 0755 "$here/$f" "$stage/$f"; done
install -m 0644 "$here/tools/vrtiming.py" "$here/tools/session-report.py" "$stage/tools/"
cp -r "$here/seed" "$stage/seed"
cp -r "$here/art" "$stage/art"
install -m 0644 "$repo/README.md" "$repo/README.upstream.md" "$repo/THIRD-PARTY-NOTICES.md" "$repo/COPYING" "$stage/"
install -m 0644 "$repo/docs/RELEASE_NOTES_v1.0.0.md" "$repo/docs/CHANGELOG.md" "$repo/docs/TECHNICAL_NOTES.md" "$stage/docs/"
install -m 0644 "$here/LICENSE-GPLv3.txt" "$stage/licenses/GPL-3.0.txt"
cp -r "$repo/LICENSES/." "$stage/LICENSES"
# The user folder, a disc image, saves and states must never get in.
if find "$stage" \( -iname '*.rvz' -o -iname '*.iso' -o -iname '*.gcm' -o -iname '*.raw' -o -iname 'GM8E01.s0*' -o -name 'rom' -o -name 'dolphin.log' \) | grep -q .; then
    fail "the staging folder holds a disc image, save, state or log"
fi
[[ ! -e $stage/user ]] || fail "a user folder got into the staging folder"

echo "release: packing"
# Fixed order, owner and times, so the same inputs make the same tarball.
(cd "$stage" && find . -mindepth 1 -printf '%P\n' | LC_ALL=C sort |
    tar --no-recursion --owner=0 --group=0 --numeric-owner --mtime='2026-01-01 00:00Z' -czf "$tarball" -T -)

echo "release: privacy check"
"$here/tools/privacy-check.sh" "$tarball"

(cd "$out" && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256" && cp "$NAME.tar.gz.sha256" SHA256SUMS)
trap 'rm -rf "$stage"' EXIT; trap - ERR
echo "release: $tarball ($(du -h "$tarball" | cut -f1))"
cat "$tarball.sha256"
