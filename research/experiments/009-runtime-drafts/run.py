#!/usr/bin/env python3
"""Run bench.c in copy and draft modes on a real device, alternating order."""
import argparse
import json
import platform
import shutil
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONNECTIONS = HERE.parents[2] / 'prototypes' / 'connections'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--work', type=Path, default=CONNECTIONS / 'build' / 'bench-drafts')
    parser.add_argument('--size', type=int, default=1048576)
    parser.add_argument('--saves', type=int, default=24)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    shutil.rmtree(args.work, ignore_errors=True)
    args.work.mkdir(parents=True)
    device = subprocess.check_output(['findmnt', '-n', '-T', str(args.work), '-o', 'SOURCE,FSTYPE'], text=True).split()
    assert device[1] != 'tmpfs', 'fsync is free on tmpfs'
    binary = args.work / 'bench'
    subprocess.run(['cc', '-std=c17', '-O2', '-Wall', '-Wextra', '-Werror', f'-I{CONNECTIONS}',
                    str(HERE / 'bench.c'), str(CONNECTIONS / 'store-tree.c'), '-o', str(binary)], check=True)
    runs = []
    for round_number in range(args.rounds):
        for mode in (('copy', 'draft') if round_number % 2 == 0 else ('draft', 'copy')):
            directory = args.work / f'{mode}-{round_number}'
            subprocess.run([str(binary), 'create', str(directory), str(args.size), '0'], check=True)
            line = subprocess.check_output([str(binary), mode, str(directory), str(args.size), str(args.saves)], text=True)
            runs.append(json.loads(line))
            print(line.strip(), flush=True)
            shutil.rmtree(directory)
    report = {'device': device[0], 'filesystem': device[1], 'machine': platform.machine(),
              'kernel': platform.release(), 'store': 'store-tree.c', 'runs': runs}
    if args.output:
        args.output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
