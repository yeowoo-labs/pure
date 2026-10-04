#!/usr/bin/env python3
"""What a subscriber costs writes (docs/design/019, research 120). pure-serve
(connection threads 3, store in RAM, cores 0-2) holds the scoreboard
(benchmarks/http/scores.aup); benchmarks/http/load (core 3) posts points
on 64 connections for SECONDS, with no subscriber, or with SUBSCRIBERS
following its total view (which every write changes), in turns, ROUNDS
times.

under_load.py PURE_SERVE AUP SECONDS ROUNDS SUBSCRIBERS   a JSON line per run, then medians
"""
import json
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
LOAD = ROOT / 'benchmarks' / 'http' / 'load'


def main():
    serve, aup, seconds, rounds, subscribers = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), *sys.argv[3:6]
    work = Path(tempfile.mkdtemp(prefix='pure-035-load-', dir='/dev/shm'))
    try:
        subprocess.run([str(aup), 'build', str(ROOT / 'benchmarks' / 'http' / 'scores.aup'), '-o', str(work / 'scores.pmod')],
                       check=True)
        host = subprocess.Popen(['taskset', '-c', '0-2', str(serve), '--module', str(work / 'scores.pmod'), '--store',
                                 str(work / 'store'), '--port', '0', '--threads', '3'], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        port = json.loads(host.stdout.readline())['port']
        rates = {}
        for round_ in range(1, int(rounds) + 1):
            ways = ['none', 'subscribed']
            for way in (ways if round_ % 2 else ways[::-1]):
                socks, taken, stop = [], [0], threading.Event()
                if way == 'subscribed':
                    for _ in range(int(subscribers)):
                        s = socket.create_connection(('127.0.0.1', port))
                        s.sendall(b'GET /views/total HTTP/1.1\r\nHost: x\r\nAccept: text/event-stream\r\n\r\n')
                        socks.append(s)

                    def follow():
                        while not stop.is_set():
                            for s in socks:
                                s.setblocking(False)
                                try:
                                    while s.recv(65536):
                                        taken[0] += 1
                                except BlockingIOError:
                                    pass
                            time.sleep(0.005)

                    reader = threading.Thread(target=follow)
                    reader.start()
                    time.sleep(0.5)
                out = subprocess.run(['taskset', '-c', '3', str(LOAD), str(port), '64', seconds, 'score', '64', 'keep-alive'],
                                     capture_output=True, text=True, check=True).stdout
                result = json.loads(out)
                stop.set()
                for s in socks:
                    s.close()
                if socks:
                    reader.join()
                assert result['2xx'] == result['completed'] and not result['errors'], result
                rates.setdefault(way, []).append(result['requests_per_second'])
                print(json.dumps({'round': round_, 'way': way, 'writes_per_second': result['requests_per_second'],
                                  'p99_us': result['p99_us'], 'reads_by_subscribers': taken[0]}), flush=True)
                time.sleep(0.5)
        host.terminate()
        _, err = host.communicate(timeout=60)
        stats = json.loads(err.decode().strip().splitlines()[-1])
        none, subscribed = statistics.median(rates['none']), statistics.median(rates['subscribed'])
        print(json.dumps({'none_median': none, 'subscribed_median': subscribed, 'change': round((subscribed / none - 1) * 100, 1),
                          'pushes': stats['pushes'], 'pushed': stats['pushed']}), flush=True)
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
