#!/usr/bin/env python3
"""How fast delivered sends drain, sealed and not (docs/research/086-sealed-sends.md).

A sender app (tests/check.py's LINK_SENDER) takes --sends orders from eight
clients while its receiver is stopped (SIGSTOP), so the sends pile up in the
sender's store; the receiver is then continued and timed until it has
applied them all. Runs plain and sealed (--seal to the receiver's --key) in
alternating order per round, and a --baseline pure-serve (plain only) when
given. Both servers on cores 0-2. Then the AEADs and X25519 on one core
(openssl speed). Prints one JSON line per run.
"""
import argparse
import json
import os
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from http.client import HTTPConnection
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
import check  # noqa: E402


def start(args):
    process = subprocess.Popen(['taskset', '-c', '0-2'] + [str(a) for a in args] + ['--port', '0'],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ready, _, _ = select.select([process.stdout], [], [], 10)
    line = process.stdout.readline() if ready else b''
    assert line, process.communicate()[1][-1000:]
    return process, json.loads(line)


def count(port):
    connection = HTTPConnection('127.0.0.1', port, timeout=5)
    connection.request('GET', '/views/count')
    value = int(connection.getresponse().read())
    connection.close()
    return value


def run(work, binary, sends, sealed):
    for name in ('a', 'b', 'key'):
        shutil.rmtree(work / name, ignore_errors=True)
    (work / 'key').mkdir()
    key = ['--key', work / 'key' / 'receiver.key'] if sealed else []
    receiver, started = start([binary, '--module', work / 'receiver.pmod', '--store', work / 'b', *key])
    os.kill(receiver.pid, signal.SIGSTOP)
    seal = ['--seal', f'ordered={started["public_key"]}'] if sealed else []
    sender, sender_started = start([binary, '--module', work / 'sender.pmod', '--store', work / 'a',
                                    '--send', f'ordered=http://127.0.0.1:{started["port"]}/inputs/order', *seal])

    def client(first):
        connection = HTTPConnection('127.0.0.1', sender_started['port'], timeout=30)
        for i in range(first, sends, 8):
            body = json.dumps({'item': f'i{i}', 'count': 1}).encode()
            connection.request('POST', '/inputs/buy', body, {'Content-Type': 'application/json'})
            connection.getresponse().read()
        connection.close()

    clients = [threading.Thread(target=client, args=(k,)) for k in range(8)]
    for thread in clients:
        thread.start()
    for thread in clients:
        thread.join()
    began = time.perf_counter()
    os.kill(receiver.pid, signal.SIGCONT)
    while count(started['port']) < sends:
        time.sleep(0.01)
    elapsed = time.perf_counter() - began
    stats = []
    for process in (sender, receiver):
        process.terminate()
        _, error = process.communicate()
        stats.append([json.loads(line) for line in error.decode().splitlines() if line.startswith('{"active"')][-1])
    return {'sends_per_second': round(sends / elapsed), 'receiver_requests': stats[1]['requests'],
            'delivery_attempts': stats[0]['delivery_attempts']}


def speeds():
    for size in (64, 1024, 16384):
        for cipher in ('aes-128-gcm', 'chacha20-poly1305'):
            out = subprocess.run(['taskset', '-c', '3', 'openssl', 'speed', '-seconds', '2', '-bytes', str(size), '-evp',
                                  cipher], capture_output=True, text=True, check=True).stdout.splitlines()[-1]
            yield {'aead': cipher, 'bytes': size, 'kb_per_second': float(out.split()[-1].rstrip('k'))}
    out = subprocess.run(['taskset', '-c', '3', 'openssl', 'speed', '-seconds', '2', 'ecdhx25519'],
                         capture_output=True, text=True, check=True).stdout.splitlines()[-1]
    yield {'x25519_per_second': float(out.split()[-1])}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--aup', type=Path, default=ROOT.parent / 'aup' / 'build' / 'aup')
    parser.add_argument('--binary', type=Path, default=ROOT / 'build' / 'pure-serve')
    parser.add_argument('--baseline', type=Path, help='an earlier pure-serve, run plain')
    parser.add_argument('--sends', type=int, default=20000)
    parser.add_argument('--rounds', type=int, default=3)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='pure-drain-') as directory:
        work = Path(directory)
        for name, text in (('sender', check.LINK_SENDER), ('receiver', check.LINK_RECEIVER)):
            (work / f'{name}.aup').write_text(text)
            check.compile_module(args.aup.resolve(), work / f'{name}.aup', work / f'{name}.pmod')
        for round_ in range(args.rounds):
            variants = [('plain', args.binary, False), ('sealed', args.binary, True)]
            if args.baseline:
                variants.append(('baseline', args.baseline.resolve(), False))
            shift = round_ % len(variants)
            for name, binary, sealed in variants[shift:] + variants[:shift]:
                result = run(work, binary, args.sends, sealed)
                print(json.dumps({'round': round_, 'variant': name, 'sends': args.sends, **result}), flush=True)
    for line in speeds():
        print(json.dumps(line), flush=True)


if __name__ == '__main__':
    main()
