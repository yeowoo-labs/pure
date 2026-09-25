#!/usr/bin/env python3
"""Build bench.c against each selected store and time durable edits on disk."""
import argparse
import json
import platform
import shutil
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONNECTIONS = HERE.parents[2] / 'prototypes' / 'connections'
STORES = {'flat': 'store.c', 'paged': 'store-paged.c', 'tree': 'store-tree.c'}
OPS = ['insert-middle', 'delete-middle', 'replace-end', 'append-end']


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--work', type=Path, default=CONNECTIONS / 'build' / 'bench-tree-cost')
    parser.add_argument('--count', type=int, default=32)
    parser.add_argument('--sizes', default='256,1048576')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    shutil.rmtree(args.work, ignore_errors=True)
    args.work.mkdir(parents=True)
    source = subprocess.check_output(['findmnt', '-n', '-T', str(args.work), '-o', 'SOURCE,FSTYPE'], text=True).split()
    assert source[1] != 'tmpfs', 'fsync is free on tmpfs; choose a real device'
    results = []
    for name, store in STORES.items():
        binary = args.work / f'bench-{name}'
        subprocess.run(['cc', '-std=c17', '-O2', '-Wall', '-Wextra', '-Werror', f'-I{CONNECTIONS}',
                        str(HERE / 'bench.c'), str(CONNECTIONS / store), '-o', str(binary)], check=True)
    for size in map(int, args.sizes.split(',')):
        for op in OPS:
            for name in STORES:  # interleave stores within each scenario
                directory = args.work / f'{name}-{op}-{size}'
                line = subprocess.check_output([str(args.work / f'bench-{name}'), name, str(directory),
                                                str(size), op, str(args.count)], text=True)
                results.append(json.loads(line))
                print(line.strip(), flush=True)
                shutil.rmtree(directory)
    report = {'device': source[0], 'filesystem': source[1], 'machine': platform.machine(),
              'kernel': platform.release(), 'compiler': subprocess.check_output(['cc', '--version'], text=True).splitlines()[0],
              'edits_per_scenario': args.count, 'results': results}
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
