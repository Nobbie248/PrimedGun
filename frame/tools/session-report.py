#!/usr/bin/env python3
"""One table from PrimedFrame sessions' logs (standard library only).

  session-report.py DIR [DIR ...]       session folders: dolphin.log and vrtiming.log
  session-report.py LOG [VRTIMING] ...  named logs: the game's log, then its vrtiming.log if any

The game logs one line a second (Dolphin log, category OpenXR, INFO):

  ... XRSTATS key=value key=value ...

The keys are read as they come (numbers and words), so a new field needs no change here. Every
number key becomes a row with its average, lowest and highest second (rows whose name ends in
_max or _max_ms show only the highest, _min the lowest); a word key lists the values seen.
Counter-like keys (submitted, late, missed, waits ...) are best read next to the seconds count.
A slow second is one where fewer than 90 % of the median number of eye sets were submitted.
A compositor hitch is a SteamVR client frame interval above 100 ms in vrtiming.log (written by
vrtiming.py, which documents its format). Several sessions are columns of one table.
"""
import os
import re
import sys

HITCH_MS = 100.0
STATS = re.compile(r"XRSTATS\s+(?P<kv>(?:\S+=\S*\s*)+)")
STALL = "PrimedGunFrameStall.txt"


def num(s):
    try:
        return float(s)
    except ValueError:
        return None


def parse_game_log(path):
    r = {"seconds": 0, "num": {}, "text": {}, "order": [], "submitted": [], "errors": 0, "fatal": 0}
    with open(path, errors="replace") as f:
        for line in f:
            m = STATS.search(line)
            if not m:
                if re.search(r"\b[EC]\[|\bERROR\b", line) and "xrAcquireSwapchainImage" not in line:
                    r["errors"] += 1
                continue
            r["seconds"] += 1
            for kv in m.group("kv").split():
                k, _, v = kv.partition("=")
                if k not in r["order"]:
                    r["order"].append(k)
                x = num(v)
                if x is None:
                    # 1728x1728 and the like: a word
                    r["text"].setdefault(k, {}).setdefault(v, 0)
                    r["text"][k][v] += 1
                else:
                    r["num"].setdefault(k, []).append(x)
                    if k == "submitted":
                        r["submitted"].append(x)
    return r


def parse_vrtiming(path):
    r = {"seconds": 0, "frames": 0, "reused": 0, "mis": 0, "dropped": 0, "rp_cpu": 0, "rp_gpu": 0,
         "comp_gpu": [], "interval_max": 0.0, "interval_avg": [], "hitch": 0, "hitch_notes": []}
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("#"):
                continue
            g = re.match(r"(\d\d:\d\d:\d\d)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(-?[\d.]+)\s+"
                         r"([\d.]+)/([\d.]+)\s+([\d.]+)/([\d.]+)\s+([\d.]+)", line)
            if not g:
                continue
            v = [float(x) for x in g.groups()[1:]]
            r["seconds"] += 1
            r["frames"] += int(v[0]); r["reused"] += int(v[1]); r["mis"] += int(v[2]); r["dropped"] += int(v[3])
            r["rp_cpu"] += int(v[4]); r["rp_gpu"] += int(v[5])
            r["comp_gpu"].append(v[8]); r["interval_avg"].append(v[9])
            r["interval_max"] = max(r["interval_max"], v[10])
            if v[10] > HITCH_MS:
                r["hitch"] += 1
                r["hitch_notes"].append("%s client interval max %.0f ms" % (g.group(1), v[10]))
    return r


def mean(xs):
    return sum(xs) / len(xs) if xs else None


def median(xs):
    xs = sorted(xs)
    return xs[len(xs) // 2] if xs else None


def fmt(v, spec="%.2f"):
    if v is None:
        return "-"
    return "%d" % v if float(v).is_integer() and abs(v) >= 100 else spec % v


def pct(a, b):
    return "-" if not b else "%.1f%%" % (100.0 * a / b)


def build(sessions):
    """sessions: list of (label, game dict or None, vr dict or None, stall bool). Returns rows [(label, [cells])]."""
    rows = []

    def add(label, fn):
        cells = []
        for _, g, v, st in sessions:
            try:
                cells.append(fn(g, v, st))
            except Exception:
                cells.append("-")
        rows.append((label, cells))

    add("seconds with XRSTATS", lambda g, v, st: str(g["seconds"]) if g else "no log")
    add("slow seconds (< 90 % of median sets)", lambda g, v, st: str(sum(1 for x in g["submitted"] if x < 0.9 * median(g["submitted"]))) if g and g["submitted"] else "-")
    add("stall file", lambda g, v, st: "YES" if st else "none")
    add("error lines (other than acquire)", lambda g, v, st: str(g["errors"]) if g else "-")
    keys = []
    for _, g, _, _ in sessions:
        if g:
            keys += [k for k in g["order"] if k not in keys]
    for k in keys:
        def cell(g, v, st, k=k):
            if k in g["num"]:
                xs = g["num"][k]
                if k.endswith("_max") or k.endswith("_max_ms"):
                    return fmt(max(xs))
                if k.endswith("_min") or k.endswith("_min_ms"):
                    return fmt(min(xs))
                return "%s (%s..%s)" % (fmt(mean(xs)), fmt(min(xs)), fmt(max(xs)))
            if k in g["text"]:
                return ",".join(sorted(g["text"][k], key=lambda t: -g["text"][k][t])[:3])
            return "-"
        add(k, cell)
    add("compositor seconds logged", lambda g, v, st: str(v["seconds"]) if v else "no vrtiming.log")
    add("compositor frames", lambda g, v, st: str(v["frames"]) if v else "-")
    add("  app frames reused", lambda g, v, st: "%d (%s)" % (v["reused"], pct(v["reused"], v["frames"])) if v else "-")
    add("  mispresented", lambda g, v, st: str(v["mis"]) if v else "-")
    add("  dropped", lambda g, v, st: str(v["dropped"]) if v else "-")
    add("  reprojected (cpu / gpu flagged)", lambda g, v, st: "%d / %d" % (v["rp_cpu"], v["rp_gpu"]) if v else "-")
    add("compositor GPU ms avg", lambda g, v, st: fmt(mean([x for x in v["comp_gpu"] if x >= 0]), "%.1f") if v else "-")
    add("client frame interval: avg / max ms", lambda g, v, st: "%s / %s" % (fmt(mean(v["interval_avg"])), fmt(v["interval_max"], "%.1f")) if v else "-")
    add("compositor hitches > %d ms" % HITCH_MS, lambda g, v, st: str(v["hitch"]) if v else "-")
    return rows


def game_log_in(d):
    """dolphin.log of a wrapper session folder, else the *.log with the most XRSTATS lines."""
    cand = [os.path.join(d, f) for f in os.listdir(d) if f.endswith(".log") and not f.startswith(("vrtiming", "launch"))]

    def lines(path):
        with open(path, errors="replace") as f:
            return sum(1 for line in f if "XRSTATS" in line)
    return max(cand, key=lambda p: (lines(p), os.path.basename(p) == "dolphin.log")) if cand else None


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0 if argv else 1
    sessions = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if os.path.isdir(a):
            lp = game_log_in(a)
            vp = os.path.join(a, "vrtiming.log")
            sessions.append((os.path.basename(os.path.realpath(a)), lp, vp if os.path.exists(vp) else None,
                             os.path.exists(os.path.join(a, STALL))))
        else:
            vp = None
            if i + 1 < len(argv) and "vrt" in os.path.basename(argv[i + 1]):
                vp = argv[i + 1]
                i += 1
            sessions.append((os.path.basename(a), a, vp, os.path.exists(os.path.join(os.path.dirname(a) or ".", STALL))))
        i += 1
    parsed = [(label, parse_game_log(lp) if lp else None, parse_vrtiming(vp) if vp else None, st) for label, lp, vp, st in sessions]
    rows = build(parsed)
    w0 = max(len(r[0]) for r in rows)
    ws = [max(len(parsed[k][0]), max(len(r[1][k]) for r in rows)) for k in range(len(parsed))]
    print("%-*s  %s" % (w0, "", "  ".join("%-*s" % (ws[k], parsed[k][0]) for k in range(len(parsed)))))
    for label, cells in rows:
        print("%-*s  %s" % (w0, label, "  ".join("%-*s" % (ws[k], cells[k]) for k in range(len(cells)))))
    notes = ["%s: %s" % (label, "; ".join(v["hitch_notes"][:8]) + (" ..." if len(v["hitch_notes"]) > 8 else ""))
             for label, g, v, st in parsed if v and v["hitch_notes"]]
    if notes:
        print()
        print("\n".join(notes))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
