#!/usr/bin/env python3
"""profile.py OUT VERSION: one TPC-C run (W=1, 64 terminals, 20 s) on a cached pure-serve with the
owner thread's user instructions sampled during the run step; writes OUT-VERSION.jsonl (run.py's
lines) and OUT-VERSION.profile.json (instructions, samples by function, owner faults and times)."""
import bisect, collections, json, os, subprocess, sys, time
from pathlib import Path
HERE = Path(__file__).resolve().parent
out, version = sys.argv[1], sys.argv[2]
serve = Path.home() / '.cache' / 'pure-040' / version / 'pure-serve'
period = 100003
lines = []
runner = subprocess.Popen(['python3', str(HERE.parents[3] / 'benchmarks' / 'tpcc' / 'run.py'),
                           '--serve', str(serve), '--warehouses', '1', '--check', '0', '--terminals', '64',
                           '--sqlite-threads', '', '--sqlite-group', '', '--seconds', '20'],
                          stdout=subprocess.PIPE, text=True)
def owner():
    pid = int(subprocess.run(['pgrep', '-x', 'pure-serve'], capture_output=True, text=True).stdout.split()[0])
    for t in Path(f'/proc/{pid}/task').iterdir():
        if (t / 'comm').read_text().strip() == 'pure-owner':
            return pid, int(t.name)
def stat(pid, tid):
    f = Path(f'/proc/{pid}/task/{tid}/stat').read_text().rsplit(')', 1)[1].split()
    return int(f[7]), int(f[11]), int(f[12])
sampler = None
for line in runner.stdout:
    lines.append(line)
    step = json.loads(line)['step']
    if step == 'load':
        pid, tid = owner()
        maps = Path(f'/proc/{pid}/maps').read_text()
        before = stat(pid, tid)
        sampler = subprocess.Popen([str(HERE / 'osample'), str(tid), str(period)], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        sampler.stderr.readline()
    elif step == 'run' and sampler:
        after = stat(pid, tid)
        sampler.stdin.close()
        text = sampler.stdout.read()
        sampler.wait()
        sampler = None
runner.wait()
Path(f'{out}-{version}.jsonl').write_text(''.join(lines))
samples, instructions = [], 0
for row in text.splitlines():
    if row.startswith('instructions'): instructions = int(row.split()[1])
    elif not row.startswith('lost'): samples.append(int(row, 16))
listed = subprocess.run(['nm', '-n', '--defined-only', str(serve)], capture_output=True, text=True, check=True).stdout
starts, names = [], []
for row in listed.splitlines():
    parts = row.split()
    if len(parts) == 3 and parts[1] in 'tTwW':
        starts.append(int(parts[0], 16)); names.append(parts[2])
ranges = []
for row in maps.splitlines():
    parts = row.split()
    if len(parts) >= 6 and 'x' in parts[1]:
        low, high = (int(x, 16) for x in parts[0].split('-'))
        ranges.append((low, high, int(parts[2], 16), parts[5]))
counts = collections.Counter()
for ip in samples:
    for low, high, offset, path in ranges:
        if low <= ip < high:
            if path == str(serve):
                i = bisect.bisect_right(starts, ip - low + offset) - 1
                counts[names[i] if i >= 0 else '?'] += 1
            else:
                counts[Path(path).name] += 1
            break
    else:
        counts['[other]'] += 1
end = [json.loads(l) for l in lines if json.loads(l)['step'] == 'end'][0]
run = [json.loads(l) for l in lines if json.loads(l)['step'] == 'run'][0]
Path(f'{out}-{version}.profile.json').write_text(json.dumps({
    'version': version, 'tps': run['transactions_per_second'], 'shares': run['core_shares'],
    'bursts': end['pure_counts']['bursts'], 'events': end['pure_counts']['burst_events'],
    'instructions': instructions, 'samples': len(samples), 'period': period,
    'faults': after[0] - before[0], 'utime': after[1] - before[1], 'stime': after[2] - before[2],
    'functions': counts.most_common()}))
print(version, run['transactions_per_second'], run['core_shares'].get('pure-owner'), 'instructions', instructions,
      'bursts', end['pure_counts']['bursts'], 'faults', after[0] - before[0])
