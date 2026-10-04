#!/usr/bin/env python3
"""Views that compute, beside writes (docs/design/018 stage 3, research
119). pure-serve (THREADS connection threads, store in RAM, cores 0-2)
holds graph.aup with a random graph of NODES nodes and EDGES edges (seed
119); a second one like it (apart) shares the cores. In turns, for SECONDS
each, ROUNDS rounds:
  writes   benchmarks/http/load posts points (16 connections, keep-alive,
           core 3): the owner applies them
  both     the same, while ASKERS connections ask the distance view (its
           body a breadth first search) without pause, random arguments
  apart    the same, the askers asking the second host: the cores shared,
           not the host
  events   the same, the askers posting the search as events (route): on
           the owner
  one, many   the askers alone, 1 and ASKERS of them
Every view answer is checked against a model, made before any run is
timed: the askers start from 64 nodes picked beforehand (a model made while
asking would time Python, not the host).

views.py PURE_SERVE AUP NODES EDGES THREADS ASKERS SECONDS ROUNDS [WAYS]   a JSON line per run, then medians
(WAYS: the ways to run, by commas; all by default)
"""
import http.client
import json
import random
import shutil
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOAD = HERE.parents[2] / 'benchmarks' / 'http' / 'load'


def main():
    serve, aup = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    nodes, edges_wanted, threads, askers, seconds, rounds = (int(a) for a in sys.argv[3:9])
    chosen = sys.argv[9].split(',') if len(sys.argv) > 9 else ['writes', 'both', 'apart', 'events', 'one', 'many']
    rng = random.Random(119)
    pairs = set()
    while len(pairs) < edges_wanted:
        a, b = rng.randrange(nodes), rng.randrange(nodes)
        if a != b:
            pairs.add((a, b))
    edges = sorted(pairs)
    out = [[] for _ in range(nodes)]
    for a, b in edges:
        out[a].append(b)
    model = {}

    def far(a, b):
        if a not in model:
            depth, frontier = {a: 0}, [a]
            for v in frontier:
                for t in out[v]:
                    if t not in depth:
                        depth[t] = depth[v] + 1
                        frontier.append(t)
            model[a] = depth
        return model[a].get(b, -1)

    work = Path(tempfile.mkdtemp(prefix='pure-034-', dir='/dev/shm'))
    try:
        subprocess.run([str(aup), 'build', str(HERE / 'graph.aup'), '-o', str(work / 'graph.pmod')], check=True)
        host = subprocess.Popen(['taskset', '-c', '0-2', str(serve), '--module', str(work / 'graph.pmod'), '--store',
                                 str(work / 'store'), '--port', '0', '--threads', str(threads)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        port = json.loads(host.stdout.readline())['port']
        apart = subprocess.Popen(['taskset', '-c', '0-2', str(serve), '--module', str(work / 'graph.pmod'), '--store',
                                  str(work / 'apart'), '--port', '0', '--threads', str(threads)],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        apart_port = json.loads(apart.stdout.readline())['port']
        for target in (port, apart_port):
            setup = http.client.HTTPConnection('127.0.0.1', target, timeout=600)

            def post(path, body=b''):
                setup.request('POST', path, body, {'Content-Length': str(len(body))})
                answer = setup.getresponse()
                assert answer.status == 200 and answer.read() is not None, path

            post(f'/inputs/size?nodes={nodes}')
            for k in range(0, len(edges), 80000):
                post('/inputs/load', b''.join(struct.pack('<dd', a * 1048576 + b, b) for a, b in edges[k:k + 80000]))

        starts = [rng.randrange(nodes) for _ in range(64)]
        for a in starts:
            far(a, a)

        def ask(target, until, seed, done, event=False):
            r = random.Random(seed)
            c = http.client.HTTPConnection('127.0.0.1', target, timeout=600)
            while time.monotonic() < until:
                a, b = r.choice(starts), r.randrange(nodes)
                if event:
                    c.request('POST', f'/inputs/route?start={a}&goal={b}', b'', {'Content-Length': '0'})
                else:
                    c.request('GET', f'/views/distance?start={a}&goal={b}')
                answer = c.getresponse()
                body = answer.read()
                assert answer.status == 200 and (event or int(body) == far(a, b)), (a, b, answer.status, body)
                done.append(1)

        def views(count):
            done, until = [], time.monotonic() + seconds
            ts = [threading.Thread(target=ask, args=(port, until, k * 7919 + len(model), done)) for k in range(count)]
            for t in ts:
                t.start()
            for t in ts:
                t.join()
            return len(done) / seconds

        def writes(way):
            done, ts = [], []
            if way != 'writes':
                until = time.monotonic() + seconds + 0.5
                ts = [threading.Thread(target=ask, args=(apart_port if way == 'apart' else port, until, k * 104729, done,
                                                         way == 'events')) for k in range(askers)]
                for t in ts:
                    t.start()
            got = subprocess.run(['taskset', '-c', '3', str(LOAD), str(port), '16', str(seconds), 'score', '64', 'keep-alive'],
                                 capture_output=True, text=True, check=True).stdout
            for t in ts:
                t.join()
            result = json.loads(got)
            assert result['2xx'] == result['completed'] and not result['errors'], result
            return result, len(done) / seconds

        rates, p99 = {}, {}
        for round_ in range(1, rounds + 1):
            ways = chosen
            for way in (ways if round_ % 2 else ways[::-1]):
                if way in ('writes', 'both', 'apart', 'events'):
                    result, v = writes(way)
                    rates.setdefault(way, []).append(result['requests_per_second'])
                    p99.setdefault(way, []).append(result['p99_us'])
                    line = {'round': round_, 'way': way, 'writes_per_second': result['requests_per_second'],
                            'write_p50_us': result['p50_us'], 'write_p99_us': result['p99_us'],
                            'write_max_us': result['max_us'], 'views_per_second': round(v, 1)}
                else:
                    v = views(1 if way == 'one' else askers)
                    rates.setdefault(way, []).append(v)
                    line = {'round': round_, 'way': way, 'views_per_second': round(v, 1)}
                print(json.dumps(line), flush=True)
        for h in (host, apart):
            h.terminate()
            h.communicate(timeout=60)
        med = {k: statistics.median(v) for k, v in rates.items()}
        tail = {k: statistics.median(v) for k, v in p99.items()}
        summary = {'nodes': nodes, 'edges': len(edges), 'threads': threads, 'askers': askers,
                   'medians': {k: round(v, 1) for k, v in med.items()}, 'write_p99_medians_us': tail}
        if 'writes' in med:
            summary['writes_kept'] = {k: round(med[k] / med['writes'] * 100, 1) for k in ('both', 'apart', 'events') if k in med}
        if 'both' in tail and 'apart' in tail:
            summary['p99_both_over_apart'] = round(tail['both'] / tail['apart'], 2)
        if 'one' in med and 'many' in med:
            summary['scaling'] = round(med['many'] / med['one'], 2)
        print(json.dumps(summary), flush=True)
    finally:
        shutil.rmtree(work)


if __name__ == '__main__':
    main()
