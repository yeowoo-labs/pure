#!/usr/bin/env python3
"""Time the same durable one-MiB document creation and one-byte revision edit."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parents[3]
PROGRAM = HERE / 'prototypes/connections/examples/editor.pure'
INITIAL = b'A' * 1_048_576
EXPECTED = INITIAL[:-1] + b'Z'


def invoke(command):
    started = time.monotonic_ns()
    result = subprocess.run([str(arg) for arg in command], capture_output=True, timeout=30)
    elapsed_ms = (time.monotonic_ns() - started) / 1_000_000
    if result.returncode:
        raise AssertionError((command, result.returncode, result.stderr[-1000:]))
    return result, elapsed_ms


def one(binary, root, initial, replacement):
    base = [binary, '--program', PROGRAM, '--store', root]
    created, create_ms = invoke([*base, 'edit', 'document', 0, 0, 0, initial])
    assert json.loads(created.stdout)['revision'] == '1'
    edited, edit_ms = invoke([*base, 'edit', 'document', 1, 1_048_575, 1, replacement])
    assert json.loads(edited.stdout)['revision'] == '2'
    shown, _ = invoke([*base, 'show', 'document'])
    assert shown.stdout == EXPECTED
    assert json.loads(shown.stderr)['revision'] == '2'
    files = [path for path in root.iterdir() if path.is_file()]
    return {'create_ms': round(create_ms, 3), 'edit_ms': round(edit_ms, 3),
            'store_files': len(files),
            'store_file_bytes': sum(path.stat().st_size for path in files),
            'store_allocated_bytes': sum(path.stat().st_blocks * 512 for path in files)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--flat', type=Path, required=True)
    parser.add_argument('--copy', type=Path, required=True)
    parser.add_argument('--paged', type=Path, required=True)
    parser.add_argument('--scratch-parent', type=Path, required=True)
    parser.add_argument('--trials', type=int, default=18)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.trials < 3 or args.trials % 3 or args.output.exists():
        parser.error('trials must be a positive multiple of three and output must be new')
    variants = {'flat': args.flat.resolve(), 'copy': args.copy.resolve(),
                'paged': args.paged.resolve()}
    for binary in variants.values():
        if not binary.is_file():
            parser.error(f'missing binary: {binary}')
    args.scratch_parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='pure-storage-timing-',
                                     dir=args.scratch_parent.resolve()) as name:
        root = Path(name)
        (root / 'initial').write_bytes(INITIAL)
        (root / 'replacement').write_bytes(b'Z')
        filesystem = subprocess.run(['findmnt', '-T', str(root), '-no', 'FSTYPE,SOURCE'],
                                    capture_output=True, text=True, check=True).stdout.strip()
        samples = {name: [] for name in variants}
        names = list(variants)
        for trial in range(args.trials):
            for name in names[trial % 3:] + names[:trial % 3]:
                sample = one(variants[name], root / f'{name}-{trial}',
                             root / 'initial', root / 'replacement')
                samples[name].append({'trial': trial, **sample})
    result = {'profile': 'native-cli-durable-one-mib-document-last-byte-edit',
              'platform': platform.platform(), 'filesystem': filesystem,
              'trials_per_variant': args.trials,
              'final_body_sha256': hashlib.sha256(EXPECTED).hexdigest(),
              'scope': ('Each sample starts a new CLI process; create and edit each '
                        'include program parsing, store initialization, file and '
                        'directory synchronization. Show verifies the final revision '
                        'but is outside the timed operations. The host page cache, '
                        'filesystem and device remain uncontrolled.'),
              'variants': {name: {'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                                  'median_create_ms': round(statistics.median(
                                      row['create_ms'] for row in samples[name]), 3),
                                  'median_edit_ms': round(statistics.median(
                                      row['edit_ms'] for row in samples[name]), 3),
                                  'samples': samples[name]}
                           for name, binary in variants.items()}}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({name: {key: value for key, value in entry.items()
                             if key.startswith('median_')}
                      for name, entry in result['variants'].items()}, indent=2))


if __name__ == '__main__':
    main()
