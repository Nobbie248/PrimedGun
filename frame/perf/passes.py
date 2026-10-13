#!/usr/bin/env python3
"""GPU time per render pass kind from a Turnip trace (MESA_GPU_TRACES=print; frame/perf/gputrace.py):
python3 -I frame/perf/passes.py TRACE

Groups render passes by size and attachments (Dolphin's EFB at the internal resolution, EFB copies,
the eye images, ...) and gives each group's GPU time per game frame (frames = eye image passes / 2),
with the draw count, GMEM or direct rendering, and the time in clears and GMEM loads/stores.
"""
import re, sys, collections
groups = collections.defaultdict(lambda: [0, 0.0, 0, collections.Counter(), 0.0, 0.0])
cur = None; total = 0.0; cmdbufs = 0; sub = None
for line in open(sys.argv[1], errors="replace"):
    m = re.match(r"(\d{16})\s+[+-]\d+: (\w+) ?(.*)", line)
    if not m:
        e = re.match(r"ELAPSED: (\d+) ns", line)
        if e: total += int(e.group(1)) / 1e6; cmdbufs += 1
        continue
    ts, ev, args = int(m.group(1)), m.group(2), dict(re.findall(r"(\w+)=([^,]*),", m.group(3)))
    if ev == "start_render_pass":
        cur = (ts, "%sx%s att%s depth=%s" % (args.get("width"), args.get("height"), args.get("attachment_count"), args.get("hasDepth")))
        clr = gm = 0.0
    elif ev in ("start_generic_clear", "start_gmem_load", "start_gmem_store"):
        sub = ts
    elif ev in ("end_generic_clear",) and sub is not None and cur:
        clr += (ts - sub) / 1e6; sub = None
    elif ev in ("end_gmem_load", "end_gmem_store") and sub is not None and cur:
        gm += (ts - sub) / 1e6; sub = None
    elif ev == "end_render_pass" and cur:
        g = groups[cur[1]]
        g[0] += 1; g[1] += (ts - cur[0]) / 1e6; g[2] += int(args.get("drawCount", 0) or 0)
        g[3]["gmem" if args.get("tiledRender") == "true" else "sysmem"] += 1; g[4] += clr; g[5] += gm
        cur = None
eyes = sum(g[0] for k, g in groups.items() if k.startswith("1728x1728") or k.startswith("2160x2160"))
frames = max(1, eyes // 2)
print("command buffers %d, GPU time in them %.1f ms, game frames %d, %.2f ms per frame (render passes below: %.2f)"
      % (cmdbufs, total, frames, total / frames, sum(g[1] for g in groups.values()) / frames))
print("| Pass (size, attachments) | Per frame | GPU ms per frame | Draws per frame | GMEM / direct | Clears ms | GMEM load+store ms |")
print("| --- | --- | --- | --- | --- | --- | --- |")
for k, g in sorted(groups.items(), key=lambda kv: -kv[1][1]):
    if g[1] / frames < 0.02: continue
    print("| %s | %.1f | %.2f | %.0f | %d / %d | %.2f | %.2f |" % (k, g[0] / frames, g[1] / frames, g[2] / frames,
          g[3]["gmem"], g[3]["sysmem"], g[4] / frames, g[5] / frames))
