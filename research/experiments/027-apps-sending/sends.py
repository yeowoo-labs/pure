#!/usr/bin/env python3
"""Two apps of one host (docs/design/017, stage 2), src sending to dst
either through the local outlet (send=out=dst/take) or over a loopback
pure:// link to the host's own --link-port (sealed with its key). src
holds 1,000 items; each burst event walks them and sends one message
each, so sending (not HTTP) is what the host does. A run: BURSTS bursts
posted one after another, then the time until dst has stored every send,
and the host's CPU over it (utime + stime). dst's sum says each send came
once. In RAM, the host on cores 0-2.

sends.py SERVE ROUNDS [BURSTS]   prints a JSON line per run
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


def started(args):
    process = subprocess.Popen(['taskset', '-c', '0-2', *map(str, args), '--port', '0'], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
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


def run(way, serve, work, modules, key, public, bursts):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1', 0))
        link_port = probe.getsockname()[1]
    to = 'dst/take' if way == 'local' else f'pure://127.0.0.1:{link_port}/dst/take seal=out={public}'
    (work / 'apps.conf').write_text(f"src {modules['src']} {work / 'src'} send=out={to}\n"
                                    f"dst {modules['dst']} {work / 'dst'}\n")
    args = [serve, '--apps', work / 'apps.conf', '--threads', 1, '--workers', 2]
    if way == 'link':
        args += ['--key', key, '--link-port', link_port, '--sender', f'src.out={public}']
    process, info = started(args)
    client = http.client.HTTPConnection('127.0.0.1', info['port'], timeout=60)

    def ask(method, path):
        client.request(method, path, b'' if method == 'POST' else None)
        answer = client.getresponse()
        body = answer.read()
        assert answer.status == 200, (path, answer.status)
        return body

    try:
        for k in range(1, ITEMS + 1):
            ask('POST', f'/src/inputs/fill?k={k}')
        began, cpu = time.perf_counter(), cpu_seconds(process.pid)
        for _ in range(bursts):
            ask('POST', '/src/inputs/burst?step=1')
        sent = bursts * ITEMS
        while int(ask('GET', '/dst/views/count')) < sent:
            time.sleep(0.005)
        took, used = time.perf_counter() - began, cpu_seconds(process.pid) - cpu
        total = int(ask('GET', '/dst/views/sum'))
        assert int(ask('GET', '/dst/views/count')) == sent and total == bursts * (ITEMS * (ITEMS + 1) // 2 + ITEMS), total
    finally:
        client.close()
        stats = stopped(process)
    return {'way': way, 'sends': sent, 'seconds': round(took, 3), 'sends_per_second': round(sent / took),
            'cpu_us_per_send': round(used * 1e6 / sent, 2), 'handed': stats.get('handed', 0),
            'link_packets': stats['link_packets']}


def main():
    serve, rounds = Path(sys.argv[1]).resolve(), int(sys.argv[2])
    bursts = int(sys.argv[3]) if len(sys.argv) > 3 else 60
    work = Path(tempfile.mkdtemp(prefix='pure-sends-', dir='/dev/shm'))
    try:
        modules = {}
        for name in ('src', 'dst'):
            modules[name] = work / f'{name}.pmod'
            subprocess.run([str(AUP), 'build', str(HERE / f'{name}.aup'), '-o', str(modules[name])], check=True)
        key = work / 'host.key'
        (work / 'one.conf').write_text(f"x {modules['dst']} {work / 'keys'}\n")
        process, info = started([serve, '--apps', work / 'one.conf', '--key', key])
        public = info['public_key']
        stopped(process)
        for round_ in range(1, rounds + 1):
            for way in (['local', 'link'] if round_ % 2 else ['link', 'local']):
                runs = work / f'run-{round_}-{way}'
                runs.mkdir()
                row = run(way, serve, runs, modules, key, public, bursts)
                print(json.dumps({'round': round_, **row}), flush=True)
                shutil.rmtree(runs)
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
