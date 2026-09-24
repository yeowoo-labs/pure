#!/usr/bin/env python3
"""Reject directory-wide segment scans on the selected small-object edit path."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent


def run(*args):
    result = subprocess.run([str(value) for value in args], capture_output=True, timeout=20)
    if result.returncode:
        raise AssertionError((result.args, result.returncode, result.stderr[-500:]))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--editor', required=True, type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    editor = args.editor.resolve()
    with tempfile.TemporaryDirectory(prefix='pure-small-scan-') as name:
        root = Path(name)
        store = root / 'store'
        one = root / 'one'
        one.write_bytes(b'A')
        base = (editor, '--program', HERE / 'examples/editor.pure', '--store', store)
        run(*base, 'edit', 'tiny', 0, 0, 0, one)
        for index in range(256):
            (store / f'unrelated-{index:03d}').write_bytes(b'')
        trace = root / 'trace'
        run('strace', '-qq', '-e', 'getdents64', '-yy', '-o', trace,
            *base, 'edit', 'tiny', 1, 0, 1, one)
        scans = [line for line in trace.read_text().splitlines()
                 if 'getdents64(' in line and str(store) in line]
        assert not scans, scans
    result = {'small_edit_store_directory_scans': 0,
              'unrelated_directory_entries': 256,
              'binary_sha256': hashlib.sha256(editor.read_bytes()).hexdigest()}
    output = json.dumps(result, indent=2) + '\n'
    if args.output:
        if args.output.exists():
            parser.error('evidence file already exists')
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output)
    print(output, end='')


if __name__ == '__main__':
    main()
