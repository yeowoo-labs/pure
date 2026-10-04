#!/usr/bin/env python3
"""Another app's latency while one app changes again and again on a running
host (docs/design/017's stage 3, research 115).

A host of apps (two connection threads, two workers, stores in RAM, cores
0-2) holds A, an app of items.aup with ITEMS items, and B, the scoreboard
(benchmarks/http/scores.aup). B takes one connection's scores
(benchmarks/http/load on core 3) for SECONDS, while A is left be (quiet)
or changes every EVERY seconds (changing: SIGHUP, its line naming in turn
items.pmod and NEXT). The two take turns, their order turned each round.

isolation.py PURE_SERVE ITEMS NEXT EVERY SECONDS ROUNDS   a JSON line per run,
                                                          then one with medians
"""
import json
import select
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

from window import AUP, HERE, grow

ROOT = HERE.parents[2]
LOAD = ROOT / 'benchmarks' / 'http' / 'load'


def main():
    serve, items, after = Path(sys.argv[1]).resolve(), int(sys.argv[2]), sys.argv[3]
    every, seconds, rounds = float(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
    work = Path(tempfile.mkdtemp(prefix='pure-030-', dir='/dev/shm'))
    try:
        modules = {}
        for name, source in (('items', HERE / 'items.aup'), (after, HERE / f'{after}.aup'),
                             ('scores', ROOT / 'benchmarks' / 'http' / 'scores.aup')):
            modules[name] = work / f'{name}.pmod'
            subprocess.run([str(AUP), 'build', str(source), '-o', str(modules[name])], check=True)
        conf = work / 'apps.conf'

        def write(module):
            conf.write_text(f"a {modules[module]} {work / 'a'}\nb {modules['scores']} {work / 'b'}\n")

        write('items')
        host = subprocess.Popen(['taskset', '-c', '0-2', str(serve), '--apps', str(conf), '--port', '0', '--threads', '2',
                                 '--workers', '2'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        port = json.loads(host.stdout.readline())['port']
        count = grow(port, items, 'a')
        p99 = {'quiet': [], 'changing': []}
        changes = 0
        for round_ in range(1, rounds + 1):
            for way in (['quiet', 'changing'] if round_ % 2 else ['changing', 'quiet']):
                load = subprocess.Popen(['taskset', '-c', '3', str(LOAD), str(port), '1', str(seconds), 'score', '1',
                                         'keep-alive', '16', '/b'], stdout=subprocess.PIPE, text=True)
                made = 0
                while way == 'changing' and load.poll() is None:
                    write(after if changes % 2 == 0 else 'items')
                    host.send_signal(signal.SIGHUP)
                    ready, _, _ = select.select([host.stdout], [], [], 60)
                    assert ready
                    said = json.loads(host.stdout.readline())
                    assert said['apps'][0]['running'], said
                    changes += 1
                    made += 1
                    time.sleep(every)
                out, _ = load.communicate()
                result = json.loads(out)
                assert result['5xx'] == 0 and result['errors'] == 0, result
                p99[way].append(result['p99_us'])
                print(json.dumps({'round': round_, 'way': way, 'items': count, 'next': after, 'changes': made, **result}),
                      flush=True)
        host.terminate()
        host.communicate(timeout=600)
        quiet, changing = statistics.median(p99['quiet']), statistics.median(p99['changing'])
        print(json.dumps({'items': count, 'next': after, 'every_s': every, 'quiet_p99_us': quiet,
                          'changing_p99_us': changing, 'ratio': round(changing / quiet, 2)}), flush=True)
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
