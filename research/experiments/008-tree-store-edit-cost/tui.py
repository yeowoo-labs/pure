#!/usr/bin/env python3
"""Ctrl+S to completed frame in the real native terminal editor, per store.

Same method as 006 (full draft comparison, runtime, durable store and redraw),
with two edit shapes: replacing the final character (same size) and typing one
new character at the end (the document grows, as ordinary typing does)."""
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
    return subprocess.run([str(arg) for arg in args], check=True, capture_output=True, timeout=30)


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


def one_session(cli, tui, root, size, edits, shape):
    root.mkdir(parents=True)
    store = root / 'store'
    body = b'A' * size
    initial = root / 'initial.bin'
    initial.write_bytes(body)
    assert json.loads(run(cli, '--store', store, 'edit', 'document', 0, 0, 0, initial).stdout)['revision'] == '1'
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 0, 0))
    process = subprocess.Popen([str(tui), '--store', str(store), 'document'],
                               stdin=slave, stdout=slave, stderr=slave,
                               env={**os.environ, 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8'})
    samples, expected_body = [], bytearray(body)
    try:
        assert b'Pure | document' in frame(master, process)
        for index in range(edits):
            character = b'B' if index % 2 == 0 else b'C'
            if shape == 'replace':
                os.write(master, b'\x7f')
                frame(master, process)
                expected_body[-1:] = character
                position = size - 1
            else:
                expected_body += character
                position = size + index
            os.write(master, character)
            frame(master, process)
            start = time.perf_counter_ns()
            os.write(master, b'\x13')
            shown = frame(master, process)
            samples.append(time.perf_counter_ns() - start)
            assert f'Saved 1-byte edit at byte {position}'.encode() in shown, shown[-1500:]
            assert f'revision {index + 2}'.encode() in shown, shown[-1500:]
        os.write(master, b'\x11')
        process.wait(timeout=8)
        assert process.returncode == 0
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        os.close(master)
        os.close(slave)
    final = run(cli, '--store', store, 'show', 'document')
    assert final.stdout == bytes(expected_body)
    assert json.loads(final.stderr)['revision'] == str(edits + 1)
    return {'samples_ns': samples, 'body_sha256': hashlib.sha256(final.stdout).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--store', nargs=3, action='append', metavar=('NAME', 'CLI', 'TUI'), required=True)
    parser.add_argument('--scratch-parent', type=Path, required=True)
    parser.add_argument('--size', type=int, default=1_048_576)
    parser.add_argument('--rounds', type=int, default=3)
    parser.add_argument('--edits', type=int, default=8)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    binaries = {name: (Path(cli).resolve(), Path(tui).resolve()) for name, cli, tui in args.store}
    parent = args.scratch_parent.resolve()
    parent.mkdir(parents=True, exist_ok=True)
    filesystem = run('findmnt', '-T', parent, '-no', 'SOURCE,FSTYPE').stdout.decode().strip()
    assert 'tmpfs' not in filesystem, 'fsync is free on tmpfs'
    raw = {shape: {name: [] for name in binaries} for shape in ('replace', 'append')}
    with tempfile.TemporaryDirectory(prefix='pure-tree-save-', dir=parent) as directory:
        root = Path(directory)
        for shape in raw:
            for round_number in range(args.rounds):
                names = list(binaries)
                order = names[round_number % len(names):] + names[:round_number % len(names)]
                for name in order:
                    cli, tui = binaries[name]
                    raw[shape][name].append(one_session(cli, tui, root / f'{shape}-{round_number}-{name}',
                                                        args.size, args.edits, shape))
    medians = {shape: {name: round(statistics.median(s for session in sessions for s in session['samples_ns']) / 1e6, 2)
                       for name, sessions in stores.items()} for shape, stores in raw.items()}
    result = {'platform': platform.platform(), 'filesystem': filesystem, 'document_bytes': args.size,
              'rounds': args.rounds, 'edits_per_session': args.edits, 'median_ms': medians,
              'binaries': {name: {'cli_sha256': hashlib.sha256(cli.read_bytes()).hexdigest(),
                                  'tui_sha256': hashlib.sha256(tui.read_bytes()).hexdigest()}
                           for name, (cli, tui) in binaries.items()},
              'runs': raw}
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'filesystem': filesystem, 'median_ms': medians}))


if __name__ == '__main__':
    main()
