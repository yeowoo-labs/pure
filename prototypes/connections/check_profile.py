#!/usr/bin/env python3
"""Check the selected default hosts' linked code and direct dependencies.

This is a Linux/GNU release-build gate, separate from sanitizer and core checks.
It does not estimate process RAM or the cost of optional UI/network hosts.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess

ROOT = Path(__file__).resolve().parent
NEEDED = re.compile(r"\(NEEDED\).*Shared library: \[([^]]+)\]")


def run(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True)


def inspect(path):
    rows = [row.split() for row in run('size', path).splitlines()]
    if len(rows) != 2 or len(rows[1]) < 6 or not all(
            rows[1][i].isdigit() for i in range(3)):
        raise ValueError(f'unsupported size output for {path}')
    sections = dict(zip(('text', 'data', 'bss'), map(int, rows[1][:3])))
    needed = sorted(NEEDED.findall(run('readelf', '-d', path)))
    return {'sections': sections, 'needed': needed,
            'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--budget', type=Path, default=ROOT / 'profile-budget.json')
    parser.add_argument('--only', action='append', metavar='BINARY',
                        help='check only this selected binary (repeatable)')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output and args.output.exists():
        parser.error('evidence file already exists')
    budget = json.loads(args.budget.read_text())
    selected = args.only or list(budget['binaries'])
    unknown = set(selected) - set(budget['binaries'])
    if unknown:
        parser.error(f'unknown budget binary: {sorted(unknown)}')
    binaries = {}
    failures = []
    for name in selected:
        limits = budget['binaries'][name]
        path = args.build / name
        try:
            observed = inspect(path)
        except (OSError, subprocess.CalledProcessError, ValueError) as error:
            failures.append(f'{name}: cannot inspect: {error}')
            continue
        binaries[name] = observed
        for section, value in observed['sections'].items():
            limit = limits[f'max_{section}_bytes']
            if value > limit:
                failures.append(f'{name} {section}: {value} > {limit}')
        if observed['needed'] != sorted(limits['needed']):
            failures.append(f'{name} dependencies: {observed["needed"]} '
                            f'!= {sorted(limits["needed"])}')
    result = {'passed': not failures, 'failures': failures,
              'profile': budget['profile'], 'scope': budget['scope'],
              'budget_sha256': hashlib.sha256(args.budget.read_bytes()).hexdigest(),
              'platform': platform.platform(), 'binaries': binaries}
    output = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open('x') as destination:
            destination.write(output)
    print(output, end='')
    if failures:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
