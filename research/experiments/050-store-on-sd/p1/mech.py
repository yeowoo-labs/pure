#!/usr/bin/env python3
"""mech.py: the owner's store calls a save (strace -c, ft-*), and its faults, user and kernel time a burst and an event
(profile, fp-*), for z0, z6, z7."""
import json, re, sys
from pathlib import Path
S = Path(sys.argv[1])
STORE = {'newfstatat', 'pread64', 'pwrite64', 'fdatasync', 'fallocate', 'openat', 'mmap', 'munmap', 'fsync', 'close', 'fstat',
         'fcntl', 'renameat', 'getpid', 'unlinkat', 'ftruncate', 'fstatat'}
for v in ('z0', 'z6', 'z7'):
    calls = {}
    for line in open(S / f'ft-{v}.strace'):
        m = re.match(r'\s*[\d.]+\s+[\d.]+\s+\d+\s+(\d+)\s+(?:\d+\s+)?(\w+)$', line)
        if m: calls[m.group(2)] = int(m.group(1))
    saves = calls.get('fdatasync', 1)
    store = {k: n for k, n in calls.items() if k in STORE}
    p = json.loads(open(S / f'fp-{v}.profile.json').read())
    events = p['events']; bursts = p['bursts']
    print('%s: store calls a save %.2f (%s), mprotect %.2f; faults a burst %.2f; per event: instructions %.0f, user %.2f us, kernel %.2f us; tps %d share %s' % (
        v, sum(store.values()) / saves, ', '.join('%s %.2f' % (k, n / saves) for k, n in sorted(store.items(), key=lambda x: -x[1])),
        calls.get('mprotect', 0) / saves, p['faults'] / bursts, p['instructions'] / events, p['utime'] * 1e4 / events, p['stime'] * 1e4 / events,
        p['tps'], p['shares'].get('pure-owner')))
