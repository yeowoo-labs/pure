#!/usr/bin/env python3
"""Verify the contract; this runner deliberately does not benchmark speed."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import tempfile
from datetime import datetime, timezone


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, help='new JSON evidence file')
    args = parser.parse_args()
    if args.output and args.output.exists():
        parser.error('output already exists')
    root = Path(__file__).resolve().parent
    cc = shlex.split(os.environ.get('CC', 'cc'))
    common = ['-std=c17', '-g', '-Wall', '-Wextra', '-Wpedantic', '-Werror']
    record = {
        'kind': 'correctness-and-bounds-only',
        'utc': datetime.now(timezone.utc).isoformat(),
        'host': platform.platform(),
        'machine': platform.machine(),
        'compiler': subprocess.check_output(cc + ['--version'], text=True).splitlines()[0],
        'source_sha256': {
            name: hashlib.sha256((root / name).read_bytes()).hexdigest()
            for name in ('core.h', 'core.c', 'test.c', 'check.py')
        },
        'runs': [],
    }
    with tempfile.TemporaryDirectory(prefix='pure-forgetful-') as temporary:
        for name, extra in (
            ('native', ['-O2', '-fstack-usage']),
            ('asan-ubsan', ['-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']),
        ):
            binary = Path(temporary) / name
            flags = common + extra
            subprocess.run(cc + flags + [str(root / 'core.c'), str(root / 'test.c'),
                           '-o', str(binary)], cwd=temporary, check=True)
            completed = subprocess.run([str(binary)], text=True, capture_output=True)
            if completed.returncode or completed.stderr:
                raise RuntimeError(f'{name} failed ({completed.returncode}):\n'
                                   f'{completed.stdout}\n{completed.stderr}')
            result = json.loads(completed.stdout)
            if record['runs'] and result != record['runs'][0]['result']:
                raise RuntimeError('native and sanitized verification disagree')
            run = {'name': name, 'flags': flags, 'result': result}
            if name == 'native':
                # Compiler estimates, not a measured process stack high-water mark.
                run['compiler_stack_usage'] = {
                    path.name: path.read_text()
                    for path in sorted(Path(temporary).glob('*core.su'))
                }
            record['runs'].append(run)
    output = json.dumps(record, ensure_ascii=False, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open('x') as destination:
            destination.write(output)
    print(output, end='')


if __name__ == '__main__':
    main()
