#!/usr/bin/env python3
"""Time durable one-byte saves in the actual native terminal document app."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import pty
import select
import statistics
import struct
import subprocess
import tempfile
import termios
import time


def run(*args):
    return subprocess.run([str(arg) for arg in args], check=True,
                          capture_output=True, timeout=30)


def frame(master, process):
    output = bytearray()
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if select.select([master], [], [], 0.5)[0]:
            output.extend(os.read(master, 65536))
            if b'\x1b[?25h' in output:
                return bytes(output)
        if process.poll() is not None:
            break
    raise AssertionError((process.poll(), output[-1000:]))


def one_session(cli, tui, root, size, iterations):
    root.mkdir(parents=True)
    store = root / 'store'
    body = b'A' * size
    insert = root / 'initial.bin'
    insert.write_bytes(body)
    assert json.loads(run(cli, '--store', store, 'edit', 'document',
                          0, 0, 0, insert).stdout)['revision'] == '1'
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 0, 0))
    original = termios.tcgetattr(slave)
    process = subprocess.Popen([str(tui), '--store', str(store), 'document'],
                               stdin=slave, stdout=slave, stderr=slave,
                               env={**os.environ, 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8'})
    samples = []
    try:
        assert b'Pure | document' in frame(master, process)
        for index in range(iterations):
            replacement = b'B' if index % 2 == 0 else b'A'
            os.write(master, b'\x7f')
            frame(master, process)
            os.write(master, replacement)
            frame(master, process)
            start = time.perf_counter_ns()
            os.write(master, b'\x13')
            shown = frame(master, process)
            elapsed = time.perf_counter_ns() - start
            expected = f'Saved 1-byte edit at byte {size - 1}'.encode()
            revision = f'revision {index + 2}'.encode()
            assert expected in shown and revision in shown, shown[-1500:]
            samples.append(elapsed)
        os.write(master, b'\x11')
        process.wait(timeout=8)
        assert process.returncode == 0
        assert termios.tcgetattr(slave) == original
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        os.close(master)
        os.close(slave)
    final = run(cli, '--store', store, 'show', 'document')
    expected_body = body[:-1] + (b'B' if iterations % 2 else b'A')
    assert final.stdout == expected_body
    assert json.loads(final.stderr)['revision'] == str(iterations + 1)
    return {'samples_ns': samples, 'median_ns': int(statistics.median(samples)),
            'final_revision': iterations + 1,
            'body_sha256': hashlib.sha256(final.stdout).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--flat-cli', type=Path, required=True)
    parser.add_argument('--flat-tui', type=Path, required=True)
    parser.add_argument('--paged-cli', type=Path, required=True)
    parser.add_argument('--paged-tui', type=Path, required=True)
    parser.add_argument('--scratch-parent', type=Path, required=True)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--edits', type=int, default=8)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.rounds < 1 or args.edits < 1 or (args.output and args.output.exists()):
        parser.error('rounds and edits must be positive; output must not exist')
    binaries = {name: (cli.resolve(), tui.resolve()) for name, cli, tui in
                (('flat', args.flat_cli, args.flat_tui),
                 ('paged', args.paged_cli, args.paged_tui))}
    parent = args.scratch_parent.resolve()
    parent.mkdir(parents=True, exist_ok=True)
    filesystem = run('findmnt', '-T', parent, '-no', 'SOURCE,FSTYPE').stdout.decode().strip()
    raw = {name: {'256': [], '1048576': []} for name in binaries}
    with tempfile.TemporaryDirectory(prefix='pure-native-save-', dir=parent) as directory:
        root = Path(directory)
        for size in (256, 1_048_576):
            for round_number in range(args.rounds):
                order = ('flat', 'paged') if round_number % 2 == 0 else ('paged', 'flat')
                for name in order:
                    cli, tui = binaries[name]
                    raw[name][str(size)].append(one_session(
                        cli, tui, root / f'{size}-{round_number}-{name}', size, args.edits))
    result = {'platform': platform.platform(), 'filesystem': filesystem,
              'workload': 'one-byte final-character replacement, Ctrl+S to complete visible frame; '
                          'includes full draft comparison, runtime, durable store and redraw; '
                          'initial creation and final independent CLI read excluded',
              'rounds': args.rounds, 'edits_per_session': args.edits,
              'binaries': {name: {'cli_sha256': hashlib.sha256(cli.read_bytes()).hexdigest(),
                                  'tui_sha256': hashlib.sha256(tui.read_bytes()).hexdigest()}
                           for name, (cli, tui) in binaries.items()},
              'runs': raw,
              'median_ns': {name: {size: int(statistics.median(
                  sample for session in sessions for sample in session['samples_ns']))
                  for size, sessions in sizes.items()} for name, sizes in raw.items()}}
    encoded = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded)
    print(json.dumps({'platform': result['platform'], 'filesystem': filesystem,
                      'median_ns': result['median_ns'], 'rounds': args.rounds,
                      'edits_per_session': args.edits}))


if __name__ == '__main__':
    main()
