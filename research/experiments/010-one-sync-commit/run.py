#!/usr/bin/env python3
"""Commit cost on the store's device (see docs/research/076-one-sync-commit.md).

Runs probe.c (the ways of making a small commit durable) and commits.c (tree
store commit latency) built against the two-sync store of --before and the
current one, alternating their order per round so device drift reaches both
alike. Prints one JSON document.
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


def build(output, sources):
    subprocess.run(['cc', '-O2', '-std=c17', f'-I{PURE}', '-o', str(output)] + [str(s) for s in sources] + ['-lpthread'],
                   check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--directory', type=Path, default=Path('/var/tmp'), help='on the device to measure')
    parser.add_argument('--before', default='ad2197c', help='a revision with the two-sync commit')
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--commits', type=int, default=200)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        before = work / 'store-tree-before.c'
        before.write_text(subprocess.run(['git', '-C', str(ROOT), 'show', f'{args.before}:prototypes/connections/store-tree.c'],
                                         capture_output=True, text=True, check=True).stdout)
        build(work / 'probe', [HERE / 'probe.c'])
        builds = {'two syncs': work / 'commits-before', 'one sync': work / 'commits-after'}
        build(builds['two syncs'], [HERE / 'commits.c', before])
        build(builds['one sync'], [HERE / 'commits.c', PURE / 'store-tree.c'])
        results = {'rounds': []}
        for round_ in range(args.rounds):
            probe = subprocess.run([str(work / 'probe'), str(args.directory)], capture_output=True, text=True, check=True)
            measured = {'probe': [json.loads(line) for line in probe.stdout.splitlines()], 'commits': {}}
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
