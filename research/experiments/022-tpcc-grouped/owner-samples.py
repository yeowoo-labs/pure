#!/usr/bin/env python3
"""Where pure-serve's owner thread is: its stack sampled with gdb (a poor man's profiler; perf is not on this Pi).

    owner-samples.py SAMPLES OUT.txt     sample, then sort
    owner-samples.py --sort STACKS.txt   sort stacks sampled before

Waits for a process with a pure-owner thread (pure-serve, whatever its file
is named), then attaches gdb SAMPLES times, a little apart, and writes the
owner's stack each time as it goes (while the process lasts). Sorts the
samples by what the owner was doing (docs/research/106), and counts the
allocator's frames and what called them.
"""
import collections
import re
import subprocess
import sys
import time
from pathlib import Path


def owner():
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():
            continue
        try:
            for task in (proc / 'task').iterdir():
                if (task / 'comm').read_text().strip() == 'pure-owner':
                    return int(proc.name), int(task.name)
        except OSError:
            continue
    return None


def sample(samples, out):
    found = None
    while not found:
        found = owner()
        time.sleep(0.2)
    pid, tid = found
    time.sleep(10)  # past loading, into the run
    with out.open('w') as written:
        for _ in range(samples):
            if not Path(f'/proc/{pid}').exists():
                break
            text = subprocess.run(['gdb', '-batch', '-nx', '-p', str(pid), '-ex', 'set pagination off',
                                   '-ex', 'thread apply all bt 40'], capture_output=True, text=True).stdout
            # The owner's section: from its "Thread ... (LWP tid)" header to the next thread's.
            match = re.search(rf'Thread [^\n]*\(LWP {tid}\)[^\n]*\n(.*?)(?=\nThread |\Z)', text, re.S)
            if match:
                frames = re.findall(r'^#\d+\s+(?:0x[0-9a-f]+ in )?(\S+)', match.group(1), re.M)
                written.write(' < '.join(frames) + '\n')
                written.flush()
            time.sleep(0.3)


def kind(frames):
    names = set(frames)
    if 'apply_effect' in names:
        if names & {'own', 'new_node', 'retire'}:
            return 'applying: copying nodes a snapshot shares (own, new_node, retire)'
        if names & {'decode_item', 'decode_cell'}:
            return 'applying: reading items'
        if 'pure_list_seek' in names or names & {'leaf_of', 'child_at', 'child', 'cell_at'}:
            return 'applying: finding items (pure_list_seek, descending)'
        if names & {'pure_list_insert', 'pure_list_set', 'pure_list_remove'}:
            return 'applying: changing items'
        return 'applying: the interpreter (run, dispatch)'
    if 'publish' in names:
        return 'publishing a snapshot, letting the old go'
    if 'save_burst' in names:
        return 'saving'
    if 'hand_back' in names:
        return 'handing answers back'
    if names & {'pthread_cond_wait', 'pthread_cond_timedwait'}:
        return 'waiting for requests'
    return 'other'


def sort(path):
    stacks = [line.split(' < ') for line in path.read_text().splitlines() if line]
    kinds, allocations = collections.Counter(), collections.Counter()
    for frames in stacks:
        kinds[kind(frames)] += 1
        for at, name in enumerate(frames):
            if name in ('malloc', 'calloc', 'realloc', 'free'):
                allocations[f'{name} < {frames[at + 1] if at + 1 < len(frames) else "?"}'] += 1
                break
    busy = len(stacks) - kinds['waiting for requests']
    print(f'{len(stacks)} samples of the owner, {busy} busy')
    for name, n in kinds.most_common():
        print(f'  {n:>3} {n / len(stacks):>4.0%}  {name}')
    print('the allocator (innermost call, and its caller):')
    for name, n in allocations.most_common():
        print(f'  {n:>3}  {name}')


if sys.argv[1] == '--sort':
    sort(Path(sys.argv[2]))
else:
    sample(int(sys.argv[1]), Path(sys.argv[2]))
    sort(Path(sys.argv[2]))
