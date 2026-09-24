#!/usr/bin/env python3
"""Check whole-process RSS for the selected CLI document workload."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent


def measure(helper, command):
    result = subprocess.run([helper, *command], capture_output=True, text=True,
                            check=True, timeout=15)
    sample = json.loads(result.stdout)
    if sample['exit_code']:
        raise AssertionError(f'workload failed: {command}: {sample}')
    return sample


def check(build, fixed=False, portable=False):
    budget_path = HERE / 'memory-budget.json'
    budget = json.loads(budget_path.read_text())
    if not portable and (platform.system() != 'Linux' or platform.machine() != 'aarch64'):
        raise RuntimeError('reviewed memory profile requires Linux AArch64')
    binary = (build / ('pure-edit-fixed' if fixed else 'pure-edit')).resolve()
    helper = (build / 'measure-peak-rss').resolve()
    program = HERE / 'examples/editor.pure'
    with tempfile.TemporaryDirectory(prefix='pure-micro-memory-') as name:
        root = Path(name)
        small, large = root / 'small', root / 'large'
        small.write_bytes(b'A')
        large.write_bytes(b'B' * 1_048_576)
        store = root / 'store'
        base = [binary]
        if not fixed:
            base += ['--program', program]
        base += ['--store', store]
        raw_samples = {
            'small_edit_rss_kib': measure(helper, [*base, 'edit', 'small', '0', '0', '0', small]),
            'one_mib_edit_rss_kib': measure(helper, [*base, 'edit', 'large', '0', '0', '0', large]),
            'one_mib_show_rss_kib': measure(helper, [*base, 'show', 'large']),
        }
        samples = {name: sample['max_rss_kib'] for name, sample in raw_samples.items()}
        shown = subprocess.run([*map(str, base), 'show', 'large'],
                               capture_output=True, check=True, timeout=15)
        if shown.stdout != large.read_bytes():
            raise AssertionError('measured document was not stored correctly')
        if portable:
            replacement = root / 'replacement'
            replacement.write_bytes(b'Z')
            raw_samples['one_mib_splice_rss_kib'] = measure(
                helper, [*base, 'edit', 'large', '1', '524288', '1', replacement])
            samples['one_mib_splice_rss_kib'] = raw_samples['one_mib_splice_rss_kib']['max_rss_kib']
            edited = subprocess.run([*map(str, base), 'show', 'large'],
                                    capture_output=True, check=True, timeout=15)
            expected = large.read_bytes()[:524288] + b'Z' + large.read_bytes()[524289:]
            if edited.stdout != expected:
                raise AssertionError('one-byte revision edit did not survive a new process')
    failures = [] if portable else [f'{name}: {value} > {budget["max_" + name]}'
                                    for name, value in samples.items() if value > budget['max_' + name]]
    if portable:
        scope = ('Child-process peak RSS for native CLI document creation, read and '
                 'one-byte revision edit; includes libc and the selected program form, '
                 'excludes filesystem cache, kernel buffers, TLS, GUI and other apps. '
                 'No cross-platform memory ceiling is asserted.')
    else:
        scope = budget['scope']
        if fixed:
            scope = scope.replace('program parsing', 'the embedded prepared program')
    return {'passed': not failures, 'failures': failures,
            'profile': 'portable-measurement-no-budget' if portable else budget['profile'],
            'variant': 'fixed-program' if fixed else 'source-program',
            'scope': scope, 'samples': samples,
            'raw_samples': raw_samples if portable else None,
            'budget_sha256': None if portable else hashlib.sha256(budget_path.read_bytes()).hexdigest(),
            'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
            'binary_bytes': binary.stat().st_size,
            'platform': platform.platform()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--fixed', action='store_true',
                        help='measure the parser-free fixed-program editor')
    parser.add_argument('--portable', action='store_true',
                        help='measure native document work without a Linux-specific RSS ceiling')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output and args.output.exists():
        parser.error('evidence file already exists')
    result = check(args.build, args.fixed, args.portable)
    output = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output)
    print(output, end='')
    if not result['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
