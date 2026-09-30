#!/usr/bin/env python3
"""Where a Pure node's memory goes when what it keeps cannot leave (review
004: an app's whole memory budget, not its requests' alone). pure-serve runs
an app while clients post as fast as they may for --seconds; its resident
memory (VmRSS) is read every 0.5 s beside what was posted, and the store's
size at the end.

  outbox      the sender's output wired nowhere: every send stays in its outbox
  budget      the same with --memory 4000000 (docs/design/013-memory-budget.md):
              sends past it are refused, 503 memory
  link-down   the output linked (pure://) to a port no one listens on
  wire-down   the output sent over HTTP to a port no one listens on
  held        a list of items whose notes are set again and again, a surface
              holding the snapshot of all of them (hold-surface.so) the whole time
  unheld      the same, no surface

    python3 research/experiments/021-memory/run.py [--seconds 30] [--cases outbox,budget,held] [--out FILE]
        [--label NAME]

PURE_SERVE=PATH runs another build's pure-serve (--label names it in the rows).
"""
import argparse
import http.client
import json
import os
import random
import select
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
SERVE = Path(os.environ.get('PURE_SERVE', ROOT / 'build/pure-serve'))  # another build's, to compare
sys.path.insert(0, str(ROOT / 'tests'))
import check  # noqa: E402

ITEMS = """record Item {
  id = 0
  note: text
}
state items: list of Item by id
input add = message {
  id: number
  note: text
}
input set = message {
  id: number
  note: text
}
on add { items.add(Item { id: add.id, note: add.note }) }
on set { items[set.id].note = set.note }
view count = items.count
"""
SEEDED, NOTE, MEMORY = 20000, 100, 4000000


def build(work):
    if 'PURE_SERVE' not in os.environ:
        subprocess.run(['make', '-s', '-C', str(ROOT), 'build/pure-serve'], check=True)
    aup = Path(os.environ.get('AUP', ROOT.parent / 'aup/build/aup'))
    for name, text in (('sender', check.LINK_SENDER), ('items', ITEMS)):
        (work / f'{name}.aup').write_text(text)
        check.compile_module(aup, work / f'{name}.aup', work / f'{name}.pmod')
    subprocess.run(['cc', '-O2', '-std=c17', '-Wall', '-Wextra', '-Werror', '-fPIC', '-shared', f'-I{ROOT}/core',
                    f'-I{ROOT}/serve', str(HERE / 'hold-surface.c'), '-o', str(work / 'hold-surface.so')], check=True)
    key = subprocess.Popen([str(SERVE), '--module', str(work / 'items.pmod'), '--store',
                            str(work / 'key-store'), '--key', str(work / 'node.key'), '--port', '0'],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    select.select([key.stdout], [], [], 10)
    key.stdout.readline()
    key.terminate()
    key.communicate(timeout=30)


def start(args):
    args = [str(a) for a in args]
    at = args.index('--') if '--' in args else len(args)  # the surface's arguments come last
    process = subprocess.Popen(args[:at] + ['--port', '0'] + args[at:], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ready, _, _ = select.select([process.stdout], [], [], 10)
    line = process.stdout.readline() if ready else b''
    assert line, process.stderr.read(2000) if process.poll() is not None else 'no start'
    return process, json.loads(line)['port']


def rss_kib(pid):
    with open(f'/proc/{pid}/status') as status:
        for line in status:
            if line.startswith('VmRSS:'):
                return int(line.split()[1])
    return 0


def store_bytes(path):
    return sum(f.stat().st_size for f in Path(path).rglob('*') if f.is_file())


def post_all(port, bodies, stop, counts, index, refused):
    """Posts (path, body) from bodies() on one keep-alive connection until
    stop: counts[index] the ones taken, refused[index] those refused 503."""
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=30)
    while not stop.is_set():
        path, body = bodies()
        data = json.dumps(body).encode()
        try:
            connection.request('POST', path, data, {'Content-Length': str(len(data)),
                                                    'Content-Type': 'application/json'})
            response = connection.getresponse()
            response.read()
            if response.status == 200:
                counts[index] += 1
            elif response.status == 503:
                refused[index] += 1
        except OSError:
            connection.close()
            connection = http.client.HTTPConnection('127.0.0.1', port, timeout=30)
    connection.close()


def run_case(case, work, seconds, clients=4):
    store = work / f'store-{case}'
    args = [SERVE, '--store', store, '--threads', '2']
    note = 'n' * NOTE
    if case in ('outbox', 'budget', 'link-down', 'wire-down'):
        args += ['--module', work / 'sender.pmod']
        if case == 'budget':
            args += ['--memory', MEMORY]
        if case == 'link-down':
            args += ['--key', work / 'node.key', '--send', 'ordered=pure://127.0.0.1:9/order',
                     '--seal', f'ordered={os.urandom(32).hex()}']
        elif case == 'wire-down':
            args += ['--send', 'ordered=http://127.0.0.1:9/inputs/order']

        def bodies():
            return '/inputs/buy', {'item': note, 'count': 1}
    else:
        args += ['--module', work / 'items.pmod']
        if case == 'held':  # the snapshot of all the items, held from then on
            args += ['--surface', work / 'hold-surface.so', '--', 'count', str(SEEDED)]
        rng = random.Random(7)

        def bodies():
            return '/inputs/set', {'id': rng.randrange(SEEDED), 'note': f'{rng.getrandbits(32):08x}' + note[8:]}
    process, port = start(args)
    try:
        if case in ('held', 'unheld'):  # the items first, then the sets
            seeded = [0]
            done = threading.Event()
            batches = [iter(range(k, SEEDED, clients)) for k in range(clients)]

            def seeding(k):
                connection = http.client.HTTPConnection('127.0.0.1', port, timeout=30)
                for i in batches[k]:
                    data = json.dumps({'id': i, 'note': note}).encode()
                    connection.request('POST', '/inputs/add', data, {'Content-Length': str(len(data)),
                                                                      'Content-Type': 'application/json'})
                    response = connection.getresponse()
                    response.read()
                    assert response.status == 200, response.status
                connection.close()
            threads = [threading.Thread(target=seeding, args=(k,)) for k in range(clients)]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            del seeded, done
            time.sleep(1.0)  # the surface reads the first snapshot after the seeding's saves
        stop = threading.Event()
        counts, refused = [0] * clients, [0] * clients
        began, rss_began = time.time(), rss_kib(process.pid)
        samples = [(0.0, rss_began, 0)]
        threads = [threading.Thread(target=post_all, args=(port, bodies, stop, counts, k, refused))
                   for k in range(clients)]
        for t in threads:
            t.start()
        while time.time() - began < seconds:
            time.sleep(0.5)
            samples.append((round(time.time() - began, 2), rss_kib(process.pid), sum(counts)))
        stop.set()
        for t in threads:
            t.join()
        rss_end = rss_kib(process.pid)
    finally:
        process.send_signal(signal.SIGTERM)
        _, error = process.communicate(timeout=60)
    stats = [json.loads(line) for line in error.decode().splitlines() if line.startswith('{"active"')]
    posted = sum(counts)
    return {'case': case, 'seconds': seconds, 'posted': posted, 'refused': sum(refused), 'rss_began_kib': rss_began,
            'rss_end_kib': rss_end,
            'rss_per_post_bytes': round((rss_end - rss_began) * 1024 / posted, 1) if posted else None,
            'store_bytes': store_bytes(store), 'samples': samples[::4] + [samples[-1]],
            'stats': stats[-1] if stats else None}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--seconds', type=int, default=30)
    parser.add_argument('--cases', default='outbox,budget,link-down,wire-down,unheld,held')
    parser.add_argument('--out', type=Path, default=None)
    parser.add_argument('--label', default=None, help='the build, named in each row')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='pure-memory-') as directory:
        work = Path(directory)
        build(work)
        for case in args.cases.split(','):
            row = run_case(case, work, args.seconds)
            if args.label:
                row = {'label': args.label, **row}
            line = json.dumps(row)
            print(line, flush=True)
            if args.out:
                with open(args.out, 'a') as out:
                    out.write(line + '\n')


if __name__ == '__main__':
    main()
