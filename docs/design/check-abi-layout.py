#!/usr/bin/env python3
"""Compile draft record assertions only; does not test a callable module ABI."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--zig', default='zig')
parser.add_argument('--output', type=Path)
args = parser.parse_args()
zig = shutil.which(args.zig)
if not zig:
    parser.error('Zig 0.15.2 required; use --zig /path/to/zig')
version = subprocess.check_output([zig, 'version'], text=True).strip()
if version != '0.15.2':
    parser.error(f'This check is pinned to Zig 0.15.2; found {version}')
header = root / 'abi-v0.h'
checks = []
with tempfile.TemporaryDirectory(prefix='pure-abi-layout-') as cache:
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=cache)
    for target in ['aarch64-linux-gnu', 'aarch64-macos-none', 'wasm32-freestanding']:
        command = [zig, 'cc', '-target', target, '-std=c17', '-ffreestanding',
                   '-Wall', '-Wextra', '-Werror', '-x', 'c', '-c', str(header),
                   '-o', str(Path(cache) / f'{target}.o')]
        result = subprocess.run(command, env=env, capture_output=True, text=True)
        checks.append({'target': target, 'command': command, 'exitCode': result.returncode,
                       'stdout': result.stdout, 'stderr': result.stderr})
report = {'recordedUtc': datetime.now(timezone.utc).isoformat(), 'zigVersion': version,
          'scope': 'Cross-compilation of C17 record sizes/offset assertions only. No target execution, function calls, module loading, memory enforcement, or performance measurement.',
          'headerSha256': hashlib.sha256(header.read_bytes()).hexdigest(),
          'checkerSha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
          'assertionsPerTarget': 8, 'checks': checks,
          'passed': all(check['exitCode'] == 0 for check in checks)}
encoded = json.dumps(report, indent=2) + '\n'
if args.output:
    with args.output.open('x') as stream:
        stream.write(encoded)
print(encoded, end='')
raise SystemExit(0 if report['passed'] else 1)
