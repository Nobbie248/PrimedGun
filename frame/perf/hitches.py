#!/usr/bin/env python3
"""Game frame time histogram and the worst frames: python3 -I frame/perf/hitches.py FRAMELOG [SKIP_S] [SECONDS]

FRAMELOG is the file PRIMEFRAME_FRAMELOG=<file> wrote (one line per presented frame: wall clock us,
frame time us, shaders created, textures uploaded, EFB copies read back to RAM, us waited for them,
disc reads in that frame). SKIP_S seconds after the first frame are left out (state load), then
SECONDS are counted (default all).
"""
import sys

path = sys.argv[1]; skip = float(sys.argv[2]) if len(sys.argv) > 2 else 10; span = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9
fr = []
for line in open(path):
    if line.startswith("#") or len(line.split()) != 7: continue
    t, dt, sh, tex, efb, efb_us, dvd = map(int, line.split())
    fr.append((t, dt, sh, tex, efb, efb_us, dvd))
t0 = fr[0][0]
fr = [f for f in fr if skip * 1e6 <= f[0] - t0 < (skip + span) * 1e6 and f[1] > 0]
dts = [f[1] / 1000 for f in fr]
n = len(dts)
bins = [(0, 14), (14, 18), (18, 22), (22, 26), (26, 34), (34, 50), (50, 100), (100, 1e9)]
print("frames %d over %.0f s, mean %.2f ms (%.1f fps)" % (n, (fr[-1][0] - fr[0][0]) / 1e6, sum(dts) / n, 1000 * n / sum(dts)))
print("| Frame time ms | Frames | Share |")
print("| --- | --- | --- |")
for a, b in bins:
    c = sum(1 for d in dts if a <= d < b)
    print("| %s | %d | %.2f %% |" % ("%g to %g" % (a, b) if b < 1e9 else ">= %g" % a, c, 100 * c / n))
s = sorted(dts)
print("p50 %.2f  p90 %.2f  p99 %.2f  p99.9 %.2f  max %.2f ms" % (tuple(s[int(p * (n - 1))] for p in (0.5, 0.9, 0.99, 0.999)) + (s[-1],)))
tot = lambda i: sum(f[i] for f in fr)
print("totals: shaders created %d, textures uploaded %d, EFB read-backs %d (%.1f ms waited, %.2f per frame), disc reads %d"
      % (tot(2), tot(3), tot(4), tot(5) / 1000, tot(4) / n, tot(6)))
# The frame a counter lands on is the frame whose work caused the wait or the one after it: look at both.
long_ = [i for i, f in enumerate(fr) if f[1] / 1000 >= 25]
def cause(i):
    w = fr[max(0, i - 1):i + 1]
    c = []
    if sum(f[2] for f in w): c.append("shaders %d" % sum(f[2] for f in w))
    if sum(f[5] for f in w) > 2000: c.append("EFB read-back %.1f ms" % (sum(f[5] for f in w) / 1000))
    if sum(f[6] for f in w): c.append("disc reads %d" % sum(f[6] for f in w))
    if sum(f[3] for f in w) > 20: c.append("texture uploads %d" % sum(f[3] for f in w))
    return ", ".join(c) or "none of the counters"
from collections import Counter
print("frames >= 25 ms: %d; with: %s" % (len(long_), dict(Counter(("shaders" in cause(i)) * "S" + ("EFB" in cause(i)) * "E" + ("disc" in cause(i)) * "D" + ("texture" in cause(i)) * "T" or "-" for i in long_))))
print("worst 12:")
for i in sorted(long_ or range(n), key=lambda i: -fr[i][1])[:12]:
    print("  t=%7.2f s  %6.1f ms  %s" % ((fr[i][0] - t0) / 1e6, fr[i][1] / 1000, cause(i)))
