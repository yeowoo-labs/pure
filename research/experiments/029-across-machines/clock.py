#!/usr/bin/env python3
"""Whether this machine's wall clock jumps (research 029): the wall clock
(time.time) against the monotonic one, read every 20 ms for SECONDS; each
step where the wall clock moved more than 20 ms apart from the monotonic
one, how far, and when (also on the system's monotonic clock). A JSON line
each, then one with the totals.

clock.py SECONDS"""
import json
import sys
import time

seconds = float(sys.argv[1])
start_wall, start_mono = time.time(), time.monotonic()
wall, mono, jumps = start_wall, start_mono, 0
while mono - start_mono < seconds:
    time.sleep(0.02)
    now_wall, now_mono = time.time(), time.monotonic()
    apart = (now_wall - wall) - (now_mono - mono)
    if abs(apart) > 0.02:
        jumps += 1
        print(json.dumps({'at_s': round(now_mono - start_mono, 2), 'monotonic': round(now_mono, 2),
                          'wall_moved_ms': round((now_wall - wall) * 1000, 1),
                          'monotonic_moved_ms': round((now_mono - mono) * 1000, 1)}), flush=True)
    wall, mono = now_wall, now_mono
drift = (time.time() - start_wall) - (time.monotonic() - start_mono)
print(json.dumps({'seconds': seconds, 'jumps': jumps, 'wall_minus_monotonic_ms': round(drift * 1000, 1)}))
