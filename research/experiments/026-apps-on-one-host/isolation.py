#!/usr/bin/env python3
"""Two apps, A and B (benchmarks/http/scores.aup, the scoreboard), as two
pure-serve processes or as one host of two apps (docs/design/017), in RAM
(/dev/shm), on cores 0-2; the load (benchmarks/http/load) on core 3. The
two ways have four threads for the apps: two processes of one connection
thread and an owner each, or one host of two connection threads and two
workers. A third, host1, is a host of one connection thread and two
workers (three threads).

Each round, both ways in turns (the order flipped every other round):
  alone: B's writes, one connection, nothing else;
  flood: A's writes from 64 connections, and B's from one, at once;
  both:  A's and B's writes from 32 connections each.
Prints a JSON line per run: B's p50 and p99, each app's writes a second.

isolation.py SERVE ROUNDS [SECONDS]
"""
import json
import select
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
LOAD = ROOT / 'benchmarks' / 'http' / 'load'
AUP = ROOT.parent / 'aup' / 'build' / 'aup'


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


def load(port, connections, seconds, prefix):
    return subprocess.Popen(['taskset', '-c', '3', str(LOAD), str(port), str(connections), str(seconds), 'score',
                             str(connections), 'keep-alive', '16', prefix], stdout=subprocess.PIPE, text=True)


def result(process):
    out, _ = process.communicate()
    return json.loads(out)


def servers(way, work, serve, module):
    """Where A and B are: ([processes], {app: (port, prefix)})."""
    if way == 'processes':
        a, info_a = started([serve, '--module', module, '--store', work / 'a', '--threads', 1])
        b, info_b = started([serve, '--module', module, '--store', work / 'b', '--threads', 1])
        return [a, b], {'A': (info_a['port'], ''), 'B': (info_b['port'], '')}
    (work / 'apps.conf').write_text(f'a {module} {work / "a"}\nb {module} {work / "b"}\n')
    host, info = started([serve, '--apps', work / 'apps.conf', '--threads', 1 if way == 'host1' else 2, '--workers', 2])
    return [host], {'A': (info['port'], '/a'), 'B': (info['port'], '/b')}


def run(way, kind, seconds, serve, module, round_):
    work = Path(tempfile.mkdtemp(prefix='pure-apps-', dir='/dev/shm'))
    try:
        processes, where = servers(way, work, serve, module)
        try:
            if kind == 'alone':
                b = result(load(where['B'][0], 1, seconds, where['B'][1]))
                a = None
            elif kind == 'flood':
                flood = load(where['A'][0], 64, seconds + 2, where['A'][1])
                time.sleep(1)
                b = result(load(where['B'][0], 1, seconds, where['B'][1]))
                a = result(flood)
            else:
                both_a = load(where['A'][0], 32, seconds, where['A'][1])
                both_b = load(where['B'][0], 32, seconds, where['B'][1])
                a, b = result(both_a), result(both_b)
        finally:
            for process in processes:
                stopped(process)
    finally:
        shutil.rmtree(work)
    row = {'round': round_, 'way': way, 'kind': kind, 'b_p50_us': b['p50_us'], 'b_p99_us': b['p99_us'],
           'b_per_second': b['requests_per_second'], 'b_5xx': b['5xx']}
    if a:
        row.update({'a_per_second': a['requests_per_second'], 'a_p99_us': a['p99_us'], 'a_5xx': a['5xx']})
    return row


def main():
    serve, rounds = Path(sys.argv[1]).resolve(), int(sys.argv[2])
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 15
    work = Path(tempfile.mkdtemp(prefix='pure-apps-module-'))
    try:
        module = work / 'scores.pmod'
        subprocess.run([str(AUP), 'build', str(ROOT / 'benchmarks' / 'http' / 'scores.aup'), '-o', str(module)], check=True)
        for round_ in range(1, rounds + 1):
            ways = ['processes', 'host', 'host1'] if round_ % 2 else ['host1', 'host', 'processes']
            for kind in ('alone', 'flood', 'both'):
                for way in ways:
                    print(json.dumps(run(way, kind, seconds, serve, module, round_)), flush=True)
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
