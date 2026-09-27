#!/usr/bin/env python3
"""A bank served by pure-serve under pgbench's TPC-B-like transaction.

bank.aup keeps accounts, tellers and branches as lists in key order and a
history list; a transfer updates one row of each of the three and appends a
history row, in one event. For each --accounts count: a fresh store, the
branches, tellers and accounts of that scale made by posts (timed), then
--seconds of transfers from --connections clients (benchmarks/http/load
transfer), servers on cores 0-2 and the load on core 3. Prints one JSON line
per count: setup time, transfers a second, latency, and server CPU time per
transfer.
"""
import argparse
import http.client
import json
import os
import select
import shutil
import subprocess
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def cpu_seconds(pid):
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')


def post_all(port, path, bodies, threads=8):
    """Each body posted once, from threads keep-alive connections."""
    failed = []

    def worker(part):
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=60)
        for body in part:
            connection.request('POST', path, body, {'Content-Type': 'application/json'})
            answer = connection.getresponse()
            answer.read()
            if answer.status != 200:
                failed.append((body, answer.status))
        connection.close()

    workers = [threading.Thread(target=worker, args=(bodies[i::threads],)) for i in range(threads)]
    for w in workers:
        w.start()
    for w in workers:
        w.join()
    assert not failed, failed[:3]


def run(serve, module, directory, accounts, seconds, connections, threads):
    store = directory / f'bank-{accounts}'
    shutil.rmtree(store, ignore_errors=True)
    process = subprocess.Popen(['taskset', '-c', '0-2', str(serve), '--module', str(module), '--store', str(store),
                                '--port', '0', '--threads', str(threads)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        select.select([process.stdout], [], [], 10)
        port = json.loads(process.stdout.readline())['port']
        scale = max(1, accounts // 100000)
        began = time.perf_counter()
        post_all(port, '/inputs/branch', [json.dumps({'bid': b + 1}).encode() for b in range(scale)], 1)
        post_all(port, '/inputs/teller', [json.dumps({'tid': t + 1, 'bid': t // 10 + 1}).encode()
                                          for t in range(10 * scale)], 1)
        post_all(port, '/inputs/account', [json.dumps({'aid': a + 1, 'bid': a // 100000 + 1}).encode()
                                           for a in range(accounts)])
        setup = time.perf_counter() - began
        used = cpu_seconds(process.pid)
        load = subprocess.run(['taskset', '-c', '3', str(ROOT / 'benchmarks' / 'http' / 'load'), str(port),
                               str(connections), str(seconds), 'transfer', str(accounts), 'keep-alive'],
                              capture_output=True, text=True, check=True)
        result = json.loads(load.stdout)
        used = cpu_seconds(process.pid) - used
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=30)
        connection.request('GET', '/views/history_count')
        history = int(connection.getresponse().read())
        connection.close()
        # Every transfer answered is in the history, and those in flight when the load stopped may be too.
        assert result['2xx'] <= history <= result['2xx'] + connections, (history, result)
        return {'accounts': accounts, 'setup_s': round(setup, 2), 'transfers_per_second': result['requests_per_second'],
                'p50_us': result['p50_us'], 'p99_us': result['p99_us'], 'errors': result['errors'],
                'non_2xx': result['completed'] - result['2xx'],
                'cpu_us_per_transfer': round(used / max(result['2xx'], 1) * 1e6, 1)}
    finally:
        process.terminate()
        process.communicate(timeout=60)
        shutil.rmtree(store, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--aup', type=Path, default=ROOT.parent / 'aup' / 'build' / 'aup')
    parser.add_argument('--serve', type=Path, default=ROOT / 'build' / 'pure-serve')
    parser.add_argument('--directory', type=Path, default=Path('/dev/shm'))
    parser.add_argument('--accounts', default='1000,10000,30000')
    parser.add_argument('--seconds', type=float, default=5)
    parser.add_argument('--connections', type=int, default=16)
    parser.add_argument('--threads', type=int, default=1)
    args = parser.parse_args()
    work = Path(tempfile.mkdtemp(prefix='pure-bank-'))
    try:
        module = work / 'bank.pmod'
        subprocess.run([str(args.aup), 'build', str(HERE / 'bank.aup'), '-o', str(module)], check=True)
        for accounts in (int(a) for a in args.accounts.split(',')):
            print(json.dumps({'directory': str(args.directory), **run(args.serve, module, args.directory, accounts,
                                                                      args.seconds, args.connections, args.threads)}),
                  flush=True)
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
