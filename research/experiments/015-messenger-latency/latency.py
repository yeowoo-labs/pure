#!/usr/bin/env python3
"""How long a line takes from one messenger node to the other (docs/research/089-messenger.md).

Two nodes of Vega's examples/messenger.aup on this machine, Alice's and Bob's,
wired to each other directly: sealed (each with a key of its own, sealing to
the other's, naming the other as its one sender) or plain. A program posts
--lines lines to Alice's node one after another; for each it times the post
(answered once Alice's node saved the line) and the moment Bob's node shows
it (its title view counts the line; polled every 0.2 ms). Nodes on cores 0-2,
stores on --directory. Prints one JSON line per variant and round.
"""
import argparse
import http.client
import json
import os
import select
import shutil
import signal
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def start(serve, module, work, name, *extra):
    process = subprocess.Popen(['taskset', '-c', '0-2', str(serve), '--module', str(module), '--store',
                                str(work / f'{name}-store'), '--port', '0', '--name', name, *map(str, extra)],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ready, _, _ = select.select([process.stdout], [], [], 10)
    assert ready, 'node did not start'
    return process, json.loads(process.stdout.readline())


def stop(process):
    process.send_signal(signal.SIGTERM)
    _, error = process.communicate(timeout=30)
    assert process.returncode == 0, error[-500:]


class Client:
    def __init__(self, port):
        self.connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)

    def call(self, method, path, body=None):
        self.connection.request(method, path, body)
        answer = self.connection.getresponse()
        return answer.status, answer.read().decode()


def run(serve, module, work, sealed, lines):
    for name in ('alice', 'bob'):
        shutil.rmtree(work / f'{name}-store', ignore_errors=True)
    keys = {}
    for name in ('alice', 'bob'):
        extra = ['--key', work / f'{name}.key'] if sealed else []
        process, started = start(serve, module, work, name, *extra)
        keys[name] = started.get('public_key')
        assert Client(started['port']).call('POST', '/inputs/rename', name.encode())[0] == 200
        stop(process)
    # Bob first, on a port of its own; then Alice, wired to it; then Bob again, wired to Alice.
    def wiring(name, other, port):
        wire = ['--send', f'said=http://127.0.0.1:{port}/inputs/heard']
        if sealed:
            wire += ['--key', work / f'{name}.key', '--seal', f'said={keys[other]}', '--sender',
                     f'{other}.said={keys[other]}']
        return wire
    bob, bob_started = start(serve, module, work, 'bob', *(['--key', work / 'bob.key'] if sealed else []))
    alice, alice_started = start(serve, module, work, 'alice', *wiring('alice', 'bob', bob_started['port']))
    to_alice, to_bob = Client(alice_started['port']), Client(bob_started['port'])
    saved, seen = [], []
    try:
        for i in range(lines):
            began = time.perf_counter()
            assert to_alice.call('POST', '/inputs/say', f'line {i}'.encode())[0] == 200
            saved.append(time.perf_counter() - began)
            want = f'bob: {i + 1} lines'
            while to_bob.call('GET', '/views/title')[1] != want:
                time.sleep(0.0002)
            seen.append(time.perf_counter() - began)
    finally:
        stop(alice)
        stop(bob)

    def ms(values, q):
        return round(statistics.quantiles(values, n=100)[q - 1] * 1000, 2)
    delivered = [b - a for a, b in zip(saved, seen)]  # from Alice's answer to Bob's view
    return {'sealed': sealed, 'lines': lines, 'saved_p50_ms': ms(saved, 50), 'saved_p99_ms': ms(saved, 99),
            'delivered_p50_ms': ms(delivered, 50), 'delivered_p99_ms': ms(delivered, 99),
            'shown_p50_ms': ms(seen, 50), 'shown_p90_ms': ms(seen, 90), 'shown_p99_ms': ms(seen, 99),
            'shown_max_ms': round(max(seen) * 1000, 2)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--aup', type=Path, default=ROOT.parent / 'aup' / 'build' / 'aup')
    parser.add_argument('--vega', type=Path, default=ROOT.parent / 'vega')
    parser.add_argument('--serve', type=Path, default=ROOT / 'build' / 'pure-serve')
    parser.add_argument('--directory', type=Path, default=Path('/var/tmp'))
    parser.add_argument('--lines', type=int, default=300)
    parser.add_argument('--rounds', type=int, default=3)
    args = parser.parse_args()
    work = Path(tempfile.mkdtemp(prefix='pure-messenger-', dir=args.directory))
    try:
        module = work / 'messenger.pmod'
        subprocess.run([str(args.aup), 'build', str(args.vega / 'examples' / 'messenger.aup'), '-o', str(module)],
                       env=dict(os.environ, AUP_PATH=str(args.vega / 'lib')), check=True)
        for round_ in range(args.rounds):
            for sealed in (True, False) if round_ % 2 == 0 else (False, True):
                print(json.dumps({'round': round_, 'directory': str(args.directory),
                                  **run(args.serve, module, work, sealed, args.lines)}), flush=True)
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
