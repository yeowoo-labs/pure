#!/usr/bin/env python3
"""bench-seek before and after prefetch.patch, taking turns five times, lists of 100,000 and 400,000 items:
the median ns of each way and the spread (slowest over fastest), and the load average when it began and ended.

    micro.py DIRECTORY   (holding bench-before and bench-after)"""
import os
import statistics
import subprocess
import sys
from pathlib import Path

here = Path(sys.argv[1])
ways = ['seek', 'seek+3 reads', 'seek+3 reads+3 changes', 'same, a share each 30']
print('load average at the start:', os.getloadavg())
for items in (100000, 400000):
    runs = {'before': [], 'after': []}
    for _ in range(5):
        for version in runs:
            out = subprocess.run([here / f'bench-{version}', str(items), '1500000'], capture_output=True, text=True,
                                 check=True).stdout.splitlines()[:4]
            runs[version].append([float(line.split()[-2]) for line in out])
    for version, measured in runs.items():
        medians = [statistics.median(m[i] for m in measured) for i in range(4)]
        spreads = [max(m[i] for m in measured) / min(m[i] for m in measured) for i in range(4)]
        print(f'{items:>7} {version:<6} ' + '  '.join(f'{w}: {m:.1f} ns (x{s:.2f})' for w, m, s in zip(ways, medians, spreads)))
print('load average at the end:', os.getloadavg())
