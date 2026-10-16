#!/usr/bin/env python3
"""Per-N-second timeline of one mockrun run: python3 -I frame/perf/timeline.py RUN_DIR [N]

Joins XRSTATS (game fps, GPU ms per set), clock.log (GPU MHz, its thermal cap, temperatures) and the
VR CPU cull report (draw calls culled per frame: a proxy for how much of the room is in view) on
Dolphin's log clock. Used for the soak question: scene change or GPU clock/thermal throttling.
"""
import re, statistics, sys

run, n = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 10
sec = lambda t: int(t.split(":")[0]) * 60 + int(t.split(":")[1])
xs, cull, clk = [], {}, {}
for line in open(run + "/dolphin.log", errors="replace"):
    if "XRSTATS" in line:
        xs.append((sec(line.split()[0]), dict(re.findall(r"(\w+)=(\S+)", line))))
    elif "VR CPU cull" in line:
        m = re.search(r"removed (\d+) draw", line)
        if m: cull.setdefault(sec(line.split()[0]), int(m.group(1)))
try:
    for line in open(run + "/clock.log"):
        if line[:2].isdigit(): clk[sec(line.split()[0])] = dict(re.findall(r"(\w+)=(\S+)", line))
except OSError:
    pass
print("t_s  fps   gpu_ms  busy%  gpu_mhz cap_mhz gpu_C  cpu_C  culled")
for i in range(0, len(xs), n):
    chunk = xs[i:i + n]
    secs = [s for s, _ in chunk]
    g = statistics.mean(float(x["gpu_ms"]) for _, x in chunk)
    fps = statistics.mean(float(x["game_fps"]) for _, x in chunk)
    busy = statistics.mean(float(x["gpu_ms"]) * float(x["submitted"]) / 10 for _, x in chunk)
    c = [clk[s] for s in secs if s in clk]
    cu = [cull[s] for s in secs if s in cull]
    m = lambda k, fn=statistics.mean: ("%.0f" % fn(float(x[k]) for x in c)) if c else "-"
    print("%4d %5.1f %7.2f %5.0f  %7s %7s %5s %6s %7s" % (i, fps, g, busy, m("gpu_mhz"), m("gpu_cap_mhz", min),
          m("gpu_c", max), m("cpu_c", max), "%.0f" % statistics.mean(cu) if cu else "-"))
