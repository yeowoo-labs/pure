#!/usr/bin/env python3
"""Where pure-serve's owner thread is: its stack sampled with gdb (a poor man's profiler; perf is not on this Pi).

    owner-samples.py SAMPLES OUT.txt

Waits for a pure-serve with a pure-owner thread, then attaches gdb SAMPLES
times, a little apart, and keeps the owner's stack each time. Prints how
many samples had each serve.c phase on the stack, and the innermost frames.
"""
import collections
import re
import subprocess
import sys
import time
from pathlib import Path

samples, out = int(sys.argv[1]), Path(sys.argv[2])


def owner():
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            if (proc / 'comm').read_text().strip() != 'pure-serve':
                continue
            for task in (proc / 'task').iterdir():
                if (task / 'comm').read_text().strip() == 'pure-owner':
                    return int(proc.name), int(task.name)
        except OSError:
            continue
    return None


found = None
while not found:
    found = owner()
    time.sleep(0.2)
pid, tid = found
time.sleep(10)  # past loading, into the run
stacks = []
for _ in range(samples):
    text = subprocess.run(['gdb', '-batch', '-nx', '-p', str(pid), '-ex', 'set pagination off',
                           '-ex', 'thread apply all bt 40'], capture_output=True, text=True).stdout
    # The owner's section: from its "Thread ... (LWP tid)" header to the next thread's.
    match = re.search(rf'Thread [^\n]*\(LWP {tid}\)[^\n]*\n(.*?)(?=\nThread |\Z)', text, re.S)
    if match:
        frames = re.findall(r'^#\d+\s+(?:0x[0-9a-f]+ in )?(\S+)', match.group(1), re.M)
        stacks.append(frames)
    time.sleep(0.3)
out.write_text('\n'.join(' < '.join(s) for s in stacks) + '\n')
phases = collections.Counter()
inner = collections.Counter()
for frames in stacks:
    names = set(frames)
    phase = ('waiting' if names & {'pthread_cond_wait', 'pthread_cond_timedwait', '__futex_abstimed_wait_common'}
             and not names & {'execute'} else
             'publish' if 'publish' in names else
             'checkpoint_round' if 'checkpoint_round' in names else
             'feed_outlets' if 'feed_outlets' in names else
             'save' if 'save_burst' in names else
             'hand_back' if 'hand_back' in names else
             'apply' if 'apply_effect' in names else
             'other')
    phases[phase] += 1
    inner[frames[0] if frames else '?'] += 1
print(f'{len(stacks)} samples of the owner (pid {pid}, thread {tid})')
for phase, n in phases.most_common():
    print(f'  {phase:<18} {n:>4}  {n / max(len(stacks), 1):.0%}')
print('innermost frames:')
for name, n in inner.most_common(15):
    print(f'  {name:<40} {n:>4}')
