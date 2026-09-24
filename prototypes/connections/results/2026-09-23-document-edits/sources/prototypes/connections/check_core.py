#!/usr/bin/env python3
"""Fail when the selected execution core grows past its reviewed cost boundary.

This is a release-build structural check using GNU-compatible ar/nm/size. Run it
separately from correctness/sanitizer tests; instrumented objects have other costs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess

ROOT = Path(__file__).resolve().parent


def run(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--layout', type=Path, required=True)
    parser.add_argument('--budget', type=Path, default=ROOT / 'core-budget.json')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output and args.output.exists():
        parser.error('evidence file already exists')
    budget = json.loads(args.budget.read_text())
    archive = args.archive.resolve()
    members = run('ar', 't', archive).splitlines()
    layout = json.loads(run(args.layout.resolve()))
    size_output = run('size', archive)
    rows = [line.split() for line in size_output.splitlines() if line.split() and line.split()[0].isdigit()]
    if len(rows) != len(members):
        parser.error('unsupported size output; no footprint claim was made')
    sizes = {name: sum(int(row[i]) for row in rows) for i, name in enumerate(('text', 'data', 'bss'))}
    defined = {line.split()[-1] for line in run('nm', '-g', '--defined-only', archive).splitlines()
               if len(line.split()) >= 3}
    undefined = {line.split()[-1] for line in run('nm', '-u', archive).splitlines()
                 if len(line.split()) == 2 and line.split()[0] == 'U'}
    external = sorted(undefined - defined)
    failures = []
    if sorted(members) != sorted(budget['archive_members']):
        failures.append('unexpected archive members')
    for name, value in sizes.items():
        if value > budget[f'max_{name}_bytes']:
            failures.append(f'{name}: {value} > {budget[f"max_{name}_bytes"]}')
    for name, maximum in budget['layout_max_bytes'].items():
        if layout[name] > maximum:
            failures.append(f'{name}: {layout[name]} > {maximum}')
    extra = set(external) - set(budget['external_symbols_allowed'])
    if extra:
        failures.append(f'unexpected dependencies: {sorted(extra)}')
    result = {'passed': not failures, 'failures': failures, 'budget': budget,
              'layout': layout, 'sections': sizes, 'members': members,
              'external_symbols': external, 'size_output': size_output,
              'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
              'layout_binary_sha256': hashlib.sha256(args.layout.resolve().read_bytes()).hexdigest(),
              'budget_sha256': hashlib.sha256(args.budget.read_bytes()).hexdigest(),
              'platform': platform.platform(), 'size_tool': run('size', '--version').splitlines()[0]}
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
