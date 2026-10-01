#!/usr/bin/env python3
"""Sends between apps of one host over links or HTTP wires, before and
after one sending loop (docs/design/017, 2b). src holds 1,000 items; a
burst walks them and sends one message each to one output (link, wire),
or to each of four outputs, each to its own app (links4, wires4). A link
goes to the host's own --link-port, sealed with its key; a wire posts to
the host's own HTTP port. A run: BURSTS bursts posted one after another,
then the time until every receiving app has stored every send, the host's
CPU over it (utime + stime), its threads, and the packets its links sent;
the receivers' sums say each send came once. In RAM, the host on cores 0-2.
The builds take turns, their order turned each round.

sends.py ROUNDS BURSTS WAY[,WAY...] NAME=PURE_SERVE [NAME=PURE_SERVE ...]
    a JSON line per run (each pure-serve in a directory of its own, with
    its pure-crypto.so)
"""
import http.client
import json
import os
import select
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
AUP = ROOT.parent / 'aup' / 'build' / 'aup'
ITEMS = 1000


def free_port(kind):
    with socket.socket(socket.AF_INET, kind) as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def started(args):
    process = subprocess.Popen(['taskset', '-c', '0-2', *map(str, args)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ready, _, _ = select.select([process.stdout], [], [], 10)
    assert ready, process.stderr.read()
    return process, json.loads(process.stdout.readline())


def stopped(process):
    process.terminate()
    _, error = process.communicate(timeout=60)
    assert process.returncode == 0, error[-500:]
    return json.loads(error.decode().strip().splitlines()[-1])


def cpu_seconds(pid):
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')


def threads(pid):
    return int(Path(f'/proc/{pid}/status').read_text().split('Threads:')[1].split()[0])


def run(way, serve, work, modules, key, public, bursts):
    link_port, http_port = free_port(socket.SOCK_DGRAM), free_port(socket.SOCK_STREAM)
    outputs = ['o1', 'o2', 'o3', 'o4'] if way.endswith('4') else ['out']
    lines = []
    for n, output in enumerate(outputs, 1):
        to = (f'pure://127.0.0.1:{link_port}/d{n}/take seal={output}={public}' if way.startswith('link')
              else f'http://127.0.0.1:{http_port}/d{n}/inputs/take')
        lines.append(f'send={output}={to}')
    conf = f"src {modules['src']} {work / 'src'} {' '.join(lines)}\n" + \
        ''.join(f"d{n} {modules['dst']} {work / f'd{n}'}\n" for n in range(1, len(outputs) + 1))
    (work / 'apps.conf').write_text(conf)
    args = [serve, '--apps', work / 'apps.conf', '--threads', 1, '--workers', 2, '--port', http_port]
    if way.startswith('link'):
        args += ['--key', key, '--link-port', link_port, *sum((['--sender', f'src.{o}={public}'] for o in outputs), [])]
    load = os.getloadavg()[0]
    process, info = started(args)
    client = http.client.HTTPConnection('127.0.0.1', info['port'], timeout=60)
    counts = {}

    def ask(method, path):
        client.request(method, path, b'' if method == 'POST' else None)
        answer = client.getresponse()
        body = answer.read()
        assert answer.status == 200, (path, answer.status, answer.getheader('X-Pure-Error'))
        return body

    try:
        for k in range(1, ITEMS + 1):
            ask('POST', f'/src/inputs/fill?k={k}')
        alive = threads(process.pid)
        began, cpu = time.perf_counter(), cpu_seconds(process.pid)
        for _ in range(bursts):
            ask('POST', '/src/inputs/burst4?step=1' if len(outputs) == 4 else '/src/inputs/burst?step=1')
        while any(int(ask('GET', f'/d{n}/views/count')) < bursts * ITEMS for n in range(1, len(outputs) + 1)):
            time.sleep(0.005)
        took, used = time.perf_counter() - began, cpu_seconds(process.pid) - cpu
        for n in range(1, len(outputs) + 1):
            assert int(ask('GET', f'/d{n}/views/sum')) == bursts * (ITEMS * (ITEMS + 1) // 2 + ITEMS)
    finally:
        client.close()
        counts = stopped(process)
    sent = bursts * ITEMS * len(outputs)
    return {'way': way, 'sends': sent, 'seconds': round(took, 3), 'sends_per_second': round(sent / took),
            'cpu_us_per_send': round(used * 1e6 / sent, 2), 'threads': alive,
            'packets': counts.get('link_packets', 0), 'load': round(load, 2)}


def main():
    rounds, bursts, ways = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3].split(',')
    serves = {name: Path(path).resolve() for name, path in (a.split('=', 1) for a in sys.argv[4:])}
    names = list(serves)
    work = Path(tempfile.mkdtemp(prefix='pure-loop-', dir='/dev/shm'))
    try:
        modules = {}
        for name in ('src', 'dst'):
            modules[name] = work / f'{name}.pmod'
            subprocess.run([str(AUP), 'build', str(HERE / f'{name}.aup'), '-o', str(modules[name])], check=True)
        key = work / 'host.key'
        (work / 'one.conf').write_text(f"x {modules['dst']} {work / 'keys'}\n")
        process, info = started([serves[names[-1]], '--apps', work / 'one.conf', '--key', key, '--port', 0])
        public = info['public_key']
        stopped(process)
        for round_ in range(1, rounds + 1):
            for way in ways:
                turn = (round_ - 1) % len(names)
                for version in names[turn:] + names[:turn]:
                    runs = work / f'run-{round_}-{way}-{version}'
                    runs.mkdir()
                    row = run(way, serves[version], runs, modules, key, public, bursts)
                    print(json.dumps({'round': round_, 'version': version, **row}), flush=True)
                    shutil.rmtree(runs)
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
