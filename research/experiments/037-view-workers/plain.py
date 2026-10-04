#!/usr/bin/env python3
"""A view of one expression read, before and after (docs/design/020's stage
2, research 122): the scoreboard (benchmarks/http/scores.aup) on pure-serve
BEFORE and AFTER in turns (connection threads 3, store in RAM, cores 0-2,
a fresh host each run), 1,000 points posted first, then benchmarks/http/load
(core 3) reads GET /views/total on 64 connections for SECONDS. ROUNDS
pairs, the first in each even round the after one.

plain.py BEFORE AFTER AUP SECONDS ROUNDS   a JSON line per run, then medians
"""
import json
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
LOAD = ROOT / 'benchmarks' / 'http' / 'load'


def run(serve, module, work, seconds):
    store = work / 'store'
    shutil.rmtree(store, ignore_errors=True)
    host = subprocess.Popen(['taskset', '-c', '0-2', str(serve), '--module', str(module), '--store', str(store),
                             '--port', '0', '--threads', '3'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        port = json.loads(host.stdout.readline())['port']
        filled = json.loads(subprocess.run(['taskset', '-c', '3', str(LOAD), str(port), '8', '1', 'score', '64',
                                            'keep-alive'], capture_output=True, text=True, check=True).stdout)
        assert filled['2xx'] == filled['completed'] and filled['completed'] > 0, filled
        result = json.loads(subprocess.run(['taskset', '-c', '3', str(LOAD), str(port), '64', str(seconds), 'total', '64',
                                            'keep-alive'], capture_output=True, text=True, check=True).stdout)
        assert result['2xx'] == result['completed'] and not result['errors'], result
        return result
    finally:
        host.terminate()
        host.communicate(timeout=60)


def main():
    before, after, aup = (Path(a).resolve() for a in sys.argv[1:4])
    seconds, rounds = int(sys.argv[4]), int(sys.argv[5])
    work = Path(tempfile.mkdtemp(prefix='pure-037-plain-', dir='/dev/shm'))
    try:
        module = work / 'scores.pmod'
        subprocess.run([str(aup), 'build', str(ROOT / 'benchmarks' / 'http' / 'scores.aup'), '-o', str(module)], check=True)
        rates = {'before': [], 'after': []}
        for round_ in range(1, rounds + 1):
            order = ['before', 'after'] if round_ % 2 else ['after', 'before']
            for version in order:
                result = run(before if version == 'before' else after, module, work, seconds)
                rates[version].append(result['requests_per_second'])
                print(json.dumps({'round': round_, 'version': version, 'reads_per_second': result['requests_per_second'],
                                  'p50_us': result['p50_us'], 'p99_us': result['p99_us']}), flush=True)
        b, a = statistics.median(rates['before']), statistics.median(rates['after'])
        print(json.dumps({'before_median': b, 'after_median': a, 'change': round((a / b - 1) * 100, 1)}), flush=True)
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
