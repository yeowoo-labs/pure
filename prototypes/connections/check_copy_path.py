#!/usr/bin/env python3
"""Compare one real immutable-file splice with the selected Linux copy path."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SUCCESS = re.compile(r'= (\d+)$')


def run(*args):
    result = subprocess.run([str(arg) for arg in args], capture_output=True,
                            timeout=30, check=True)
    return result.stdout


def inspect(binary, helper, root, initial, patch):
    store = root / binary.parent.name
    base = [binary, '--program', HERE / 'examples/editor.pure', '--store', store]
    run(*base, 'edit', 'large', 0, 0, 0, initial)
    trace = root / (binary.parent.name + '.trace')
    run('strace', '-f', '-qq', '-yy', '-e',
        'trace=pread64,write,copy_file_range', '-o', trace,
        *base, 'edit', 'large', 1, 1_048_575, 1, patch)
    expected = b'A' * 1_048_575 + b'Z'
    assert run(*base, 'show', 'large') == expected

    lines = trace.read_text().splitlines()
    reads = [line for line in lines if 'pread64(' in line and '/obj-large>' in line]
    copies = [line for line in lines if 'copy_file_range(' in line]
    writes = [line for line in lines if 'write(' in line and '/.pending-' in line]
    assert all(SUCCESS.search(line) for line in reads + copies + writes)

    measured_store = root / (binary.parent.name + '-rss')
    measured = [binary, '--program', HERE / 'examples/editor.pure',
                '--store', measured_store]
    run(*measured, 'edit', 'large', 0, 0, 0, initial)
    usage = json.loads(run(helper, *measured, 'edit', 'large', 1, 1_048_575, 1, patch))
    assert usage['exit_code'] == 0
    assert run(*measured, 'show', 'large') == expected
    return {'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
            'result_sha256': hashlib.sha256(expected).hexdigest(),
            'object_pread_calls': len(reads),
            'copy_file_range_calls': len(copies),
            'copy_file_range_bytes': sum(int(SUCCESS.search(line)[1]) for line in copies),
            'pending_write_calls': len(writes),
            'pending_write_bytes': sum(int(SUCCESS.search(line)[1]) for line in writes),
            'one_mib_splice_peak_rss_kib': usage['max_rss_kib']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--baseline-helper', type=Path, required=True)
    parser.add_argument('--candidate-helper', type=Path, required=True)
    parser.add_argument('--scratch-dir', type=Path, default=HERE / 'build',
                        help='parent filesystem for the actual document stores')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output and args.output.exists():
        parser.error('evidence file already exists')
    if platform.system() != 'Linux':
        parser.error('strace and wait4 RSS evidence require Linux')
    args.scratch_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='pure-copy-path-',
                                     dir=args.scratch_dir.resolve()) as name:
        root = Path(name)
        filesystem = run('findmnt', '-T', root, '-no', 'FSTYPE').decode().strip().splitlines()[0]
        initial, patch = root / 'initial', root / 'patch'
        initial.write_bytes(b'A' * 1_048_576)
        patch.write_bytes(b'Z')
        baseline = inspect(args.baseline.resolve(), args.baseline_helper.resolve(),
                           root, initial, patch)
        candidate = inspect(args.candidate.resolve(), args.candidate_helper.resolve(),
                            root, initial, patch)
    assert baseline['result_sha256'] == candidate['result_sha256']
    assert baseline['copy_file_range_calls'] == 0
    assert candidate['copy_file_range_bytes'] == 1_048_575
    assert candidate['object_pread_calls'] < baseline['object_pread_calls']
    result = {'profile': 'linux-immutable-one-mib-last-byte-splice',
              'platform': platform.platform(), 'baseline': baseline,
              'candidate': candidate,
              'filesystem': filesystem,
              'scratch_parent': str(args.scratch_dir.resolve()),
              'scope': 'Successful object-file syscalls and child peak RSS; excludes physical device writes, page cache and proof of lower latency.'}
    body = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(body)
    print(body, end='')


if __name__ == '__main__':
    main()
