#!/usr/bin/env python3
"""Measure one visible PTY redraw per edit in the real fixed document app."""
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

HERE = Path(__file__).resolve().parent
WORKLOADS = {
    '256-byte-ascii': b'Q' * 256,
    'one-mib-ascii': b'Q' * 1_048_576,
    'one-mib-utf8': '안녕'.encode() * 174_762,
}


def frame(fd, process):
    data = bytearray()
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if select.select([fd], [], [], 0.5)[0]:
            data.extend(os.read(fd, 65536))
            if b'\x1b[?25h' in data:
                return bytes(data)
        if process.poll() is not None:
            break
    raise RuntimeError(f'editor did not finish a frame: exit={process.poll()}, output={data[-300:]!r}')


def measure(tui, cli, store, root, name, body):
    insert = root / f'{name}.insert'
    insert.write_bytes(body)
    subprocess.run([cli, '--store', store, 'edit', name, '0', '0', '0', insert],
                   check=True, capture_output=True, timeout=15)
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 0, 0))
    process = subprocess.Popen([tui, '--store', store, name], stdin=slave,
                               stdout=slave, stderr=slave,
                               env={**os.environ, 'LANG': 'C.UTF-8'})
    try:
        frame(master, process)
        samples = []
        for _ in range(10):
            start = time.perf_counter_ns()
            os.write(master, b'Z')
            shown = frame(master, process)
            samples.append({'nanoseconds': time.perf_counter_ns() - start,
                            'frame_bytes': len(shown),
                            'frame_sha256': hashlib.sha256(shown).hexdigest()})
        os.write(master, b'\x11')
        frame(master, process)
        os.write(master, b'\x11')
        process.wait(timeout=5)
        if process.returncode:
            raise RuntimeError(f'editor exit={process.returncode}')
        return {'document_bytes': len(body), 'samples': samples,
                'median_nanoseconds': int(statistics.median(s['nanoseconds'] for s in samples))}
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        os.close(master)
        os.close(slave)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tui', type=Path, required=True)
    parser.add_argument('--cli', type=Path, default=HERE / 'build/pure-edit-fixed')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.output and args.output.exists():
        parser.error('evidence file already exists')
    tui, cli = args.tui.resolve(), args.cli.resolve()
    with tempfile.TemporaryDirectory(prefix='pure-tui-latency-') as directory:
        root = Path(directory)
        workloads = {name: measure(tui, cli, root / 'store', root, name, body)
                     for name, body in WORKLOADS.items()}
    result = {'platform': platform.platform(), 'terminal': 'PTY 24x80, C.UTF-8',
              'tui_sha256': hashlib.sha256(tui.read_bytes()).hexdigest(),
              'cli_sha256': hashlib.sha256(cli.read_bytes()).hexdigest(),
              'workloads': workloads}
    text = json.dumps(result, indent=2) + '\n'
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    print(text, end='')


if __name__ == '__main__':
    main()
