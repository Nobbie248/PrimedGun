#!/usr/bin/env python3
"""One table row per mockrun run dir: python3 -I frame/perf/summary.py RUN_DIR... [--seconds N] [--skip S]

Reads dolphin.log (XRSTATS lines; the first N seconds after S skipped, default all after 10),
clock.log (mockrun --clocklog) and the VR CPU cull report lines. Prints a Markdown row:
run, eye, game fps avg (min), layers/s, new sets/s, GPU ms per set avg / p99 / max (per-second
averages), GPU busy %, late (app), GPU MHz avg (min), GPU max temp, draws culled per frame avg.
"""
import os, re, statistics, sys

def pct(v, p):
    v = sorted(v); return v[min(len(v) - 1, int(round(p / 100 * (len(v) - 1))))]

def rows_of(run, seconds, skip):
    xs, stamps, cull = [], [], []
    for line in open(os.path.join(run, "dolphin.log"), errors="replace"):
        if "XRSTATS" in line:
            xs.append({k: v for k, v in re.findall(r"(\w+)=(\S+)", line)}); stamps.append(line.split()[0])
        elif "VR CPU cull" in line:
            m = re.search(r"removed (\d+) draw", line)
            if m: cull.append((line.split()[0], int(m.group(1))))
    end = skip + seconds if seconds else None
    return xs[skip:end], stamps[skip:end], cull

def sec(t):
    p = t.split(":"); return int(p[0]) * 60 + int(p[1])

def window(stamps):
    a, b = sec(stamps[0]), sec(stamps[-1])
    return lambda t: (a <= sec(t) <= b) if a <= b else (sec(t) >= a or sec(t) <= b)

def main():
    args, seconds, skip = [], 0, 10
    it = iter(sys.argv[1:])
    for a in it:
        if a == "--seconds": seconds = int(next(it))
        elif a == "--skip": skip = int(next(it))
        else: args.append(a)
    print("| Run | Eye | Game fps avg (min) | Layers/s | New sets/s | GPU ms/set avg / p99 / max | GPU busy | Late (app) | GPU MHz avg (min) | GPU C max | Culled draws/frame |")
    print("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for run in args:
        xs, st, cull = rows_of(run, seconds, skip)
        if not xs:
            print("| %s | no XRSTATS |" % os.path.basename(run)); continue
        f = lambda k: [float(x.get(k, 0)) for x in xs]
        gpu, sub = f("gpu_ms"), f("submitted")
        busy = statistics.mean(g * s / 1000 for g, s in zip(gpu, sub))
        inw = window(st)
        clk = ""
        cp = os.path.join(run, "clock.log")
        if os.path.exists(cp):
            cl = [dict(re.findall(r"(\w+)=(\S+)", l)) for l in open(cp) if l[:2].isdigit() and inw(l.split()[0])]
            if cl:
                mhz = [float(c["gpu_mhz"]) for c in cl]
                clk = "%.0f (%.0f) | %.1f" % (statistics.mean(mhz), min(mhz), max(float(c["gpu_c"]) for c in cl))
        clk = clk or "- | -"
        cu = [n for t, n in cull if inw(t)]
        print("| %s | %s | %.1f (%.1f) | %.1f | %.1f | %.2f / %.2f / %.2f | %.0f %% | %d | %s | %s |" % (
            os.path.basename(run), xs[0].get("eye", "?"), statistics.mean(f("game_fps")), min(f("game_fps")),
            statistics.mean(f("layers")), statistics.mean(f("shown")), statistics.mean(gpu), pct(gpu, 99), max(gpu),
            100 * busy, sum(f("late")), clk, "%.0f" % statistics.mean(cu) if cu else "-"))

main()
