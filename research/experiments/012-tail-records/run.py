#!/usr/bin/env python3
"""Commit cost by where the head goes (see docs/research/081-tail-records.md).

Runs probe.c (a small commit's sync with its head in a slot at the start, or
just before its bytes) for 2 KiB and 8 KiB commits, and ../010-one-sync-commit
/commits.c (tree store commit latency) built against the store of --before
(heads in slots) and the current one (heads as records before the bytes),
alternating their order per round so device drift reaches both alike. Prints
one JSON document.
"""
import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
PURE = ROOT / 'prototypes' / 'connections'
COMMITS = HERE.parent / '010-one-sync-commit' / 'commits.c'


def build(output, sources):
    subprocess.run(['cc', '-O2', '-std=c17', f'-I{PURE}', '-o', str(output)] + [str(s) for s in sources] + ['-lpthread'],
                   check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--directory', type=Path, default=Path('/var/tmp'), help='on the device to measure')
    parser.add_argument('--before', default='2ef0d40', help='a revision with heads in slots')
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--commits', type=int, default=400)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        before = work / 'store-tree-before.c'
        before.write_text(subprocess.run(['git', '-C', str(ROOT), 'show', f'{args.before}:prototypes/connections/store-tree.c'],
                                         capture_output=True, text=True, check=True).stdout)
        build(work / 'probe', [HERE / 'probe.c'])
        builds = {'heads in slots': work / 'commits-before', 'heads as records': work / 'commits-after'}
        build(builds['heads in slots'], [COMMITS, before])
        build(builds['heads as records'], [COMMITS, PURE / 'store-tree.c'])
        results = {'rounds': []}
        for round_ in range(args.rounds):
            measured = {'probe': [], 'commits': {}}
            for size in (2048, 8192):
                probe = subprocess.run([str(work / 'probe'), str(args.directory), str(size)], capture_output=True,
                                       text=True, check=True)
                measured['probe'] += [json.loads(line) for line in probe.stdout.splitlines()]
            names = list(builds) if round_ % 2 == 0 else list(builds)[::-1]
            for name in names:
                store = Path(tempfile.mkdtemp(dir=args.directory, prefix='pure-commit-cost-'))
                shutil.rmtree(store)
                try:
                    run = subprocess.run([str(builds[name]), str(store), str(args.commits)], capture_output=True, text=True,
                                         check=True)
                    measured['commits'][name] = json.loads(run.stdout)
                finally:
                    shutil.rmtree(store, ignore_errors=True)
            results['rounds'].append(measured)
            print(json.dumps(measured), file=sys.stderr, flush=True)
        print(json.dumps(results, indent=1))


if __name__ == '__main__':
    sys.exit(main())
