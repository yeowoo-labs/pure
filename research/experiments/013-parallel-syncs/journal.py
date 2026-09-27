#!/usr/bin/env python3
"""The tree store's commits under each store variant of variants.py, without
HTTP: latency percentiles of research/experiments/010-one-sync-commit
/commits.c, and how many ext4 journal commits (jbd2 transactions, read from
/proc/fs/jbd2) each run caused. Rounds rotate the variants' order."""
import argparse
import json
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

import variants

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
COMMITS = HERE.parent / '010-one-sync-commit' / 'commits.c'


def transactions(device):
    return int(re.match(r'(\d+) transactions', (Path('/proc/fs/jbd2') / device / 'info').read_text()).group(1))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--directory', type=Path, default=Path('/var/tmp'), help='on the device to measure')
    parser.add_argument('--journal', default='mmcblk0p2-8', help='its /proc/fs/jbd2 entry')
    parser.add_argument('--rounds', type=int, default=4)
    parser.add_argument('--commits', type=int, default=4000)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        binaries = {}
        for number, (name, patches) in enumerate(variants.PATCHES.items()):
            source = (ROOT / 'store' / 'store-tree.c').read_text()
            for old, new in patches:
                assert old in source, old
                source = source.replace(old, new)
            (work / f'store-{number}.c').write_text(source)
            binaries[name] = work / f'commits-{number}'
            subprocess.run(['cc', '-O2', '-std=c17', f'-I{ROOT / "core"}', '-o', str(binaries[name]), str(COMMITS),
                            str(work / f'store-{number}.c'), '-lpthread'], check=True)
        names, runs = list(binaries), {name: [] for name in binaries}
        for round_ in range(args.rounds):
            for name in names[round_ % len(names):] + names[:round_ % len(names)]:
                store = Path(tempfile.mkdtemp(dir=args.directory, prefix='pure-journal-'))
                shutil.rmtree(store)
                try:
                    before = transactions(args.journal)
                    out = subprocess.run([str(binaries[name]), str(store), str(args.commits)], capture_output=True,
                                         text=True, check=True)
                    measured = {**json.loads(out.stdout), 'journal_commits': transactions(args.journal) - before}
                finally:
                    shutil.rmtree(store, ignore_errors=True)
                runs[name].append(measured)
                print(json.dumps({'round': round_, 'store': name, **measured}), flush=True)
        for name in names:
            print(json.dumps({'store': name, 'commits': args.commits, **{key: statistics.median(r[key] for r in runs[name])
                              for key in ('commits_per_second', 'p50_ms', 'p90_ms', 'p99_ms', 'journal_commits')}}))


if __name__ == '__main__':
    sys.exit(main())
