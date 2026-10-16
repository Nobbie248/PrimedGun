#!/usr/bin/env python3
"""Keeps a few seconds of Turnip's GPU trace: python3 -I frame/perf/gputrace.py FIFO OUT START_S SECONDS

Run the game with MESA_GPU_TRACES=print MESA_GPU_TRACEFILE=FIFO (a named pipe); this reads the pipe
the whole run (so the driver never blocks) and writes to OUT only the lines between START_S and
START_S + SECONDS after the first line arrived (the heavy room is reached ~105 s after the s02 load).
"""
import sys, time
fifo, out, start, span = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
t0 = None
with open(out, "wb") as o:
    # The driver may open the trace file more than once (one Vulkan device per probe): reopen at EOF
    # until the game is gone (the open blocks until a writer appears; mockrun ends the game).
    while True:
        with open(fifo, "rb", buffering=1 << 20) as f:
            for line in f:
                now = time.monotonic()
                if t0 is None: t0 = now
                if start <= now - t0 < start + span: o.write(line)
        if t0 is not None and time.monotonic() - t0 > start + span: break
