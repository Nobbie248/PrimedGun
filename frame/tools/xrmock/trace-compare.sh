#!/bin/bash
# Compares two mock traces (MOCK_XR_TRACE of two frame/tools/mockrun.sh runs: two builds, or the
# same build in two pacing modes) by what the frame loop does, not by when.
#
#   frame/tools/xrmock/trace-compare.sh <trace A> <trace B>
#
# The setup (everything before the first waitFrame: instance extensions, session binding,
# swapchains) is diffed line by line, without frame numbers. The frames are compared as
# patterns: each compositor cycle (waitFrame to the next waitFrame) becomes one line of its
# calls in order, swapchain image indices and session events left out, and the share of the
# cycles following each pattern is listed for both traces, most frequent first. A game run's
# timing differs from run to run, so frame-by-frame identity is not expected; the patterns show
# what changed (inline pacing: acquire, release and endFrame in one cycle; pipelined: the
# release and endFrame of the set acquired in the cycle before, then the next acquire).
# Exit status 0 when the setups match.
set -e
A=${1:?trace A}; B=${2:?trace B}
norm() { sed 's/^f[0-9]* //; s/ idx=[0-9]*//' "$1" | grep -v '^event\|^focus blip\|^  binding\|^suggestBindings'; }
setup() { norm "$1" | awk '/^waitFrame/ {exit} {print}'; }
patterns() {
  norm "$1" | awk '
    /^waitFrame/ { if (n) print line; line = "wait"; n = 1; next }
    n { sub(/ proj\[flags=[0-9a-f]*/, " proj[", $0); line = line " | " $0 }
    END { if (n) print line }' | sed -n '200,$p' | sort | uniq -c | sort -rn |
    awk '{ c = $1; $1 = ""; t += c; a[NR] = c; s[NR] = $0 } END { for (i = 1; i <= NR && i <= 8; i++) printf "%6.2f%% %7d %s\n", 100 * a[i] / t, a[i], s[i]; printf "  (%d cycles)\n", t }'
}
status=0
if diff <(setup "$A") <(setup "$B") > /dev/null; then
  echo "setup: identical ($(setup "$A" | wc -l) lines)"
else
  echo "setup: DIFFERS"; diff <(setup "$A") <(setup "$B") | head -n 20; status=1
fi
echo "frames of A ($A), most frequent cycle patterns after the first 200:"
patterns "$A"
echo "frames of B ($B):"
patterns "$B"
for t in "$A" "$B"; do
  printf '%s: ERROR lines %d\n' "$t" "$(grep -c 'ERROR' "$t" || true)"
done
exit $status
