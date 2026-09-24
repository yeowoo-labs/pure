#!/usr/bin/env python3
"""Trace one real document replacement through each selected storage binary."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
WRITE = re.compile(r'\bwrite\(\d+<([^>]+)>, .*?, (\d+)\) = (\d+)$')


def command(*args):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, timeout=20)
    if result.returncode:
        raise AssertionError((result.args, result.returncode, result.stderr[-1000:]))
    return result


def inspect(binary, root, key, initial, patch, offset):
    program = HERE / 'examples/editor.pure'
    store = root / (binary.parent.name + '-' + key)
    base = [binary, '--program', program, '--store', store]
    command(*base, 'edit', key, 0, 0, 0, initial)
    trace = root / (binary.parent.name + '-' + key + '.trace')
    command('strace', '-qq', '-e', 'trace=write,pwrite64,writev,pwritev,fsync',
            '-yy', '-o', trace, *base, 'edit', key, 1, offset, 1, patch)
    expected = initial.read_bytes()[:offset] + patch.read_bytes() + initial.read_bytes()[offset + 1:]
    assert command(*base, 'show', key).stdout == expected
    writes = []
    for line in trace.read_text().splitlines():
        if '/.pending-' not in line and '/dat-' + key + '-' not in line:
            continue
        if not any(word in line for word in ('write(', 'pwrite64(', 'writev(', 'pwritev(')):
            continue
        match = WRITE.search(line)
        if not match:
            raise AssertionError(f'unaccounted object write: {line}')
        requested, completed = int(match[2]), int(match[3])
        if requested != completed:
            raise AssertionError(f'partial object write: {line}')
        writes.append(completed)
    return {'object_write_syscall_bytes': sum(writes), 'object_write_calls': len(writes),
            'object_files': {item.name: item.stat().st_size for item in store.iterdir()
                             if item.name.startswith(('obj-', 'dat-'))},
            'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--flat', type=Path, required=True)
    parser.add_argument('--paged', '--candidate', dest='candidate', type=Path, required=True)
    parser.add_argument('--candidate-mode', choices=('paged', 'hybrid'), default='paged')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output and args.output.exists():
        parser.error('evidence file already exists')
    with tempfile.TemporaryDirectory(prefix='pure-write-cost-') as name:
        root = Path(name)
        initial, tiny, patch = root / 'initial', root / 'tiny', root / 'patch'
        initial.write_bytes(b'A' * 1_048_576)
        tiny.write_bytes(b'A')
        patch.write_bytes(b'Z')
        large_flat = inspect(args.flat.resolve(), root, 'large', initial, patch, 1_048_575)
        large_paged = inspect(args.candidate.resolve(), root, 'large', initial, patch, 1_048_575)
        tiny_flat = inspect(args.flat.resolve(), root, 'tiny', tiny, patch, 0)
        tiny_paged = inspect(args.candidate.resolve(), root, 'tiny', tiny, patch, 0)
    if [large_flat['object_write_syscall_bytes'], large_paged['object_write_syscall_bytes'],
        tiny_flat['object_write_syscall_bytes'], tiny_paged['object_write_syscall_bytes']] != \
            [1_048_600, 16_944, 25, 25 if args.candidate_mode == 'hybrid' else 16_440]:
        raise AssertionError((large_flat, large_paged, tiny_flat, tiny_paged))
    result = {'profile': 'linux-aarch64-16k-page-document-replacement',
              'candidate_mode': args.candidate_mode,
              'workloads': ['replace last byte of 1 MiB document', 'replace one-byte document'],
              'unit': 'successful user-space write syscall bytes targeting object files',
              'excludes': 'physical device writes, filesystem journal, metadata and directory sync cost',
              'one_mib': {'flat': large_flat, args.candidate_mode: large_paged,
                          f'flat_to_{args.candidate_mode}_ratio': large_flat['object_write_syscall_bytes'] /
                                                 large_paged['object_write_syscall_bytes']},
              'one_byte': {'flat': tiny_flat, args.candidate_mode: tiny_paged,
                           f'{args.candidate_mode}_to_flat_ratio': tiny_paged['object_write_syscall_bytes'] /
                                                  tiny_flat['object_write_syscall_bytes']}}
    output = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output)
    print(output, end='')


if __name__ == '__main__':
    main()
