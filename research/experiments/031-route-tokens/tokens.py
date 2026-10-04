#!/usr/bin/env python3
"""What an app's tokens cost its requests (docs/design/017's 3b, research 116).
A host of apps (one connection thread, two workers, stores in RAM, cores
0-2) holds the scoreboard (benchmarks/http/scores.aup) twice: open, with
no tokens, and held, whose tokens file grants a write token. The load
(benchmarks/http/load, core 3) reads its total view for SECONDS on
CONNECTIONS connections, keep-alive (a connection's token hashed once) or
close (a connection a request: hashed every time): open with no token,
open bearing the token all the same (its head as long, nothing checked),
and held; the three in turns, their order turned each round.

tokens.py PURE_SERVE SECONDS ROUNDS   a JSON line per run, then medians
"""
import hashlib
import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
AUP = ROOT.parent / 'aup' / 'build' / 'aup'
LOAD = ROOT / 'benchmarks' / 'http' / 'load'
TOKEN = 'a' * 64


def load(port, way, connections, seconds, mode):
    env = dict(os.environ)
    app = 'held' if way == 'held' else 'open'
    if way != 'open':
        env['PURE_LOAD_TOKEN'] = TOKEN
    out = subprocess.run(['taskset', '-c', '3', str(LOAD), str(port), str(connections), str(seconds), 'total', '1', mode,
                          '16', f'/{app}'], capture_output=True, text=True, env=env, check=True).stdout
    return json.loads(out)


def main():
    serve, seconds, rounds = Path(sys.argv[1]).resolve(), int(sys.argv[2]), int(sys.argv[3])
    work = Path(tempfile.mkdtemp(prefix='pure-031-', dir='/dev/shm'))
    try:
        module = work / 'scores.pmod'
        subprocess.run([str(AUP), 'build', str(ROOT / 'benchmarks' / 'http' / 'scores.aup'), '-o', str(module)], check=True)
        (work / 'tokens').write_text(f'write {hashlib.sha256(TOKEN.encode()).hexdigest()}\n')
        (work / 'apps.conf').write_text(f"open {module} {work / 'open'}\nheld {module} {work / 'held'} "
                                        f"tokens={work / 'tokens'}\n")
        host = subprocess.Popen(['taskset', '-c', '0-2', str(serve), '--apps', str(work / 'apps.conf'), '--port', '0',
                                 '--threads', '1', '--workers', '2'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        port = json.loads(host.stdout.readline())['port']
        rates = {}
        for mode, connections in (('keep-alive', 64), ('close', 16)):
            for round_ in range(1, rounds + 1):
                ways = ['open', 'bearing', 'held']
                for way in (ways if round_ % 2 else ways[::-1]):
                    result = load(port, way, connections, seconds, mode)
                    assert result['2xx'] == result['completed'] and not result['errors'], result
                    rates.setdefault((mode, way), []).append(result['requests_per_second'])
                    print(json.dumps({'mode': mode, 'round': round_, 'way': way, **result}), flush=True)
        host.terminate()
        _, error = host.communicate(timeout=60)
        stats = json.loads(error.decode().strip().splitlines()[-1])
        for mode in ('keep-alive', 'close'):
            open_, bearing, held = (statistics.median(rates[(mode, way)]) for way in ('open', 'bearing', 'held'))
            print(json.dumps({'mode': mode, 'open_median': open_, 'bearing_median': bearing, 'held_median': held,
                              'bearing_change': round((bearing / open_ - 1) * 100, 2),
                              'held_change': round((held / open_ - 1) * 100, 2)}), flush=True)
        print(json.dumps({'unauthorized': stats['unauthorized'], 'forbidden': stats['forbidden']}), flush=True)
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
