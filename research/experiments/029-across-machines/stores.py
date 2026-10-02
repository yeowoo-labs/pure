#!/usr/bin/env python3
"""Stores made on one machine, opened and written on another (research 029).
aup's examples tasks.aup (a list) and notes.aup (a text) run on a tree
store from seeded events, saving in batches. The store's format does not
depend on the page size (it is read only to map a read in place), so a
store of some hundred KiB, across many 16 KiB pages, is enough.

pure-run prints the whole state after each event: its output grows with
the square of the events (tasks: 33 MB for 1,000 events, 13 GB for
20,000). It is streamed into a digest and never held: only its first line
and its last few MiB are kept.

stores.py make DIR         the modules and the stores in DIR, and
                           DIR/made.json: the digest of the snapshots and
                           of the last, the digest of each store's files,
                           their sizes
stores.py open DIR OUT     a copy of each store in DIR opened here and
                           written on: a show, 300 more events, a show; the
                           snapshots' digest into OUT (JSON)"""
import hashlib
import json
import random
import shutil
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

HERE = Path(__file__).resolve().parent
PURE = HERE.parents[2]
AUP = PURE.parent / 'aup'
PURE_RUN = PURE / 'build' / 'pure-run'
EVENTS = {'tasks': 2000, 'notes': 4000}
TAIL = 8 << 20  # the last line is in the last 8 MiB (a snapshot here is at most some hundred KiB)


def events(name, count, seed):
    rng, lines = random.Random(seed), []
    if name == 'tasks':
        words = ['milk', '빵', 'a"b', '😀', 'x' * 40, 'é']
        keys = ['up', 'down', 'toggle', 'top', 'bottom']
        for _ in range(count):
            if rng.random() < 0.8:
                lines.append('type typed ' + rng.choice(words).encode().hex())
            else:
                lines.append('down ' + rng.choice(keys))
    else:
        words = ['a', '안녕', '\n', '😀', 'é', 'pure ']
        keys = ['left', 'right', 'up', 'down', 'home', 'finish']
        for _ in range(count):
            if rng.random() < 0.9:
                lines.append('type typed ' + rng.choice(words).encode().hex())
            else:
                lines.append('down ' + rng.choice(keys))
    return '\n'.join(lines) + '\n'


def run(module, store, given, *more):
    """pure-run's snapshots, streamed: their digest, how many, the first and the last."""
    with tempfile.TemporaryFile() as errors:
        process = subprocess.Popen([str(PURE_RUN), str(module), '--store', str(store), *more], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=errors)

        def feed():  # its input while its output is read: neither waits on the other
            process.stdin.write(given.encode())
            process.stdin.close()

        feeding = threading.Thread(target=feed)
        feeding.start()
        digest, lines, head, tail = hashlib.sha256(), 0, b'', b''
        while chunk := process.stdout.read(1 << 20):
            digest.update(chunk)
            lines += chunk.count(b'\n')
            if b'\n' not in head:
                head += chunk
            tail = (tail + chunk)[-TAIL:]
        feeding.join()
        process.wait()
        errors.seek(0)
        assert process.returncode in (0, 1), errors.read()[-500:]
    last = tail.rstrip(b'\n').rsplit(b'\n', 1)
    assert len(last) == 2 or lines <= 1, 'the last snapshot is longer than the tail kept'
    return {'snapshots': digest.hexdigest()[:24], 'lines': lines,
            'first': hashlib.sha256(head.split(b'\n', 1)[0]).hexdigest()[:24],
            'last': hashlib.sha256(last[-1]).hexdigest()[:24]}


def files_digest(store):
    digest, sizes = hashlib.sha256(), {}
    for path in sorted(p for p in store.rglob('*') if p.is_file()):
        relative = str(path.relative_to(store))
        digest.update(relative.encode() + b'\0' + path.read_bytes())
        sizes[relative] = path.stat().st_size
    return digest.hexdigest()[:24], sizes


def make(directory):
    directory.mkdir(parents=True, exist_ok=True)
    made = {}
    for name, count in EVENTS.items():
        module = directory / f'{name}.pmod'
        subprocess.run([str(AUP / 'build' / 'aup'), 'build', str(AUP / 'examples' / f'{name}.aup'), '-o', str(module)],
                       check=True)
        store = directory / f'{name}.store'
        shutil.rmtree(store, ignore_errors=True)
        shown = run(module, store, events(name, count, 29), '--batch', '64')
        files, sizes = files_digest(store)
        made[name] = {'module': hashlib.sha256(module.read_bytes()).hexdigest()[:24], 'events': count, **shown,
                      'files': files, 'bytes': sum(sizes.values()), 'file_count': len(sizes)}
    (directory / 'made.json').write_text(json.dumps(made, indent=1) + '\n')
    print(json.dumps(made))


def open_(directory, out):
    opened = {}
    for name in EVENTS:
        copy = directory / f'{name}.opened'
        shutil.rmtree(copy, ignore_errors=True)
        shutil.copytree(directory / f'{name}.store', copy)
        opened[name] = run(directory / f'{name}.pmod', copy, 'show\n' + events(name, 300, 31) + 'show\n')
        shutil.rmtree(copy)
    Path(out).write_text(json.dumps(opened) + '\n')
    print(json.dumps(opened))


if __name__ == '__main__':
    if sys.argv[1] == 'make':
        make(Path(sys.argv[2]))
    else:
        open_(Path(sys.argv[2]), sys.argv[3])
